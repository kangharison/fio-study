/*
 * [한국어 설명] 장치 마운트 상태 확인 (mountcheck.c)
 *
 * === 파일의 역할 ===
 * 주어진 장치 경로(예: "/dev/sda1")가 현재 OS 에 의해 어떤 파일시스템으로 마운트되어
 * 있는지 여부를 boolean 으로 반환한다. fio 는 블록 디바이스를 원시(raw) 경로로 직접
 * 쓰기(O_DIRECT WRITE) 하는 엔진(sync/libaio/io_uring 등) 을 지원하는데, 그 디바이스가
 * 이미 ext4/xfs/btrfs 등으로 마운트되어 사용 중이면 벤치마크 write 가 그 위에 얹힌
 * 파일시스템 메타데이터를 파괴하여 복구 불가능한 손상을 낸다. 본 함수는 fio 초기화
 * 단계에서 안전 검사로 호출되어 "실수로 라이브 FS 를 벤치마크하려는" 사용자 경로를
 * 경고/거부할 수 있도록 한다.
 *
 * 여러 UNIX 계열의 이식성을 위해 빌드 시점(configure)에 선택된 API 로 분기한다:
 *   CONFIG_GETMNTENT         — Linux/glibc 계열(setmntent/getmntent/endmntent, /etc/mtab).
 *   CONFIG_GETMNTINFO        — FreeBSD/macOS 계열(getmntinfo + statfs).
 *   CONFIG_GETMNTINFO_STATVFS — NetBSD 계열(getmntinfo + statvfs).
 *   (없음) — Windows 등, 안전하게 "마운트 아님(0)" 으로 보수 반환.
 *
 * === 전체 아키텍처에서의 위치 ===
 * init.c 의 파일/디바이스 오픈 전에 호출되는 안전 가드. 각 잡의 설정에서 filename
 * 으로 블록 디바이스 경로가 지정되고 쓰기 I/O 가 예정된 경우에만 실행된다.
 * 호출 체인: init.c(setup_files/check_mount_writes 등) → device_is_mounted → 플랫폼 API.
 *
 * === 타 모듈과의 연결 ===
 * - mountcheck.h:  프로토타입(int device_is_mounted(const char *dev)) 선언.
 * - init.c:         유일한 직접 호출자(벤치마크 시작 전 검증).
 * - <mntent.h>/<sys/mount.h>/<sys/statvfs.h>: 플랫폼별 마운트 조회 API.
 * - configure/config-host.h: CONFIG_GETMNTENT/_GETMNTINFO/_STATVFS 매크로 정의.
 *
 * === 주요 함수/구조체 요약 ===
 * - device_is_mounted(dev): 플랫폼별 4분기 구현.
 *   반환: 1=해당 경로가 어떤 파일시스템에 의해 마운트되어 있음(벤치마크 위험);
 *        0=마운트되지 않음 또는 조회 실패(조회 실패도 "안전한 기본값"으로 0 반환).
 * 자체 구조체 없음 — <mntent.h>::struct mntent 또는 <sys/mount.h>::struct statfs 사용.
 */
#include <stdio.h>              /* [한국어] FILE* 타입 — setmntent 가 사실상 fopen 래퍼라 스트림 핸들을 반환 */
#include <string.h>             /* [한국어] strcmp 로 장치 경로 문자열 비교 */

#ifdef CONFIG_GETMNTENT
#include <mntent.h>             /* [한국어] Linux/glibc — struct mntent{mnt_fsname,mnt_dir,...}, setmntent/getmntent/endmntent */

#include "mountcheck.h"          /* [한국어] 본 파일의 공개 API 프로토타입 — CONFIG 분기 안에 들어와 있으나 어느 브랜치든 동일 선언 */

/* [한국어] Linux 의 전통적인 마운트 테이블 경로.
 * 최신 Linux 에서는 /etc/mtab 이 /proc/self/mounts 의 심볼릭 링크인 경우가 많다.
 * /proc/mounts 는 커널 내부 마운트 네임스페이스를 실시간 반영하므로 "user namespace 안에서도" 정확 */
#define MTAB	"/etc/mtab"

/*
 * [한국어]
 * device_is_mounted - 주어진 블록/캐릭터 디바이스 경로가 마운트되어 있는지 확인(Linux 경로).
 *
 * @dev: 장치 경로 문자열(예: "/dev/sda1", "/dev/nvme0n1p1"). NULL 금지.
 * @return: 1=일치하는 mnt_fsname 엔트리 발견(마운트됨), 0=미발견 또는 /etc/mtab 접근 실패.
 *
 * 동작 단계:
 *   1) setmntent(MTAB, "r") 로 마운트 테이블을 읽기 전용 스트림으로 연다.
 *   2) getmntent() 를 반복 호출하며 각 엔트리의 mnt_fsname(디바이스) 과 dev 비교.
 *      mnt_fsname 이 NULL 일 수 있으므로(일부 가상 FS) NULL 체크 후 비교.
 *   3) 일치 발견 시 ret=1 로 설정 후 루프 탈출.
 *   4) endmntent() 로 스트림 닫고 ret 반환.
 *
 * 실행 컨텍스트: fio 초기화 단계(메인 스레드). 파일 I/O 만 수행 — 재진입 안전하지 않음
 *               (setmntent 내부에 static 상태가 있을 수 있으므로 동시 호출 지양).
 *
 * 호출 체인: init.c → [device_is_mounted] → setmntent/getmntent/endmntent(3) → open("/etc/mtab") 등.
 *
 * 에러 처리: /etc/mtab 접근 실패(권한 부재 등) → 0 반환. 이 경우 fio 는 "마운트 아님" 으로
 *           판단하지만, 반환 0 이 "확실히 안전" 을 의미하지는 않음 — 진정한 안전 검사는
 *           더 상위의 정책(예: --allow_mounted_write) 에 의존.
 */
