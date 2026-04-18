/*
 * [한국어 설명] 디바이스 마운트 상태 검사 공개 API 헤더 (mountcheck.h)
 *
 * === 파일의 역할 ===
 * lib/mountcheck.c 가 구현하는 `device_is_mounted(path)` 함수의 유일한 선언을
 * 제공한다. 본 헤더는 구조체/매크로/전역 변수를 일절 두지 않는 순수 선언 헤더
 * 이며, fio 가 블록 디바이스 경로(예: "/dev/sdX") 또는 마운트 포인트 경로를
 * 받았을 때 "이 경로가 현재 커널 마운트 테이블에 있어 파일시스템이 얹혀
 * 있는가" 를 boolean 정수로 답해 주는 경계 API 를 노출한다. 마운트된 블록
 * 디바이스에 raw I/O 를 수행하면 파일시스템 메타데이터를 파괴할 위험이
 * 있으므로, fio 는 잡 초기화 단계에서 이 함수를 호출해 사용자가 의도치 않게
 * 라이브 파일시스템을 대상으로 벤치마크 쓰기를 하는 것을 사전 차단한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "잡 검증/안전장치" 계층에 속한다. init.c / filesetup.c 가 잡의
 * filename 옵션을 순회하면서 각 경로에 대해 device_is_mounted() 를 호출하고,
 * 반환값이 1 이고 잡이 쓰기 워크로드라면 allow_mounted_write 가 설정되지
 * 않은 한 parent-side 에서 잡을 거부한다. 실제 I/O 엔진(io_uring, libaio,
 * sync 등) 에는 이 검사가 주입되지 않고 init 경로에서만 한 번 평가된다.
 * 호출 체인:
 *   parse_jobs_ini/parse_cmd_line → add_job → fixup_options
 *     → device_is_mounted(path)  [본 파일이 선언, mountcheck.c 가 정의]
 *         → (Linux) getmntent(/proc/mounts 또는 /etc/mtab)
 *         → (BSD)  getmntinfo()
 *         → (Windows) 스텁 — 항상 0 반환
 *
 * === 타 모듈과의 연결 ===
 * - mountcheck.c : 본 헤더의 단일 함수 구현. OS 별 mount 테이블 API 분기.
 * - init.c / filesetup.c : fio_opt 검증 시점에 호출.
 * - os/os-{linux,freebsd,windows,...}.h : 각 OS 의 getmntent/getmntinfo
 *   가용성을 결정. 본 헤더는 os 매크로에 의존하지 않도록 순수 선언만 둔다.
 * 데이터 흐름: path 문자열(const char *) → device_is_mounted() → int (0/1)
 *   → 잡 수락/거부 결정.
 *
 * === 주요 함수/구조체 요약 ===
 * - device_is_mounted(path) : 문자열 경로가 마운트되어 있으면 1, 아니면 0.
 *   시스템 콜이 실패해도(예: /proc 접근 불가) 0 을 반환하여 안전하게
 *   폴백한다(잘못된 허용보다는 false-negative 가 덜 위험하다는 설계 선택은
 *   없음 — 오히려 정반대로 "확실히 마운트된 경우만 1" 을 반환해 보수적으로
 *   잡 거부를 막는다).
 */
#ifndef FIO_MOUNT_CHECK_H
#define FIO_MOUNT_CHECK_H
/* [한국어] 헤더 가드. filesetup.c, init.c 등 여러 소스에서 포함되더라도 중복
 * 선언이 발생하지 않도록 표준적으로 ifndef/define/endif 삼중 가드를 사용. */

/*
 * [한국어]
 * device_is_mounted - 주어진 경로가 마운트된 블록 디바이스인지 확인한다.
 *
 * @dev: 검사할 디바이스 경로(예: "/dev/sdX", "/dev/nvme0n1", "/mnt/data").
 *       호출자 소유 문자열. NULL 전달 시 동작은 구현 정의(대개 0 반환).
 * @return: 1=마운트됨(해당 잡이 쓰기라면 --allow_mounted_write 없이는 거부),
 *          0=마운트 안 됨 또는 조회 실패(보수적으로 허용).
 *
 * fio 가 라이브 파일시스템 위에 얹힌 블록 디바이스에 raw write 를 시도하여
 * 사용자 데이터/FS 메타데이터를 파괴하는 것을 막기 위해 잡 초기화 경로에서
 * 호출된다. 구현은 Linux 에서 getmntent(3)/setmntent(3) 로 /etc/mtab 또는
 * /proc/mounts 를 순회하여 경로 일치를 찾고, BSD 에서는 getmntinfo(3),
 * Windows 에서는 스텁으로 처리된다(본 헤더에서는 단일 선언만 노출).
 *
 * 실행 컨텍스트: fio 메인(파서) 스레드의 잡 초기화 구간에서 동기적으로 1회
 * 호출. 재진입 안전성은 underlying getmntent() 가 thread-safe 가 아니므로
 * 호출자가 메인 스레드에서만 호출해야 한다.
 */
extern int device_is_mounted(const char *);
/* [한국어] extern : 실제 정의는 mountcheck.c. 컴파일러가 링커 단계까지 심볼을
 * 미해결 상태로 두게 한다. const char * 로 읽기 전용 경로를 받으며, 반환은
 * 0/1 의 작은 정수로 shell 스크립트 호환성을 갖춘다. */

#endif
/* [한국어] FIO_MOUNT_CHECK_H 가드 종료. */
