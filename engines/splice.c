/*
 * [한국어 설명] splice(2) 기반 제로카피 I/O 엔진 구현 (splice.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux 의 splice(2)/vmsplice(2) 시스템 호출을 이용해 파일 ↔ 사용자 버퍼 사이
 * 데이터 전송을 **커널 파이프 버퍼**를 매개로 수행하는 fio I/O 엔진이다. 일반 read/write
 * 경로가 "커널 페이지캐시 → 사용자 버퍼" 복사를 동반하는 것과 달리, splice 는 페이지 단위
 * 참조(스트라이드)만 파이프로 옮기고, vmsplice 는 사용자 버퍼의 페이지를 파이프에 **맵핑**
 * 하거나 파이프로부터 **언맵**해 가져오는 식으로 카피를 최소화한다. 이 엔진의 존재 이유는
 * splice 경로 자체의 대역폭/오버헤드 측정과, vmsplice-to-user 의 커널 지원 여부 탐지이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: main → fio_backend → load_ioengine("splice") → ioengines.c 가 이 파일의
 * fio_spliceio_register(constructor) 로 등록해 둔 ioengine_ops 를 잡 스레드에 연결한다.
 * 잡 스레드 루프: td_io_queue → fio_spliceio_queue → (READ/WRITE 분기) → splice/vmsplice
 * 시스템 호출 → 커널 파이프 버퍼(struct pipe_inode_info) → 타깃 파일/사용자 메모리.
 * 실행 컨텍스트는 전적으로 **잡 스레드 유저스페이스**이며, I/O 가 동기적으로 끝나므로
 * getevents/event 콜백은 두지 않고 queue 에서 FIO_Q_COMPLETED 로 즉시 반환한다.
 *
 * === 타 모듈과의 연결 ===
 * 상위: fio 코어(ioengines.c, io_u.c)가 io_u 를 채워 넣고, td_io_queue 로 제출한다.
 * 하위: Linux 커널의 splice/vmsplice/pipe/read/poll/mmap/munmap 시스템 콜.
 * 공유 상태: struct spliceio_data(파이프 fd 쌍과 vmsplice 지원 플래그)를 td->io_ops_data
 * 에 저장한다. generic_open_file/close_file/get_file_size 는 filesetup.c 의 공용 구현을
 * 재사용해 파일 fd 수명은 fio 코어가 관리한다.
 * 데이터 흐름(READ): 파일 fd → splice → pipe[1] → pipe[0] → (vmsplice/read) → 사용자 버퍼.
 * 데이터 흐름(WRITE): 사용자 버퍼 → vmsplice → pipe[1] → pipe[0] → splice → 파일 fd.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_spliceio_init: pipe(2) 로 파이프 쌍 생성, 낙관적 vmsplice 지원 가정 플래그 ON.
 * - fio_splice_read: 신형 — file→pipe(splice) + pipe→user(vmsplice). EFAULT 시 mmap 폴백 해제,
 *   EBADF 시 구형 경로로 폴백 요청.
 * - fio_splice_read_old: 구형 — file→pipe(splice) + pipe→user(read). vmsplice-to-user
 *   를 지원하지 않는 커널용.
 * - fio_splice_write: user→pipe(vmsplice) + pipe→file(splice). 파이프가 가득 차면 poll.
 * - fio_spliceio_queue: ddir 분기 및 폴백 제어, 부분 전송(resid) 처리.
 * - struct spliceio_data: 파이프 fd 쌍과 폴백 플래그 2종.
 */

/*
 * splice engine
 *
 * IO engine that transfers data by doing splices to/from pipes and
 * the files.
 *
 */
#include <stdio.h>          /* [한국어] 표준 입출력(에러 메시지 포매팅에 간접적으로 사용). */
#include <stdlib.h>         /* [한국어] malloc/free 사용. */
#include <unistd.h>         /* [한국어] read/close/pipe 등 POSIX 시스템 콜 래퍼. */
#include <errno.h>          /* [한국어] errno 전역 — splice/vmsplice 에러 분기에 필수. */
#include <poll.h>           /* [한국어] 파이프 쓰기 가능 여부 대기용 poll(2). */
#include <sys/mman.h>       /* [한국어] vmsplice-to-user 의 선택적 mmap 경로(PROT_READ/MAP_PRIVATE). */

