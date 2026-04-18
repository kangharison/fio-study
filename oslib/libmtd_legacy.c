/*
 * Copyright (C) 2009 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Author: Artem Bityutskiy
 *
 * This file  is part of the MTD library. Implements pre-2.6.30 kernels support,
 * where MTD did not have sysfs interface. The main limitation of the old
 * kernels was that the sub-page size was not exported to user-space, so it was
 * not possible to get sub-page size.
 */

/* Imported from mtd-utils by dehrenberg */

/*
 * [한국어 설명] MTD 라이브러리 레거시(sysfs 미지원 커널) 구현 (libmtd_legacy.c)
 *
 * === 파일의 역할 ===
 * Linux 커널 2.6.30 이전(MTD 서브시스템의 sysfs 인터페이스 도입 이전) 환경을
 * 위한 MTD 장치 정보 수집 경로를 제공한다. 이 시기의 커널은
 * /sys/class/mtd/ 디렉토리를 만들지 않고, 사용자 공간은 전통적으로 두 가지
 * 메커니즘으로 장치 정보를 얻었다:
 *   1) /proc/mtd 텍스트 파일 — 시스템의 MTD 인스턴스 목록(번호, 크기,
 *      소거블록 크기, 이름)을 한 줄씩 나열.
 *   2) MEMGETINFO ioctl — 개별 장치의 상세 타입·플래그·OOB 크기 등.
 * 이 파일은 위 두 메커니즘을 결합해 libmtd.h 의 API 규약(mtd_info,
 * mtd_dev_info)을 구형 커널에서도 일관되게 채우는 함수군을 제공한다.
 * 주요 제약: 구 커널은 "subpage_size" (NAND 내부 partial-page 쓰기 단위)를
 * 사용자 공간에 노출하지 않으므로 subpage_size = min_io_size 로 폴백한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * libmtd.c::libmtd_open() 이 /sys/class/mtd 존재 여부와 mtdN/name 파일 유무
 * 를 검증해 sysfs_supported 플래그를 설정한다. 미지원이면 이 파일의 함수
 * 들로 라우팅된다:
 *   libmtd.c::mtd_dev_present() → legacy_dev_present()
 *   libmtd.c::mtd_get_info()    → legacy_mtd_get_info()
 *   libmtd.c::mtd_get_dev_info1()→ legacy_get_dev_info1() → _get_dev_info()
 * 전체 fio 호출 체인:
 *   engines/mtd.c (ioengine) → libmtd.h API → libmtd.c 분기 → [본 파일]
 *
 * === 타 모듈과의 연결 ===
 * - libmtd.c: 본 파일 함수들의 유일한 호출자(sysfs 미지원 분기).
 * - libmtd_int.h: struct libmtd 내부 상태(여기선 직접 사용 없음) + legacy_*
 *   함수 선언.
 * - libmtd_common.h: sys_errmsg/errmsg/normsg 에러 매크로, xmalloc.
 * - <mtd/mtd-user.h>: MEMGETINFO / MEMGETBADBLOCK ioctl 번호, struct
 *   mtd_info_user, MTD_ABSENT/MTD_NANDFLASH 등 타입 상수, MTD_WRITEABLE
 *   플래그.
 * - /proc/mtd 텍스트 파일(커널 procfs 제공) — 형식은 고정 포맷.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct proc_parse_info: /proc/mtd 의 한 엔트리를 파싱하는 이터레이터 상태.
 * - proc_parse_start/next(): /proc/mtd 의 헤더 검증 + 라인별 sscanf 파싱.
 * - legacy_libmtd_open(): /proc/mtd 접근 가능 여부로 MTD 존재 여부 검사.
 * - legacy_dev_present(): 특정 mtd_num 가 /proc/mtd 에 있는지 확인.
 * - legacy_mtd_get_info(): 장치 개수, 최소/최대 번호 산출.
 * - legacy_get_dev_info(): 장치 노드(/dev/mtdN)로부터 상세 정보 수집 —
 *   stat() + MEMGETINFO ioctl + /proc/mtd 이름 파싱.
 * - legacy_get_dev_info1(): mtd_num 으로 노드 경로를 구성해 위 함수 호출.
 */
