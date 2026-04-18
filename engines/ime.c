/*
 * FIO engines for DDN's Infinite Memory Engine.
 * This file defines 3 engines: ime_psync, ime_psyncv, and ime_aio
 *
 * Copyright (C) 2018      DataDirect Networks. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2 as published by the Free Software Foundation..
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/*
 * [한국어 설명] DDN IME(Infinite Memory Engine) 버스트 버퍼 I/O 엔진 묶음 (ime.c)
 *
 * === 파일의 역할 ===
 * DDN(DataDirect Networks)의 분산 버스트 버퍼/캐시 플랫폼 "Infinite Memory Engine(IME)"
 * 을 fio가 직접 두드리도록 해주는 세 가지 I/O 엔진을 하나의 translation unit에서
 * 함께 구현한다. 세 엔진은 동일한 IME 네이티브 라이브러리(libim_client)를 공유하지만,
 * fio 코어가 요구하는 ioengine_ops 콜백 계약을 각각 다른 방식으로 채워서 "동기 vs
 * iovec 일괄 vs 진짜 비동기" 세 가지 사용 패턴을 노출한다.
 *
 *   1) ime_psync   — 가장 단순한 동기 엔진. queue() 콜백 안에서 ime_native_pread/
 *                    pwrite/fsync를 직접 호출하고 즉시 FIO_Q_COMPLETED를 반환한다.
 *                    iodepth>1 의미가 없고, ime_data 구조체도 사용하지 않는다.
 *                    fio가 보낸 io_u 1개 = IME 호출 1회 = 결과 반환 1회.
 *   2) ime_psyncv  — iovec 누적 동기 엔진. fio가 iodepth_batch 옵션으로 io_u를 묶어
 *                    submit하면, queue()는 iovec 슬롯에 쌓아두고 FIO_Q_QUEUED만
 *                    반환한다. 이후 commit()이 호출되면 누적된 iovec을 ime_native_
 *                    preadv/pwritev 한 번의 호출로 IME에 보낸다(scatter/gather I/O).
 *                    호출 자체는 동기이지만 fio API는 비동기 인터페이스를 사용한다.
 *                    한 배치는 단일 fd/단일 방향/연속 오프셋이어야 한다.
 *   3) ime_aio     — 진정한 비동기 엔진. ime_native_aio_read/aio_write로 여러 요청을
 *                    동시에 IME에 던지고, IME 라이브러리의 내부 스레드가 완료 콜백
 *                    fio_ime_aio_complete_cb()를 호출하면 status/cond_signal로
 *                    잡 스레드를 깨운다. 한 요청 안에서도 인접한 io_u는 iovec 배열의
 *                    연속 슬롯으로 묶어 vector AIO로 효율화한다(can_append 경로).
 *
 * 세 엔진 모두 IME만의 경로 규약("im://path" 또는 DEFAULT_IME_FILE_PREFIX 접두)을
 * 처리하기 위해 generic_open_file 대신 자체 fio_ime_open_file을 둔다. POSIX stat 도
 * 사용할 수 없으므로 fio_ime_get_file_size 가 ime_native_stat 으로 메타데이터를 얻고,
 * fio 코어가 stat 단계를 건너뛰도록 fio_ime_setup이 real_file_size=0으로 일시 표식한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   사용자가 --ioengine=ime_psync | ime_psyncv | ime_aio 중 하나를 선택하면
 *   load_ioengine(ioengines.c) 이 본 파일이 fio_init 생성자 경로(fio_ime_register)에서
 *   global engine_list 에 등록해 둔 ioengine_prw / ioengine_pvrw / ioengine_aio 중
 *   해당 ioengine_ops 를 잡 스레드의 td->io_ops 로 바인딩한다.
 *
 *   잡 라이프사이클(엔진별 콜백 호출 순서, backend.c 의 잡 루프 기준):
 *
 *     fio_backend
 *       └→ thread_main (잡 스레드 1개)
 *            ├→ td_io_init     ─→ ops->setup     = fio_ime_setup     (real_file_size=0)
 *            │                ─→ ops->init      = fio_ime_engine_init / _psyncv_init / _aio_init
 *            │                                     (ime_native_init + ime_data 할당 + cond/mutex 초기화)
 *            ├→ td_io_open_file─→ ops->open_file = fio_ime_open_file (TRIM 거부, ime_native_open)
 *            ├→ 본 루프
 *            │    ├→ td_io_queue   ─→ ops->queue
 *            │    │                     · psync : ime_native_pread/pwrite/fsync 동기 호출
 *            │    │                     · psyncv: iovec 적재 (FIO_Q_QUEUED 또는 BUSY)
 *            │    │                     · aio   : iovec 적재 (FIO_Q_QUEUED 또는 BUSY)
 *            │    ├→ td_io_commit ─→ ops->commit   (psync 미사용)
 *            │    │                     · psyncv: ime_native_preadv/pwritev 단일 호출
 *            │    │                     · aio   : ime_native_aio_read/write 루프 제출
 *            │    ├→ td_io_getevents ─→ ops->getevents
 *            │    │                     · psyncv: 동기 commit 결과를 event_io_us 로 복사
 *            │    │                     · aio   : status==IN_PROGRESS면 cond_wait
 *            │    └→ ops->event(idx) ─→ event_io_us[idx]
 *            ├→ td_io_close_file─→ ops->close_file = fio_ime_close_file (ime_native_close)
 *            └→ ops->cleanup    ─→ fio_ime_engine_finalize / _psyncv_clean / _aio_clean
 *
 *   실행 컨텍스트:
 *     - 모든 fio 콜백은 잡 스레드(use_thread=0이면 별도 프로세스의 main, =1이면 pthread)에서 실행.
 *     - ime_aio 의 fio_ime_aio_complete_cb 만 IME 라이브러리 내부 스레드에서 비동기 호출됨
 *       → status_mutex / cond_endio 로 잡 스레드와 동기화.
 *     - fio_ime_is_initialized 전역 플래그는 잡 스레드 생성 이전(라이브러리 한번 init/finalize
 *       원칙) 에 결정되므로 별도 락 없이 사용. use_thread=1 모드에서는 여러 잡이 같은
 *       프로세스의 IME 라이브러리 인스턴스를 공유한다.
 *
 * === 타 모듈과의 연결 ===
 *   상단(fio core):
 *     - ioengines.c 의 td_io_* 디스패처가 본 파일의 콜백을 호출.
 *     - fio.h 가 공급하는 thread_data, io_u, fio_file, ioengine_ops, FIO_Q_*,
 *       td_verror, fio_ro_check, register_ioengine, dprint, log_err, td_read/td_write/
 *       td_trim, for_each_file, FIO_VERROR_SIZE 등을 사용.
 *   하단(IME SDK = libim_client):
 *     - 초기화/종료    : ime_native_init() / ime_native_finalize()
 *     - 메타데이터     : ime_native_stat() (POSIX stat 호환 buf 반환)
 *     - 파일 핸들     : ime_native_open(path, flags, mode) / ime_native_close(fd) /
 *                       ime_native_ftruncate(fd, size)
 *     - 동기 I/O      : ime_native_pread/pwrite(fd, buf, len, off) /
 *                       ime_native_preadv/pwritev(fd, iov, iovcnt, off) /
 *                       ime_native_fsync(fd)
 *     - 비동기 I/O    : ime_native_aio_read(struct ime_aiocb *) /
 *                       ime_native_aio_write(struct ime_aiocb *)
 *                       (완료는 iocb->complete_cb로 콜백 통지)
 *     - 경로 규약     : DEFAULT_IME_FILE_PREFIX(보통 "ime://")가 ime_native.h에서 제공.
 *                       모든 IME API는 prefix 가 붙은 경로를 요구.
 *   하단(libc): unlink(2)는 IME가 POSIX FUSE 마운트로도 노출되는 경우를 가정해 직접 호출.
 *
 *   데이터 흐름:
 *     fio io_u (xfer_buf, xfer_buflen, offset, ddir)
 *        │
 *        ├─[psync]→ ime_native_p{read,write,fsync} ──┐
 *        ├─[psyncv]→ iovec[head] 슬롯 적재 ──→ commit 시 ime_native_p{readv,writev}
 *        └─[aio]   → ime_aiocb (fd/offset/iov/iovcnt) 적재 ──→ commit 시
 *                    ime_native_aio_{read,write} ──→ IME 라이브러리 ──┐
 *                                                                       ▼
 *                                                            ┌──────────────────┐
 *                                                            │ DDN IME 클라이언트 │
 *                                                            │  (RDMA/TCP RPC)   │
 *                                                            └────────┬─────────┘
 *                                                                       ▼
 *                                                ┌────────────────────────────────────┐
 *                                                │ IME 분산 버스트 버퍼 노드(메모리·NVMe)│
 *                                                │   ─ NVMe SSD를 backing store로 사용 │
 *                                                │   ─ HDD 기반 PFS 앞단의 캐시 계층    │
 *                                                └────────────────────────────────────┘
 *
 *   공유 상태:
 *     - struct ime_data*  : td->io_ops_data 에 저장. psyncv/aio가 사용. 잡 스레드 단독 소유.
 *     - struct imeaio_req::status : aio 완료 콜백(라이브러리 스레드) ↔ getevents(잡 스레드)
 *                                    경계. status_mutex + cond_endio 페어로 보호.
 *     - fio_ime_is_initialized : 프로세스 전역 플래그. ime_native_init이 한 번이라도
 *                                 성공했는지 추적. 잡 스레드 생성 이전 단계에서만 변경.
 *
 * === 주요 함수/구조체 요약 ===
 *   [공통 파일/메타]
 *     - fio_set_ime_filename()    : 사용자 경로에 DEFAULT_IME_FILE_PREFIX 부착(thread-local 버퍼).
 *     - fio_ime_get_file_size()   : ime_native_stat 으로 real_file_size 채움.
 *     - fio_ime_open_file()       : TRIM 거부, ime_native_open + 필요 시 ftruncate.
 *     - fio_ime_close_file()      : ime_native_close, fd=-1 무효화.
 *     - fio_ime_unlink_file()     : POSIX unlink (IME가 FUSE 마운트 노출 가정).
 *     - fio_ime_setup()           : real_file_size=0 으로 fio가 stat 호출하지 않도록 유도.
 *     - fio_ime_engine_init()     : ime_native_init() + 임시 real_file_size 채움.
 *     - fio_ime_engine_finalize() : fork 모드에서만 ime_native_finalize().
 *     - fio_ime_event()           : event_io_us[event] 반환 (psyncv/aio 공용).
 *
 *   [ime_psync 동기 경로]
 *     - fio_ime_psync_queue()     : ime_native_pread/pwrite/fsync 즉시 호출.
 *     - fio_ime_psync_end()       : 반환값 → io_u->error/resid 환산, FIO_Q_COMPLETED 반환.
 *
 *   [ime_psyncv iovec 누적 + 단일 preadv/pwritev]
 *     - fio_ime_psyncv_can_queue(): 빈 큐 또는 동일 fd/ddir/연속 오프셋 + 미수확 이벤트 0.
 *     - fio_ime_psyncv_enqueue()  : iovec 슬롯에 (xfer_buf, xfer_buflen) 기록.
 *     - fio_ime_psyncv_queue()    : 누적 후 FIO_Q_QUEUED, 만원이면 FIO_Q_BUSY.
 *     - fio_ime_psyncv_commit()   : ime_native_preadv/pwritev 단일 호출.
 *     - fio_ime_psyncv_end()      : 반환 바이트를 batch 안의 io_u 들에 순차 분배.
 *     - fio_ime_psyncv_getevents(): io_us → event_io_us 복사 + queue 리셋.
 *     - fio_ime_psyncv_init/clean(): ime_data + sioreq + iovecs + io_us 할당/해제.
 *
 *   [ime_aio 진짜 비동기 경로]
 *     - fio_ime_aio_complete_cb() : IME 라이브러리 콜백. status 갱신 + cond_signal.
 *     - fio_ime_aio_can_append()  : head!=0 + 동일 fd/ddir/연속 오프셋이면 기존 iocb 확장.
 *     - fio_ime_aio_enqueue()     : append 또는 새 iocb 채움 (complete_cb/user_context 포함).
 *     - fio_ime_aio_queue()       : 큐 채움. 만원이면 FIO_Q_BUSY.
 *     - fio_ime_aio_commit()      : ime_native_aio_read/write 루프 제출.
 *     - fio_ime_aio_getevents()   : status!=IN_PROGRESS이면 io_u 복원, 아니면 cond_wait.
 *     - fio_ime_aio_init/clean()  : aioreqs + cond/mutex iodepth개 초기화/파괴.
 *
 *   [구조체]
 *     - struct imesio_req : psyncv 1배치당 단일 (fd/ddir/offset).
 *     - struct imeaio_req : aio 요청 1개 (iocb + status + cond/mutex + ddir).
 *     - struct ime_data   : psyncv/aio 공통 링 큐 컨테이너 (iovecs/io_us/queued/events/
 *                            depth/head/tail/cur_commit/last_offset/last_req).
 *
 *   [엔진 등록]
 *     - ioengine_prw / ioengine_pvrw / ioengine_aio : 세 ioengine_ops vtable.
 *     - fio_ime_register()    : fio_init constructor. 세 vtable 모두 등록.
 *     - fio_ime_unregister()  : fio_exit destructor. 등록 해제 + 지연된 finalize.
 *
 * === DDN IME 도메인 메모 ===
 *   - IME 는 HDD 기반 PFS(Lustre, GPFS, Spectrum Scale 등) "앞에 끼워넣는" 분산
 *     캐시·버스트 버퍼 계층이다. 실제 capacity는 분산된 IME 서버 노드의 RAM + NVMe SSD.
 *   - HPC 잡이 체크포인트나 burst write를 IME 에 빠르게 토하면, IME가 백그라운드로
 *     PFS 에 flush/migrate 한다(write-behind). 읽기는 prefetch.
 *   - 클라이언트 라이브러리(libim_client)는 자체 RPC(보통 RDMA/IB 또는 TCP)로 IME
 *     서버와 통신. POSIX 시스템콜을 거치지 않는다.
 *   - 경로 네임스페이스: "ime://path/to/file" 또는 DEFAULT_IME_FILE_PREFIX("ime:/")
 *     로 시작. ime_native_open()은 이 prefix 가 붙은 경로만 인식.
 *   - TRIM/discard 미지원(블록 디바이스 추상화가 없는 객체-스타일 캐시).
 *
 * === 빌드 게이트 (CONFIG_IME) ===
 *   본 파일은 configure 스크립트가 IME SDK(ime_native.h + libim_client) 설치를
 *   감지했을 때에만 컴파일된다. 검사 흐름:
 *     1) `./configure` 실행 시 `cat_output=yes`로 ime_native.h 테스트 컴파일 시도.
 *     2) 성공 시 configure 가 `config-host.mak` 에 `CONFIG_IME=y` 기록 + 소스
 *        `output_sym "CONFIG_IME"` 로 `config-host.h` 에 `#define CONFIG_IME` 심벌 생성.
 *     3) Makefile 의 `ifdef CONFIG_IME` 블록이 `engines/ime.o` 를 `SOURCE` 에 append,
 *        `-lim_client` 를 `ENGINE_LDLIBS` 에 추가.
 *     4) options.c 의 `#ifdef CONFIG_IME` 블록이 --ioengine 의 possible values 에
 *        ime_psync/ime_psyncv/ime_aio 3개를 help 텍스트와 함께 노출.
 *   본 파일 자체에는 `#ifdef CONFIG_IME` 같은 내부 가드가 없다 — 파일 전체가
 *   configure 단계에서 "컴파일 대상에 포함시킬지/말지" 결정되기 때문이다. 따라서
 *   ime_native.h 가 항상 존재한다고 가정해도 안전하고, NULL-빌드 대응 stub 도 필요 없다.
 *
 * === IMESDK 매크로/에러 코드 관례 ===
 *   - ime_native_open/close/pread/pwrite/preadv/pwritev/fsync/ftruncate/stat 은 모두
 *     POSIX 스타일 반환값을 사용한다: 성공 >= 0, 실패 -1 with errno 설정.
 *     따라서 본 파일에서 `if (ret == -1)` / `if (ret < 0)` 분기 + `errno` 기록이 정석.
 *   - ime_native_aio_read/write 는 비동기 — 반환값은 "제출 성공 0 / 제출 실패 < 0" 로
 *     처리한다(본 파일 fio_ime_aio_commit 참조). 실제 전송 바이트나 완료 상태는
 *     ime_aiocb::complete_cb 로 전달.
 *   - ime_native.h 자체는 DEFAULT_IME_FILE_PREFIX 외의 매크로 상수를 본 파일에서
 *     직접 사용하지 않으며, 에러 코드도 표준 errno(EIO/EINVAL/ENOMEM 등) 로 매핑된다.
 *   - AIO 완료 콜백 시그니처: `void (*ime_complete_cb_t)(struct ime_aiocb *aiocb,
 *     int err, ssize_t bytes);` — err==0 성공/err!=0 에러, bytes = 성공 시 전송량.
 *     IMESDK 헤더에서 typedef im_client_cb_t 로 공급되며, 본 파일은 함수 포인터
 *     형식(&fio_ime_aio_complete_cb) 로 직접 iocb->complete_cb 에 대입한다.
 *
 * === VERIFY / TRIM ddir 처리 ===
 *   - DDIR_READ/WRITE  : 본 파일의 주된 경로. 세 엔진이 각자 방식으로 처리.
 *   - DDIR_SYNC        : ime_native_fsync(fd) 로 즉시 처리. queue() 안에서 동기 실행
 *                        후 FIO_Q_COMPLETED 반환. psyncv/aio 에서도 배치를 거치지 않고
 *                        바이패스.
 *   - DDIR_TRIM        : IME 미지원. fio_ime_open_file 에서 td_trim(td) 검사로 잡
 *                        시작 단계에 EINVAL 거부. 우회: IME 는 객체-스타일 캐시이며
 *                        블록 디바이스 추상화가 없어 discard 개념 자체가 부재.
 *   - VERIFY           : fio 코어가 DDIR_READ 로 재발행하므로 IME 엔진 관점에선 읽기.
 *                        단, open_file 에서 td_write(td)==true 이면 O_RDWR 로 열어 두어
 *                        write → verify read-back 가능.
 *   - DDIR_DATASYNC    : 본 파일은 DDIR_SYNC 만 처리 — DDIR_DATASYNC 진입 시 "wrong ddir"
 *                        으로 EINVAL. 필요하면 ime_native_fdatasync (SDK 제공 시) 추가 가능.
 *   - DDIR_SYNC_FILE_RANGE : 미지원.
 */

