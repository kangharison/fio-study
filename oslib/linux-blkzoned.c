/*
 * Copyright (C) 2020 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 */
/*
 * [한국어 설명] Linux ZBD(Zoned Block Device) UAPI 어댑터 (linux-blkzoned.c)
 *
 * === 파일의 역할 ===
 * Linux 커널의 ZBD(구역 블록 장치) 인터페이스를 fio 의 zbd.c 코어가 사용할
 * 수 있는 OS-중립 API(zbd.h 의 blkzoned_*)로 어댑팅한다. ZBD 는 SMR HDD
 * (Shingled Magnetic Recording)와 ZNS SSD(Zoned Namespace, NVMe 2.0 Chapter 6
 * §11)의 공통 추상화로, 매체를 "zone" 단위로 분할하고 순차 쓰기 제약을 강제
 * 하는 모델이다. 이 파일은 다음 5개 작업을 제공한다:
 *   1) 모델 탐지 — sysfs `queue/zoned` 속성("none"/"host-aware"/"host-managed")
 *      을 읽어 장치가 어떤 부류인지 판정.
 *   2) 동시 오픈/활성 zone 수 한계 — sysfs `queue/max_open_zones` /
 *      `queue/max_active_zones` 조회.
 *   3) Zone 리포트 — BLKREPORTZONE ioctl 로 [offset, offset+?] 범위의 zone
 *      정보(start/wp/len/capacity/type/cond) 수집.
 *   4) 쓰기 포인터 리셋/완료 — BLKRESETZONE / BLKFINISHZONE ioctl.
 *   5) 쓰기 포인터 이동 — fallocate(FL_ZERO_RANGE) 또는 pwrite 로 현재 wp
 *      위치에 데이터 기록(ZBD 사전 조건 충족용).
 *
 * fio 의 zbd 로직은 ZNS/SMR 의 제약(순차 쓰기, write pointer 진행)을 시뮬
 * 레이션하는 워크로드를 작성·검증하기 위해 이 어댑터를 광범위하게 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   zbd.c (zbd_setup_files / zbd_report_zones / zbd_reset_zone 등 코어)
 *     → blkzoned.h (공통 API)
 *       → [linux-blkzoned.c] (Linux 구현)
 *         → sysfs 읽기 (장치 속성) / ioctl (zone 연산) / pwrite·fallocate (데이터)
 * 다른 OS 에서는 이 파일이 빌드되지 않고 no-op/오류 스텁이 제공된다.
 *
 * === 타 모듈과의 연결 ===
 * - zbd.c: 이 파일의 모든 함수의 실질적 호출자. 잡 초기화 시 장치 모델을
 *   판정하고, 런타임에 각 io_u 의 zone 상태를 추적하며, 필요 시 reset/finish
 *   를 호출한다.
 * - blkzoned.h: 공개 프로토타입 — OS 간 동일 시그니처.
 * - zbd_types.h: zbd_zone / zbd_zoned_model / ZBD_ZONE_TYPE_* / ZBD_ZONE_COND_*
 *   enum 정의.
 * - <linux/blkzoned.h>: 커널 UAPI — struct blk_zone(64B, 섹터 단위),
 *   struct blk_zone_report, struct blk_zone_range, BLK_ZONE_TYPE_* /
 *   BLK_ZONE_COND_* 상수, BLKREPORTZONE/BLKRESETZONE/BLKFINISHZONE ioctl.
 * - <linux/falloc.h>: FALLOC_FL_ZERO_RANGE 상수 — 데이터를 명시 0 으로 채워
 *   wp 를 이동시킬 때 사용.
 * - oslib/asprintf.h: sysfs 경로 동적 조립용.
 * - log.h/verify.h/fio.h: dprint(FD_ZBD) 매크로, thread_data/fio_file,
 *   td_verror.
 *
 * === 주요 함수 요약 ===
 * - read_file(): sysfs 한 줄 파일 읽기 헬퍼.
 * - blkzoned_get_sysfs_attr(): /sys/dev/block/major:minor/<attr> 탐색
 *   (파티션이면 holder 로 상향).
 * - blkzoned_get_zoned_model(): queue/zoned → ZBD_NONE/HOST_AWARE/HOST_MANAGED.
 * - blkzoned_get_max_open_zones / _max_active_zones: sysfs 숫자 속성.
 * - zone_capacity(): blk_zone 의 capacity(또는 len)를 바이트로 환산.
 * - blkzoned_report_zones(): BLKREPORTZONE ioctl — 커널 struct blk_zone 을
 *   fio zbd_zone 으로 변환.
 * - blkzoned_reset_wp / _finish_zone: BLKRESETZONE / BLKFINISHZONE ioctl.
 * - blkzoned_move_zone_wp(): pwrite 또는 fallocate(FL_ZERO_RANGE) 로 wp 전진.
 */