#include <limits.h>  /* [한국어] INT_MAX — lowest_mtd_num 초기값으로 사용. */
#include <fcntl.h>  /* [한국어] open(2) 플래그 O_RDONLY 등. */
#include <unistd.h>  /* [한국어] read/close. */
#include <stdlib.h>  /* [한국어] free 선언. xmalloc 은 libmtd_common.h. */
#include <errno.h>  /* [한국어] errno 설정/검사. ENOENT/EINVAL/EOPNOTSUPP 구분. */
#include <sys/types.h>  /* [한국어] major/minor 매크로 조상 타입. */
#include <sys/stat.h>  /* [한국어] stat(2) + S_ISCHR 검사 — MTD 장치는 char device 여야 함. */
#include <sys/ioctl.h>  /* [한국어] ioctl(2) 선언. */
#include <mtd/mtd-user.h>  /* [한국어] MEMGETINFO/MEMGETBADBLOCK/MTD_* 상수 + struct mtd_info_user. */

#include "libmtd.h"  /* [한국어] 공개 API 선언 + struct mtd_info / mtd_dev_info. */
#include "libmtd_int.h"  /* [한국어] legacy_* 함수 선언. */
#include "libmtd_common.h"  /* [한국어] sys_errmsg / errmsg / normsg / xmalloc 매크로. */

/* [한국어] /proc/mtd: 구형 커널에서 MTD 장치 목록을 제공하는 proc 파일 */
#define MTD_PROC_FILE "/proc/mtd"
/* [한국어] MTD 장치 노드 패턴: /dev/mtd0, /dev/mtd1, ... — printf %d 로 번호 삽입. */
#define MTD_DEV_PATT  "/dev/mtd%d"
/* [한국어] MTD 문자 장치의 major 번호 (리눅스 커널에서 90 으로 고정 예약됨).
 * include/uapi/linux/mtd/mtd.h 에서 확인 가능. */
#define MTD_DEV_MAJOR 90

#define PROC_MTD_FIRST     "dev:    size   erasesize  name\n"  /* [한국어] /proc/mtd 헤더 — 정확 일치 검증용. */
#define PROC_MTD_FIRST_LEN (sizeof(PROC_MTD_FIRST) - 1)  /* [한국어] NUL 제외 길이 — memcmp 에 사용. */
#define PROC_MTD_MAX_LEN   4096  /* [한국어] /proc/mtd 전체 버퍼 상한 — MTD 장치 수십 개까지 여유. */
#define PROC_MTD_PATT      "mtd%d: %llx %x"  /* [한국어] 엔트리 포맷 — mtd_num / size(16진 long long) / eb_size(16진). */

/*
 * [한국어] /proc/mtd 파싱용 상태 구조체
 * /proc/mtd 파일은 "dev:    size   erasesize  name\n" 헤더 후
 * "mtdN: XXXXXXXX XXXX "이름"" 형태의 줄을 포함한다.
 * 이 구조체로 파일 내용을 버퍼에 읽고 순차적으로 파싱한다.
 */
/**
 * struct proc_parse_info - /proc/mtd parsing information.
 * @mtd_num: MTD device number
 * @size: device size
 * @eb_size: eraseblock size
 * @name: device name
 * @buf: contents of /proc/mtd
 * @data_size: how much data was read into @buf
 * @pos: next string in @buf to parse
 */
struct proc_parse_info
{
	int mtd_num;
	/* [한국어] 현재 파싱 중인 MTD 장치 번호(/proc/mtd 의 "mtdN:" 의 N).
	 * 설정자: proc_parse_next 의 sscanf.
	 * 읽는 자: 호출자(legacy_dev_present/legacy_mtd_get_info/legacy_get_dev_info).
	 * 값 범위: 커널이 등록한 MTD 인스턴스 번호(0 이상).
	 * 동기화: 스택 지역 구조체 — 스레드/재진입 안전. */

	long long size;
	/* [한국어] 장치 총 크기(바이트). /proc/mtd 는 16진 long long 표기.
	 * 설정자: sscanf("%llx"). 읽는 자: 호출자. 값 범위: 양수. */

	char name[MTD_NAME_MAX + 1];
	/* [한국어] 큰따옴표 안에서 추출한 MTD 장치 이름. NUL-종단 보장.
	 * 설정자: proc_parse_next 의 memcpy. 읽는 자: legacy_get_dev_info 가
	 * mtd->name 에 복사. 값 범위: 문자열. 동기화: 지역. */

