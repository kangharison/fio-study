/*
 * [한국어 설명] /proc/self/mountinfo 기반 블록 디바이스 경로 조회 헤더 (linux-dev-lookup.h)
 *
 * === 파일의 역할 ===
 * blktrace 재생 모드에서 기록된 (major, minor) 디바이스 번호로부터 실제
 * 디바이스 경로(/dev/sdX, /dev/nvme0n1 등)를 검색하는 blktrace_lookup_device()
 * 의 공개 API 를 정의한다. blktrace 로그(/sys/kernel/debug/tracing/events/block/)
 * 에는 I/O 대상이 major:minor 로만 기록되므로 fio 의 replay 기능이 실제 open(2)
 * 을 수행하려면 디바이스 노드 경로로 해석되어야 한다. 구현은 /proc/self/
 * mountinfo 를 파싱하여 마운트된 소스 디바이스에서 일치하는 major/minor 를
 * 찾고, 필요 시 /dev/ 하위를 재귀 탐색한다. redirect 옵션으로 사용자가 다른
 * 디바이스로 대체할 수도 있다(fio 의 replay_redirect=/dev/other).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 blktrace 재생 경로:
 *   iolog.c::read_blktrace() → read_ipo_alloc() → blktrace_lookup_device()
 *     → /proc/self/mountinfo 파싱 또는 /dev/ 탐색
 *     → 발견 시 path 에 경로 기록, 호출자가 open(path, ...) 사용.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/linux-dev-lookup.c: /proc/self/mountinfo 파싱 및 /dev/ 재귀 탐색 구현.
 * - iolog.c / blktrace.c: replay 기능의 주 호출자.
 * - <sys/stat.h>(간접): major()/minor() 매크로로 디바이스 번호 추출.
 *
 * === 주요 함수/구조체 요약 ===
 * - blktrace_lookup_device(redirect, path, maj, min): major:minor → 경로 변환.
 */
#ifndef LINUX_DEV_LOOKUP
/* [한국어] 헤더 가드 — iolog.c / blktrace.c 동시 포함 대비. */
#define LINUX_DEV_LOOKUP

/*
 * [한국어]
 * blktrace_lookup_device - (major, minor) 디바이스 번호 → 디바이스 경로 조회
 *
 * @redirect: 사용자 지정 replay_redirect 경로(NULL 이면 실제 디바이스 탐색).
 *            이 값이 NULL 이 아니면 원본 디바이스 무시하고 redirect 를 그대로 사용
 *            (fio 가 원본 I/O 를 다른 디스크로 재생하고 싶을 때 활용).
 * @path:     결과 경로 버퍼(호출자 소유, PATH_MAX 크기 권장). 성공 시 "/dev/sdX" 등 기록.
 * @maj:      조회 대상 major 번호.
 * @min:      조회 대상 minor 번호.
 * @return:   성공 시 1(또는 0 — 구현 반환 규약 확인), 미발견/실패 시 0 또는 음수.
 *
 * 동작:
 *   1) redirect != NULL → path 에 redirect 복사 후 반환.
 *   2) /proc/self/mountinfo 한 줄씩 파싱, 필드에서 소스 디바이스의 major:minor
 *      와 마운트 소스 경로 추출, 일치 시 path 에 기록.
 *   3) mountinfo 에서 못 찾으면 /dev/ 하위 재귀 탐색(stat(2) 로 major/minor 비교).
 *
 * 호출 체인: iolog.c::read_blktrace_file() → blktrace_lookup_device() → open(path)
 * 실행 컨텍스트: 메인 스레드(replay 시작 전 파일 준비 단계).
 * 에러 경로: 찾지 못하면 호출자가 해당 trace 항목을 스킵하거나 에러 보고.
 */
int blktrace_lookup_device(const char *redirect, char *path, unsigned int maj,
			   unsigned int min);

#endif