int device_is_mounted(const char *dev)
{
	/* [한국어] /etc/mtab 을 가리키는 스트림 — setmntent 가 반환 */
	FILE *mtab;
	/* [한국어] getmntent 가 돌려주는 엔트리 포인터(내부 static 버퍼) */
	struct mntent *mnt;
	/* [한국어] 결과 — 기본 0(미마운트/실패) */
	int ret = 0;

	/* [한국어] 마운트 테이블 열기. 실패 시(파일 부재/권한 부재) 즉시 0 반환 */
	mtab = setmntent(MTAB, "r");
	if (!mtab)
		return 0;

	/* [한국어] 엔트리를 하나씩 순회. NULL 반환 시 순회 종료 */
	while ((mnt = getmntent(mtab)) != NULL) {
		/* [한국어] mnt_fsname 이 NULL 인 비정상 엔트리는 안전하게 건너뜀 */
		if (!mnt->mnt_fsname)
			continue;
		/* [한국어] 정확 일치 비교 — 심볼릭 링크/UUID 별칭까지는 검출하지 못함(단순 경로 비교).
		 * 호출자는 필요 시 realpath() 사전 처리로 보강해야 함 */
		if (!strcmp(mnt->mnt_fsname, dev)) {
			/* [한국어] 마운트 발견 — 결과 설정 후 조기 종료 */
			ret = 1;
			break;
		}
	}

	/* [한국어] 스트림 닫기(내부 static 버퍼 자원 회수) */
	endmntent(mtab);
	return ret;
}

#elif defined(CONFIG_GETMNTINFO)
/* for most BSDs */
#include <sys/param.h>          /* [한국어] MNT_NOWAIT 등 마운트 플래그 정의 */
#include <sys/mount.h>          /* [한국어] struct statfs 및 getmntinfo(3) 프로토타입 */

/* [한국어]
 * device_is_mounted - FreeBSD/macOS 구현.
 * getmntinfo 는 내부 정적 배열 포인터를 돌려주며(동시 호출 비안전),
 * 반환값은 엔트리 개수 또는 0 이하(에러).
 *
 * @dev: 장치 경로.
 * @return: 1=마운트됨, 0=미마운트 또는 조회 실패.
 *
 * 실행 컨텍스트: fio 초기화.
 * 호출 체인: init.c → [device_is_mounted] → getmntinfo(3) → 커널.
 */
int device_is_mounted(const char *dev)
{
	/* [한국어] getmntinfo 가 돌려주는 엔트리 배열 포인터 */
	struct statfs *st;
	/* [한국어] i: 순회 인덱스, ret: 엔트리 개수 또는 에러 */
	int i, ret;

	/* [한국어] MNT_NOWAIT — 커널 캐시 사용, I/O 동기화 대기 안 함(빠름) */
	ret = getmntinfo(&st, MNT_NOWAIT);
	/* [한국어] 0 이하는 에러 또는 엔트리 없음 → 보수적으로 미마운트 반환 */
	if (ret <= 0)
		return 0;

	/* [한국어] 엔트리들을 순회하며 f_mntfromname(마운트 원본 디바이스 경로) 비교 */
	for (i = 0; i < ret; i++) {
		if (!strcmp(st[i].f_mntfromname, dev))
			return 1;        /* [한국어] 일치 발견 — 즉시 1 반환 */
	}

	/* [한국어] 전체 순회 완료 후 일치 없음 */
	return 0;
}

#elif defined(CONFIG_GETMNTINFO_STATVFS)
/* for NetBSD */
#include <sys/statvfs.h>        /* [한국어] NetBSD 의 struct statvfs 및 getmntinfo(3) */

/* [한국어]
 * device_is_mounted - NetBSD 구현(getmntinfo + statvfs 시그니처).
 * BSD 구현과 구조는 같으나 반환 타입이 struct statvfs 로 다름.
 * @return: 1=마운트됨, 0=미마운트/실패.
 */
int device_is_mounted(const char *dev)
{
	/* [한국어] 엔트리 배열 포인터(NetBSD 타입) */
	struct statvfs *st;
	/* [한국어] i: 인덱스, ret: 엔트리 개수 또는 에러 */
	int i, ret;

	/* [한국어] 빠른 조회(NOWAIT) */
	ret = getmntinfo(&st, MNT_NOWAIT);
	if (ret <= 0)
		return 0;   /* [한국어] 에러/엔트리 없음 → 미마운트로 판단 */

	for (i = 0; i < ret; i++) {
		/* [한국어] NetBSD statvfs 의 f_mntfromname 도 BSD 와 동일 의미 */
		if (!strcmp(st[i].f_mntfromname, dev))
			return 1;
	}

	return 0;
}

#else
/* others */
/* [한국어] 위 세 API 모두 미지원(Windows, 특수 임베디드 등) — 안전한 기본값으로 0 반환.
 * 이 경우 fio 는 마운트 검사를 사실상 생략하며, 상위 정책에 의존해 사용자가 명시적으로
 * 디바이스 쓰기 여부를 결정하게 된다 */

int device_is_mounted(const char *dev)
{
	return 0;   /* [한국어] 지원하지 않는 플랫폼 — 항상 "마운트 아님" 으로 폴백 */
}

#endif