#include <errno.h>  /* [한국어] errno / E* 상수 — ENOTTY 감지로 커널 버전 감별. */
#include <string.h>  /* [한국어] strcmp/strrchr 선언. */
#include <stdlib.h>  /* [한국어] free/calloc/atoll 선언. */
#include <dirent.h>  /* [한국어] 현재 직접 사용 없음 — 간접 의존. */
#include <fcntl.h>  /* [한국어] open(2) 플래그 O_RDONLY/O_RDWR/O_LARGEFILE/O_DIRECT/O_WRONLY. */
#include <sys/ioctl.h>  /* [한국어] ioctl(2) — BLK* 명령 발행. */
#include <sys/stat.h>  /* [한국어] stat(2) + st_rdev. */
#include <unistd.h>  /* [한국어] read/write/close/pwrite/readlink. */

#include "file.h"  /* [한국어] struct fio_file + filetype enum. */
#include "fio.h"  /* [한국어] thread_data / dprint 등 fio 코어 심볼 카탈로그. */
#include "lib/pow2.h"  /* [한국어] is_power_of_2 등 — 현재 간접. */
#include "log.h"  /* [한국어] dprint / log_err 매크로. FD_ZBD 디버그 채널. */
#include "oslib/asprintf.h"  /* [한국어] sysfs 경로 동적 조립용 asprintf 폴리필. */
#include "smalloc.h"  /* [한국어] 공유 메모리 할당자 — 간접 의존. */
#include "verify.h"  /* [한국어] verify 경로 의존성. */
#include "zbd_types.h"  /* [한국어] zbd_zone / zbd_zoned_model / ZBD_ZONE_* enum. */

#include <linux/blkzoned.h>  /* [한국어] 커널 UAPI — struct blk_zone / blk_zone_report / blk_zone_range / BLKREPORTZONE / BLKRESETZONE / BLK_ZONE_TYPE_* / BLK_ZONE_COND_* 등. */
/* [한국어] BLKFINISHZONE: 구역 완료 ioctl - 커널 5.5 이전 헤더에 없을 수 있으므로 매크로 확장으로 대응.
 * _IOW: 인자를 커널로 전달하는 ioctl 매크로. 0x12 는 BLK 계열 magic, 136 은 BLKFINISHZONE 의 cmd 번호.
 * 이 ifndef 는 헤더 누락 시 컴파일을 살리는 방어 — 런타임에 커널이 미지원이면 ENOTTY 로 실패해 무시된다. */
#ifndef BLKFINISHZONE
#define BLKFINISHZONE _IOW(0x12, 136, struct blk_zone_range)
#endif
#include <linux/falloc.h>  /* [한국어] FALLOC_FL_ZERO_RANGE — wp 이동 시 명시적 zero-fill. */

/*
 * [한국어] 시스템의 uapi 헤더가 구역 용량(capacity) 필드를 지원하지 않는 경우,
 * 로컬 버전의 구조체를 정의한다. 구역 용량은 구역 크기(len)보다 작을 수 있으며,
 * 이는 ZNS SSD에서 구역의 실제 쓸 수 있는 용량을 나타낸다. capacity 필드는
 * Linux 5.9 커널(struct blk_zone v2)에서 추가됨. CONFIG_HAVE_REP_CAPACITY 는
 * configure 가 헤더의 capacity 필드 존재 여부를 탐지해 정의.
 */
/*
 * If the uapi headers installed on the system lacks zone capacity support,
 * use our local versions. If the installed headers are recent enough to
 * support zone capacity, do not redefine any structs.
 */
#ifndef CONFIG_HAVE_REP_CAPACITY
#define BLK_ZONE_REP_CAPACITY	(1 << 0)  /* [한국어] blk_zone_report.flags 비트 0 — capacity 필드 유효 신호. */