/*
 * Some details about the new engines are given below:
 *
 *
 * ime_psync:
 * Most basic engine that issues calls to ime_native whenever an IO is queued.
 *
 * ime_psyncv:
 * This engine tries to queue the IOs (by creating iovecs) if asked by FIO (via
 * iodepth_batch). It refuses to queue when the iovecs can't be appended, and
 * waits for FIO to issue a commit. After a call to commit and get_events, new
 * IOs can be queued.
 *
 * ime_aio:
 * This engine tries to queue the IOs (by creating iovecs) if asked by FIO (via
 * iodepth_batch). When the iovecs can't be appended to the current request, a
 * new request for IME is created. These requests will be issued to IME when
 * commit is called. Contrary to ime_psyncv, there can be several requests at
 * once. We don't need to wait for a request to terminate before creating a new
 * one.
 */

#include <stdio.h>       /* [한국어] snprintf() 사용. fio_set_ime_filename()이 PATH_MAX 버퍼에 prefix+원본 경로를 안전 합성할 때 호출. NULL 종료와 길이 초과 검출(반환 >= PATH_MAX)을 위해 표준 stdio가 필요하다. */
#include <stdlib.h>      /* [한국어] malloc/calloc/free 동적 메모리 관리. struct ime_data, aioreqs[], iovecs[], io_us[] 배열, 단일 sioreq 객체 모두 동적 할당된다. calloc(1, sizeof(*ime_d))는 head/tail/queued/events/cur_commit/last_offset 등 모든 카운터를 0 으로 일괄 초기화하는 코드량 절감 트릭. */
#include <errno.h>       /* [한국어] errno 전역 변수 노출. ime_native_*() / unlink(2)가 실패하면 errno에 부가 정보가 들어가며, td_verror(td, errno, "tag") 로 잡 컨텍스트에 기록한다. EAGAIN/EINVAL/ENOMEM 등 ABI 매크로도 같이 따라옴. */
#include <linux/limits.h>/* [한국어] PATH_MAX 상수(보통 4096) 제공. fio_set_ime_filename()이 thread-local 정적 버퍼 크기로 사용. <limits.h>가 아닌 리눅스 전용 헤더를 쓰는 이유는 일부 환경에서 <limits.h>가 PATH_MAX 를 정의하지 않을 수 있기 때문(POSIX는 PATH_MAX 정의를 강제하지 않는다). */
#include <ime_native.h>  /* [한국어] DDN IME 네이티브 클라이언트 SDK(libim_client) 헤더.
                          *   공급 심볼:
                          *     - ime_native_init() / ime_native_finalize()         : 라이브러리 라이프사이클
                          *     - ime_native_stat(path, struct stat*)               : 메타데이터 조회(POSIX stat 호환 buf)
                          *     - ime_native_open(path, flags, mode) / _close(fd)   : 파일 핸들
                          *     - ime_native_ftruncate(fd, size)                    : 파일 크기 조정
                          *     - ime_native_pread/pwrite(fd, buf, len, off)        : 동기 단일 버퍼 I/O
                          *     - ime_native_preadv/pwritev(fd, iov, cnt, off)      : 동기 scatter/gather I/O
                          *     - ime_native_aio_read/aio_write(struct ime_aiocb*)  : 비동기 I/O 제출
                          *     - ime_native_fsync(fd)                              : 캐시 플러시
                          *     - struct ime_aiocb { fd, file_offset, iov, iovcnt,
                          *                          flags, complete_cb, user_context } : AIO 제어 블록
                          *     - DEFAULT_IME_FILE_PREFIX                           : 경로 prefix(보통 "ime:/")
                          *   링크 시 -lim_client(또는 빌드 시스템이 결정한 라이브러리)가 필요. */

#include "../fio.h"      /* [한국어] fio 코어 공용 헤더.
                          *   본 파일이 사용하는 주요 심볼:
                          *     타입       : struct thread_data, struct io_u, struct fio_file, struct ioengine_ops,
                          *                   enum fio_ddir(DDIR_READ/DDIR_WRITE/DDIR_SYNC), enum fio_q_status,
                          *                   struct timespec.
                          *     반환 매크로: FIO_Q_COMPLETED, FIO_Q_QUEUED, FIO_Q_BUSY (io_u 큐잉 결과 코드).
                          *     플래그     : FIO_SYNCIO, FIO_DISKLESSIO, FIO_IOOPS_VERSION, FIO_VERROR_SIZE.
                          *     판별 매크로: td_read(td), td_write(td), td_trim(td) — 잡의 작업 방향 검사.
                          *     반복 매크로: for_each_file(td, f, i).
                          *     로그/에러 : dprint(FD_FILE/FD_IO, ...), log_err(...), td_verror(td, errno, "tag"),
                          *                   io_u_log_error(td, io_u).
                          *     보조      : fio_ro_check(td, io_u) — read-only 잡에서 쓰기 시도 잡아냄.
                          *     상태/통계  : io_u_mark_submit(td, n) — 제출된 io_u 수 누적(stat.c).
                          *     등록 매크로: fio_init / fio_exit (생성자/소멸자 속성), register_ioengine(),
                          *                   unregister_ioengine().
                          *     기타     : read_only(전역 read-only 모드), fio_unused(매개변수 미사용 표식). */


/**************************************************************
 *              Types and constants definitions
 *
 **************************************************************/

/* define constants for async IOs */
/* [한국어] imeaio_req->status 의 3-state 머신을 구현하는 두 sentinel.
 *
 *   ssize_t status 의 의미 :
 *     -1 (FIO_IME_IN_PROGRESS) : enqueue 직후, 아직 완료 콜백 없음.
 *     -2 (FIO_IME_REQ_ERROR)   : 라이브러리가 err!=0 보고했거나 ime_native_aio_*()
 *                                 자체가 음수 반환(=제출 실패).
 *      0 .. SSIZE_MAX          : 완료 콜백이 전달한 실제 전송 바이트 수.
 *                                 (요청 총 바이트가 부분 전송된 경우에도 유효한 양수.)
 *
 *   왜 ssize_t 음수 영역을 sentinel 로 쓰는가:
 *     IME aio 콜백은 성공 시 bytes>=0 만 전달하므로 음수 영역이 자유롭다.
 *     "완료/미완료/에러" 3상태를 단일 워드로 표현해서 mutex 보호 하에 원자적
 *     기록·판독이 가능하다. 추가 bool/플래그 없이 lock-step 으로 갱신된다. */
/* [한국어] FIO_IME_IN_PROGRESS — "아직 진행 중" sentinel.
 *   - 설정자: fio_ime_aio_enqueue() 가 새 요청을 만들 때 초기값.
 *   - 읽는 자: fio_ime_aio_getevents() 루프가 status 비교, == IN_PROGRESS 면 대기.
 *   - 변환: 완료 콜백이 덮어쓴다(>=0 또는 REQ_ERROR).
 *   - 값 범위: 음수 -1 단일값. */
#define FIO_IME_IN_PROGRESS -1
/* [한국어] FIO_IME_REQ_ERROR — "에러 발생" sentinel.
 *   - 설정자: (a) 완료 콜백 fio_ime_aio_complete_cb()가 err!=0 인 경우.
 *              (b) fio_ime_aio_commit() 가 ime_native_aio_*() 음수 반환을 받았을 때
 *                   콜백 없이 직접 마킹.
 *   - 읽는 자: fio_ime_aio_getevents() 가 이 값을 보면 해당 요청에 묶인 모든
 *              io_u 의 ->error = EIO 로 매핑. (구체적 errno 보존 없이 EIO 단일화.)
 *   - 값 범위: 음수 -2 단일값. */
#define FIO_IME_REQ_ERROR   -2

/* This flag is used when some jobs were created using threads. In that
   case, IME can't be finalized in the engine-specific cleanup function,
   because other threads might still use IME. Instead, IME is finalized
   in the destructor (see fio_ime_unregister), only when the flag
   fio_ime_is_initialized is true (which means at least one thread has
   initialized IME). */
/* [한국어] fio_ime_is_initialized — IME 라이브러리가 "이 프로세스에서 한 번이라도"
 *          ime_native_init() 으로 초기화되었는지 추적하는 프로세스 전역 bool.
 *
 *   왜 필요한가 (use_thread vs fork 모드 구분):
 *     - use_thread=0 (fork 모드, 기본): 잡마다 별도 프로세스. 각 프로세스가 자기
 *       IME 인스턴스를 가지므로 cleanup(fio_ime_engine_finalize)에서 즉시
 *       ime_native_finalize() 호출 안전.
 *     - use_thread=1 (pthread 모드): 모든 잡이 단일 프로세스의 단일 IME 인스턴스를
 *       공유. 한 잡이 cleanup 에서 finalize 하면 다른 잡이 망가진다. 따라서
 *       cleanup 은 finalize 를 건너뛰고, 프로세스 종료 시 fio_exit destructor
 *       (fio_ime_unregister) 가 이 플래그를 보고 한 번만 finalize 한다.
 *
 *   설정자:
 *     - fio_ime_engine_init() 이 ime_native_init() 직후 true 로 설정.
 *   해제자:
 *     - fio_ime_engine_finalize() (fork 모드 cleanup) 가 finalize 후 false.
 *     - fio_ime_unregister() (.fini_array) 가 thread 모드에서 마지막 finalize 후 암묵적.
 *
 *   값 범위: true / false.
 *
 *   동기화:
 *     - fio 의 잡 초기화 순서는 "메인 스레드가 모든 잡 워커를 spawn 하기 전 init/setup"
 *       이므로 잡 스레드 생성 이전에 첫 init() 가 발생할 수 있다. 정상 시나리오에서는
 *       잡 워커 spawn 이전 단계에서만 변경되어 별도 atomic/lock 없이 동작.
 *     - 만일 잡 스레드 생성 후 init 이 호출되는 비정상 패턴이면 fio_ime_engine_init()
 *       가 log_err 경고만 출력하고 진행한다(데이터 경쟁 가능성 인정). */
static bool fio_ime_is_initialized = false;

/* [한국어] IME 동기 I/O 요청 구조체 (ime_psync/ime_psyncv 엔진용)
 *
 * ime_psyncv가 iovec을 누적할 때, 이 하나의 요청 기술자가 "현재 모아놓은 배치가 어느
 * 파일/오프셋/방향에 대한 것인지"를 기억한다. 배치가 비어 있으면(queued==0) 다음
 * enqueue가 필드를 초기화한다. */
struct imesio_req {
	int 			fd;
	/* [한국어] 이 배치의 대상 파일 디스크립터(ime_native_open 반환값).
	 * 설정자: fio_ime_psyncv_enqueue()에서 큐가 비어 있을 때 io_u->file->fd로 채움.
	 * 읽는 자: fio_ime_psyncv_can_queue()가 "새 io_u가 같은 fd인지" 비교,
	 *          fio_ime_psyncv_commit()이 preadv/pwritev 첫 인자로 사용.
	 * 값 범위: 유효한 IME fd (>=0). -1이면 의도치 않은 상태.
	 * 동기화: 잡 스레드 단독 소유(싱글 스레드 컨텍스트). */
	/* [한국어 원주석] 파일 디스크립터 */

	enum fio_ddir	ddir;
	/* [한국어] 이 배치의 I/O 방향(DDIR_READ/DDIR_WRITE). IME psyncv는 readv/writev가
	 *          분리되어 있어 한 배치는 동일 방향이어야 한다.
	 * 설정자: fio_ime_psyncv_enqueue()가 큐가 빌 때 io_u->ddir로 초기화.
	 * 읽는 자: can_queue()가 같은 방향인지 확인, commit()이 preadv/pwritev 선택 분기에 사용.
	 * 값 범위: DDIR_READ(0) 또는 DDIR_WRITE(1). DDIR_SYNC는 이 구조체를 거치지 않고
	 *          queue()에서 즉시 ime_native_fsync로 처리. */
	/* [한국어 원주석] I/O 방향 (READ 또는 WRITE) */

	off_t			offset;
	/* [한국어] 배치의 시작 파일 오프셋. preadv/pwritev 호출 시 네 번째 인자로 전달.
	 * 설정자: enqueue()가 큐가 빌 때 io_u->offset으로 초기화.
	 * 읽는 자: commit()이 ime_native_preadv/pwritev 인자로 사용.
	 * 값 범위: 0 이상의 파일 오프셋(바이트).
	 * 주의: ime_d->last_offset으로 "연속성"을 검증한 경우에만 iovec을 추가하므로
	 *       모든 iovec은 offset부터 last_offset까지 끊김 없이 연결된다. */
	/* [한국어 원주석] 파일 오프셋 */
};
/* [한국어] IME 비동기 I/O 요청 구조체 (ime_aio 엔진용)
 *
 * ime_aio는 iodepth만큼의 요청을 링으로 운용하며, 각 요청은 인접한 iovec 여러 개를
 * 하나의 ime_native_aio_{read,write} 호출로 묶는다. 완료 콜백은 IME 라이브러리의
 * 내부 스레드에서 호출되므로 status는 mutex/cond 쌍으로 동기화된다. */