	int eb_size;
	/* [한국어] 소거블록(eraseblock) 크기 바이트.
	 * 설정자: sscanf("%x"). 값 범위: NAND 는 보통 16KiB~2MiB, NOR 는 다양. */

	char *buf;
	/* [한국어] /proc/mtd 전체 내용을 담은 힙 버퍼(xmalloc).
	 * 설정자: proc_parse_start. 읽는 자: proc_parse_next. 해제: next 가
	 * EOF 도달 시 free. 값 범위: PROC_MTD_MAX_LEN(4096) 까지. */

	int data_size;
	/* [한국어] buf 에 실제로 읽어온 바이트 수 — read 리턴값.
	 * 설정자: proc_parse_start. 읽는 자: proc_parse_next 범위 검사. */

	char *next;
	/* [한국어] buf 내에서 다음으로 파싱할 라인의 시작 포인터.
	 * 설정자: 초기 proc_parse_start(헤더 다음) + next 가 라인 끝 후 전진.
	 * 읽는 자: proc_parse_next 시작점. */
};

/*
 * [한국어]
 * proc_parse_start - /proc/mtd 전체를 메모리에 로드하고 헤더 검증
 *
 * @pi: 파싱 상태 구조체(out). buf/data_size/next 가 초기화된다.
 * @return: 성공 0, 실패 -1.
 *
 * 왜 한 번에 전부 읽는가: /proc/mtd 는 파일 크기가 작고(수십~수백 바이트),
 * 스트림 읽기 중 다른 프로세스가 장치를 추가/제거하면 엔트리 일관성이
 * 깨질 수 있다. 한 번에 스냅샷해 일관성 확보.
 *
 * 동작 단계:
 * 1) open("/proc/mtd", O_RDONLY).
 * 2) xmalloc(PROC_MTD_MAX_LEN) — 전체 버퍼 확보.
 * 3) read 로 최대 4096 바이트 로드.
 * 4) 헤더 바이트 시퀀스 PROC_MTD_FIRST 와 memcmp — 커널이 포맷을 바꾸었는지
 *    방어(이 파서는 고정 포맷 전제).
 * 5) pi->next 를 헤더 다음 바이트로 설정.
 *
 * 호출 체인:
 *   legacy_* → [proc_parse_start()] → open/read/close
 */
static int proc_parse_start(struct proc_parse_info *pi)
{
	int fd, ret;  /* [한국어] fd: /proc/mtd 파일 디스크립터, ret: read 반환값. */

	fd = open(MTD_PROC_FILE, O_RDONLY);  /* [한국어] 읽기 전용 오픈. 실패 시 MTD 서브시스템 부재 또는 권한 문제. */
	if (fd == -1)
		return -1;

	pi->buf = xmalloc(PROC_MTD_MAX_LEN);  /* [한국어] 4096B 버퍼 할당 — 실패 시 xmalloc 이 abort. */

	ret = read(fd, pi->buf, PROC_MTD_MAX_LEN);  /* [한국어] 한 번 read 로 전체 읽기 시도 — /proc 파일은 보통 한 번에 처리. */
	if (ret == -1) {
		sys_errmsg("cannot read \"%s\"", MTD_PROC_FILE);
		goto out_free;
	}

	if (ret < PROC_MTD_FIRST_LEN ||
	    memcmp(pi->buf, PROC_MTD_FIRST, PROC_MTD_FIRST_LEN)) {  /* [한국어] 헤더 바이트 단위 일치 검증. */
		errmsg("\"%s\" does not start with \"%s\"", MTD_PROC_FILE,
		       PROC_MTD_FIRST);
		goto out_free;
	}

	pi->data_size = ret;  /* [한국어] 실제 버퍼 내 유효 바이트. */
	pi->next = pi->buf + PROC_MTD_FIRST_LEN;  /* [한국어] 첫 엔트리 라인 시작점 — 헤더 직후. */

	close(fd);
	return 0;

out_free:
	free(pi->buf);  /* [한국어] 실패 시 버퍼 반환. */
	close(fd);
	return -1;
}