struct blk_zone_v2 {
	__u64	start;          /* Zone start sector */
	__u64	len;            /* Zone length in number of sectors */
	__u64	wp;             /* Zone write pointer position */
	__u8	type;           /* Zone type */
	__u8	cond;           /* Zone condition */
	__u8	non_seq;        /* Non-sequential write resources active */
	__u8	reset;          /* Reset write pointer recommended */
	__u8	resv[4];
	__u64	capacity;       /* Zone capacity in number of sectors */
	__u8	reserved[24];
};
#define blk_zone blk_zone_v2  /* [한국어] 파일 내부에서 blk_zone 이름을 로컬 정의로 대체. */

struct blk_zone_report_v2 {
	__u64	sector;
	__u32	nr_zones;
	__u32	flags;
struct blk_zone zones[0];
};
#define blk_zone_report blk_zone_report_v2
#endif /* CONFIG_HAVE_REP_CAPACITY */

/*
 * [한국어]
 * read_file - sysfs 속성 파일의 첫 줄을 NUL-종단 힙 문자열로 읽음
 *
 * @path: 읽을 sysfs 파일 경로(예: "/sys/dev/block/8:0/queue/zoned").
 * @return: strdup 된 첫 줄 내용(호출자가 free 해야 함). 실패 시 NULL.
 *
 * sysfs 파일은 보통 한 줄의 짧은 값(예: "host-managed\n"). fopen 으로
 * 열어 fgets 로 한 줄 읽고, strsep 으로 "\n" 구분자를 NUL 로 바꿔 개행 제거,
 * strdup 로 복사. 실패 경로에서도 fclose 는 수행(누수 방지).
 *
 * 호출 체인:
 *   blkzoned_get_sysfs_attr() → [read_file()] → fopen/fgets/strsep/strdup
 */
/*
 * Read up to 255 characters from the first line of a file. Strip the trailing
 * newline.
 */
static char *read_file(const char *path)
{
	char line[256], *p = line;  /* [한국어] 256B 라인 버퍼 + strsep 이동용 포인터 p. */
	FILE *f;

	f = fopen(path, "rb");  /* [한국어] 바이너리 모드(줄바꿈 변환 방지). */
	if (!f)
		return NULL;  /* [한국어] 파일 없음/권한 — 호출자 NULL 판정. */
	if (!fgets(line, sizeof(line), f))
		line[0] = '\0';  /* [한국어] 빈 파일 — 빈 문자열로 초기화. */
	strsep(&p, "\n");  /* [한국어] 개행까지 읽고 '\n' 을 NUL 로 치환(파괴적). p 는 다음 토큰으로 전진되지만 미사용. */
	fclose(f);

	return strdup(line);  /* [한국어] 호출자 소유 힙 사본. NULL 도 가능(malloc 실패). */
}

/*
 * [한국어]
 * blkzoned_get_sysfs_attr - 블록 장치의 sysfs 속성값을 문자열로 읽음
 *
 * @file_name: 블록 장치 노드(예: "/dev/sda", "/dev/nvme0n1p1").
 * @attr: 속성 상대 경로(예: "queue/zoned", "queue/max_open_zones").
 * @return: 힙 문자열(호출자 free), 실패 시 NULL.
 *
 * 동작 단계:
 * 1) stat(file_name) — 장치의 st_rdev(major:minor) 취득.
 * 2) /sys/dev/block/<major>:<minor> 심볼릭 링크 readlink 로 canonical sysfs
 *    경로 취득(예: ../../devices/.../sda/sda1).
 * 3) 해당 경로의 "partition" 파일을 확인해 파티션인지 판정. 파티션이면
 *    canonical 경로의 마지막 세그먼트를 제거해 holder 장치(예: sda)로 상향
 *    — 대부분 sysfs 속성은 파티션이 아닌 holder 에만 존재.
 * 4) "/sys/dev/block/<canonical>/<attr>" 경로 조립 후 read_file 호출.
 *
 * 왜 복잡한가: 사용자는 파티션 경로(/dev/sda1)를 지정해도 동작해야 하는데,
 * queue/* 같은 속성은 홀더에만 있기 때문. 경로 상향 로직이 이를 자동화한다.
 *
 * 호출 체인:
 *   blkzoned_get_zoned_model / _max_open_zones / _max_active_zones
 *     → [blkzoned_get_sysfs_attr()] → stat/readlink/read_file/asprintf
 */
/*
 * Get the value of a sysfs attribute for a block device.
 *
 * Returns NULL on failure.
 * Returns a pointer to a string on success.
 * The caller is responsible for freeing the memory.
 */