struct imeaio_req {
	struct ime_aiocb 	iocb;
	/* [한국어] IME가 요구하는 비동기 I/O 제어 블록(ime_native.h 정의).
	 * 필드: fd/file_offset/iov/iovcnt/flags/complete_cb/user_context.
	 * 설정자: fio_ime_aio_enqueue()가 배치 시작 시 전부 채움. append 경로에서는
	 *          iovcnt만 증가시켜 기존 iocb를 확장.
	 * 읽는 자: commit()이 ime_native_aio_read/write()에 포인터 전달.
	 *          완료 콜백에서 user_context(→ 자기 자신)를 통해 역참조.
	 * 동기화: 제출 후 완료 콜백까지 IME 라이브러리가 소유. fio는 status로만 확인. */
	/* [한국어 원주석] IME AIO 제어 블록 */

	ssize_t      		status;
	/* [한국어] 요청 상태 머신.
	 *   FIO_IME_IN_PROGRESS(-1): enqueue 직후, 완료 콜백 전.
	 *   FIO_IME_REQ_ERROR(-2):   라이브러리가 err!=0을 보고 또는 제출 실패.
	 *   >=0:                     콜백이 전달한 실제 전송 바이트 수.
	 * 설정자: enqueue(IN_PROGRESS), commit 실패(REQ_ERROR),
	 *          완료 콜백 fio_ime_aio_complete_cb()(bytes 또는 REQ_ERROR).
	 * 읽는 자: fio_ime_aio_getevents()가 완료 여부 판정 및 io_u->resid 계산에 사용.
	 * 동기화: status_mutex로 보호. 콜백 기록 → signal → getevents가 wait에서 깨어남. */
	/* [한국어 원주석] 요청 상태 (IN_PROGRESS/완료 바이트/에러) */

	enum fio_ddir		ddir;
	/* [한국어] 요청의 I/O 방향. commit()이 aio_read vs aio_write 선택 분기에 사용.
	 * 설정자: enqueue()가 새 요청을 만들 때 io_u->ddir로 초기화.
	 * 읽는 자: can_append()가 연속 io_u와 같은 방향인지 검사, commit()의 분기. */
	/* [한국어 원주석] I/O 방향 */

	pthread_cond_t		cond_endio;
	/* [한국어] 완료 콜백 → getevents 스레드 간 wake-up을 위한 POSIX 조건변수.
	 * 설정자: init()이 pthread_cond_init으로 생성, clean()이 destroy.
	 * 시그널: 완료 콜백이 status 갱신 후 pthread_cond_signal.
	 * 대기: getevents()가 status==IN_PROGRESS인 동안 pthread_cond_wait 루프.
	 * 동기화: 반드시 status_mutex와 함께 사용(전형적 mutex+cond 패턴). */
	/* [한국어 원주석] 완료 통지용 조건 변수 */

	pthread_mutex_t		status_mutex;
	/* [한국어] status 필드의 원자적 기록/판독을 보호하는 POSIX 뮤텍스.
	 * 설정자/해제자: init/clean에서 init/destroy.
	 * 경쟁: 완료 콜백(IME 내부 스레드) vs getevents(잡 스레드) 동시 접근.
	 * 보호 범위: status 읽기/쓰기 및 cond_wait/signal 코드 경로. */
	/* [한국어 원주석] 상태 보호용 뮤텍스 */
};

/* This structure will be used for 2 engines: ime_psyncv and ime_aio */
/* [한국어] struct ime_data — ime_psyncv 와 ime_aio 가 공유하는 엔진별 상태 컨테이너.
 *
 *   생명주기:
 *     - 생성: fio_ime_psyncv_init() 또는 fio_ime_aio_init() 에서 calloc(1, ...).
 *     - 소유: td->io_ops_data 에 저장되어 잡 스레드 단독 소유. 다른 잡과 공유되지 않음.
 *     - 파괴: fio_ime_psyncv_clean() 또는 fio_ime_aio_clean() 에서 free.
 *
 *   ime_psync 는 commit/getevents 단계가 없는 단순 동기 엔진이므로 이 구조체를 쓰지 않는다.
 *   (td->io_ops_data 가 NULL 인 채로 동작.)
 *
 *   링 큐 의미론:
 *     - aioreqs[depth] / iovecs[depth] / io_us[2*depth] 가 모두 같은 head/tail 인덱스를
 *       공유하는 "병행 링". head 는 enqueue 가 쓰는 다음 슬롯, tail 은 getevents 가
 *       소비할 다음 슬롯.
 *     - psyncv 는 사실상 링이 아니라 "한 배치 단일 윈도우"로 동작 — getevents 후
 *       fio_ime_queue_reset() 이 head/tail/cur_commit/queued/events 를 모두 0 으로 리셋.
 *     - aio 는 진짜 링. head 가 wrap-around 하면(=다시 0) can_append 가 false 를 돌려
 *       기존 iocb->iov 의 배열 연속성이 깨지지 않게 새 요청을 시작한다. */
struct ime_data {
	union {
		struct imeaio_req 	*aioreqs;
		/* [한국어] ime_aio 전용 — iodepth 크기의 요청 객체 배열.
		 * 설정자: fio_ime_aio_init() 이 malloc 한 뒤 각 슬롯의 cond/mutex 를 init.
		 * 읽는 자: enqueue (head), commit (cur_commit), getevents (tail) 인덱스로 접근.
		 * 해제자: fio_ime_aio_clean() 이 cond/mutex destroy 후 free.
		 * 인덱싱: 세 인덱스가 모두 [0..depth) 범위에서 순환.
		 * 동기화: 한 슬롯의 status 필드만 라이브러리 콜백 스레드와 공유(슬롯 내부의
		 *          status_mutex/cond_endio 로 보호). 배열 자체의 head/tail/cur_commit
		 *          은 잡 스레드 단독 갱신이라 락 불필요. */
		struct imesio_req	*sioreq;
		/* [한국어] ime_psyncv 전용 — 단일 배치 요청 기술자(요청 1개만 사용).
		 * psyncv 는 한 commit 당 단 1회의 ime_native_preadv/pwritev 호출에 모든 iovec 을
		 * 묶어 보내므로 "현재 배치가 어떤 fd/ddir/offset 에 대한 것인지"를 기억하는 객체도 1개면 충분.
		 * 설정자: fio_ime_psyncv_init() 이 malloc, enqueue()가 큐 비었을 때 fd/ddir/offset 채움.
		 * 읽는 자: can_queue() 가 새 io_u 와 fd/ddir 비교, commit() 이 preadv/pwritev 인자로 사용.
		 * 해제자: fio_ime_psyncv_clean() 이 free. */
		/* [한국어 원주석] array of aio requests / pointer to the only syncio request
		 * 두 엔진이 같은 메모리 슬롯을 공유하기 위해 union 사용 — 엔진 선택은
		 * 컴파일 시 결정되므로 같은 잡 안에서 두 멤버가 동시에 의미를 갖는 일은 없다. */
	};
	struct iovec 	*iovecs;
	/* [한국어] iodepth 크기의 scatter/gather 버퍼 배열.
	 * 각 iovec 의 (iov_base, iov_len) 슬롯에 enqueue 시점의 (io_u->xfer_buf, xfer_buflen) 을 기록.
	 * 설정자: init() 가 malloc, psyncv_enqueue()/aio_enqueue() 가 head 위치에 기록.
	 * 읽는 자:
	 *   - psyncv commit  : ime_native_preadv/pwritev 의 iovec 인자로 통째 전달.
	 *   - aio enqueue    : iocb->iov 가 iovecs[head] 를 가리키게 하여 요청별로 연속 슬롯
	 *                      구간을 묶음(iovcnt 만큼). 따라서 head 가 0 으로 wrap 되면
	 *                      iocb 가 가리키는 연속 구간이 깨지므로 can_append 가 false 반환.
	 * 인덱싱: head (enqueue) 위치에 추가. tail (getevents) 위치는 io_us 와 동기.
	 * 해제자: clean() 이 free.
	 * 동기화: 잡 스레드 전용. */
	/* [한국어 원주석] array of queued iovecs */

	struct io_u 	**io_us;
	/* [한국어] iovec 과 1:1 대응되는 io_u 포인터 배열.
	 * 할당 크기는 2 * iodepth — 앞쪽 iodepth 는 "queued" 슬롯, 뒤쪽 iodepth 는 event_io_us 슬롯.
	 *   (단일 malloc 으로 두 영역을 합쳐 free 도 한 번이면 끝나는 작은 최적화.)
	 * 설정자: enqueue() 가 io_us[head] = io_u.
	 * 읽는 자: getevents() 가 io_us[tail] 을 꺼내 event_io_us[events++] 로 복사.
	 *           psyncv_end()/aio_getevents() 가 io_u->error / io_u->resid 를 기록.
	 * 해제자: clean() 이 free. (event_io_us 는 같은 할당의 후반부이므로 별도 free 금지.) */
	/* [한국어 원주석] array of queued io_u pointers */

	struct io_u 	**event_io_us;
	/* [한국어] fio_ime_event(td, idx) 가 fio 코어에 반환할 완료 io_u 배열.
	 * 위치: io_us 의 뒤쪽 iodepth 구간(io_us + td->o.iodepth) 을 가리키는 별칭 포인터.
	 * 설정자: getevents() 루프가 idx 위치에 채움.
	 * 읽는 자: fio_ime_event(td, event_idx) → event_io_us[event_idx]. */
	/* [한국어 원주석] array of the events retrieved after get_events */

	unsigned int 	queued;
	/* [한국어] 현재 큐에 적재된(iovec 슬롯에 기록된) io_u 총 개수.
	 *           "아직 commit 안 된 것 + 이미 commit 됐지만 수확 안 된 것" 모두 포함.
	 * 설정자: queue_incr()=+1, queue_red()=-1, queue_reset()=0.
	 * 읽는 자:
	 *   - queue()   가 queued==depth 비교해 FIO_Q_BUSY 결정.
	 *   - commit()  이 (queued - events) 로 미커밋 요청 수 계산.
	 *   - psyncv commit 이 events = queued 로 일괄 승격.
	 * 값 범위: 0 .. depth. */
	/* [한국어 원주석] iovecs/io_us in the queue */

	unsigned int 	events;
	/* [한국어] "이미 IME 에 commit 했지만 아직 fio 가 getevents 로 가져가지 않은" io_u 수.
	 * 설정자:
	 *   - psyncv commit() : events = queued (배치를 통째로 발사).
	 *   - aio queue_commit() : iovcnt 만큼 누적 증가.
	 *   - getevents()/queue_red()/queue_reset() 이 감소.
	 * 의미:
	 *   - psyncv 는 events>0 이면 새 배치 시작 금지(can_queue 검사 — 한 번에 한 배치만).
	 *   - aio 는 events 와 (queued-events) 두 영역이 링에 공존(in-flight + 대기).
	 * 값 범위: 0 .. queued (≤ depth). */
	/* [한국어 원주석] number of committed iovecs/io_us */

	/* variables used to implement a "ring" queue */
	unsigned int depth;
	/* [한국어] 링 버퍼 용량 = td->o.iodepth.
	 * 설정자: init() 1회. 이후 불변(읽기 전용). */
	/* [한국어 원주석] max entries in the queue */

	unsigned int head;
	/* [한국어] 다음 enqueue 가 쓸 슬롯 인덱스(write pointer).
	 * 설정자: queue_incr() = (head+1) % depth, queue_reset() = 0.
	 * 읽는 자:
	 *   - psyncv/aio enqueue()  : iovecs[head] / io_us[head] 슬롯 위치 결정.
	 *   - aio can_append()      : head==0 이면 링이 막 wrap 되어 iov 연속성이 깨졌으므로
	 *                              새 요청을 강제(false 반환). */
	/* [한국어 원주석] index used to append */

	unsigned int tail;
	/* [한국어] 다음 pop 이 읽을 슬롯 인덱스(read pointer).
	 * 설정자: queue_red() = (tail+1) % depth, queue_reset() = 0.
	 * 읽는 자: aio_getevents() 가 aioreqs[tail] / io_us[tail] 위치에서 완료 수확. */
	/* [한국어 원주석] index used to pop */

	unsigned int cur_commit;
	/* [한국어] 다음 commit 이 제출할 첫 요청의 인덱스(aio 전용).
	 * 설정자: queue_commit() = (cur_commit + iovcnt) % depth, queue_reset() = 0.
	 * 읽는 자: aio_commit() 의 while 루프가 (queued - events) > 0 인 동안 cur_commit
	 *          위치에서 요청을 꺼내 ime_native_aio_read/write 호출. */
	/* [한국어 원주석] index of the first uncommitted req */

	/* offset used by the last iovec (used to check if the iovecs can be appended)*/
	unsigned long long	last_offset;
	/* [한국어] 가장 최근 enqueue 된 iovec 의 끝 오프셋 = (offset + xfer_buflen).
	 *          새 io_u 의 시작 오프셋이 이 값과 정확히 일치해야 배치에 append 가능.
	 *          (= 파일상 연속 I/O 조건. 비연속이면 새 요청을 시작해야 한다.)
	 * 설정자: psyncv_enqueue/aio_enqueue 가 매번 새 끝 오프셋으로 갱신.
	 * 읽는 자:
	 *   - psyncv can_queue()  : last_offset == io_u->offset 검사.
	 *   - aio can_append()    : last_offset == io_u->offset 검사.
	 * 값 범위: 0 .. 파일 크기 상한.
	 * 동기화: 잡 스레드 전용. */

	/* The variables below are used for aio only */
	struct imeaio_req	*last_req;
	/* [한국어] aio 전용 — 현재 "확장 가능한 마지막 요청" 포인터.
	 *          새 io_u 가 동일 fd/ddir/연속 오프셋이면 last_req->iocb.iovcnt 만 증가시켜
	 *          하나의 ime_native_aio_*() 호출로 묶는다(vector AIO 효과).
	 * 설정자: aio_enqueue() 가 새 요청을 만들 때 last_req = ioreq.
	 *          (append 경로에서는 갱신하지 않음 — 같은 요청 계속 확장.)
	 * 읽는 자: can_append() 가 last_req->ddir / iocb.fd 비교. */
	/* [한국어 원주석] last request awaiting committing */
};


/**************************************************************
 *         Private functions for queueing/unqueueing
 *
 **************************************************************/

/*
 * [한국어]
 * fio_ime_queue_incr - 링 큐에 하나를 추가했을 때 head/queued를 전진.
 *
 * @ime_d: 엔진별 상태(psyncv/aio 공용).
 *
 * enqueue 헬퍼. 호출 측은 "새 iovec/io_us[head]에 데이터를 이미 기록한" 상태.
 * 실행 컨텍스트: 잡 스레드(queue 콜백) 단독.
 * 호출 체인: fio_ime_psyncv_enqueue / fio_ime_aio_enqueue → [이 함수].
 */
static void fio_ime_queue_incr (struct ime_data *ime_d)
{
	ime_d->head = (ime_d->head + 1) % ime_d->depth;  /* [한국어] 링 버퍼 특성상 depth-1 다음은 0으로 순환. */
	ime_d->queued++;                                 /* [한국어] 큐 적재량 증가 - queue()의 depth 비교에 사용. */
}

/*
 * [한국어]
 * fio_ime_queue_red - 하나의 iovec이 완료되어 fio에 반환되었을 때 tail 전진 및 카운터 감소.
 *
 * @ime_d: 엔진별 상태.
 *
 * getevents의 완료 처리 내부에서 호출. queued와 events 양쪽을 동시에 감소시키는 이유는
 * "커밋된 요청"이 소비되면 큐에서도 빠져야 하기 때문.
 * 호출 체인: fio_ime_aio_getevents → [이 함수].
 */
static void fio_ime_queue_red (struct ime_data *ime_d)
{
	ime_d->tail = (ime_d->tail + 1) % ime_d->depth;  /* [한국어] 읽기 포인터 순환 전진. */
	ime_d->queued--;                                 /* [한국어] 큐에서 제거되었으므로 적재량 감소. */
	ime_d->events--;                                 /* [한국어] 커밋되어 반환된 항목이므로 완료 대기량도 감소. */
}