#include "../fio.h"         /* [한국어] thread_data/io_u/fio_file 및 ioengine_ops, SPLICE_DEF_SIZE, OS_MAP_ANON 등 fio 공용 선언. */

/*
 * [한국어] struct spliceio_data — splice 엔진이 잡 당 보유하는 상태.
 * td->io_ops_data 로 부착되어 init 에서 생성되고 cleanup 에서 해제된다.
 * 단일 잡 스레드에서만 접근되므로 동기화는 필요 없다.
 */
/* [한국어] splice 엔진의 내부 상태 구조체 */
struct spliceio_data {
	int pipe[2];
	/* [한국어] splice 의 중간 매개체가 되는 익명 파이프의 fd 쌍.
	 * pipe[0] = 읽기 끝(reader), pipe[1] = 쓰기 끝(writer).
	 * 설정자: fio_spliceio_init 의 pipe(2).
	 * 읽는 자: READ 경로는 pipe[1] 로 splice-in, pipe[0] 으로 vmsplice-out.
	 *          WRITE 경로는 pipe[1] 로 vmsplice-in, pipe[0] 으로 splice-out.
	 * 동기화: 단일 잡 스레드 전용 → 락 불필요.
	 * 해제: cleanup 에서 두 fd 모두 close. */

	int vmsplice_to_user;
	/* [한국어] 커널이 "파이프→사용자 메모리" 방향의 vmsplice 를 지원하는지의 런타임 플래그.
	 * 초기값 1(낙관). queue 에서 fio_splice_read 가 EBADF 를 리턴하면 0 으로 폴백하여
	 * 이후 호출부터 fio_splice_read_old(read(2) 기반) 경로를 사용한다.
	 * 쓰기 경로(fio_splice_write)에는 영향을 주지 않는다. */

	int vmsplice_to_user_map;
	/* [한국어] vmsplice-to-user 의 **mmap 매핑 기반 경로** 지원 여부.
	 * 초기값 1. fio_splice_read 내부에서 vmsplice 가 EFAULT 를 내면 0 으로 떨어뜨리고
	 * 현재 요청부터 io_u->xfer_buf 를 그대로 쓰는 경로로 전환한다. 이후 재진입 시
	 * 처음부터 비-mmap 경로를 타게 된다. */
};

/*
 * vmsplice didn't use to support splicing to user space, this is the old
 * variant of getting that job done. Doesn't make a lot of sense, but it
 * uses splices to move data from the source into a pipe.
 */
/*
 * [한국어]
 * fio_splice_read_old - vmsplice-to-user 미지원 커널용 읽기 구현.
 *
 * @td: 잡 스레드 thread_data.
 * @io_u: 읽기 대상 I/O 유닛(파일 fd, 오프셋, 사용자 버퍼 포함).
 * @return: 성공 시 전송한 총 바이트(= io_u->xfer_buflen), 실패 시 -errno.
 *
 * 동작: "파일 → splice → 파이프" 로 제로카피 이동한 뒤, "파이프 → read(2) → 사용자 버퍼"
 *       로 실제 바이트를 꺼낸다. 파이프의 한 번 최대 용량을 SPLICE_DEF_SIZE 로 쪼개 반복.
 * 컨텍스트: 잡 스레드 동기 실행.
 * 호출 체인: fio_spliceio_queue(DDIR_READ, vmsplice_to_user=0) → fio_splice_read_old.
 */