static char *blkzoned_get_sysfs_attr(const char *file_name, const char *attr)
{
	char *attr_path = NULL;  /* [한국어] 최종 속성 경로(해제 대상). */
	struct stat statbuf;  /* [한국어] stat 결과. */
	char *sys_devno_path = NULL;  /* [한국어] "/sys/dev/block/major:minor" 임시 경로. */
	char *part_attr_path = NULL;  /* [한국어] 파티션 플래그 파일 경로. */
	char *part_str = NULL;  /* [한국어] partition 파일 내용("1" 이면 파티션). */
	char sys_path[PATH_MAX];  /* [한국어] readlink 결과(canonical 경로). */
	ssize_t sz;  /* [한국어] readlink 반환값. */
	char *delim = NULL;  /* [한국어] sys_path 의 마지막 '/' 포인터 — holder 상향 시 NUL 치환. */
	char *attr_str = NULL;  /* [한국어] 최종 결과 문자열. */

	if (stat(file_name, &statbuf) < 0)
		goto out;  /* [한국어] 장치 없음/권한 — 즉시 실패. */

	if (asprintf(&sys_devno_path, "/sys/dev/block/%d:%d",
		     major(statbuf.st_rdev), minor(statbuf.st_rdev)) < 0)
		goto out;  /* [한국어] sysfs 진입 링크 경로 생성. */

	sz = readlink(sys_devno_path, sys_path, sizeof(sys_path) - 1);
	if (sz < 0)
		goto out;  /* [한국어] 심링크 실패 — ZBD 지원 불가. */
	sys_path[sz] = '\0';  /* [한국어] readlink 는 NUL 을 붙이지 않음 — 수동 종단. */

	/*
	 * If the device is a partition device, cut the device name in the
	 * canonical sysfs path to obtain the sysfs path of the holder device.
	 *   e.g.:  /sys/devices/.../sda/sda1 -> /sys/devices/.../sda
	 */
	if (asprintf(&part_attr_path, "/sys/dev/block/%s/partition",
		     sys_path) < 0)
		goto out;
	part_str = read_file(part_attr_path);  /* [한국어] "1\n" 이면 파티션, 없거나 "0" 이면 홀더. */
	if (part_str && *part_str == '1') {
		delim = strrchr(sys_path, '/');  /* [한국어] canonical 경로에서 마지막 '/' 찾기. */
		if (!delim)
			goto out;
		*delim = '\0';  /* [한국어] 마지막 세그먼트 절단 — holder 경로로 상향. */
	}

	if (asprintf(&attr_path,
		     "/sys/dev/block/%s/%s", sys_path, attr) < 0)
		goto out;

	attr_str = read_file(attr_path);  /* [한국어] 최종 속성값 읽기. */
out:
	free(attr_path);  /* [한국어] 각 임시 경로 반환 — free(NULL) 은 안전. */
	free(part_str);
	free(part_attr_path);
	free(sys_devno_path);

	return attr_str;  /* [한국어] 성공 시 힙 문자열, 실패 시 NULL. */
}

/*
 * [한국어]
 * blkzoned_get_zoned_model - 장치의 ZBD 모델(none/host-aware/host-managed)을 판정
 *
 * @td: 스레드 컨텍스트(현재 본 함수에서는 미사용, 인터페이스 통일용).
 * @f: 대상 fio_file — filetype 이 FIO_TYPE_BLOCK 이 아니면 -EINVAL.
 * @model: 결과를 저장할 enum zbd_zoned_model 포인터.
 *   - ZBD_NONE: 일반 장치.
 *   - ZBD_HOST_AWARE: zone 을 이해하지만 비-zoned I/O 도 허용(SMR HA).
 *   - ZBD_HOST_MANAGED: 호스트가 zone 규약 강제(ZNS SSD, SMR HM).
 * @return: 성공 0, 블록 장치 아님 -EINVAL.
 *
 * sysfs queue/zoned 문자열을 파싱. 파일이 없으면 미지원 커널 → ZBD_NONE 유지.
 *
 * 호출 체인: zbd.c::zbd_get_zoned_model → [blkzoned_get_zoned_model()] → sysfs
 */
int blkzoned_get_zoned_model(struct thread_data *td, struct fio_file *f,
			     enum zbd_zoned_model *model)
{
	char *model_str = NULL;  /* [한국어] 속성 문자열 힙 사본 — 함수 끝에서 free. */

	if (f->filetype != FIO_TYPE_BLOCK)
		return -EINVAL;  /* [한국어] 정규 파일/캐릭터 장치에는 ZBD 의미 없음. */

	*model = ZBD_NONE;  /* [한국어] 기본값 — 실패/미지원 시 None 유지. */