/*
 * [한국어]
 * fio_ime_queue_commit - aio의 한 요청이 IME에 제출되었을 때 cur_commit/events를 그 요청의
 *                        iovcnt만큼 전진.
 *
 * @iovcnt: 이 요청에 묶여 있던 iovec 개수(1 이상).
 *
 * commit 루프에서 매 요청 제출 후 호출.
 * 호출 체인: fio_ime_aio_commit → [이 함수].
 */
static void fio_ime_queue_commit (struct ime_data *ime_d, int iovcnt)
{
	ime_d->cur_commit = (ime_d->cur_commit + iovcnt) % ime_d->depth;  /* [한국어] 다음 미커밋 요청의 인덱스. */
	ime_d->events += iovcnt;                                          /* [한국어] 커밋되었지만 아직 수확되지 않은 항목 수 증가. */
}

/*
 * [한국어]
 * fio_ime_queue_reset - psyncv에서 한 배치가 완전히 소비된 뒤 모든 포인터를 0으로 리셋.
 *
 * psyncv는 매번 "append 시작 인덱스 = 0"을 가정하므로 getevents 종료 시점에 전체를 리셋.
 * aio와 달리 psyncv는 링 구조가 아닌 "한 배치 단일 윈도우"로 동작함에 유의.
 * 호출 체인: fio_ime_psyncv_getevents → [이 함수].
 */
static void fio_ime_queue_reset (struct ime_data *ime_d)
{
	ime_d->head = 0;         /* [한국어] 다음 enqueue는 배열 시작부터. */
	ime_d->tail = 0;         /* [한국어] pop 인덱스도 초기화. */
	ime_d->cur_commit = 0;   /* [한국어] 커밋 인덱스 초기화(psyncv는 사실상 사용 안 함). */
	ime_d->queued = 0;       /* [한국어] 큐에 남은 항목 없음. */
	ime_d->events = 0;       /* [한국어] 미수확 완료 없음. */
}

/**************************************************************
 *                   General IME functions
 *             (needed for both sync and async IOs)
 **************************************************************/

/*
 * [한국어]
 * fio_set_ime_filename - fio가 넘겨준 일반 파일 경로 앞에 IME 전용 prefix를 붙여
 *                         IME 네이티브 API가 인식할 수 있는 경로로 변환한다.
 *
 * @filename: fio가 보유한 원본 파일명(예: "/mnt/test/foo").
 * @return:   성공 시 thread-local 버퍼 포인터("ime://"+원본 등), 너무 길면 NULL.
 *
 * IME 경로 규약상 DEFAULT_IME_FILE_PREFIX(ime_native.h)가 앞에 있어야 한다. 반환 버퍼는
 * __thread 저장소에 있으므로 같은 스레드 내에서 다음 호출 전까지만 유효.
 * 실행 컨텍스트: 잡 스레드. 재진입 불가(같은 스레드 내 연속 호출은 이전 값 덮어씀).
 * 호출 체인: fio_ime_open_file / fio_ime_get_file_size / fio_ime_unlink_file → [이 함수].
 */
static char *fio_set_ime_filename(char* filename)
{
	static __thread char ime_filename[PATH_MAX];  /* [한국어] 스레드별 전용 버퍼 - 반환 포인터 유효 기간은 "다음 호출 전까지". */
	int ret;                                      /* [한국어] snprintf의 "필요 길이" 반환값을 받아 절단 여부 검사. */

	ret = snprintf(ime_filename, PATH_MAX, "%s%s", DEFAULT_IME_FILE_PREFIX, filename);  /* [한국어] prefix+원본 파일명 조합. snprintf는 버퍼 초과 시에도 "필요했을 길이"를 반환. */
	if (ret < PATH_MAX)                            /* [한국어] 버퍼 내에 완전히 들어간 경우에만 유효 - PATH_MAX 이상이면 절단된 것. */
		return ime_filename;                       /* [한국어] 성공: thread-local 포인터 반환. */

	return NULL;                                  /* [한국어] 경로가 너무 길어 생성 실패 - 호출자는 에러로 처리. */
}

/*
 * [한국어]
 * fio_ime_get_file_size - ioengine_ops.get_file_size 콜백. IME 상의 파일 크기를 질의하여
 *                          fio_file->real_file_size를 채운다.
 *
 * @td: 잡 스레드 컨텍스트(에러 보고용).
 * @f:  대상 파일 디스크립터 래퍼.
 * @return: 성공 0, 실패 1.
 *
 * fio 코어는 파일 레이아웃/verify 등에 실제 크기를 요구한다. POSIX stat이 아닌
 * ime_native_stat을 호출해야 IME 상의 메타데이터를 얻을 수 있다.
 * 호출 체인: fio backend → [이 함수] → fio_set_ime_filename → ime_native_stat.
 * 에러 경로: prefix 생성 실패 또는 ime_native_stat 실패 시 td_verror로 errno 전파.
 */
static int fio_ime_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct stat buf;   /* [한국어] ime_native_stat의 출력 버퍼(POSIX stat 레이아웃과 호환). */
	int ret;           /* [한국어] ime_native_stat 반환값(-1: 에러). */
	char *ime_filename;/* [한국어] prefix가 붙은 IME 경로(thread-local 버퍼 포인터). */

	dprint(FD_FILE, "get file size %s\n", f->file_name);  /* [한국어] FD_FILE 디버그 채널에 추적 로그. */

	ime_filename = fio_set_ime_filename(f->file_name);  /* [한국어] "ime://" prefix 부여하여 IME API용 경로 생성. */
	if (ime_filename == NULL)                            /* [한국어] 경로가 PATH_MAX 초과로 생성 불가. */
		return 1;                                         /* [한국어] fio 측에 실패 반환 - 잡이 에러로 종료. */
	ret = ime_native_stat(ime_filename, &buf);          /* [한국어] IME 네이티브 stat - 메타데이터 조회(네트워크 RPC 가능성). */
	if (ret == -1) {                                     /* [한국어] 파일 없음/권한 없음 등. errno가 설정됨. */
		td_verror(td, errno, "fstat");                  /* [한국어] fio 에러 기록 - 파일명 컨텍스트 "fstat"로 저장. */
		return 1;                                        /* [한국어] 실패 반환. */
	}

	f->real_file_size = buf.st_size;                    /* [한국어] fio가 사용할 실제 파일 크기 기록 - 이후 layout/verify가 참조. */
	return 0;                                           /* [한국어] 성공. */
}

/* This functions mimics the generic_file_open function, but issues
   IME native calls instead of POSIX calls. */
/*
 * [한국어]
 * fio_ime_open_file - ioengine_ops.open_file 콜백. fio의 generic_file_open을 IME API 버전으로 재구현.
 *
 * @td: 잡 스레드(옵션/에러 보고).
 * @f:  열어야 할 파일 기술자(fd는 여기서 채움).
 * @return: 성공 0, 실패 1.
 *
 * POSIX open() 대신 ime_native_open()을 쓰도록 플래그 조합을 수행하고, TRIM은 IME가
 * 지원하지 않으므로 거부한다. 또한 fio 코어가 POSIX stat으로 크기를 얻지 않도록
 * 여기서 직접 get_file_size와 ftruncate를 호출해 파일 크기를 맞춘다.
 * 실행 컨텍스트: 잡 스레드(각 파일당 1회). 호출 체인: backend → td_io_open_file → [이 함수].
 * 에러 경로: TRIM 요청/지원 불가 조합/open 실패/stat 실패/ftruncate 실패 모두 td_verror 후 1 반환.
 */
static int fio_ime_open_file(struct thread_data *td, struct fio_file *f)
{
	int flags = 0;         /* [한국어] ime_native_open 두 번째 인자로 전달할 열기 플래그 집합. 0부터 OR로 조립. */
	int ret;               /* [한국어] 내부 호출 반환값 저장. */
	uint64_t desired_fs;   /* [한국어] 이 잡이 요구하는 최소 파일 크기 = io_size + file_offset. */
	char *ime_filename;    /* [한국어] IME prefix가 붙은 경로 문자열. */

	dprint(FD_FILE, "fd open %s\n", f->file_name);  /* [한국어] FD_FILE 채널에 열기 시도 로그. */

	if (td_trim(td)) {                                  /* [한국어] 이 잡이 TRIM(discard)을 포함하는지 검사 - IME는 미지원. */
		td_verror(td, EINVAL, "IME does not support TRIM operation");  /* [한국어] 사용자에게 명확한 오류 메시지로 보고. */
		return 1;                                        /* [한국어] 즉시 실패 반환 - 파일 오픈 포기. */
	}

	if (td->o.odirect)                                  /* [한국어] --direct=1 요청 시 O_DIRECT 부여 - IME는 페이지 캐시 우회 경로. */
		flags |= O_DIRECT;
	flags |= td->o.sync_io;                             /* [한국어] O_SYNC 등 sync_io 옵션 비트를 그대로 OR - 동기 쓰기 보장. */
	if (td->o.create_on_open && td->o.allow_create)     /* [한국어] "오픈 시 생성" + "생성 허용" 동시에 true면 O_CREAT. */
		flags |= O_CREAT;

	if (td_write(td)) {                                 /* [한국어] 잡이 쓰기(verify 포함) 방향인 경우. */
		if (!read_only)                                  /* [한국어] 전역 read_only 플래그가 꺼져 있어야 실제 쓰기 가능. */
			flags |= O_RDWR;                              /* [한국어] 읽기/쓰기 모두 - verify read-back을 위해 RW 필요. */

		if (td->o.allow_create)                          /* [한국어] 부재 파일을 만들 수 있는 옵션이면 O_CREAT 보강. */
			flags |= O_CREAT;
	} else if (td_read(td)) {                           /* [한국어] 순수 읽기 잡. */
		flags |= O_RDONLY;                               /* [한국어] 읽기 전용 열기 - 데이터 변경 방지. */
	} else {
		/* We should never go here. */
		/* [한국어] read/write/trim 어디에도 속하지 않는 비정상 조합. 보통 옵션 파서가 막지만 방어적 처리. */
		td_verror(td, EINVAL, "Unsopported open mode");  /* [한국어] 잡 종료시킬 수준의 치명적 오류로 기록. */
		return 1;
	}

	ime_filename = fio_set_ime_filename(f->file_name);  /* [한국어] IME용 경로로 변환. */
	if (ime_filename == NULL)                            /* [한국어] PATH_MAX 초과 등으로 경로 생성 불가. */
		return 1;
	f->fd = ime_native_open(ime_filename, flags, 0600); /* [한국어] IME 네이티브 open - mode 0600: rw- 소유자만. RPC 가능성 있음. */
	if (f->fd == -1) {                                   /* [한국어] open 실패(접근/부재/생성 불가 등). */
		char buf[FIO_VERROR_SIZE];                        /* [한국어] fio 오류 메시지 포맷용 지역 버퍼. */
		int __e = errno;                                  /* [한국어] 후속 호출이 errno를 덮기 전에 스냅샷. */

		snprintf(buf, sizeof(buf), "open(%s)", f->file_name);  /* [한국어] "open(<파일명>)" 형태로 컨텍스트 기록. */
		td_verror(td, __e, buf);                          /* [한국어] fio에 에러 등록 - 잡 상태 전이. */
		return 1;
	}

	/* Now we need to make sure the real file size is sufficient for FIO
	   to do its things. This is normally done before the file open function
	   is called, but because FIO would use POSIX calls, we need to do it
	   ourselves */
	/* [한국어] fio 코어가 POSIX stat을 쓰지 못하므로 여기서 직접 IME stat으로 크기 획득. */
	ret = fio_ime_get_file_size(td, f);
	if (ret < 0) {                                       /* [한국어] 음수 반환은 내부적으로 에러 시그널(본 함수는 1을 돌려줌에 주의). */
		ime_native_close(f->fd);                          /* [한국어] 리소스 누수 방지 - 방금 연 fd 닫기. */
		td_verror(td, errno, "ime_get_file_size");       /* [한국어] errno 전파. */
		return 1;
	}

	desired_fs = f->io_size + f->file_offset;           /* [한국어] 잡 설정이 요구하는 최소 파일 크기 계산. */
	if (td_write(td)) {                                 /* [한국어] 쓰기 잡: 필요 시 파일을 확장(layout). */
		dprint(FD_FILE, "Laying out file %s%s\n",
			DEFAULT_IME_FILE_PREFIX, f->file_name);      /* [한국어] 레이아웃 단계 추적 로그. */
		if (!td->o.create_on_open &&                     /* [한국어] "오픈 시 생성" 옵션이 없을 때만 사전 확장 필요. */
				f->real_file_size < desired_fs &&         /* [한국어] 현재 크기가 요구 크기보다 작을 때만. */
				ime_native_ftruncate(f->fd, desired_fs) < 0) {  /* [한국어] IME 파일 크기를 확장 - 실패 시 음수. */
			ime_native_close(f->fd);                      /* [한국어] 확장 실패 시 fd 정리. */
			td_verror(td, errno, "ime_native_ftruncate");
			return 1;
		}
		if (f->real_file_size < desired_fs)              /* [한국어] truncate가 성공했다면 캐시된 크기를 갱신. */
			f->real_file_size = desired_fs;
	} else if (td_read(td) && f->real_file_size < desired_fs) {  /* [한국어] 읽기 잡인데 파일이 너무 작음 - 확장 불가능한 상황. */
		ime_native_close(f->fd);                          /* [한국어] 리소스 정리. */
		log_err("error: can't read %lu bytes from file with "
						"%lu bytes\n", desired_fs, f->real_file_size);  /* [한국어] 사용자에게 친절한 메시지 출력. */
		return 1;
	}

	return 0;                                           /* [한국어] 모든 준비 완료 - fd는 f->fd에 저장됨. */
}

/*
 * [한국어]
 * fio_ime_close_file - ioengine_ops.close_file 콜백. open_file에서 얻은 fd를 IME에 반환.
 *
 * @td: unused(fio_unused 매크로로 표식).
 * @f:  닫을 파일.
 * @return: 성공 0, 실패 errno.
 *
 * 호출 체인: backend → td_io_close_file → [이 함수] → ime_native_close.
 */
static int fio_ime_close_file(struct thread_data fio_unused *td, struct fio_file *f)
{
	int ret = 0;  /* [한국어] 기본 성공. 닫기 실패 시에만 errno로 덮어씀. */

	dprint(FD_FILE, "fd close %s\n", f->file_name);  /* [한국어] 디버그 추적 로그. */

	if (ime_native_close(f->fd) < 0)                  /* [한국어] IME 네이티브 close - 리소스/연결 해제(RPC 가능성). */
		ret = errno;                                   /* [한국어] 실패 시 errno 스냅샷하여 반환. */

	f->fd = -1;                                       /* [한국어] fd 무효화 - 재사용 시 잘못된 핸들 사용 방지. */
	return ret;
}

/*
 * [한국어]
 * fio_ime_unlink_file - ioengine_ops.unlink_file 콜백. IME prefix 경로를 POSIX unlink로 제거.
 *
 * 이 함수는 IME 네이티브 unlink가 아닌 POSIX unlink를 호출한다 - IME가 POSIX 마운트 지점에
 * 경로를 노출하므로 파일 제거는 OS 레이어에서 처리 가능함을 전제로 한다.
 * 호출 체인: fio cleanup → td_io_unlink_file → [이 함수].
 */
static int fio_ime_unlink_file(struct thread_data *td, struct fio_file *f)
{
	char *ime_filename = fio_set_ime_filename(f->file_name);  /* [한국어] IME prefix 경로 확보. */
	int ret;

	if (ime_filename == NULL)                                  /* [한국어] 경로 생성 실패. */
		return 1;

	ret = unlink(ime_filename);                                /* [한국어] POSIX unlink - 디렉토리 엔트리 제거. */
	return ret < 0 ? errno : 0;                                /* [한국어] 실패 시 errno, 성공 시 0. */
}

/*
 * [한국어]
 * fio_ime_event - ioengine_ops.event 콜백. getevents 이후 fio가 "n번째 완료 io_u"를 질의할 때 반환.
 *
 * @event: 0..(getevents 반환값-1) 범위의 인덱스.
 * @return: event_io_us[event]로 저장해둔 io_u 포인터.
 *
 * 두 비동기 엔진(psyncv/aio)이 모두 같은 구현을 쓰므로 공용화.
 */