static int fio_splice_read_old(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops_data;                             /* [한국어] 엔진 상태(파이프 fd) 획득. */
	struct fio_file *f = io_u->file;                                        /* [한국어] 소스 파일 메타. */
	int ret, ret2, buflen;                                                  /* [한국어] ret=splice 결과, ret2=read 결과, buflen=남은 바이트. */
	off_t offset;                                                           /* [한국어] 파일 내 현재 오프셋(splice 가 in-place 갱신). */
	char *p;                                                                /* [한국어] 사용자 버퍼 진행 포인터. */

	offset = io_u->offset;                                                  /* [한국어] 요청 시작 오프셋 로드. */
	buflen = io_u->xfer_buflen;                                             /* [한국어] 총 요청 길이. */
	p = io_u->xfer_buf;                                                     /* [한국어] 사용자 버퍼 시작. */
	while (buflen) {                                                        /* [한국어] 남은 바이트가 0 될 때까지. */
		int this_len = buflen;                                          /* [한국어] 이번 iteration 에서 처리할 길이. */

		if (this_len > SPLICE_DEF_SIZE)                                 /* [한국어] 파이프 최대 버퍼(보통 64KiB) 로 제한. */
			this_len = SPLICE_DEF_SIZE;

		ret = splice(f->fd, &offset, sd->pipe[1], NULL, this_len, SPLICE_F_MORE); /* [한국어] 파일→파이프 방향 splice. offset 이 자동 증가. SPLICE_F_MORE=곧 또 보낼 것이란 힌트. */
		if (ret < 0) {                                                  /* [한국어] splice 실패. */
			if (errno == ENODATA || errno == EAGAIN)                /* [한국어] 일시적 상황이면 재시도(비파괴). */
				continue;

			return -errno;                                          /* [한국어] 그 외 에러는 -errno 로 상위 전파. */
		}

		buflen -= ret;                                                  /* [한국어] 파이프에 들어간 만큼 남은 양 감소. */

		while (ret) {                                                   /* [한국어] 파이프에 실제로 쌓인 바이트를 사용자 버퍼로 draining. */
			ret2 = read(sd->pipe[0], p, ret);                       /* [한국어] 파이프 읽기 끝에서 read(2). 여기서 실제 카피 1회 발생. */
			if (ret2 < 0)
				return -errno;                                  /* [한국어] read 에러 → 실패. */

			ret -= ret2;                                            /* [한국어] 아직 남은 파이프 데이터. */
			p += ret2;                                              /* [한국어] 버퍼 포인터 전진. */
		}
	}

	return io_u->xfer_buflen;                                               /* [한국어] 전체 요청 바이트 수 반환(성공). */
}

/*
 * We can now vmsplice into userspace, so do the transfer by splicing into
 * a pipe and vmsplicing that into userspace.
 */
/*
 * [한국어]
 * fio_splice_read - vmsplice-to-user 지원 커널용 신형 읽기 구현.
 *
 * @td/@io_u: 위와 동일 의미.
 * @return: 성공 시 총 전송 바이트, 에러 시 음수(-errno 또는 -EBADF/폴백 요청).
 *
 * 동작: file→pipe(splice) + pipe→user(vmsplice)[SPLICE_F_MOVE]. 필요 시 PROT_READ/MAP_PRIVATE
 *       로 사용자 버퍼 주소 위에 익명 매핑을 덮어 씌워 vmsplice 가 그 매핑으로 페이지를
 *       실제로 "이사" 시키도록 한다. EFAULT → mmap 경로 비활성화 후 그 자리에서 재시도.
 *       EBADF → 엔진 전체에서 vmsplice-to-user 를 포기하고 구형 경로로 폴백 신호.
 * 컨텍스트: 잡 스레드 동기 실행.
 */