	model_str = blkzoned_get_sysfs_attr(f->file_name, "queue/zoned");
	if (!model_str)
		return 0;  /* [한국어] 미지원 커널 — ZBD 없음으로 처리하고 성공 반환. */

	dprint(FD_ZBD, "%s: zbd model string: %s\n", f->file_name, model_str);  /* [한국어] 디버그 로그 — FD_ZBD 채널만 출력. */
	if (strcmp(model_str, "host-aware") == 0)
		*model = ZBD_HOST_AWARE;
	else if (strcmp(model_str, "host-managed") == 0)
		*model = ZBD_HOST_MANAGED;

	free(model_str);

	return 0;
}

/*
 * [한국어]
 * blkzoned_get_max_open_zones - 최대 동시 오픈 zone 수 조회
 *
 * @td: 스레드 컨텍스트.
 * @f: 대상 fio_file.
 * @max_open_zones: 결과(0 = 제한 없음).
 * @return: 성공 0, 블록 장치 아님 -EIO.
 *
 * ZNS 와 일부 SMR 드라이브는 동시에 오픈된 zone 수에 하드 제한을 둔다.
 * fio 의 zbd 엔진이 이를 넘지 않도록 스케줄링할 때 참조.
 * sysfs queue/max_open_zones → 10진 정수로 파싱.
 *
 * 호출 체인: zbd.c → [blkzoned_get_max_open_zones()]
 */
int blkzoned_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				unsigned int *max_open_zones)
{
	char *max_open_str;

	if (f->filetype != FIO_TYPE_BLOCK)
		return -EIO;

	max_open_str = blkzoned_get_sysfs_attr(f->file_name, "queue/max_open_zones");
	if (!max_open_str) {
		*max_open_zones = 0;  /* [한국어] 속성 부재 — 제한 없음으로 간주. */
		return 0;
	}

	dprint(FD_ZBD, "%s: max open zones supported by device: %s\n",
	       f->file_name, max_open_str);
	*max_open_zones = atoll(max_open_str);  /* [한국어] 10진 파싱. 음수/비숫자는 0 으로 절삭. */

	free(max_open_str);

	return 0;
}

/*
 * [한국어]
 * blkzoned_get_max_active_zones - 최대 활성 zone 수 조회
 *
 * @td: 스레드 컨텍스트.
 * @f: 대상 fio_file.
 * @max_active_zones: 결과(0 = 제한 없음).
 * @return: 성공 0, 블록 장치 아님 -EIO.
 *
 * 활성 zone = IMPLICIT_OPEN + EXPLICIT_OPEN + CLOSED 합계(FULL/EMPTY 제외).
 * ZNS 는 일반적으로 max_active_zones <= max_open_zones 관계.
 *
 * 호출 체인: zbd.c → [blkzoned_get_max_active_zones()]
 */
int blkzoned_get_max_active_zones(struct thread_data *td, struct fio_file *f,
				  unsigned int *max_active_zones)
{
	char *max_active_str;

	if (f->filetype != FIO_TYPE_BLOCK)
		return -EIO;

	max_active_str = blkzoned_get_sysfs_attr(f->file_name, "queue/max_active_zones");
	if (!max_active_str) {
		*max_active_zones = 0;
		return 0;
	}

	dprint(FD_ZBD, "%s: max active zones supported by device: %s\n",
	       f->file_name, max_active_str);
	*max_active_zones = atoll(max_active_str);

	free(max_active_str);

	return 0;
}

/*
 * [한국어]
 * zone_capacity - blk_zone 의 유효 쓰기 용량을 바이트 단위로 반환
 *
 * @hdr: blk_zone_report 헤더 — flags 의 BLK_ZONE_REP_CAPACITY 비트로
 *       capacity 필드 유효 여부 전달.
 * @blkz: 개별 zone.
 * @return: capacity(바이트).
 *
 * ZNS 에서는 capacity 가 len 보다 작을 수 있다(예: 1GiB len 중 768MiB 만
 * 사용 가능). 구형 커널/일반 SMR 는 capacity 필드가 없으므로 len 으로 폴백.
 * << 9 는 섹터(512B) → 바이트 환산(2^9 = 512).
 */
static uint64_t zone_capacity(struct blk_zone_report *hdr,
			      struct blk_zone *blkz)
{
	if (hdr->flags & BLK_ZONE_REP_CAPACITY)
		return blkz->capacity << 9;  /* [한국어] capacity 지원 — 섹터→바이트. */
	return blkz->len << 9;  /* [한국어] 미지원 — len 으로 근사. */
}