static struct io_u *fio_ime_event(struct thread_data *td, int event)
{
	struct ime_data *ime_d = td->io_ops_data;  /* [한국어] 잡별 엔진 상태를 꺼냄. */

	return ime_d->event_io_us[event];          /* [한국어] getevents 루프가 채워둔 완료 io_u 배열에서 인덱스 참조. */
}

/* Setup file used to replace get_file_sizes when settin up the file.
   Instead we will set real_file_sie to 0 for each file. This way we
   can avoid calling ime_native_init before the forks are created. */
/*
 * [한국어]
 * fio_ime_setup - ioengine_ops.setup 콜백. ime_native_init()을 fork 이후로 늦추기 위한 트릭.
 *
 * fio 코어는 setup에서 get_file_size를 호출해 파일 크기를 얻으려 하지만, IME 라이브러리는
 * fork 전에 초기화되면 자식에서 문제를 일으킨다. 따라서 real_file_size를 0으로 세팅하여
 * "크기 미지정 상태"로 남기고, 실제 크기 획득은 open_file 시점으로 지연한다.
 * 호출 체인: fio_backend → td_io_setup → [이 함수].
 */
static int fio_ime_setup(struct thread_data *td)
{
	struct fio_file *f;   /* [한국어] 반복 포인터. */
	unsigned int i;       /* [한국어] for_each_file 매크로용 인덱스. */

	for_each_file(td, f, i) {  /* [한국어] td에 등록된 모든 파일을 순회. */
		dprint(FD_FILE, "setup: set file size to 0 for %p/%d/%s\n",
			f, i, f->file_name);       /* [한국어] 디버그 추적 - 지연 초기화 의도를 로그로 남김. */
		f->real_file_size = 0;          /* [한국어] 크기 0 = "아직 모름" - fio가 이후 단계에서 다시 질의하도록 유도. */
	}

	return 0;                          /* [한국어] setup 단계는 항상 성공. */
}

/*
 * [한국어]
 * fio_ime_engine_init - ioengine_ops.init 콜백. IME 라이브러리 초기화 및 파일 크기 임시 세팅.
 *
 * @td: 잡 스레드.
 * @return: 항상 0 (IME init은 본 함수에서 실패 반환을 하지 않음 - 실패 시 라이브러리가 abort).
 *
 * use_thread=0 모드(즉 fork 모드)에서 잡 스레드 생성 전에 init이 이미 true이면 순서 오류
 * 경고만 출력한다. use_thread=1 모드에서는 여러 잡이 하나의 프로세스를 공유하므로
 * IME 초기화를 한 번만 수행해도 OK.
 * 호출 체인: backend → td_io_init → [이 함수].
 */
static int fio_ime_engine_init(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	dprint(FD_IO, "ime engine init\n");                  /* [한국어] FD_IO 채널 추적. */
	if (fio_ime_is_initialized && !td->o.use_thread) {   /* [한국어] fork 모드인데 이미 초기화되었다면 순서 이상 - 경고. */
		log_err("Warning: something might go wrong. Not all threads/forks were"
				" created before the FIO jobs were initialized.\n");
	}

	ime_native_init();                                    /* [한국어] IME 클라이언트 초기화 - 내부 스레드/연결 준비. */
	fio_ime_is_initialized = true;                        /* [한국어] 전역 플래그 세팅 - unregister에서 finalize 판단에 사용. */

	/* We have to temporarily set real_file_size so that
	   FIO can initialize properly. It will be corrected
	   on file open. */
	/* [한국어] init 단계에서 fio가 real_file_size를 참조할 수 있도록 임시 값 채움 - open에서 정확한 값으로 덮어씀. */
	for_each_file(td, f, i)
		f->real_file_size = f->io_size + f->file_offset;  /* [한국어] 최소 필요 크기로 세팅 - fio의 크기 관련 검증 통과용. */

	return 0;
}

/*
 * [한국어]
 * fio_ime_engine_finalize - ioengine_ops.cleanup 콜백(또는 공통 finalize 헬퍼).
 *
 * fork 모드(각 잡이 독립 프로세스)에서는 잡 종료 시 ime_native_finalize가 안전하다.
 * thread 모드에서는 다른 잡이 아직 IME를 사용 중일 수 있으므로 여기서 finalize하지 않고
 * fio_ime_unregister(프로세스 종료 시)에서 처리.
 */
static void fio_ime_engine_finalize(struct thread_data *td)
{
	/* Only finalize IME when using forks */
	if (!td->o.use_thread) {                              /* [한국어] fork 모드 판단 - 각 프로세스가 자기 IME를 가짐. */
		if (ime_native_finalize() < 0)                     /* [한국어] IME 종료 - 연결/스레드 해제. */
			log_err("error in ime_native_finalize\n");     /* [한국어] 실패해도 프로세스는 계속 종료 진행. */
		fio_ime_is_initialized = false;                    /* [한국어] 전역 플래그 리셋. */
	}
}


/**************************************************************
 *             Private functions for blocking IOs
 *                     (without iovecs)
 **************************************************************/

/* Notice: this function comes from the sync engine */
/* It is used by the commit function to return a proper code and fill
   some attributes in the io_u used for the IO. */
/*
 * [한국어]
 * fio_ime_psync_end - ime_native_p{read,write}/fsync 결과를 io_u에 반영하고 완료 상태 반환.
 *
 * @td:     잡 스레드 컨텍스트(td_verror/io_u_log_error 대상).
 * @io_u:   방금 처리된 io_u — 성공/실패 상태를 기록할 대상.
 * @ret:    시스템 호출 반환값(성공 시 바이트 수, 부분 전송 시 작은 수, 실패 시 -1).
 * @return: 항상 FIO_Q_COMPLETED (psync는 동기 엔진).
 *
 * 부분 전송(short I/O)은 io_u->resid에 미전송 바이트를 기록하고 error=0으로 둔다.
 * 완전 실패는 io_u->error에 errno를 기록하고 td_verror로 fio에 보고.
 *
 * 원본 출처: engines/sync.c 의 fio_io_end() 와 동일 로직. IME 가 POSIX read/write 와
 * 유사한 "반환값 = 전송 바이트 또는 -1" 규약을 따르므로 그대로 재사용. 향후 sync.c 의
 * fio_io_end 변경 시 여기도 맞춰야 한다.
 *
 * 실행 컨텍스트: 잡 스레드 동기. 호출 체인:
 *   backend → td_io_queue → fio_ime_psync_queue → ime_native_p{read,write,fsync} →
 *     [이 함수] → FIO_Q_COMPLETED.
 *
 * 반환 규약 재확인:
 *   - ret == xfer_buflen : 완전 성공. resid/error 손대지 않음.
 *   - 0 <= ret < xfer_buflen : 부분 전송. resid=xfer_buflen-ret, error=0.
 *   - ret == -1 : 실패. io_u->error = errno, td_verror("xfer").
 */
static int fio_ime_psync_end(struct thread_data *td, struct io_u *io_u, ssize_t ret)
{
	if (ret != (ssize_t) io_u->xfer_buflen) {           /* [한국어] 요청 바이트 전부가 전송되지 않았음. */
		if (ret >= 0) {                                   /* [한국어] 0 이상: 부분 전송(short I/O) - fio는 이를 에러가 아닌 resid로 처리. */
			io_u->resid = io_u->xfer_buflen - ret;         /* [한국어] 남은 바이트 = 요청 - 실제. */
			io_u->error = 0;                               /* [한국어] 부분 성공이므로 에러 없음. */
			return FIO_Q_COMPLETED;                        /* [한국어] 완료로 반환 - fio가 남은 부분 재시도할 수 있음. */
		} else
			io_u->error = errno;                           /* [한국어] -1: 시스템 호출 실패 - errno 기록. */
	}

	if (io_u->error) {                                   /* [한국어] 에러가 있으면 로그에 남기고 td 상태에도 반영. */
		io_u_log_error(td, io_u);                         /* [한국어] io_u 컨텍스트(오프셋/버퍼/방향)를 로그에 출력. */
		td_verror(td, io_u->error, "xfer");               /* [한국어] 잡에 치명 에러로 보고 - "xfer" 태그. */
	}

	return FIO_Q_COMPLETED;                              /* [한국어] psync는 반환 시 항상 완료 - fio는 별도 getevents 불필요. */
}

/*
 * [한국어]
 * fio_ime_psync_queue - ime_psync 엔진의 queue 콜백. 동기적으로 I/O를 수행하고 즉시 완료 반환.
 *
 * @io_u: 제출할 I/O 유닛(버퍼/오프셋/방향 포함).
 * @return: FIO_Q_COMPLETED - 이 엔진은 비동기 큐잉 없음.
 *
 * ddir에 따라 ime_native_pread/pwrite/fsync를 직접 호출. 그 외 방향(TRIM 등)은 EINVAL.
 * 실행 컨텍스트: 잡 스레드 동기. 호출 체인: backend → td_io_queue → [이 함수].
 */
static enum fio_q_status fio_ime_psync_queue(struct thread_data *td,
					   struct io_u *io_u)
{
	struct fio_file *f = io_u->file;  /* [한국어] 대상 파일(오픈된 IME fd 포함). */
	ssize_t ret;                       /* [한국어] 전송 바이트 또는 -1. */

	fio_ro_check(td, io_u);            /* [한국어] read-only 잡이면 쓰기 시도를 assert로 잡음 - 방어적 점검. */

	if (io_u->ddir == DDIR_READ)
		ret = ime_native_pread(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);    /* [한국어] IME pread - 주어진 오프셋에서 xfer_buflen 바이트 읽기. VERIFY 도 이 경로 사용. */
	else if (io_u->ddir == DDIR_WRITE)
		ret = ime_native_pwrite(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);   /* [한국어] IME pwrite - 주어진 오프셋에 쓰기. */
	else if (io_u->ddir == DDIR_SYNC)
		ret = ime_native_fsync(f->fd);                                                      /* [한국어] IME fsync - 캐시 플러시. 실패 시 -1 반환 → end() 에서 errno 로 매핑. */
	else {
		/* [한국어] DDIR_TRIM/DDIR_DATASYNC 등 미지원 방향.
		 *   ret 을 xfer_buflen 으로 위장해서 "resid 계산 = 0" 으로 만들고, io_u->error
		 *   를 EINVAL 로 기록 — 이후 end() 가 if (io_u->error) 분기를 타서 td_verror 보고. */
		ret = io_u->xfer_buflen;                                                            /* [한국어] 알 수 없는 방향 - ret을 "전체 전송"으로 세팅해서 resid 0이 되도록. */
		io_u->error = EINVAL;                                                               /* [한국어] 그러나 에러 플래그는 명시. */
	}

	return fio_ime_psync_end(td, io_u, ret);                                              /* [한국어] 결과를 io_u에 반영하고 상태 반환. */
}


/**************************************************************
 *             Private functions for blocking IOs
 *                       (with iovecs)
 **************************************************************/

/*
 * [한국어]
 * fio_ime_psyncv_can_queue - psyncv 엔진에서 새 io_u를 현재 iovec 배치에 이어붙일 수 있는지 판정.
 *
 * psyncv는 하나의 preadv/pwritev 호출로 여러 iovec을 동시에 처리하므로, 배치는 동일 fd,
 * 동일 방향, 파일상 연속 오프셋을 요구한다. 또한 이미 커밋되어 getevents 대기 중인
 * 이벤트가 남아 있으면 새 배치를 시작할 수 없다.
 *
 * @return: true면 enqueue 허용, false면 FIO_Q_BUSY로 되돌려 잡이 commit/getevents 후 재시도.
 */
static bool fio_ime_psyncv_can_queue(struct ime_data *ime_d, struct io_u *io_u)
{
	/* We can only queue if:
	  - There are no queued iovecs
	  - Or if there is at least one:
		 - There must be no event waiting for retrieval
		 - The offsets must be contiguous
		 - The ddir and fd must be the same */
	return (ime_d->queued == 0 || (                     /* [한국어] 빈 큐면 언제나 OK. */
			ime_d->events == 0 &&                        /* [한국어] 커밋된 미수확 이벤트가 없어야 함. */
			ime_d->last_offset == io_u->offset &&        /* [한국어] 이전 iovec의 끝과 새 io_u 시작이 정확히 맞닿아야 함. */
			ime_d->sioreq->ddir == io_u->ddir &&         /* [한국어] 동일 방향 - preadv/pwritev는 한 방향만 처리. */
			ime_d->sioreq->fd == io_u->file->fd));       /* [한국어] 동일 fd에 대한 배치여야 함. */
}

/* Before using this function, we should have already
   ensured that the queue is not full */
/*
 * [한국어]
 * fio_ime_psyncv_enqueue - io_u를 psyncv 배치의 현재 head 위치에 iovec으로 기록.
 *
 * 사전조건: queued < depth 이고 can_queue()가 true. 큐가 비어 있으면 요청 메타(ioreq)를
 * 새로 초기화하고, 아니면 기존 메타에 이어붙인다.
 */
static void fio_ime_psyncv_enqueue(struct ime_data *ime_d, struct io_u *io_u)
{
	struct imesio_req *ioreq = ime_d->sioreq;        /* [한국어] psyncv의 유일한 요청 기술자. */
	struct iovec *iov = &ime_d->iovecs[ime_d->head]; /* [한국어] 새 iovec이 들어갈 슬롯. */

	iov->iov_base = io_u->xfer_buf;                  /* [한국어] 사용자 버퍼 주소. preadv/pwritev가 DMA 대상으로 사용. */
	iov->iov_len = io_u->xfer_buflen;                /* [한국어] 전송할 바이트 수. */

	if (ime_d->queued == 0) {                        /* [한국어] 배치의 첫 번째 iovec - 메타데이터 초기화. */
		ioreq->offset = io_u->offset;                 /* [한국어] 배치 시작 오프셋 기록. */
		ioreq->ddir = io_u->ddir;                     /* [한국어] 방향 확정. */
		ioreq->fd = io_u->file->fd;                   /* [한국어] 대상 fd. */
	}

	ime_d->io_us[ime_d->head] = io_u;                /* [한국어] iovec과 1:1로 대응되는 io_u 저장 - getevents에서 재활용. */
	ime_d->last_offset = io_u->offset + io_u->xfer_buflen;  /* [한국어] 다음 enqueue가 연속성 검사할 끝 오프셋 갱신. */
	fio_ime_queue_incr(ime_d);                       /* [한국어] head/queued 전진. */
}

/* Tries to queue an IO. It will fail if the IO can't be appended to the
   current request or if the current request has been committed but not
   yet retrieved by get_events. */
/*
 * [한국어]
 * fio_ime_psyncv_queue - ime_psyncv queue 콜백. iovec 누적 후 commit에서 단일 preadv/pwritev로 플러시.
 *
 * @return: FIO_Q_QUEUED(배치에 누적됨), FIO_Q_BUSY(큐가 꽉 찼거나 연속성 위반),
 *           FIO_Q_COMPLETED(fsync나 에러로 즉시 완료).
 *
 * 호출 체인: backend → td_io_queue → [이 함수]. fsync는 psyncv 배치를 거치지 않고 즉시 처리.
 */
static enum fio_q_status fio_ime_psyncv_queue(struct thread_data *td,
	struct io_u *io_u)
{
	struct ime_data *ime_d = td->io_ops_data;  /* [한국어] 엔진 상태 꺼냄. */

	fio_ro_check(td, io_u);                    /* [한국어] read-only 잡 보호. */