static int fio_splice_read(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops_data;                             /* [한국어] 엔진 상태. */
	struct fio_file *f = io_u->file;                                        /* [한국어] 소스 파일. */
	struct iovec iov;                                                       /* [한국어] vmsplice 로 전달할 사용자 영역 설명자(주소+길이). */
	int ret , buflen, mmap_len;                                             /* [한국어] ret=단계별 결과, buflen=남은 전송량, mmap_len=munmap 에 쓸 길이. */
	off_t offset;                                                           /* [한국어] 파일 오프셋(splice 가 갱신). */
	void *map;                                                              /* [한국어] 덮어쓴 mmap 의 시작 주소(없으면 NULL). */
	char *p;                                                                /* [한국어] 현재 기록 포인터. */

	ret = 0;                                                                /* [한국어] 루프 밖 에러 판정용 초기화. */
	offset = io_u->offset;                                                  /* [한국어] 시작 오프셋. */
	mmap_len = buflen = io_u->xfer_buflen;                                  /* [한국어] munmap 길이와 남은 양을 동일 초기화. */

	if (sd->vmsplice_to_user_map) {                                         /* [한국어] mmap 기반 vmsplice 경로가 아직 살아 있으면. */
		map = mmap(io_u->xfer_buf, buflen, PROT_READ, MAP_PRIVATE|OS_MAP_ANON, 0, 0); /* [한국어] 사용자 버퍼 위에 익명 맵을 덮어 씌움 — vmsplice 가 페이지를 "이동" 시킬 대상 확보. */
		if (map == MAP_FAILED) {                                        /* [한국어] mmap 실패(주소 오염/메모리 부족 등). */
			td_verror(td, errno, "mmap io_u");
			return -1;
		}

		p = map;                                                        /* [한국어] 기록 포인터를 매핑 영역으로. */
	} else {
		map = NULL;                                                     /* [한국어] 매핑을 쓰지 않음. */
		p = io_u->xfer_buf;                                             /* [한국어] 원래 사용자 버퍼로 직접 기록. */
	}

	while (buflen) {                                                        /* [한국어] 요청 바이트가 남아 있는 동안. */
		int this_len = buflen;                                          /* [한국어] 이번 라운드 처리량. */
		int flags = 0;                                                  /* [한국어] splice 플래그(마지막 조각이면 0, 아니면 SPLICE_F_MORE). */

		if (this_len > SPLICE_DEF_SIZE) {                               /* [한국어] 파이프 용량 초과면 자르고 "더 있음" 힌트. */
			this_len = SPLICE_DEF_SIZE;
			flags = SPLICE_F_MORE;
		}

		ret = splice(f->fd, &offset, sd->pipe[1], NULL, this_len,flags); /* [한국어] 파일→파이프. offset 자동 증가, flags 는 성능 힌트. */
		if (ret < 0) {                                                  /* [한국어] splice 실패. */
			if (errno == ENODATA || errno == EAGAIN)                /* [한국어] 일시적이면 재시도. */
				continue;

			td_verror(td, errno, "splice-from-fd");                 /* [한국어] 그 외 에러 기록. */
			break;
		}

		buflen -= ret;                                                  /* [한국어] 파이프로 옮긴 만큼 감소. */
		iov.iov_base = p;                                               /* [한국어] 사용자 도착지 주소. */
		iov.iov_len = ret;                                              /* [한국어] 이번에 옮겨온 바이트 수만큼 받아야 함. */

		while (iov.iov_len) {                                           /* [한국어] 파이프에 쌓인 분을 다 빼낼 때까지. */
			ret = vmsplice(sd->pipe[0], &iov, 1, SPLICE_F_MOVE);    /* [한국어] 파이프→유저 방향 vmsplice. SPLICE_F_MOVE=페이지 소유권 이동 시도. */
			if (ret < 0) {                                          /* [한국어] vmsplice 에러. */
				if (errno == EFAULT &&
				    sd->vmsplice_to_user_map) {                 /* [한국어] mmap 기반 경로에서만 일어나는 EFAULT 처리. */
					sd->vmsplice_to_user_map = 0;           /* [한국어] 이후부터 mmap 경로 끄기(엔진 수명 내내). */
					munmap(map, mmap_len);                  /* [한국어] 씌웠던 맵 해제. */
					map = NULL;                             /* [한국어] dangling 방지. */
					p = io_u->xfer_buf;                     /* [한국어] 기록 대상 포인터를 원래 버퍼로 환원. */
					iov.iov_base = p;                       /* [한국어] 같은 루프에서 즉시 재시도. */
					continue;
				}
				if (errno == EBADF) {                           /* [한국어] vmsplice-to-user 자체를 커널이 거부. */
					ret = -EBADF;                           /* [한국어] 상위(queue) 가 엔진 전체 폴백 처리하도록 시그널. */
					break;
				}
				td_verror(td, errno, "vmsplice");               /* [한국어] 그 외 에러 기록. */
				break;
			} else if (!ret) {                                      /* [한국어] 0 바이트 반환 = 데이터 없음(비정상). */
				td_verror(td, ENODATA, "vmsplice");
				ret = -1;
				break;
			}

			iov.iov_len -= ret;                                     /* [한국어] 아직 꺼낼 양. */
			iov.iov_base += ret;                                    /* [한국어] 도착지 포인터 전진. */
			p += ret;                                               /* [한국어] 기록 포인터 전진(다음 outer iteration 에서 iov.iov_base 기준). */
		}
		if (ret < 0)                                                    /* [한국어] 내부 루프에서 에러 발생했으면 바깥 루프도 종료. */
			break;
	}

	if (sd->vmsplice_to_user_map && munmap(map, mmap_len) < 0) {            /* [한국어] 정상 종료 시에도 덮어쓴 매핑을 해제해야 함. */
		td_verror(td, errno, "munnap io_u");                            /* [한국어] (원본 오탈자 유지) munmap 에러 기록. */
		return -1;
	}
	if (ret < 0)                                                            /* [한국어] 에러로 끝났다면 음수 전달(-EBADF 포함). */
		return ret;

	return io_u->xfer_buflen;                                               /* [한국어] 전체 성공: 총 전송량 반환. */
}