/*
 * [한국어]
 * blkzoned_report_zones - BLKREPORTZONE ioctl 로 [offset, ...] 범위 zone 정보 수집
 *
 * @td: 스레드 컨텍스트(에러 보고용).
 * @f: 대상 fio_file.
 * @offset: 리포트 시작 바이트 오프셋(zone 경계 정렬).
 * @zones: 결과 zbd_zone 배열(fio 포맷).
 * @nr_zones: 최대 조회 개수.
 * @return: 성공 시 실제 리포트된 zone 수, 실패 시 음수 errno.
 *
 * 동작 단계:
 * 1) O_RDONLY | O_LARGEFILE 로 장치 오픈(리포트에 쓰기 권한 불필요).
 * 2) calloc(blk_zone_report + nr_zones * blk_zone) — 가변 길이 배열 기반
 *    구조체(마지막 멤버 zones[0]) 패턴.
 * 3) hdr->nr_zones = nr_zones; hdr->sector = offset >> 9 (바이트→섹터).
 * 4) ioctl(BLKREPORTZONE, hdr) — 커널이 실제 리포트된 zone 수로 nr_zones
 *    덮어쓴다.
 * 5) 루프: 각 blk_zone 을 zbd_zone 으로 변환 —
 *    - start/wp/len << 9 (섹터→바이트).
 *    - capacity = zone_capacity(hdr, blkz).
 *    - type switch: CONVENTIONAL/SEQWRITE_REQ/SEQWRITE_PREF → ZBD_ZONE_TYPE_*.
 *    - cond switch: NOT_WP/EMPTY/IMP_OPEN/EXP_OPEN/CLOSED/FULL → fio enum.
 *      READONLY/OFFLINE/기타 → ZBD_ZONE_COND_OFFLINE 로 보수 처리, wp = start.
 *
 * 호출 체인: zbd.c::zbd_report_zones → [blkzoned_report_zones()] → ioctl
 */