	if (ime_d->queued == ime_d->depth)         /* [한국어] 큐가 가득 - fio에 커밋/수확을 먼저 하라고 요청. */
		return FIO_Q_BUSY;

	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {  /* [한국어] 일반 read/write 경로 — VERIFY 도 코어가 DDIR_READ 로 디스패치하므로 이 분기 사용. */
		if (!fio_ime_psyncv_can_queue(ime_d, io_u))              /* [한국어] 연속성/동일 fd/동일 방향/미수확 이벤트 없음 확인. */
			return FIO_Q_BUSY;                                     /* [한국어] 배치 불가 - fio가 먼저 commit. */

		dprint(FD_IO, "queue: ddir=%d at %u commit=%u queued=%u events=%u\n",
			io_u->ddir, ime_d->head, ime_d->cur_commit,
			ime_d->queued, ime_d->events);                        /* [한국어] 큐 상태 디버그 - FD_IO 채널로 현재 head/cur_commit/queued/events 찍어 배치 진행 추적. */
		fio_ime_psyncv_enqueue(ime_d, io_u);                     /* [한국어] iovec 슬롯에 기록 + head 전진. */
		return FIO_Q_QUEUED;                                      /* [한국어] 비동기 성공 - 완료는 getevents에서. */
	} else if (io_u->ddir == DDIR_SYNC) {                       /* [한국어] fsync는 배치 대상이 아니므로 즉시 실행. */
		if (ime_native_fsync(io_u->file->fd) < 0) {              /* [한국어] IME fsync - 버스트 버퍼 캐시 플러시. */
			io_u->error = errno;                                   /* [한국어] 실패 시 errno 기록. */
			td_verror(td, io_u->error, "fsync");                  /* [한국어] "fsync" 태그로 잡에 치명 에러 등록 - 이후 td 상태가 오류로 전이. */
		}
		return FIO_Q_COMPLETED;                                   /* [한국어] 동기 완료. */
	} else {
		/* [한국어] DDIR_TRIM/DDIR_DATASYNC/DDIR_SYNC_FILE_RANGE 등 IME 미지원 방향.
		 *   DDIR_TRIM 은 fio_ime_open_file 에서 잡 시작 시 이미 거부되지만, 방어적으로
		 *   여기서도 EINVAL 보고한다. (버그나 옵션 파서 변경으로 도달 가능.) */
		io_u->error = EINVAL;                                     /* [한국어] TRIM 등 미지원 방향. */
		td_verror(td, io_u->error, "wrong ddir");                /* [한국어] "wrong ddir" 태그로 잡에 에러 기록. */
		return FIO_Q_COMPLETED;                                   /* [한국어] COMPLETED 로 돌려야 fio 코어가 io_u 를 다시 큐에 넣지 않고 에러 처리한다. */
	}
}

/* Notice: this function comes from the sync engine */
/* It is used by the commit function to return a proper code and fill
   some attributes in the io_us appended to the current request. */
/*
 * [한국어]
 * fio_ime_psyncv_end - preadv/pwritev 반환값을 배치에 속한 각 io_u에 분배.
 *
 * @bytes: preadv/pwritev 반환값(총 전송 바이트, 실패 시 -1).
 *
 * preadv/pwritev는 iovec 순서대로 채우므로 bytes를 iovec 크기 순으로 소진시킨다.
 * 실패 시 모든 io_u에 errno 기록.
 */
static int fio_ime_psyncv_end(struct thread_data *td, ssize_t bytes)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct io_u *io_u;
	unsigned int i;
	int err = errno;   /* [한국어] 후속 로깅이 errno를 덮기 전에 보존. */

	for (i = 0; i < ime_d->queued; i++) {  /* [한국어] 배치에 포함된 모든 io_u 순회. iovec 순서 = io_us 순서이므로 순차 소진 가능. */
		io_u = ime_d->io_us[i];            /* [한국어] i 번째 슬롯의 io_u 포인터 꺼냄. */

		if (bytes == -1)                     /* [한국어] 전체 실패 - 모든 io_u에 같은 errno 기록. */
			io_u->error = err;                /* [한국어] 루프 시작 전 스냅샷한 errno 를 모든 io_u 에 전파. */
		else {
			/* [한국어] 부분/전체 성공 경로.
			 *   preadv/pwritev 는 iovec 배열을 "왼쪽부터 순차로" 채우므로,
			 *   bytes 를 iovec 사이즈 순서대로 소진하면서 각 io_u 에 개별 resid 계산. */
			unsigned int this_io;              /* [한국어] 이 io_u에 할당된 전송량. */

			this_io = bytes;                   /* [한국어] 남은 bytes를 가용 상한으로. */
			if (this_io > io_u->xfer_buflen)  /* [한국어] io_u 한도를 넘지 않도록 clamp. */
				this_io = io_u->xfer_buflen;    /* [한국어] 이 io_u 가 요청한 만큼만 가져가고 나머지는 다음 io_u 로 이월. */

			io_u->resid = io_u->xfer_buflen - this_io;  /* [한국어] 미전송 바이트. */
			io_u->error = 0;                             /* [한국어] 부분 성공 - 에러 아님. */
			bytes -= this_io;                            /* [한국어] 남은 전체 바이트에서 소진. */
		}
	}

	if (bytes == -1) {                   /* [한국어] 전체 실패 경로 - fio에 보고. */
		td_verror(td, err, "xfer psyncv");
		return -err;                      /* [한국어] 음수 errno를 반환(commit 호출자가 에러로 판정). */
	}

	return 0;                            /* [한국어] 성공 또는 부분 성공 모두 0. */
}

/* Commits the current request by calling ime_native (with one or several
   iovecs). After this commit, the corresponding events (one per iovec)
   can be retrieved by get_events. */
/*
 * [한국어]
 * fio_ime_psyncv_commit - 누적된 iovec 배치를 단일 preadv/pwritev로 IME에 제출.
 *
 * 이 엔진은 동기 엔진이므로 commit 반환 시점에 모든 전송이 끝나 있다. getevents는
 * 단지 io_us → event_io_us로 복사만 한다.
 * 호출 체인: backend → td_io_commit → [이 함수].
 */
static int fio_ime_psyncv_commit(struct thread_data *td)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct imesio_req *ioreq;
	int ret = 0;

	/* Exit if there are no (new) events to commit
	   or if the previous committed event haven't been retrieved */
	if (!ime_d->queued || ime_d->events)  /* [한국어] 누적된 iovec이 없거나, 직전 배치가 아직 수확되지 않았으면 아무것도 안 함. */
		return 0;

	ioreq = ime_d->sioreq;                 /* [한국어] 유일 요청 기술자(fd/ddir/offset 포함). */
	ime_d->events = ime_d->queued;         /* [한국어] queued 전체를 "커밋됨" 상태로 이동 - getevents가 이만큼 돌려줌. */
	if (ioreq->ddir == DDIR_READ)
		ret = ime_native_preadv(ioreq->fd, ime_d->iovecs, ime_d->queued, ioreq->offset);   /* [한국어] 읽기 배치 제출 - 동기 호출. */
	else
		ret = ime_native_pwritev(ioreq->fd, ime_d->iovecs, ime_d->queued, ioreq->offset);  /* [한국어] 쓰기 배치 제출. */

	dprint(FD_IO, "committed %d iovecs\n", ime_d->queued);  /* [한국어] 배치 크기 로깅. */

	return fio_ime_psyncv_end(td, ret);                      /* [한국어] 결과를 각 io_u에 분배. */
}

/*
 * [한국어]
 * fio_ime_psyncv_getevents - commit에서 이미 완료된 전송을 fio에 알리는 단계.
 *
 * psyncv는 commit이 동기적으로 끝나므로 여기서는 io_us → event_io_us로 단순 복사만 하고
 * 큐 포인터를 0으로 리셋한다.
 */
static int fio_ime_psyncv_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct io_u *io_u;
	int events = 0;           /* [한국어] 반환할 완료 이벤트 수. */
	unsigned int count;

	if (ime_d->events) {      /* [한국어] 커밋되었으나 아직 수확 안 된 것이 있을 때만 처리. */
		for (count = 0; count < ime_d->events; count++) {
			io_u = ime_d->io_us[count];            /* [한국어] 커밋된 순서대로 io_u 추출. */
			ime_d->event_io_us[events] = io_u;     /* [한국어] fio가 fio_ime_event로 조회할 배열에 저장. */
			events++;
		}
		fio_ime_queue_reset(ime_d);              /* [한국어] psyncv는 한 배치가 끝나면 전체 포인터 리셋(링 동작 아님). */
	}

	dprint(FD_IO, "getevents(%u,%u) ret=%d queued=%u events=%u\n",
		min, max, events, ime_d->queued, ime_d->events);
	return events;
}

/*
 * [한국어]
 * fio_ime_psyncv_init - ime_psyncv의 init 콜백. IME 라이브러리 초기화 + ime_data 할당.
 *
 * ime_data 내부 필드:
 *   - sioreq: 단일 요청 기술자(imesio_req).
 *   - iovecs: iodepth 크기 iovec 배열.
 *   - io_us: 2*iodepth 크기(앞쪽 iodepth는 queued, 뒤쪽 iodepth는 event용으로 분할).
 */
static int fio_ime_psyncv_init(struct thread_data *td)
{
	struct ime_data *ime_d;

	if (fio_ime_engine_init(td) < 0)   /* [한국어] 공통 IME 초기화(라이브러리 init + real_file_size 임시 세팅). */
		return 1;

	ime_d = calloc(1, sizeof(*ime_d)); /* [한국어] 모든 필드 0 초기화 - head/tail/queued/events 모두 0부터. */

	ime_d->sioreq = malloc(sizeof(struct imesio_req));         /* [한국어] 단일 요청 구조체. */
	ime_d->iovecs = malloc(td->o.iodepth * sizeof(struct iovec));  /* [한국어] iodepth만큼의 iovec. */
	ime_d->io_us = malloc(2 * td->o.iodepth * sizeof(struct io_u *));  /* [한국어] 2*iodepth: 앞 절반은 queued, 뒤 절반은 event용. */
	ime_d->event_io_us = ime_d->io_us + td->o.iodepth;          /* [한국어] 뒤 절반 시작 포인터. */

	ime_d->depth = td->o.iodepth;                               /* [한국어] 링 용량. */

	td->io_ops_data = ime_d;                                    /* [한국어] 엔진 상태를 잡에 부착. */
	return 0;
}

/*
 * [한국어]
 * fio_ime_psyncv_clean - ime_psyncv의 cleanup 콜백. init에서 할당한 자원 해제.
 */
static void fio_ime_psyncv_clean(struct thread_data *td)
{
	struct ime_data *ime_d = td->io_ops_data;

	if (ime_d) {                          /* [한국어] init이 성공했을 때만 해제. */
		free(ime_d->sioreq);              /* [한국어] 요청 기술자. */
		free(ime_d->iovecs);              /* [한국어] iovec 배열. */
		free(ime_d->io_us);               /* [한국어] io_us는 event_io_us까지 포함하는 단일 할당. */
		free(ime_d);                      /* [한국어] 상태 컨테이너 자체. */
		td->io_ops_data = NULL;           /* [한국어] 이후 오접근 방지. */
	}

	fio_ime_engine_finalize(td);          /* [한국어] 공통 finalize - fork 모드면 ime_native_finalize 호출. */
}


/**************************************************************
 *           Private functions for non-blocking IOs
 *
 **************************************************************/

/*
 * [한국어]
 * fio_ime_aio_complete_cb - IME 라이브러리가 AIO 완료 시 호출하는 콜백.
 *
 * @aiocb: 제출 시 넘긴 iocb 포인터. user_context에 struct imeaio_req* 들어 있음.
 * @err:   0이면 성공, 그 외는 라이브러리 에러 코드.
 * @bytes: 성공 시 실제 전송 바이트 수.
 *
 * 실행 컨텍스트: IME 라이브러리의 내부 스레드(잡 스레드가 아님). 따라서 status는
 * mutex로 보호하고, wait 중일 수 있는 getevents 스레드를 cond_signal로 깨운다.
 */
void fio_ime_aio_complete_cb  (struct ime_aiocb *aiocb, int err,
							   ssize_t bytes)
{
	struct imeaio_req *ioreq = (struct imeaio_req *) aiocb->user_context;  /* [한국어] 제출 시 저장한 self 포인터 복원. */

	pthread_mutex_lock(&ioreq->status_mutex);                              /* [한국어] status 기록 직전 락 획득 - getevents와 상호배제. */
	ioreq->status = err == 0 ? bytes : FIO_IME_REQ_ERROR;                  /* [한국어] 성공 시 바이트 수, 실패 시 에러 sentinel 기록. */
	pthread_mutex_unlock(&ioreq->status_mutex);                            /* [한국어] 락 해제 - 이후 signal은 락 없이 가능. */

	pthread_cond_signal(&ioreq->cond_endio);                               /* [한국어] getevents가 cond_wait 중이면 깨움. */
}

/*
 * [한국어]
 * fio_ime_aio_can_queue - aio 엔진에서 새 io_u를 큐에 받아들일지 판단하는 훅.
 *
 * 현재 구현은 depth 체크를 호출자에서 이미 수행하고 용량 외 추가 거부 조건이 없으므로
 * 항상 true. 미래에 backpressure 정책 삽입을 위한 확장점.
 */
static bool fio_ime_aio_can_queue (struct ime_data *ime_d, struct io_u *io_u)
{
	/* So far we can queue in any case. */
	return true;  /* [한국어] 무조건 허용. */
}
/*
 * [한국어]
 * fio_ime_aio_can_append - 새 io_u를 기존 "열린" AIO 요청에 iovec으로 붙일 수 있는지 판단.
 *
 * 기존 요청에 append하면 iocb 하나로 여러 iovec을 한 번에 제출할 수 있어 효율적이다.
 * 단, IME의 iov 포인터가 ime_d->iovecs 배열의 연속 구간을 가리키므로 head가 0으로
 * 감긴 상태면 연속성이 깨지므로 새 요청을 시작해야 한다.
 */
static bool fio_ime_aio_can_append (struct ime_data *ime_d, struct io_u *io_u)
{
	/* We can only append if:
		- The iovecs will be contiguous in the array
		- There is already a queued iovec
		- The offsets are contiguous
		- The ddir and fs are the same */
	return (ime_d->head != 0 &&                       /* [한국어] head==0은 링이 감겨 배열 시작 - 기존 iov 구간과 불연속. */
			ime_d->queued - ime_d->events > 0 &&       /* [한국어] 아직 미커밋된 요청이 큐에 있어야 append 대상 존재. */
			ime_d->last_offset == io_u->offset &&      /* [한국어] 파일 오프셋 연속성. */
			ime_d->last_req->ddir == io_u->ddir &&     /* [한국어] 방향 일치(aio_read vs aio_write). */
			ime_d->last_req->iocb.fd == io_u->file->fd); /* [한국어] fd 일치. */
}

/* Before using this function, we should have already
   ensured that the queue is not full */
/*
 * [한국어]
 * fio_ime_aio_enqueue - aio 큐에 io_u를 추가. 가능하면 기존 요청에 iovec append, 아니면 새 요청 생성.
 *
 * 사전조건: queued < depth. can_queue()가 true.
 * 새 요청 생성 경로에서는 iocb 전체 필드를 채우고 last_req를 이 요청으로 갱신한다.
 */