/*
 * For splice writing, we can vmsplice our data buffer directly into a
 * pipe and then splice that to a file.
 */
/*
 * [한국어]
 * fio_splice_write - 쓰기 경로.
 *
 * @td/@io_u: 잡/요청.
 * @return: 전송 바이트 수 또는 -errno.
 *
 * 동작: 사용자 버퍼 전체를 iovec 하나로 감싸서 "유저→파이프(vmsplice)",
 *       파이프가 가득 차지 않도록 poll(POLLOUT) 로 동기화하고,
 *       이어서 "파이프→파일(splice)" 로 디스크에 기록. splice 는 NONBLOCK 이 아닌
 *       기본 모드로 호출되어 커널이 필요한 만큼 대기한다.
 */
static int fio_splice_write(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops_data;                             /* [한국어] 엔진 상태(파이프). */
	struct iovec iov = {
		.iov_base = io_u->xfer_buf,                                     /* [한국어] 전송 소스 = 사용자 버퍼 시작. */
		.iov_len = io_u->xfer_buflen,                                   /* [한국어] 전체 길이. */
	};
	struct pollfd pfd = { .fd = sd->pipe[1], .events = POLLOUT, };          /* [한국어] 파이프 쓰기 끝에 대한 POLLOUT 대기 설정(파이프 가득 찼을 때). */
	struct fio_file *f = io_u->file;                                        /* [한국어] 타깃 파일. */
	off_t off = io_u->offset;                                               /* [한국어] 파일 내 기록 시작 오프셋(splice 가 증가시킴). */
	int ret, ret2;                                                          /* [한국어] ret=vmsplice 결과, ret2=splice 결과. */

	while (iov.iov_len) {                                                   /* [한국어] 사용자 버퍼 다 보낼 때까지. */
		if (poll(&pfd, 1, -1) < 0)                                      /* [한국어] 파이프가 쓰기 가능해질 때까지 블록 대기. */
			return errno;                                           /* [한국어] 관례상 양수 errno 반환(상위에서 에러 처리). */

		ret = vmsplice(sd->pipe[1], &iov, 1, SPLICE_F_NONBLOCK);        /* [한국어] 유저 페이지를 파이프에 "참조"로 밀어넣음(카피 없음). */
		if (ret < 0)
			return -errno;                                          /* [한국어] vmsplice 실패 → -errno. */

		iov.iov_len -= ret;                                             /* [한국어] 아직 안 보낸 바이트. */
		iov.iov_base += ret;                                            /* [한국어] 소스 포인터 전진. */

		while (ret) {                                                   /* [한국어] 파이프에 넣은 만큼을 즉시 파일로 흘려보냄. */
			ret2 = splice(sd->pipe[0], NULL, f->fd, &off, ret, 0);  /* [한국어] 파이프→파일 splice. off 자동 증가. */
			if (ret2 < 0)
				return -errno;

			ret -= ret2;                                            /* [한국어] 파이프에 아직 남은 양. */
		}
	}

	return io_u->xfer_buflen;                                               /* [한국어] 전체 성공. */
}