int blkzoned_report_zones(struct thread_data *td, struct fio_file *f,
			  uint64_t offset, struct zbd_zone *zones,
			  unsigned int nr_zones)
{
	struct blk_zone_report *hdr = NULL;  /* [한국어] 가변 길이 리포트 헤더 + 배열. */
	struct blk_zone *blkz;  /* [한국어] 배열 순회 포인터. */
	struct zbd_zone *z;  /* [한국어] 결과 배열 순회 포인터. */
	unsigned int i;
	int fd = -1, ret;

	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);  /* [한국어] O_LARGEFILE: 2TB 초과 장치 대응(32비트 ABI). */
	if (fd < 0)
		return -errno;  /* [한국어] errno 음수 반환 — 호출자 관례. */

	hdr = calloc(1, sizeof(struct blk_zone_report) +
			nr_zones * sizeof(struct blk_zone));  /* [한국어] 가변 배열 구조체 + 0 초기화. */
	if (!hdr) {
		ret = -ENOMEM;
		goto out;
	}

	hdr->nr_zones = nr_zones;  /* [한국어] 최대 조회 개수. 커널이 실제 수로 덮어씀. */
	hdr->sector = offset >> 9;  /* [한국어] 바이트 → 섹터 변환. */
	ret = ioctl(fd, BLKREPORTZONE, hdr);  /* [한국어] BLKREPORTZONE = _IOWR(0x12, 130, blk_zone_report). */
	if (ret) {
		log_err("%s: BLKREPORTZONE ioctl failed, ret=%d, err=%d.\n",
			f->file_name, ret, -errno);
		ret = -errno;
		goto out;
	}

	nr_zones = hdr->nr_zones;  /* [한국어] 실제 리포트된 수로 갱신. */
	blkz = (void *) hdr + sizeof(*hdr);  /* [한국어] 헤더 직후 = zones[] 배열 시작. */
	z = &zones[0];
	for (i = 0; i < nr_zones; i++, z++, blkz++) {
		z->start = blkz->start << 9;  /* [한국어] 섹터→바이트. */
		z->wp = blkz->wp << 9;  /* [한국어] write pointer (현재 쓰기 가능 위치). */
		z->len = blkz->len << 9;  /* [한국어] zone 전체 크기. */
		z->capacity = zone_capacity(hdr, blkz);  /* [한국어] 유효 쓰기 용량(capacity <= len). */

		switch (blkz->type) {  /* [한국어] 커널 enum → fio enum 매핑. */
		case BLK_ZONE_TYPE_CONVENTIONAL:
			z->type = ZBD_ZONE_TYPE_CNV;  /* [한국어] 관습 영역 — 순차 제약 없음. */
			break;
		case BLK_ZONE_TYPE_SEQWRITE_REQ:
			z->type = ZBD_ZONE_TYPE_SWR;  /* [한국어] 순차 쓰기 필수 — HM 스타일. */
			break;
		case BLK_ZONE_TYPE_SEQWRITE_PREF:
			z->type = ZBD_ZONE_TYPE_SWP;  /* [한국어] 순차 권장 — HA 스타일. */
			break;
		default:
			td_verror(td, errno, "invalid zone type");  /* [한국어] 예상 밖 값 — fio 에러 기록. */
			log_err("%s: invalid type for zone at sector %llu.\n",
				f->file_name, (unsigned long long)offset >> 9);
			ret = -EIO;
			goto out;
		}

		switch (blkz->cond) {  /* [한국어] zone 상태 enum 매핑. */
		case BLK_ZONE_COND_NOT_WP:
			z->cond = ZBD_ZONE_COND_NOT_WP;  /* [한국어] CNV zone 기본 — wp 개념 없음. */
			break;
		case BLK_ZONE_COND_EMPTY:
			z->cond = ZBD_ZONE_COND_EMPTY;  /* [한국어] wp = start, 완전 비어있음. */
			break;
		case BLK_ZONE_COND_IMP_OPEN:
			z->cond = ZBD_ZONE_COND_IMP_OPEN;  /* [한국어] 쓰기로 암묵 오픈됨. */
			break;
		case BLK_ZONE_COND_EXP_OPEN:
			z->cond = ZBD_ZONE_COND_EXP_OPEN;  /* [한국어] 호스트가 명시 오픈(OPENZONE ioctl). */
			break;
		case BLK_ZONE_COND_CLOSED:
			z->cond = ZBD_ZONE_COND_CLOSED;  /* [한국어] 쓰기 보류 상태, 재개 가능. */
			break;
		case BLK_ZONE_COND_FULL:
			z->cond = ZBD_ZONE_COND_FULL;  /* [한국어] wp = start+capacity, 더 못 씀. */
			break;
		case BLK_ZONE_COND_READONLY:
		case BLK_ZONE_COND_OFFLINE:
		default:
			/* Treat all these conditions as offline (don't use!) */
			z->cond = ZBD_ZONE_COND_OFFLINE;  /* [한국어] 사용 불가 zone — 안전 매핑. */
			z->wp = z->start;  /* [한국어] wp 값을 zone 시작으로 클램프(방어적). */
		}
	}

	ret = nr_zones;  /* [한국어] 실제 리포트 수 반환. */
out:
	free(hdr);
	close(fd);

	return ret;
}

/*
 * [한국어]
 * blkzoned_reset_wp - BLKRESETZONE ioctl 로 [offset, offset+length) 구간의 zone(s) 쓰기 포인터 리셋
 *
 * @td: 스레드 컨텍스트.
 * @f: 대상 fio_file.
 * @offset: 리셋 시작 바이트 오프셋(zone 경계).
 * @length: 리셋 범위 바이트(zone 크기 배수).
 * @return: 성공 0, 실패 -errno.
 *
 * 리셋 = zone 내용 무효화 + wp = start + cond = EMPTY. HM zone 을 재사용
 * 하려면 반드시 먼저 리셋해야 쓰기가 가능. fd 가 이미 열려 있으면 그대로
 * 사용, 아니면 임시 O_RDWR 오픈(리셋은 쓰기 권한 필요).
 *
 * 호출 체인: zbd.c::zbd_reset_zone → [blkzoned_reset_wp()] → ioctl
 */
int blkzoned_reset_wp(struct thread_data *td, struct fio_file *f,
		      uint64_t offset, uint64_t length)
{
	struct blk_zone_range zr = {
		.sector         = offset >> 9,  /* [한국어] 바이트→섹터. */
		.nr_sectors     = length >> 9,
	};
	int fd, ret = 0;

	/* If the file is not yet opened, open it for this function. */
	fd = f->fd;
	if (fd < 0) {
		fd = open(f->file_name, O_RDWR | O_LARGEFILE);  /* [한국어] 리셋은 쓰기 권한 필요. */
		if (fd < 0)
			return -errno;
	}

	if (ioctl(fd, BLKRESETZONE, &zr) < 0)  /* [한국어] BLKRESETZONE = _IOW(0x12, 131, blk_zone_range). */
		ret = -errno;

	if (f->fd < 0)
		close(fd);  /* [한국어] 임시로 오픈했으면 닫음. */

	return ret;
}