static void fio_ime_aio_enqueue(struct ime_data *ime_d, struct io_u *io_u)
{
	struct imeaio_req *ioreq = &ime_d->aioreqs[ime_d->head];  /* [한국어] head 슬롯의 요청 객체. */
	struct ime_aiocb *iocb = &ioreq->iocb;                     /* [한국어] 그 안의 iocb. */
	struct iovec *iov = &ime_d->iovecs[ime_d->head];           /* [한국어] 같은 head 위치의 iovec. iov 배열과 요청 배열이 head로 동기 진행. */

	iov->iov_base = io_u->xfer_buf;                            /* [한국어] 사용자 버퍼 등록. */
	iov->iov_len = io_u->xfer_buflen;                          /* [한국어] 바이트 수. */

	if (fio_ime_aio_can_append(ime_d, io_u))                   /* [한국어] 기존 요청과 연속이면 iovcnt만 증가시키는 경량 경로. */
		ime_d->last_req->iocb.iovcnt++;                        /* [한국어] 기존 iocb가 새 iov까지 포함하도록 확장 - 배열 연속성 덕. */
	else {
		ioreq->status = FIO_IME_IN_PROGRESS;                   /* [한국어] 새 요청은 기본 "진행 중" 상태로 시작. */
		ioreq->ddir = io_u->ddir;                              /* [한국어] 방향 기록. */
		ime_d->last_req = ioreq;                               /* [한국어] append 대상 포인터 갱신. */

		iocb->complete_cb = &fio_ime_aio_complete_cb;          /* [한국어] 완료 콜백 등록 - 라이브러리가 호출. */
		iocb->fd = io_u->file->fd;                             /* [한국어] 대상 IME fd. */
		iocb->file_offset = io_u->offset;                      /* [한국어] 이 요청 전체의 시작 오프셋. */
		iocb->iov = iov;                                       /* [한국어] iov 배열의 해당 위치를 가리키게 하여 이후 append 시 확장 가능. */
		iocb->iovcnt = 1;                                      /* [한국어] 초기 1개, append마다 증가. */
		iocb->flags = 0;                                       /* [한국어] 추가 플래그 없음. */
		iocb->user_context = (intptr_t) ioreq;                 /* [한국어] 완료 콜백이 역참조할 self 포인터 저장(intptr_t 캐스팅으로 IME ABI 맞춤). */
	}

	ime_d->io_us[ime_d->head] = io_u;                          /* [한국어] iov/iocb와 동기 인덱스로 io_u 저장. */
	ime_d->last_offset = io_u->offset + io_u->xfer_buflen;     /* [한국어] 다음 can_append 검사용. */
	fio_ime_queue_incr(ime_d);                                 /* [한국어] head/queued 전진. */
}

/* Tries to queue an IO. It will create a new request if the IO can't be
   appended to the current request. It will fail if the queue can't contain
   any more io_u/iovec. In this case, commit and then get_events need to be
   called. */
/*
 * [한국어]
 * fio_ime_aio_queue - ime_aio queue 콜백. iovec을 링 큐에 누적(가능하면 기존 요청에 확장).
 *
 * 실제 IME 제출은 commit 단계에서 이루어지며, 이 함수는 일단 누적만 한다. 큐가 꽉 차면
 * FIO_Q_BUSY로 fio에 "커밋/수확 먼저 하세요"를 알림.
 */
static enum fio_q_status fio_ime_aio_queue(struct thread_data *td,
		struct io_u *io_u)
{
	struct ime_data *ime_d = td->io_ops_data;

	fio_ro_check(td, io_u);

	dprint(FD_IO, "queue: ddir=%d at %u commit=%u queued=%u events=%u\n",
		io_u->ddir, ime_d->head, ime_d->cur_commit,
		ime_d->queued, ime_d->events);

	if (ime_d->queued == ime_d->depth)                   /* [한국어] 링 용량 초과 차단. fio 코어가 FIO_Q_BUSY 를 받으면 commit+getevents 를 돌린 뒤 재시도. */
		return FIO_Q_BUSY;

	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {  /* [한국어] VERIFY 는 DDIR_READ 로 디스패치되므로 이 분기 활용. TRIM 은 이 분기로 진입 불가(open_file 에서 사전 거부). */
		if (!fio_ime_aio_can_queue(ime_d, io_u))          /* [한국어] 확장 훅 - 현재는 항상 true. */
			return FIO_Q_BUSY;

		fio_ime_aio_enqueue(ime_d, io_u);                 /* [한국어] 실제 누적(append 또는 새 요청). */
		return FIO_Q_QUEUED;                              /* [한국어] 비동기 - 완료는 getevents에서. */
	} else if (io_u->ddir == DDIR_SYNC) {
		if (ime_native_fsync(io_u->file->fd) < 0) {       /* [한국어] fsync는 배치 대상 아니므로 즉시 동기 수행. */
			io_u->error = errno;                            /* [한국어] 실패 시 errno 스냅샷하여 io_u 에 보존. */
			td_verror(td, io_u->error, "fsync");           /* [한국어] "fsync" 태그로 잡에 치명 에러 기록. */
		}
		return FIO_Q_COMPLETED;                           /* [한국어] SYNC 는 한 번의 syscall 로 끝나므로 즉시 완료 반환. */
	} else {
		/* [한국어] DDIR_TRIM/DDIR_DATASYNC/SYNC_FILE_RANGE 등 미지원 방향 방어 경로.
		 *   TRIM 은 open_file 에서 td_trim(td) 검사로 이미 거부됐지만 방어적 처리. */
		io_u->error = EINVAL;                             /* [한국어] 미지원 방향. */
		td_verror(td, io_u->error, "wrong ddir");        /* [한국어] "wrong ddir" 태그 잡에 에러 등록. */
		return FIO_Q_COMPLETED;                           /* [한국어] COMPLETED 반환 - fio 코어는 io_u->error 를 확인해 에러 처리. */
	}
}

/*
 * [한국어]
 * fio_ime_aio_commit - 누적된 AIO 요청들을 ime_native_aio_read/write로 실제 제출.
 *
 * cur_commit에서 시작하여 (queued - events)가 0이 될 때까지 요청 단위로 제출한다.
 * 제출 실패 시 해당 요청에 REQ_ERROR를 기록하고 음수 errno를 반환.
 * 호출 체인: backend → td_io_commit → [이 함수].
 */
static int fio_ime_aio_commit(struct thread_data *td)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct imeaio_req *ioreq;
	int ret = 0;

	/* Loop while there are events to commit */
	while (ime_d->queued - ime_d->events) {              /* [한국어] 미커밋 요청이 남은 동안 반복. */
		ioreq = &ime_d->aioreqs[ime_d->cur_commit];       /* [한국어] 다음 제출 대상. */
		if (ioreq->ddir == DDIR_READ)
			ret = ime_native_aio_read(&ioreq->iocb);      /* [한국어] 비동기 읽기 제출 - 완료는 콜백으로. */
		else
			ret = ime_native_aio_write(&ioreq->iocb);     /* [한국어] 비동기 쓰기 제출. */

		fio_ime_queue_commit(ime_d, ioreq->iocb.iovcnt);  /* [한국어] cur_commit/events를 iovcnt만큼 전진. */

		/* fio needs a negative error code */
		if (ret < 0) {                                    /* [한국어] 제출 자체 실패. */
			ioreq->status = FIO_IME_REQ_ERROR;             /* [한국어] 콜백 없이 직접 에러 마크. */
			return -errno;                                 /* [한국어] fio는 음수 errno를 요구. */
		}

		io_u_mark_submit(td, ioreq->iocb.iovcnt);         /* [한국어] 통계: 제출된 io_u 수 누적(iovcnt = 이 배치의 io_u 수). */
		dprint(FD_IO, "committed %d iovecs commit=%u queued=%u events=%u\n",
			ioreq->iocb.iovcnt, ime_d->cur_commit,
			ime_d->queued, ime_d->events);
	}

	return 0;
}

/*
 * [한국어]
 * fio_ime_aio_getevents - 커밋된 AIO 요청들의 완료를 수집하여 fio에 보고.
 *
 * @min, @max: fio가 기대하는 최소/최대 완료 수. @t: 타임아웃(현재 구현은 무시).
 *
 * tail부터 커밋된 순서대로 확인. 요청이 이미 완료되어 있으면 속한 모든 iovec/io_u를 이벤트로
 * 변환하고, 아직 진행 중이면 mutex+cond로 완료 대기. 완료된 요청 하나가 iovcnt>1이면
 * 다음 요청을 받으면 max를 넘길 수 있어 break로 현재 라운드를 종료.
 */
static int fio_ime_aio_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct imeaio_req *ioreq;
	struct io_u *io_u;
	int events = 0;          /* [한국어] 이번 호출에서 반환할 이벤트 수. */
	unsigned int count;       /* [한국어] 요청 내 iovec 순회 인덱스. */
	ssize_t bytes;            /* [한국어] 요청이 전송한 총 바이트 - 각 io_u에 분배. */

	while (ime_d->events) {  /* [한국어] 커밋되었으나 수확 안 된 것이 남아 있는 동안. events==0 이면 반환. */
		ioreq = &ime_d->aioreqs[ime_d->tail];  /* [한국어] tail 위치의 요청. */

		/* Break if we already got events, and if we will
		   exceed max if we append the next events */
		/* [한국어] aio 에서 한 요청은 iovcnt 개의 io_u 를 품을 수 있으므로, 이 요청을
		 *   통째로 수확하면 events+iovcnt 가 되어 fio 코어가 요구한 max 를 넘을 수 있다.
		 *   이미 events>0 으로 부분 결과가 있다면 다음 라운드로 미루는 것이 안전. */
		if (events && events + ioreq->iocb.iovcnt > max)  /* [한국어] 이미 일부 반환했고 이 요청을 추가하면 max 초과 - 중단. */
			break;

		if (ioreq->status != FIO_IME_IN_PROGRESS) {       /* [한국어] 이미 완료된 요청(성공 바이트 수 또는 REQ_ERROR). IN_PROGRESS 가 아니면 콜백이 이미 status 를 덮어썼다는 뜻. */

			bytes = ioreq->status;                         /* [한국어] 총 전송 바이트(또는 에러 sentinel). */
			for (count = 0; count < ioreq->iocb.iovcnt; count++) {  /* [한국어] 이 요청에 묶인 iovec 수만큼 io_u 처리. */
				io_u = ime_d->io_us[ime_d->tail];          /* [한국어] tail 위치의 io_u. */
				ime_d->event_io_us[events] = io_u;         /* [한국어] fio가 볼 이벤트 배열에 기록. */
				events++;
				fio_ime_queue_red(ime_d);                  /* [한국어] tail/queued/events 감소. 다음 반복에서 tail이 이미 전진해 있음. */

				if (ioreq->status == FIO_IME_REQ_ERROR)    /* [한국어] 요청 전체가 에러였으면 개별 io_u에 EIO. */
					io_u->error = EIO;
				else {
					io_u->resid = bytes > io_u->xfer_buflen ?
									0 : io_u->xfer_buflen - bytes;  /* [한국어] 남은 전송량보다 요청 크기가 크면 resid=xfer-bytes, 아니면 0. */
					io_u->error = 0;                        /* [한국어] 성공(부분 포함). */
					bytes -= io_u->xfer_buflen - io_u->resid; /* [한국어] 이번 io_u가 소비한 실제 전송량을 총합에서 차감. */
				}
			}
		} else {                                           /* [한국어] 아직 진행 중 - 완료 콜백 대기. */
			pthread_mutex_lock(&ioreq->status_mutex);      /* [한국어] status 검사 전 락. */
			while (ioreq->status == FIO_IME_IN_PROGRESS)   /* [한국어] spurious wakeup 방어 루프. */
				pthread_cond_wait(&ioreq->cond_endio, &ioreq->status_mutex);  /* [한국어] 콜백이 signal할 때까지 대기. */
			pthread_mutex_unlock(&ioreq->status_mutex);    /* [한국어] 락 해제 후 다음 루프 반복이 status를 다시 읽음. */
		}

	}

	dprint(FD_IO, "getevents(%u,%u) ret=%d queued=%u events=%u\n", min, max,
		events, ime_d->queued, ime_d->events);
	return events;
}

/*
 * [한국어]
 * fio_ime_aio_init - ime_aio init 콜백. psyncv와 유사하지만 aioreqs 배열 할당 + 각 요청의
 *                     cond/mutex를 init해야 한다.
 */
static int fio_ime_aio_init(struct thread_data *td)
{
	struct ime_data *ime_d;
	struct imeaio_req *ioreq;
	unsigned int i;

	if (fio_ime_engine_init(td) < 0)  /* [한국어] 공통 IME 초기화. */
		return 1;

	ime_d = calloc(1, sizeof(*ime_d));  /* [한국어] 0으로 초기화된 상태 컨테이너. */

	ime_d->aioreqs = malloc(td->o.iodepth * sizeof(struct imeaio_req));  /* [한국어] iodepth 크기 요청 배열. */
	ime_d->iovecs = malloc(td->o.iodepth * sizeof(struct iovec));        /* [한국어] 동일 크기 iovec 배열. */
	ime_d->io_us = malloc(2 * td->o.iodepth * sizeof(struct io_u *));    /* [한국어] queued+event 용 2배 크기. */
	ime_d->event_io_us = ime_d->io_us + td->o.iodepth;                    /* [한국어] 뒤 절반 시작. */

	ime_d->depth = td->o.iodepth;
	for (i = 0; i < ime_d->depth; i++) {        /* [한국어] 각 요청의 cond/mutex 초기화. */
		ioreq = &ime_d->aioreqs[i];
		pthread_cond_init(&ioreq->cond_endio, NULL);       /* [한국어] 기본 속성 cond 생성. */
		pthread_mutex_init(&ioreq->status_mutex, NULL);    /* [한국어] 기본 속성 mutex 생성. */
	}

	td->io_ops_data = ime_d;
	return 0;
}

/*
 * [한국어]
 * fio_ime_aio_clean - ime_aio cleanup 콜백. 각 요청의 cond/mutex destroy + 배열 해제.
 */
static void fio_ime_aio_clean(struct thread_data *td)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct imeaio_req *ioreq;
	unsigned int i;

	if (ime_d) {
		for (i = 0; i < ime_d->depth; i++) {     /* [한국어] 각 요청의 동기화 객체 파괴. */
			ioreq = &ime_d->aioreqs[i];
			pthread_cond_destroy(&ioreq->cond_endio);
			pthread_mutex_destroy(&ioreq->status_mutex);
		}
		free(ime_d->aioreqs);                     /* [한국어] 요청 배열. */
		free(ime_d->iovecs);                      /* [한국어] iovec 배열. */
		free(ime_d->io_us);                       /* [한국어] io_us(+event_io_us) 통합 할당. */
		free(ime_d);
		td->io_ops_data = NULL;
	}

	fio_ime_engine_finalize(td);                  /* [한국어] fork 모드면 라이브러리도 종료. */
}


/**************************************************************
 *                   IO engines definitions
 *
 **************************************************************/

/* The FIO_DISKLESSIO flag used for these engines is necessary to prevent
   FIO from using POSIX calls. See fio_ime_open_file for more details. */