/*
 * [한국어]
 * proc_parse_next - /proc/mtd 에서 다음 엔트리 한 줄 파싱(이터레이터)
 *
 * @pi: 파싱 상태. 성공 시 mtd_num/size/eb_size/name 가 채워지고 next 가
 *      다음 줄로 전진.
 * @return: 성공 1(엔트리 하나 파싱됨), EOF 0(버퍼 해제됨), 에러 음수.
 *
 * 라인 포맷: `mtdN: XXXXXXXX YYYY "NAME"\n`
 *  - sscanf 로 앞부분 3필드 추출.
 *  - " (0x22) 두 개 사이를 name 으로 memcpy.
 *  - 마지막 \n 확인으로 라인 정합성 검증.
 *
 * 에러 케이스: sscanf 실패(포맷 깨짐), 따옴표 누락, 이름 길이 초과, 개행
 * 누락 — 모두 errmsg 로 로깅.
 *
 * 호출 체인:
 *   legacy_* → [proc_parse_next()] ← loop
 */
static int proc_parse_next(struct proc_parse_info *pi)
{
	int ret, len, pos = pi->next - pi->buf;  /* [한국어] pos: 현 시점의 버퍼 내 오프셋. */
	char *p, *p1;  /* [한국어] p: 여는 따옴표 위치, p1: 닫는 따옴표 위치. */

	if (pos >= pi->data_size) {  /* [한국어] 버퍼 소진 — 이터레이션 종료. */
		free(pi->buf);  /* [한국어] 버퍼 해제 — start/next 쌍의 자원 관리. */
		return 0;
	}

	ret = sscanf(pi->next, PROC_MTD_PATT, &pi->mtd_num, &pi->size,
		     &pi->eb_size);  /* [한국어] "mtd%d: %llx %x" — 세 필드 추출. */
	if (ret != 3)
		return errmsg("\"%s\" pattern not found", PROC_MTD_PATT);  /* [한국어] 부분 매치도 실패로 간주. */

	p = memchr(pi->next, '\"', pi->data_size - pos);  /* [한국어] 여는 따옴표 탐색. */
	if (!p)
		return errmsg("opening \" not found");
	p += 1;  /* [한국어] 따옴표 다음 문자 — 이름 시작. */
	pos = p - pi->buf;
	if (pos >= pi->data_size)
		return errmsg("opening \" not found");

	p1 = memchr(p, '\"', pi->data_size - pos);  /* [한국어] 닫는 따옴표 탐색. */
	if (!p1)
		return errmsg("closing \" not found");
	pos = p1 - pi->buf;
	if (pos >= pi->data_size)
		return errmsg("closing \" not found");

	len = p1 - p;  /* [한국어] 이름 길이. */
	if (len > MTD_NAME_MAX)
		return errmsg("too long mtd%d device name", pi->mtd_num);

	memcpy(pi->name, p, len);  /* [한국어] 이름 복사. */
	pi->name[len] = '\0';  /* [한국어] NUL 종단 — sscanf 등에서 안전 사용 위해. */

	if (p1[1] != '\n')  /* [한국어] 닫는 따옴표 바로 뒤는 '\n' 이어야 정상 라인. */
		return errmsg("opening \"\n\" not found");
	pi->next = p1 + 2;  /* [한국어] 다음 라인 시작 = 따옴표 + '\n' 건너뜀. */
	return 1;
}

/**
 * legacy_libmtd_open - legacy version of 'libmtd_open()'.
 *
 * This function is just checks that MTD is present in the system. Returns
 * zero in case of success and %-1 in case of failure. In case of failure,
 * errno contains zero if MTD is not present in the system, or contains the
 * error code if a real error happened. This is similar to the 'libmtd_open()'
 * return conventions.
 */
/*
 * [한국어]
 * legacy_libmtd_open - MTD 서브시스템 존재 여부 검사(레거시)
 *
 * @return: 존재 0, 부재 또는 에러 -1. errno 규약: 부재는 errno=0, 진짜 에러는
 *          system errno 값.
 *
 * /proc/mtd 를 단순 open/close 로 프로빙하여 MTD 커널 모듈이 로드되어 있는지
 * 확인. libmtd.c::libmtd_open 의 sysfs 분기 실패 시 호출.
 *
 * 호출 체인: libmtd.c → [legacy_libmtd_open()] → open/close
 */