/*
 * [한국어]
 * blkzoned_finish_zone - BLKFINISHZONE ioctl 로 구간의 zone(s) 을 FULL 로 전환
 *
 * @td: 스레드 컨텍스트.
 * @f: 대상 fio_file.
 * @offset/length: 완료할 범위.
 * @return: 성공 0, 실패 -errno. 단 ENOTTY(커널 미지원)는 0 으로 흡수.
 *
 * FINISH = zone 의 남은 capacity 를 쓰지 않고도 FULL 로 만듦 — 호스트가
 * 더 이상 해당 zone 에 쓰지 않을 것을 명시. 이 ioctl 은 Linux 5.5 에서
 * 도입되었으므로 구 커널은 ENOTTY 로 실패 → 조용히 무시(그런 커널은
 * 자동 FULL 전환에 의존).
 *
 * 호출 체인: zbd.c::zbd_close_zone / _finish_zone → [blkzoned_finish_zone()]
 */
int blkzoned_finish_zone(struct thread_data *td, struct fio_file *f,
			 uint64_t offset, uint64_t length)
{
	struct blk_zone_range zr = {
		.sector         = offset >> 9,
		.nr_sectors     = length >> 9,
	};
	int fd, ret = 0;

	/* If the file is not yet opened, open it for this function. */
	fd = f->fd;
	if (fd < 0) {
		fd = open(f->file_name, O_RDWR | O_LARGEFILE);
		if (fd < 0)
			return -errno;
	}

	if (ioctl(fd, BLKFINISHZONE, &zr) < 0) {
		ret = -errno;
		/*
		 * Kernel versions older than 5.5 do not support BLKFINISHZONE
		 * and return the ENOTTY error code. These old kernels only
		 * support block devices that close zones automatically.
		 */
		if (ret == ENOTTY)
			ret = 0;  /* [한국어] 구 커널 호환 — 자동 FULL 에 의존. */
	}

	if (f->fd < 0)
		close(fd);

	return ret;
}

/*
 * [한국어]
 * blkzoned_move_zone_wp - wp 를 length 만큼 전진(실제 데이터 또는 0 채움)
 *
 * @td: 스레드 컨텍스트.
 * @f: 대상 fio_file.
 * @z: 대상 zone 정보(z->wp 현재 위치 사용).
 * @length: 전진시킬 바이트.
 * @buf: 쓸 데이터(NULL 이면 fallocate 로 0 채움).
 * @return: 성공 0, 실패 -errno.
 *
 * 왜 필요한가: ZBD 테스트 시나리오에서 "zone 이 특정 wp 위치에서 시작하도록
 * 미리 채워두기"가 필요할 때 사용. buf==NULL 면 fallocate(FALLOC_FL_ZERO_RANGE)
 * 로 커널이 내부적으로 효율적인 0 기록 수행(장치 지원 시 DISCARD+재할당으로
 * 물리 기록 없이 가능). buf!=NULL 면 pwrite 로 z->wp 에 직접 기록.
 * O_DIRECT 사용 — page cache 우회로 실제 하드웨어 도달 보장.
 *
 * 호출 체인: zbd.c → [blkzoned_move_zone_wp()] → pwrite / fallocate
 */
int blkzoned_move_zone_wp(struct thread_data *td, struct fio_file *f,
			  struct zbd_zone *z, uint64_t length, const char *buf)
{
	int fd, ret = 0;

	/* If the file is not yet open, open it for this function */
	fd = f->fd;
	if (fd < 0) {
		fd = open(f->file_name, O_WRONLY | O_DIRECT);  /* [한국어] 쓰기 전용 + 캐시 우회. */
		if (fd < 0)
			return -errno;
	}

	/* If write data is not provided, fill zero to move the write pointer */
	if (!buf) {
		ret = fallocate(fd, FALLOC_FL_ZERO_RANGE, z->wp, length);  /* [한국어] 0 채움 — 장치 지원 시 효율적. */
		goto out;
	}

	if (pwrite(fd, buf, length, z->wp) < 0)  /* [한국어] z->wp 오프셋에 length 바이트 기록. */
		ret = -errno;

out:
	if (f->fd < 0)
		close(fd);

	return ret;
}