/* [한국어] 본 파일이 노출하는 세 ioengine_ops 의 공통 규약 메모.
 *
 *   ┌── 공통 .flags 비트 풀이 ───────────────────────────────────────────────────┐
 *   │ FIO_DISKLESSIO  (모든 IME 엔진)
 *   │   - "디스크 기반 가정 금지" 플래그. fio 코어가 POSIX open/stat/close 등을
 *   │     스스로 호출하지 않도록 막는다. IME 경로(im://...)는 일반 POSIX
 *   │     네임스페이스에 보장되지 않으므로 모든 파일 핸들 관련 호출을 엔진이
 *   │     인수해야 한다(fio_ime_open_file/close_file/get_file_size/unlink_file).
 *   │     또한 size 계산/레이아웃 단계에서 디스크 사용량 보고(NODISKUTIL)와
 *   │     별개로 파일이 디스크 블록을 점유한다는 가정도 비활성화한다.
 *   │
 *   │ FIO_SYNCIO     (ime_psync, ime_psyncv 만)
 *   │   - "이 엔진은 동기적으로 큐잉/완료한다" 힌트. fio 러너가 issue 시간을
 *   │     queue() 진입 시점에 자동 기록하고, getevents 루프를 단순화한다.
 *   │     ime_psync 는 queue() 안에서 ime_native_pread/pwrite 를 끝내고 즉시
 *   │     FIO_Q_COMPLETED 반환. ime_psyncv 는 queue() 시점은 비동기지만
 *   │     commit() 의 ime_native_preadv/pwritev 호출이 동기이므로 SYNCIO.
 *   │   - ime_aio 는 진정한 비동기이므로 이 비트를 켜지 않는다(미설정 의미:
 *   │     코어가 issue 시점을 commit() 호출 시점으로 기록하고, getevents 루프
 *   │     동기화는 엔진의 cond_wait 에 위임).
 *   │
 *   │ 미설정 비트 의미(세 엔진 공통):
 *   │   FIO_RAWIO    : 블록 디바이스 raw 접근 아님.
 *   │   FIO_NOEXTEND : 잡 중 파일 확장 허용 (fio_ime_open_file 이 ftruncate 사용).
 *   │   FIO_PIPEIO   : 파이프 I/O 아님.
 *   │   FIO_BARRIER  : 배리어 의미론 별도 추적 안 함.
 *   │   FIO_UNIDIR   : 양방향 R/W 모두 가능.
 *   │   FIO_NODISKUTIL : 디스크 사용량 통계 비활성화 안 함(IME가 backing NVMe 사용).
 *   │   FIO_ASYNCIO_SYNC_TRIM/SYNCFS : TRIM/syncfs 동기 폴백 없음(IME TRIM 미지원).
 *   │   FIO_ASYNCIO_SETS_ISSUE_TIME : 엔진이 issue 시간을 따로 기록하지 않음.
 *   └─────────────────────────────────────────────────────────────────────────────┘
 *
 *   ┌── 공통 ioengine_ops 필드 규약 ─────────────────────────────────────────────┐
 *   │ .name           : --ioengine=<NAME> 으로 선택될 식별자. load_ioengine() 의
 *   │                    strcmp 매칭 키.
 *   │ .version        : FIO_IOOPS_VERSION (현재 fio 빌드의 ABI 버전). 불일치 시
 *   │                    check_engine_ops() 가 로드 거부.
 *   │ .setup          : td_io_setup 시점, init 직전. 본 엔진들은 모두
 *   │                    fio_ime_setup 으로 real_file_size=0 만 표시(IME 라이브러리
 *   │                    init 을 fork 이후로 미루기 위함).
 *   │ .init/.cleanup  : 잡당 1회. ime_native_init/finalize 와 ime_data 라이프.
 *   │ .queue          : io_u 1개 제출. 반환 FIO_Q_COMPLETED|QUEUED|BUSY.
 *   │ .commit         : queue 가 누적한 배치를 실제 IME 에 발사. ime_psync 는
 *   │                    필요 없어 NULL(코어가 commit 단계 건너뜀).
 *   │ .getevents      : 완료 수확. 반환 = 완료된 io_u 수.
 *   │ .event          : getevents 가 보고한 idx 번째 io_u 반환(event_io_us[idx]).
 *   │ .open_file      : ime_native_open 으로 fd 획득. TRIM 거부.
 *   │ .close_file     : ime_native_close.
 *   │ .get_file_size  : ime_native_stat 으로 real_file_size 채움.
 *   │ .unlink_file    : POSIX unlink (IME가 FUSE 마운트로도 노출되는 경우 지원).
 *   └─────────────────────────────────────────────────────────────────────────────┘
 */

/* [한국어] ioengine_prw — "ime_psync" 동기 엔진 vtable.
 *          단순 ime_native_pread/pwrite/fsync 직접 호출. commit/getevents 미사용.
 *          ime_data 도 사용하지 않음(td->io_ops_data == NULL). iodepth>1 의미 없음. */
static struct ioengine_ops ioengine_prw = {
	.name		= "ime_psync",
	/* [한국어] --ioengine=ime_psync 식별자. load_ioengine() 의 strcmp 매칭 키.
	 *          fio 명령줄/잡 파일에서 이 이름으로 선택된다. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] fio 빌드의 ioengine ABI 버전. 불일치 시 check_engine_ops() 가 로드 거부.
	 *          본 파일이 외부 .so 가 아닌 in-tree 엔진이라 사실상 항상 일치. */

	.setup		= fio_ime_setup,
	/* [한국어] td_io_setup 콜백. real_file_size=0 으로 채워 fio 코어가 stat 단계에서
	 *          POSIX stat 을 쓰지 못하도록 함. 실제 크기는 open_file 에서 채워짐. */

	.init		= fio_ime_engine_init,
	/* [한국어] td_io_init 콜백. ime_native_init() 호출 + real_file_size 임시 채움.
	 *          fork 모드에서 잡 스레드(=프로세스) 생성 직후, IME 라이브러리 첫 사용 전. */

	.cleanup	= fio_ime_engine_finalize,
	/* [한국어] 잡 종료 시 콜백. fork 모드에서만 ime_native_finalize() 즉시 호출.
	 *          thread 모드에서는 다른 잡이 IME 사용 중일 수 있어 finalize 보류. */

	.queue		= fio_ime_psync_queue,
	/* [한국어] io_u 1개 동기 처리. ime_native_pread/pwrite/fsync 직접 호출.
	 *          항상 FIO_Q_COMPLETED 반환 — commit/getevents 단계 우회.
	 *          DDIR_TRIM 등은 EINVAL 로 처리. */

	.open_file	= fio_ime_open_file,
	/* [한국어] IME 경로 prefix 부여 + ime_native_open(). TRIM 잡 거부.
	 *          generic_file_open 을 IME API 로 재구현한 것. */

	.close_file	= fio_ime_close_file,
	/* [한국어] ime_native_close() + f->fd = -1 무효화. */

	.get_file_size	= fio_ime_get_file_size,
	/* [한국어] ime_native_stat() 으로 real_file_size 채움. */

	.unlink_file  	= fio_ime_unlink_file,
	/* [한국어] POSIX unlink() 사용. IME 가 FUSE 마운트로도 노출되어
	 *          OS 레이어에서 디렉토리 엔트리 제거 가능함을 전제. */

	.flags	    	= FIO_SYNCIO | FIO_DISKLESSIO,
	/* [한국어] FIO_SYNCIO  : queue() 가 즉시 완료(FIO_Q_COMPLETED) — 코어가 issue
	 *                        시간을 queue 시점에 자동 기록.
	 *          FIO_DISKLESSIO : POSIX open/stat 등 코어 호출 금지(엔진이 인수). */
};

/* [한국어] ioengine_pvrw — "ime_psyncv" iovec 누적 동기 엔진 vtable.
 *          fio API 는 비동기지만(queue→commit→getevents 분리), 실제 IME 호출
 *          (ime_native_preadv/pwritev) 은 commit() 안에서 동기 실행. */
static struct ioengine_ops ioengine_pvrw = {
	.name		= "ime_psyncv",
	/* [한국어] --ioengine=ime_psyncv 식별자. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전 가드. */

	.setup		= fio_ime_setup,
	/* [한국어] real_file_size=0 표식. ime_psync 와 공유. */

	.init		= fio_ime_psyncv_init,
	/* [한국어] fio_ime_engine_init() + ime_data + sioreq + iovecs[depth] +
	 *          io_us[2*depth] 할당. event_io_us = io_us + depth. */

	.cleanup	= fio_ime_psyncv_clean,
	/* [한국어] init 의 모든 free + fio_ime_engine_finalize. */

	.queue		= fio_ime_psyncv_queue,
	/* [한국어] iovec 슬롯에 io_u 적재. 동일 fd/ddir/연속 오프셋 검사 후
	 *          FIO_Q_QUEUED. 큐 만원 또는 위반이면 FIO_Q_BUSY. fsync/오류는 즉시 COMPLETED. */

	.commit		= fio_ime_psyncv_commit,
	/* [한국어] 누적 iovec 을 단일 ime_native_preadv/pwritev 로 IME 에 동기 전송.
	 *          commit 반환 시점에 모든 전송 끝남. events = queued 로 일괄 승격. */

	.getevents	= fio_ime_psyncv_getevents,
	/* [한국어] commit 이 이미 끝낸 io_us 를 event_io_us 로 단순 복사. 큐 리셋.
	 *          반환 = 복사한 이벤트 수(=커밋된 iovec 수). */

	.event		= fio_ime_event,
	/* [한국어] event_io_us[event] 반환. ime_aio 와 공유 구현. */

	.open_file	= fio_ime_open_file,
	.close_file	= fio_ime_close_file,
	.get_file_size	= fio_ime_get_file_size,
	.unlink_file  	= fio_ime_unlink_file,
	/* [한국어] ime_psync 와 동일 — 모든 IME 엔진이 같은 파일 관리 콜백 공유. */

	.flags	    	= FIO_SYNCIO | FIO_DISKLESSIO,
	/* [한국어] FIO_SYNCIO : commit 의 ime_native_preadv/pwritev 가 동기 호출.
	 *                       getevents 가 cond_wait 등의 비동기 동기화를 하지 않음.
	 *          FIO_DISKLESSIO : POSIX 파일 호출 금지. */
};

/* [한국어] ioengine_aio — "ime_aio" 진정한 비동기 엔진 vtable.
 *          ime_native_aio_{read,write} 로 다중 in-flight 운용.
 *          완료는 IME 라이브러리 콜백 → status_mutex/cond_endio 로 동기화. */
static struct ioengine_ops ioengine_aio = {
	.name		= "ime_aio",
	/* [한국어] --ioengine=ime_aio 식별자. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전 가드. */

	.setup		= fio_ime_setup,
	/* [한국어] real_file_size=0 표식. */

	.init		= fio_ime_aio_init,
	/* [한국어] fio_ime_engine_init() + ime_data + aioreqs[depth] + iovecs[depth] +
	 *          io_us[2*depth] 할당 + 각 aioreq 의 cond/mutex pthread_init. */

	.cleanup	= fio_ime_aio_clean,
	/* [한국어] 각 aioreq 의 cond/mutex destroy + 모든 free + finalize. */

	.queue		= fio_ime_aio_queue,
	/* [한국어] iovec 슬롯에 io_u 적재. can_append 면 기존 iocb 의 iovcnt 증가,
	 *          아니면 새 ime_aiocb 채움. FIO_Q_QUEUED|BUSY|COMPLETED. */

	.commit		= fio_ime_aio_commit,
	/* [한국어] 미커밋(cur_commit 부터 head 직전까지) 요청들을 ime_native_aio_{read,write}
	 *          로 발사. 한 요청은 iovcnt 만큼의 iovec 를 묶어 vector AIO. */

	.getevents	= fio_ime_aio_getevents,
	/* [한국어] tail 부터 검사. status==IN_PROGRESS 면 cond_wait, 완료면 io_u 들에
	 *          error/resid 분배 후 event_io_us 로 이전. 반환 = 수확한 io_u 수. */

	.event		= fio_ime_event,
	/* [한국어] event_io_us[event] 반환. ime_psyncv 와 공유. */

	.open_file	= fio_ime_open_file,
	.close_file	= fio_ime_close_file,
	.get_file_size	= fio_ime_get_file_size,
	.unlink_file  	= fio_ime_unlink_file,
	/* [한국어] 모든 IME 엔진 공유 파일 콜백. */

	.flags       	= FIO_DISKLESSIO,
	/* [한국어] FIO_SYNCIO 미설정 — 진정한 비동기 엔진. fio 코어가 commit 시점에
	 *          issue 시간 기록하고, getevents 가 cond_wait 으로 라이브러리 콜백을 기다림.
	 *          FIO_DISKLESSIO 만 유지. */
};

/*
 * [한국어]
 * fio_ime_register - 프로세스 로더가 main() 이전에 자동 호출하는 생성자.
 *                     세 IME 엔진 vtable 을 fio 의 전역 engine_list 에 등록한다.
 *
 * @return: void (생성자는 반환값 무시).
 *
 * 동작 메커니즘:
 *   - fio_init 매크로 = __attribute__((constructor)) = GCC/Clang 의 생성자 속성.
 *     이 속성이 붙은 함수는 ELF 섹션 .init_array 에 들어가고, ld.so(동적 로더)
 *     또는 정적 링크의 __libc_start_main 이 main() 호출 직전에 모든 .init_array
 *     엔트리를 순서대로 실행한다.
 *   - 결과적으로 사용자가 --ioengine=ime_psync 같은 옵션을 파싱하는 load_ioengine()
 *     호출 시점에는 이미 세 엔진이 engine_list 에 등록되어 있어 find_ioengine()
 *     의 strcmp 매칭에 걸린다.
 *   - register_ioengine() 은 ioengines.c 가 제공하는 헬퍼로, flist_add_tail 로
 *     전역 engine_list 에 엔트리를 끼워 넣는다. 동일 이름 중복 등록은 체크하지
 *     않으므로 .so 를 여러 번 dlopen 하면 문제가 될 수 있으나, 정적 링크
 *     (in-tree) 에서는 생성자가 1회만 돈다.
 *
 * 실행 컨텍스트: 메인 스레드(잡 스레드 생성 이전). 다른 생성자들과의 순서는
 *   링커 결정. 동기화 불필요.
 *
 * 호출 체인:
 *   ld.so / __libc_start_main → [이 함수] → register_ioengine × 3.
 */
static void fio_init fio_ime_register(void)
{
	register_ioengine(&ioengine_prw);
	/* [한국어] ime_psync vtable 을 engine_list 끝에 link.
	 *          --ioengine=ime_psync 로 찾을 수 있게 됨. */

	register_ioengine(&ioengine_pvrw);
	/* [한국어] ime_psyncv vtable 등록. */

	register_ioengine(&ioengine_aio);
	/* [한국어] ime_aio vtable 등록. 세 엔진 모두 같은 translation unit 에서
	 *          하나의 constructor 로 일괄 등록(공유 헬퍼 재사용이 목적). */
}

/*
 * [한국어]
 * fio_ime_unregister - 프로세스 종료 시 자동 호출되는 소멸자. 엔진 vtable 등록을
 *                       해제하고, thread 모드에서 미뤄두었던 ime_native_finalize 를
 *                       마지막으로 호출한다.
 *
 * @return: void.
 *
 * 동작 메커니즘:
 *   - fio_exit 매크로 = __attribute__((destructor)) = ELF .fini_array 섹션.
 *     ld.so / exit() 경로의 atexit 체인이 main() 반환 직후 호출.
 *   - unregister_ioengine() = flist_del_init 로 engine_list 에서 엔트리 unlink.
 *     .so 로 재적재 시 중복 등록 방지용 안전장치.
 *   - thread 모드(use_thread=1)에서는 fio_ime_engine_finalize() 가
 *     ime_native_finalize 를 건너뛰었으므로 이 소멸자가 "프로세스 수명 끝까지"
 *     라이브러리 종료를 지연시켰다가 여기서 한 번에 호출한다. fork 모드에서는
 *     이미 잡 cleanup 단계에서 finalize 가 끝났으므로 여기 도달 시점에는
 *     fio_ime_is_initialized 가 false 라 이 분기 스킵.
 *
 * 에러 처리: ime_native_finalize 실패해도 경고만 로그. 프로세스는 계속 종료.
 *
 * 실행 컨텍스트: 메인 스레드(모든 잡 스레드 조인 후).
 *
 * 호출 체인:
 *   exit() / _fini → [이 함수] → unregister_ioengine × 3 + 조건부 ime_native_finalize.
 */
static void fio_exit fio_ime_unregister(void)
{
	unregister_ioengine(&ioengine_prw);
	/* [한국어] ime_psync 를 engine_list 에서 unlink. */

	unregister_ioengine(&ioengine_pvrw);
	/* [한국어] ime_psyncv unlink. */

	unregister_ioengine(&ioengine_aio);
	/* [한국어] ime_aio unlink. */

	if (fio_ime_is_initialized && ime_native_finalize() < 0)
		/* [한국어] thread 모드에서 지연된 최종 finalize.
		 *   조건: fio_ime_is_initialized == true  (ime_native_init 이 한 번이라도 성공했고,
		 *                                           cleanup 이 thread 모드라 finalize 를 건너뜀).
		 *   호출: ime_native_finalize() — IME 라이브러리 내부 스레드/RPC 연결 해제.
		 *   실패: 음수 반환 시 log_err 로 경고만 출력하고 exit 흐름 계속.
		 *   fork 모드에서는 fio_ime_engine_finalize 가 이미 finalize 했으므로
		 *   fio_ime_is_initialized == false → 이 분기 스킵. */
		log_err("Warning: IME did not finalize properly\n");
}