int legacy_libmtd_open(void)
{
	int fd;

	fd = open(MTD_PROC_FILE, O_RDONLY);  /* [한국어] 존재/권한 확인 — 읽을 필요는 없으므로 실패만 확인. */
	if (fd == -1) {
		if (errno == ENOENT)
			errno = 0;  /* [한국어] 파일 없음 = MTD 서브시스템 미탑재 — errno=0 으로 "정상 부재" 표시. */
		return -1;
	}

	close(fd);
	return 0;
}

/**
 * legacy_dev_presentl - legacy version of 'mtd_dev_present()'.
 * @info: the MTD device information is returned here
 *
 * When the kernel does not provide sysfs files for the MTD subsystem,
 * fall-back to parsing the /proc/mtd file to determine whether an mtd device
 * number @mtd_num is present.
 */
/*
 * [한국어]
 * legacy_dev_present - 특정 mtd_num 장치 존재 여부(레거시)
 *
 * @mtd_num: 검사할 장치 번호.
 * @return: 존재 1, 부재 0(또는 파일 오류 시 -1).
 *
 * /proc/mtd 를 파싱하며 mtd_num 일치 엔트리를 찾으면 1. 엔트리 전체 순회 후
 * 부재 확정 시 0. libmtd.c::mtd_dev_present 의 sysfs 미지원 분기가 호출.
 *
 * 호출 체인: libmtd.c → [legacy_dev_present()] → proc_parse_start/next
 */
int legacy_dev_present(int mtd_num)
{
	int ret;
	struct proc_parse_info pi;

	ret = proc_parse_start(&pi);  /* [한국어] /proc/mtd 로드 + 헤더 검증. */
	if (ret)
		return -1;

	while (proc_parse_next(&pi)) {  /* [한국어] 엔트리 순회. EOF=0, 에러=음수 → 루프 탈출(에러 전파는 호출자 판단). */
		if (pi.mtd_num == mtd_num)
			return 1;  /* [한국어] 일치 — buf 는 해제되지 않지만 프로세스 종료 시 회수됨(짧게 존재). */
	}

	return 0;
}

/**
 * legacy_mtd_get_info - legacy version of 'mtd_get_info()'.
 * @info: the MTD device information is returned here
 *
 * This function is similar to 'mtd_get_info()' and has the same conventions.
 */
/*
 * [한국어]
 * legacy_mtd_get_info - 시스템 MTD 장치 수/번호 범위 요약(레거시)
 *
 * @info: 결과 구조체(mtd_dev_cnt / lowest_mtd_num / highest_mtd_num 채움).
 * @return: 성공 0, 실패 -1.
 *
 * /proc/mtd 전체 순회로 장치 총 개수, 최소/최대 번호를 산출.
 * libmtd.c::mtd_get_info 의 sysfs 미지원 경로.
 *
 * 호출 체인: libmtd.c → [legacy_mtd_get_info()] → proc_parse_start/next
 */
int legacy_mtd_get_info(struct mtd_info *info)
{
	int ret;
	struct proc_parse_info pi;

	ret = proc_parse_start(&pi);
	if (ret)
		return -1;

	info->lowest_mtd_num = INT_MAX;  /* [한국어] min 계산 초기값 — 어떤 값이 와도 감소하도록. */
	while (proc_parse_next(&pi)) {
		info->mtd_dev_cnt += 1;  /* [한국어] 장치 개수 증가. */
		if (pi.mtd_num > info->highest_mtd_num)
			info->highest_mtd_num = pi.mtd_num;  /* [한국어] 상한 갱신. */
		if (pi.mtd_num < info->lowest_mtd_num)
			info->lowest_mtd_num = pi.mtd_num;  /* [한국어] 하한 갱신. */
	}

	return 0;
}

/**
 * legacy_get_dev_info - legacy version of 'mtd_get_dev_info()'.
 * @node: name of the MTD device node
 * @mtd: the MTD device information is returned here
 *
 * This function is similar to 'mtd_get_dev_info()' and has the same
 * conventions.
 */