/*
 * [한국어]
 * fio_spliceio_queue - splice 엔진의 I/O 제출 콜백
 *
 * READ: vmsplice 지원 시 fio_splice_read(파이프+vmsplice), 미지원 시
 *       fio_splice_read_old(파이프+read) 사용. EBADF로 폴백을 감지한다.
 * WRITE: fio_splice_write(vmsplice→파이프→splice→파일) 사용.
 * 모든 I/O가 동기적으로 완료되므로 FIO_Q_COMPLETED를 반환한다.
 *
 * 호출 체인: td_io_queue() → [이 함수] → splice(2)/vmsplice(2)
 *
 * 추가 설명: 반환값이 요청 길이와 다르면 부분 전송(resid) 으로 간주하고,
 * 음수면 errno 를 io_u->error 에 기록. EINVAL 은 대부분 해당 파일시스템이
 * splice 를 지원하지 않는 상황이라 별도 안내 메시지를 남긴다.
 */
static enum fio_q_status fio_spliceio_queue(struct thread_data *td,
					    struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops_data;                             /* [한국어] 엔진 상태. */
	int ret = 0;                                                            /* [한국어] 하위 함수 반환값 누적. */

	fio_ro_check(td, io_u);                                                 /* [한국어] 읽기전용 잡 보호. 쓰기 요청이면 에러 처리. */

	if (io_u->ddir == DDIR_READ) {                                          /* [한국어] 읽기 요청. */
		if (sd->vmsplice_to_user) {                                     /* [한국어] 신형 경로가 아직 가능하면 먼저 시도. */
			ret = fio_splice_read(td, io_u);
			/*
			 * This kernel doesn't support vmsplice to user
			 * space. Reset the vmsplice_to_user flag, so that
			 * we retry below and don't hit this path again.
			 */
			if (ret == -EBADF)                                      /* [한국어] 커널 미지원 → 플래그 내리고 구형 경로로 재시도. */
				sd->vmsplice_to_user = 0;
		}
		if (!sd->vmsplice_to_user)                                      /* [한국어] (처음부터 0 이었거나 방금 내려갔다면). */
			ret = fio_splice_read_old(td, io_u);                    /* [한국어] 구형 경로 실행. */
	} else if (io_u->ddir == DDIR_WRITE)                                    /* [한국어] 쓰기. */
		ret = fio_splice_write(td, io_u);
	else if (io_u->ddir == DDIR_TRIM)                                       /* [한국어] TRIM 은 splice 가 처리할 수 없으므로 공용 헬퍼로 위임. */
		ret = do_io_u_trim(td, io_u);
	else                                                                    /* [한국어] SYNC/FLUSH 계열도 공용 헬퍼로 위임. */
		ret = do_io_u_sync(td, io_u);

	if (ret != (int) io_u->xfer_buflen) {                                   /* [한국어] 요청 길이와 불일치(부분전송 또는 에러). */
		if (ret >= 0) {                                                 /* [한국어] 부분전송. */
			io_u->resid = io_u->xfer_buflen - ret;                  /* [한국어] 나머지 바이트 기록 → fio 코어가 재제출 여부 판단. */
			io_u->error = 0;                                        /* [한국어] 에러는 없음. */
			return FIO_Q_COMPLETED;                                 /* [한국어] 동기 엔진이므로 즉시 완료 통지. */
		} else
			io_u->error = errno;                                    /* [한국어] 하위 함수가 -errno 반환 → errno 전역으로부터 캡처. */
	}

	if (io_u->error) {                                                      /* [한국어] 에러 처리 공통. */
		td_verror(td, io_u->error, "xfer");                             /* [한국어] fio 에 에러 등록. */
		if (io_u->error == EINVAL)                                      /* [한국어] EINVAL 은 대개 FS 가 splice 미지원. */
			log_err("fio: looks like splice doesn't work on this"
					" file system\n");
	}

	return FIO_Q_COMPLETED;                                                 /* [한국어] 동기 완료 통지. */
}

/*
 * [한국어]
 * fio_spliceio_cleanup - 엔진 종료 시 파이프 fd 와 상태 구조체를 해제한다.
 * 실행 컨텍스트: 잡 스레드. sd NULL 안전.
 */