/*
 * [한국어]
 * legacy_get_dev_info - 장치 노드(/dev/mtdN)로부터 상세 정보 수집(레거시)
 *
 * @node: 장치 노드 경로(예: "/dev/mtd0").
 * @mtd: 결과 mtd_dev_info.
 * @return: 성공 0, 실패 -1.
 *
 * 동작 단계:
 * 1) stat(node) 로 major/minor 취득 + char device 여부 검증.
 * 2) major == MTD_DEV_MAJOR(90) 확인 — 다른 디바이스 드라이버 노드를
 *    MTD 로 오인하지 않도록.
 * 3) mtd_num = minor / 2 (구 MTD 규약: 짝수 minor=raw, 홀수=ro).
 * 4) open + MEMGETINFO ioctl — type, size, erasesize, writesize, oobsize,
 *    flags 수집.
 * 5) MEMGETBADBLOCK ioctl(offs=0) — 배드블록 관리 가능 여부 탐지.
 *    EOPNOTSUPP 면 bb_allowed=0.
 * 6) 각 필드 sanity check(양수, 정렬 등).
 * 7) type → type_str 문자열 매핑.
 * 8) subpage_size = min_io_size (구 커널 제약).
 * 9) /proc/mtd 파싱으로 장치 이름 취득(ioctl 로는 이름 제공 안 됨).
 *
 * 호출 체인: libmtd.c → [legacy_get_dev_info()] → stat/ioctl/proc_parse_*
 */
int legacy_get_dev_info(const char *node, struct mtd_dev_info *mtd)
{
	struct stat st;  /* [한국어] st_mode 검증 + st_rdev 로 major/minor 추출. */
	struct mtd_info_user ui;  /* [한국어] MEMGETINFO ioctl 결과 구조체(UAPI). */
	int fd, ret;
	loff_t offs = 0;  /* [한국어] MEMGETBADBLOCK 의 질의 오프셋 — 0 으로 프로빙. */
	struct proc_parse_info pi;

	if (stat(node, &st)) {  /* [한국어] 장치 노드 존재 확인. */
		sys_errmsg("cannot open \"%s\"", node);
		if (errno == ENOENT)
			normsg("MTD subsystem is old and does not support "
			       "sysfs, so MTD character device nodes have "
			       "to exist");
	}

	if (!S_ISCHR(st.st_mode)) {  /* [한국어] MTD 는 char device. block/regular file 거부. */
		errno = EINVAL;
		return errmsg("\"%s\" is not a character device", node);
	}

	memset(mtd, '\0', sizeof(struct mtd_dev_info));  /* [한국어] 출력 버퍼 0 클리어 — 미설정 필드 정의 상태. */
	mtd->major = major(st.st_rdev);
	mtd->minor = minor(st.st_rdev);

	if (mtd->major != MTD_DEV_MAJOR) {  /* [한국어] major 90 확인 — 다른 드라이버로부터 오인 방지. */
		errno = EINVAL;
		return errmsg("\"%s\" has major number %d, MTD devices have "
			      "major %d", node, mtd->major, MTD_DEV_MAJOR);
	}

	mtd->mtd_num = mtd->minor / 2;  /* [한국어] 짝수 minor = raw mtdN, 홀수 = readonly. mtd_num = minor>>1. */

	fd = open(node, O_RDONLY);  /* [한국어] ioctl 용 fd 오픈. 읽기 전용이어도 MEMGETINFO/MEMGETBADBLOCK 가능. */
	if (fd == -1)
		return sys_errmsg("cannot open \"%s\"", node);

	if (ioctl(fd, MEMGETINFO, &ui)) {  /* [한국어] ioctl MEMGETINFO: 구 UAPI — type/size/erasesize/writesize/oobsize/flags. */
		sys_errmsg("MEMGETINFO ioctl request failed");
		goto out_close;
	}

	ret = ioctl(fd, MEMGETBADBLOCK, &offs);  /* [한국어] 배드블록 관리 가능 프로빙 — offs=0 지점 질의. */
	if (ret == -1) {
		if (errno != EOPNOTSUPP) {
			sys_errmsg("MEMGETBADBLOCK ioctl failed");
			goto out_close;
		}
		errno = 0;  /* [한국어] 미지원 = 정상 경우. */
		mtd->bb_allowed = 0;  /* [한국어] 배드블록 관리 API 사용 불가. */
	} else
		mtd->bb_allowed = 1;

	mtd->type = ui.type;  /* [한국어] MTD_NANDFLASH/MTD_NORFLASH 등 enum. */
	mtd->size = ui.size;
	mtd->eb_size = ui.erasesize;
	mtd->min_io_size = ui.writesize;
	mtd->oob_size = ui.oobsize;

	if (mtd->min_io_size <= 0) {  /* [한국어] writesize 0 은 비정상 — 커널 버그 의심. */
		errmsg("mtd%d (%s) has insane min. I/O unit size %d",
		       mtd->mtd_num, node, mtd->min_io_size);
		goto out_close;
	}
	if (mtd->eb_size <= 0 || mtd->eb_size < mtd->min_io_size) {  /* [한국어] eraseblock >= page 관계. */
		errmsg("mtd%d (%s) has insane eraseblock size %d",
		       mtd->mtd_num, node, mtd->eb_size);
		goto out_close;
	}
	if (mtd->size <= 0 || mtd->size < mtd->eb_size) {  /* [한국어] 장치 크기 > 블록 하나 이상. */
		errmsg("mtd%d (%s) has insane size %lld",
		       mtd->mtd_num, node, mtd->size);
		goto out_close;
	}
	mtd->eb_cnt = mtd->size / mtd->eb_size;  /* [한국어] 총 소거블록 개수. */

	switch(mtd->type) {  /* [한국어] enum → 사람이 읽을 문자열 매핑 — 호출자 로그 출력용. */
	case MTD_ABSENT:
		errmsg("mtd%d (%s) is removable and is not present",
		       mtd->mtd_num, node);
		goto out_close;
	case MTD_RAM:
		strcpy((char *)mtd->type_str, "ram");
		break;
	case MTD_ROM:
		strcpy((char *)mtd->type_str, "rom");
		break;
	case MTD_NORFLASH:
		strcpy((char *)mtd->type_str, "nor");
		break;
	case MTD_NANDFLASH:
		strcpy((char *)mtd->type_str, "nand");
		break;
	case MTD_MLCNANDFLASH:
		strcpy((char *)mtd->type_str, "mlc-nand");
		break;
	case MTD_DATAFLASH:
		strcpy((char *)mtd->type_str, "dataflash");
		break;
	case MTD_UBIVOLUME:
		strcpy((char *)mtd->type_str, "ubi");
		break;
	default:
		goto out_close;  /* [한국어] 알 수 없는 타입 — 안전하게 실패. */
	}

	if (ui.flags & MTD_WRITEABLE)
		mtd->writable = 1;  /* [한국어] 쓰기 가능 플래그 — ROM 이나 RO 파티션은 0. */
	mtd->subpage_size = mtd->min_io_size;  /* [한국어] 구 커널은 subpage 노출 불가 — page 크기로 폴백. */

	close(fd);

	/*
	 * Unfortunately, the device name is not available via ioctl, and
	 * we have to parse /proc/mtd to get it.
	 */
	ret = proc_parse_start(&pi);  /* [한국어] 이름은 ioctl 로 얻을 수 없어 /proc/mtd 재파싱. */
	if (ret)
		return -1;

	while (proc_parse_next(&pi)) {
		if (pi.mtd_num == mtd->mtd_num) {
			strcpy((char *)mtd->name, pi.name);  /* [한국어] 일치 엔트리에서 이름 복사. */
			return 0;
		}
	}

	errmsg("mtd%d not found in \"%s\"", mtd->mtd_num, MTD_PROC_FILE);
	errno = ENOENT;
	return -1;

out_close:
	close(fd);
	return -1;
}

/**
 * legacy_get_dev_info1 - legacy version of 'mtd_get_dev_info1()'.
 * @node: name of the MTD device node
 * @mtd: the MTD device information is returned here
 *
 * This function is similar to 'mtd_get_dev_info1()' and has the same
 * conventions.
 */
/*
 * [한국어]
 * legacy_get_dev_info1 - mtd_num 으로 노드 경로를 구성해 상세 정보 수집
 *
 * @mtd_num: MTD 장치 번호.
 * @mtd: 결과 구조체.
 * @return: legacy_get_dev_info 반환값 그대로.
 *
 * MTD_DEV_PATT("/dev/mtd%d") 로 경로 포맷 후 legacy_get_dev_info 위임.
 */
int legacy_get_dev_info1(int mtd_num, struct mtd_dev_info *mtd)
{
	char node[sizeof(MTD_DEV_PATT) + 20];  /* [한국어] "/dev/mtd" + 숫자 여유 공간. */

	sprintf(node, MTD_DEV_PATT, mtd_num);  /* [한국어] "/dev/mtdN" 조립. */
	return legacy_get_dev_info(node, mtd);
}