static void fio_spliceio_cleanup(struct thread_data *td)
{
	struct spliceio_data *sd = td->io_ops_data;                             /* [한국어] 엔진 상태. */

	if (sd) {                                                               /* [한국어] 중복/실패 초기화 대비. */
		close(sd->pipe[0]);                                             /* [한국어] 파이프 읽기 끝 닫기. */
		close(sd->pipe[1]);                                             /* [한국어] 파이프 쓰기 끝 닫기. */
		free(sd);                                                       /* [한국어] 상태 해제. */
	}
}

/*
 * [한국어]
 * fio_spliceio_init - splice 엔진 초기화 콜백
 *
 * 데이터 전송 중간 매개체인 파이프를 생성하고,
 * vmsplice 사용자공간 전송을 낙관적으로 활성화한다(실패 시 자동 폴백).
 *
 * 주의: malloc 실패 체크가 없고, pipe 실패 경로에서도 sd 를 해제하는 점(할당 성공을 전제).
 * 실행 컨텍스트: 잡 스레드(잡 시작 1회).
 */
static int fio_spliceio_init(struct thread_data *td)
{
	struct spliceio_data *sd = malloc(sizeof(*sd));                         /* [한국어] 엔진 상태 할당. */

	if (pipe(sd->pipe) < 0) {                                               /* [한국어] 익명 파이프 생성. fd[0]=read, fd[1]=write. */
		td_verror(td, errno, "pipe");
		free(sd);                                                       /* [한국어] 실패 시 상태 해제(부분 초기화 정리). */
		return 1;
	}

	/*
	 * Assume this work, we'll reset this if it doesn't
	 */
	sd->vmsplice_to_user = 1;                                               /* [한국어] 일단 신형 경로 사용 시도. 실패 시 queue 에서 0 으로 내린다. */

	/*
	 * Works with "real" vmsplice to user, eg mapping pages directly.
	 * Reset if we fail.
	 */
	sd->vmsplice_to_user_map = 1;                                           /* [한국어] mmap 기반 경로도 우선 활성화. EFAULT 발생 시 0 으로 내린다. */

	td->io_ops_data = sd;                                                   /* [한국어] fio 코어에 상태 등록. */
	return 0;                                                               /* [한국어] 성공. */
}

/*
 * [한국어] ioengine — splice 엔진의 콜백 테이블.
 * FIO_SYNCIO: queue 가 동기 완료하므로 getevents 불필요.
 * FIO_PIPEIO: 엔진이 파이프를 사용한다는 표식(실제 파일 시크 가정이 다름을 코어에 알림).
 * 파일 수명(open/close/size)은 공용 generic_* 를 그대로 사용.
 */
static struct ioengine_ops ioengine = {
	.name		= "splice",                                             /* [한국어] --ioengine=splice 로 선택. */
	.version	= FIO_IOOPS_VERSION,                                    /* [한국어] ABI 버전. */
	.init		= fio_spliceio_init,                                    /* [한국어] 초기화 훅. */
	.queue		= fio_spliceio_queue,                                   /* [한국어] I/O 제출 훅(동기). */
	.cleanup	= fio_spliceio_cleanup,                                 /* [한국어] 종료 훅. */
	.open_file	= generic_open_file,                                    /* [한국어] filesetup.c 의 공용 open 재사용. */
	.close_file	= generic_close_file,                                   /* [한국어] 공용 close. */
	.get_file_size	= generic_get_file_size,                                /* [한국어] 공용 stat 기반 크기 계산. */
	.flags		= FIO_SYNCIO | FIO_PIPEIO,                              /* [한국어] 동기 엔진 + 파이프 사용 표식. */
};

/*
 * [한국어]
 * fio_spliceio_register - 모듈 로드 시 엔진 등록 (fio_init=constructor 속성).
 */
static void fio_init fio_spliceio_register(void)
{
	register_ioengine(&ioengine);                                           /* [한국어] ioengines.c 의 전역 리스트에 추가. */
}

/*
 * [한국어]
 * fio_spliceio_unregister - 모듈 언로드 시 엔진 해제 (fio_exit=destructor 속성).
 */
static void fio_exit fio_spliceio_unregister(void)
{
	unregister_ioengine(&ioengine);                                         /* [한국어] 전역 리스트에서 제거. */
}
