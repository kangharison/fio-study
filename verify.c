/*
 * IO verification helpers
 */
/*
 * [한국어 설명] I/O 데이터 무결성 검증 엔진 (verify.c)
 *
 * === 파일의 역할 ===
 * fio가 "쓴 데이터를 다시 읽어서 비트 수준까지 동일한지" 확인하는 데이터 무결성
 * 검증 서브시스템의 엔드투엔드 구현이다. 쓰기 단계에서는 각 I/O 블록의 선두에
 * `struct verify_header`(16바이트 고정 헤더) + 알고리즘별 다이제스트 헤더
 * `vhdr_*`(1~128바이트)를 삽입하고 나머지를 패턴/PRNG로 채운 뒤, 읽기 단계에서는
 * 같은 알고리즘으로 체크섬을 재계산하여 불일치를 검출한다. 지원 알고리즘은
 * VERIFY_NONE / HDR_ONLY / MD5 / CRC64 / CRC32 / CRC32C / CRC32C_INTEL(SSE4.2 PCLMUL)
 * / CRC16 / CRC7 / SHA1 / SHA256 / SHA512 / SHA3_224/256/384/512 / XXHASH / META /
 * PATTERN / PATTERN_NO_HDR / NULL 총 18종이며, VERIFY_PATTERN_NO_HDR은 헤더 없이
 * 순수 패턴만 비교해 드라이브 내부 검증(T10 DIF/NVMe PI 등)과 병행 가능하다.
 *
 * 이 파일은 또한 검증 오프로드(async verify thread pool)를 구현한다. verify_async
 * 옵션이 설정되면 메인 잡 스레드가 I/O를 제출/완료하는 동안 검증 연산(특히 SHA/
 * MD5 같은 CPU 무거운 해시)은 별도 pthread들이 `td->verify_list`에서 꺼내어
 * 동시에 실행한다. 또한 장시간 검증 잡이 비정상 종료된 후 재시작을 지원하기 위해
 * `verify_save_state`/`verify_load_state`가 각 스레드의 numberio·난수 상태·
 * in-flight 슬롯을 .vstate 파일에 CRC32C로 직렬화/복원한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * [쓰기 경로]
 *   backend.c::do_io() → io_u.c::get_io_u() → io_u.c::fill_io_buffer()
 *     → [이 파일] populate_verify_io_u() → fill_pattern_headers()
 *         → fill_verify_pattern() (버퍼 바디를 랜덤/패턴으로 채움)
 *         → populate_hdr() (각 검증 블록 선두에 fill_hdr + fill_<alg>)
 *     → td_io_queue() → engines/*.c::queue() → 디바이스
 *
 * [읽기 → 검증 경로 — 인라인 (verify_async=0)]
 *   backend.c::do_verify() → [이 파일] get_next_verify()
 *     → io_hist_tree/list에서 io_piece 하나 꺼내 io_u에 바인딩
 *     → td_io_queue() → engines/*.c::queue() → 디바이스로부터 read
 *     → io_u_sync_complete() → [이 파일] verify_io_u()
 *         → verify_header() (magic/version/len/rand_seed/offset/numberio/crc32 검사)
 *         → switch(verify_type){ verify_io_u_md5/crc32c/sha256/... } (알고리즘별)
 *         → 불일치 시 log_verify_failure() → dump_verify_buffers()
 *
 * [읽기 → 검증 경로 — 오프로드 (verify_async=N>0)]
 *   do_verify() 쓰레드: [이 파일] verify_io_u_async()
 *     → td->verify_list에 flist_add_tail, pthread_cond_signal(td->verify_cond)
 *   워커 쓰레드 N개: [이 파일] verify_async_thread()
 *     → pthread_cond_wait → flist_splice_init → verify_io_u() 반복
 *
 * [실행 컨텍스트]
 * - 쓰기 단계 `populate_verify_io_u` 이하: 잡 스레드 1개(동시성 없음).
 * - 검증 단계 `verify_io_u` 이하: 잡 스레드 단독 또는 verify_async 워커 N개
 *   — `td->io_u_lock`과 `td->verify_cond`로 wake-up / list consume 동기화.
 * - `get_next_verify`: 잡 스레드에서만 호출. io_hist_tree/list는 단일 생산자(I/O
 *   완료 핸들러가 io_piece 삽입) + 단일 소비자(get_next_verify 꺼냄) 구조라 명시
 *   락이 필요 없다 (in-flight 플래그는 atomic_load_acquire로 동기화).
 *
 * === 타 모듈과의 연결 ===
 * - backend.c : do_io()에서 populate_verify_io_u() 호출(쓰기), do_verify()가
 *               get_next_verify() / verify_io_u() / verify_io_u_async() 호출.
 *               verify_async_init()/verify_async_exit()도 backend.c 생명주기에 연결.
 * - io_u.c    : io_u 할당·해제, buf_filled_len 관리. verify는 io_u->buf에 직접 쓴다.
 * - io_u.h    : io_u에 rand_seed/verify_offset/numberio/start_time/flags(IO_U_F_*)
 *               verify_list(flist)가 포함되어 이 파일이 읽고 쓴다.
 * - iolog.c / trim.c : io_hist_tree/list + io_piece(IP_F_ONRB/ONLIST/IN_FLIGHT/TRIMMED)
 *                      를 관리. get_next_verify()가 io_piece → io_u 바인딩 수행.
 * - verify.h : struct verify_header, vhdr_*, enum VERIFY_* 선언 및 외부 API.
 * - verify-state.h / verify-state.c : struct verify_state_hdr/thread_io_list/
 *                                     all_io_list + verify_state_gen_name 제공.
 * - lib/pattern.c : cpy_pattern/cmp_pattern/paste_format/paste_format_inplace로
 *                   verify_pattern 처리 (format specifier %o → paste_blockoff).
 * - lib/rand.c    : __rand(&td->verify_state)로 검증 시드 생성.
 * - lib/hweight.c : hweight8() - 불일치 바이트의 비트 에러 수를 계산해
 *                   "하드웨어 소프트 에러"인지 진단에 활용.
 * - crc/*.c    : 각 알고리즘 구현. SSE4.2 PCLMUL(crc32c_intel)과 ARMv8 crc32c
 *                (crc32c_arm64)는 fio_verify_init()가 런타임 probing.
 * - os/os.h    : FIO_OS_PATH_SEPARATOR, cpu_to_le*/le*_to_cpu 바이트 순서 변환.
 * - oslib/asprintf.h : 덤프 파일명 동적 생성용 asprintf() OS 추상화.
 *
 * [데이터 흐름 — 검증 블록 하나의 바이트 레이아웃]
 *
 *   | 0  1 | 2 | 3 | 4..7 | 8..15      | 16..23     | 24..27  | 28..31   |
 *   |magic |ver|ty |len   |rand_seed   |offset      |time_sec |time_nsec |
 *                                                                        ^
 *   | 32..39    | 40..41 | 42..? | 44..crc_len  | ..... data ........   |
 *   |numberio   |thread  |pad    |crc32(self)   | <- vhdr_*  | <- body |
 *
 *   - 0..43 바이트: struct verify_header (고정 44바이트 가정, 패딩 포함)
 *   - 그 다음 vhdr_<alg> 가변 (MD5 16B, CRC32 4B, SHA512 128B, CRC7 1B 등)
 *   - 그 다음 실제 데이터 영역 = hdr->len - (sizeof(verify_header) + sizeof(vhdr_alg))
 *   - 여러 블록이 하나의 io_u->buflen 안에 verify_interval 간격으로 반복될 수 있다.
 *
 * [공유 상태]
 * - td->verify_list (flist_head): 오프로드 대기 큐. 생산자 verify_io_u_async,
 *   소비자 verify_async_thread — td->io_u_lock 보호.
 * - td->verify_cond / td->free_cond / td->verify_thread_exit / td->nr_verify_threads:
 *   워커 풀 생애주기. verify_async_init 생성, verify_async_exit 소멸.
 * - td->io_hist_tree / io_hist_list / io_hist_len: write 완료 시 iolog.c가 삽입,
 *   get_next_verify가 제거. IP_F_IN_FLIGHT는 atomic_load_acquire로 읽어 아직
 *   장치에 in-flight인 쓰기를 검증 대상에서 제외한다.
 * - td->verify_state (frand_state): VERIFY_NONE이 아닌데 verify_pattern_bytes==0인
 *   경우 잡 시작 시 init_rand_seed로 초기화되어 쓰기/검증 양쪽에서 같은 시드 시퀀스를
 *   재생성 — 덕분에 데이터를 실제로 보존하지 않고도 기대값 재계산이 가능.
 * - td->vstate (thread_io_list*): verify_load_state로 로드된 이전 실행의 상태.
 *
 * === 주요 함수/구조체 요약 ===
 * [쓰기 경로]
 * - populate_verify_io_u(td, io_u) : 쓰기 I/O 버퍼에 패턴 + N개의 검증 헤더 삽입.
 * - fill_pattern_headers() / fill_verify_pattern() : 버퍼 바디 채우기 (랜덤/패턴).
 * - populate_hdr() / fill_hdr() / __fill_hdr() : verify_header + vhdr_<alg> 기록.
 * - fill_md5/crc32/crc32c/crc7/crc16/crc64/sha1/sha256/sha512/sha3_*/xxhash(),
 *   fill_sha3() 공용 : 각 알고리즘 체크섬 계산해 vhdr에 기록.
 *
 * [읽기/검증 경로]
 * - get_next_verify() : io_hist에서 다음 검증 대상 블록 꺼내 io_u 설정.
 * - verify_io_u() : 메인 디스패처 — verify_header() + verify_io_u_<alg>() 순회.
 * - verify_header() : 고정 헤더 7항목(magic/version/len/rand_seed/offset/numberio/crc32)
 *                     점검.
 * - verify_io_u_md5/crc32c/sha256/sha512/sha3_*/xxhash/sha1/crc64/crc32/crc16/crc7() :
 *   알고리즘별 체크섬 재계산 + memcmp.
 * - verify_io_u_pattern() : 패턴 비교 3가지 모드 (전체/verify_interval/
 *                           verify_pattern_interval).
 * - check_pattern() : fast cmp_pattern → slow byte 비교 → hweight8 비트 에러 보고.
 * - verify_trimmed_io_u() : TRIM된 영역이 정말 0으로 읽히는지 확인 (trim_zero).
 * - dump_buf() / __dump_verify_buffers() / dump_verify_buffers() :
 *   실패 시 received/expected/hdr_fail 3종 덤프.
 * - log_verify_failure() : 실패 로그 + expected/received CRC 16진 덤프.
 * - mem_is_zero() / mem_is_zero_slow() : Rusty Russell의 "memcmp with self" 트릭
 *   + 바이트별 스캔 (느린 버전은 첫 비제로 오프셋 반환).
 *
 * [비동기 검증 오프로드]
 * - verify_io_u_async() : 메인 스레드가 호출, td->verify_list에 io_u 추가.
 * - verify_async_thread() : 워커 메인 루프 (pthread_cond_wait → consume → verify).
 * - verify_async_init() : verify_async 개수만큼 pthread_create + detach.
 * - verify_async_exit() : verify_thread_exit=1 + broadcast + join-wait.
 *
 * [검증 상태 저장/복원 — 재시작 지원]
 * - get_all_io_list() : 각 스레드의 (depth/numberio/rand state/in-flight) 직렬화.
 * - write_thread_list_state() / __verify_save_state() / verify_save_state() :
 *   CRC32C 헤더 + thread_io_list 본문을 <prefix>.<index>.<name>.state 파일로 저장.
 * - open_state_file() : O_CREAT|O_TRUNC|O_WRONLY|O_SYNC(쓰기) 또는 O_RDONLY(읽기).
 * - verify_load_state() / verify_assign_state() / verify_state_hdr() :
 *   상태 파일 읽고 CRC 검증, 바이트 순서 변환하여 td->vstate에 설치.
 * - verify_state_should_stop() : 로드된 상태를 참조해 특정 numberio 이후는
 *   검증 중단 (이전 실행에서 쓰지 않았거나 in-flight였던 블록).
 * - verify_free_state() : td->vstate 해제.
 * - paste_blockoff() : verify_pattern_fmt "%o" specifier 콜백 (io_u->offset을
 *   리틀엔디안 64비트로 패턴 버퍼에 삽입).
 * - fio_verify_init() : CRC32C ARM64/Intel HW 가속 probing (잡 시작 시 1회).
 *
 * [구조체]
 * - struct vcont : 단일 검증 호출의 입력(io_u/hdr_num/td)과 출력(실패 시 name/
 *                  good_crc/bad_crc/crc_len)을 함께 전달하는 컨테이너.
 */

/* ===== 표준 라이브러리 및 시스템 헤더 ===== */
/* [한국어] POSIX 기본 I/O — 상태 파일 write(2)/read(2)/close(2), lseek 등. */
#include <unistd.h>
/* [한국어] open(2)과 O_CREAT/O_TRUNC/O_WRONLY/O_RDONLY/O_SYNC/O_BINARY(Windows)
 * 플래그 — 검증 상태 파일 및 덤프 파일을 열 때 사용. */
#include <fcntl.h>
/* [한국어] memcpy/memcmp/memset/strdup/strerror — 버퍼 비교·이동·에러 메시지.
 * verify_io_u_*에서 vh->digest와 재계산값 비교에 memcmp가 핵심. */
#include <string.h>
/* [한국어] assert() — 패턴 크기 비영(非零) 등 불변식 검증. Release 빌드에서는
 * NDEBUG로 제거되지만 fio는 보통 assert를 유지 빌드하여 조기 실패를 유도. */
#include <assert.h>
/* [한국어] pthread_create/detach/join/mutex/cond - 비동기 검증 워커 풀 구현에
 * 필수. td->io_u_lock + td->verify_cond 조합이 wake/consume을 동기화. */
#include <pthread.h>
/* [한국어] basename(3) (libgen 판본) — 덤프 파일명 생성 시 `f->file_name`에서
 * 디렉토리 부분을 제거하기 위해 사용. POSIX는 basename이 입력 문자열을 수정할
 * 수 있어서 strdup으로 복사 후 전달한다. */
#include <libgen.h>

/* ===== fio 내부 헤더들 ===== */
/* [한국어] CPU 아키텍처별 정의 — read_barrier/write_barrier/atomic_load_acquire 등
 * 메모리 배리어 매크로를 간접 공급. verify_async_thread의 td->verify_thread_exit
 * 체크 전 read_barrier(), get_next_verify의 IP_F_IN_FLIGHT 검사 등에서 사용. */
#include "arch/arch.h"
/* [한국어] fio 핵심 심볼 카탈로그 — struct thread_data / thread_options / io_u /
 * io_piece / fio_file / FD_VERIFY dprint 카테고리 / td_write/td_min_bs/td_max_bs /
 * io_u_set/io_u_clear / IO_U_F_* 플래그 / fio_option_is_set / td_verror /
 * fio_mark_td_terminate / td_non_fatal_error / update_error_count / td_clear_error /
 * td_ioengine_flagged / FIO_FAKEIO / FIO_WARN_VERIFY_BUF / fio_did_warn / aux_path
 * / put_file_log / get_file / remove_trim_entry / fio_file_open / td_io_open_file /
 * fio_setaffinity / verify_state_gen_name / compiletime_assert / for_each_td /
 * end_for_each / __td_index / IO_LIST_ALL / TD_F_VSTATE_SAVED 등 — 거의 전체
 * verify.c 동작에 필요한 중앙 허브. */
#include "fio.h"
/* [한국어] 이 파일의 공개 API와 내부 구조체 선언 — verify.h가 enum VERIFY_*,
 * struct verify_header, struct vhdr_* 알고리즘 다이제스트 구조체, FIO_HDR_MAGIC
 * (0xacca), VERIFY_HEADER_VERSION(0x81), populate_verify_io_u/get_next_verify/
 * verify_io_u/verify_io_u_async/fill_verify_pattern/fill_buffer_pattern/
 * fio_verify_init/verify_async_init/exit/paste_blockoff 선언을 공급한다. */
#include "verify.h"
/* [한국어] TRIM 관련 유틸리티 — trim_block_range 등 IO_U_F_TRIMMED 처리와 관련.
 * verify_trimmed_io_u에서 trim 영역의 0 검증에 필요한 공용 선언 포함. */
#include "trim.h"
/* [한국어] 난수 생성기 — struct frand_state/init_rand_seed/__rand/frand_* 함수.
 * td->verify_state로 데이터 바디의 재현 가능한 랜덤 채움, io_u->rand_seed 시드
 * 생성에 사용. 32비트 vs 64비트 RNG 분기(if sizeof(int)!=sizeof(long*))는
 * 64비트 ABI에서 상위 엔트로피를 추가로 확보하기 위한 관례. */
#include "lib/rand.h"
/* [한국어] hweight8/16/32/64 — 바이트/워드의 "1" 비트 수를 계산.
 * check_pattern()의 slow path에서 buf[i] ^ pattern[mod]의 비트 에러 수를 보고하여
 * "소프트 에러 몇 비트가 뒤집혔는지"를 진단 로그로 출력한다 (ECC가 놓친 패턴
 * 분석에 유용). */
#include "lib/hweight.h"
/* [한국어] 패턴 버퍼 조작 유틸 — cpy_pattern/cmp_pattern/paste_format/
 * paste_format_inplace 및 format specifier 정의(paste_blockoff 등).
 * fill_buffer_pattern / fill_verify_pattern / check_pattern / verify_io_u_pattern
 * 의 엔진 역할. */
#include "lib/pattern.h"
/* [한국어] asprintf(3) OS 추상화 — 동적 파일명 생성에 사용. Linux glibc는 기본
 * 제공하지만 Windows/Solaris에서는 이 래퍼가 대체 구현을 제공. dump_buf()에서
 * "<aux_path>/<basename>.<offset>.<type>" 생성. */
#include "oslib/asprintf.h"

/* ===== 체크섬/해시 알고리즘 구현 헤더들 ===== */
/* [한국어] MD5(RFC 1321) 128비트 해시 — fio_md5_ctx/init/update/final API.
 * 암호학적으로는 깨졌으나 fio는 데이터 무결성(변조 감지 아닌 우발적 손상 감지)
 * 용도라 여전히 실용적. 빠르고 vhdr_md5는 16바이트 고정. */
#include "crc/md5.h"
/* [한국어] CRC64 (ISO/ECMA 다항식 중 하나) — 64비트 체크섬. 긴 블록에서 CRC32의
 * 충돌 확률을 줄이고 싶을 때. vhdr_crc64는 8바이트. */
#include "crc/crc64.h"
/* [한국어] CRC32 (IEEE 802.3 이더넷 다항식) — 클래식 CRC32. */
#include "crc/crc32.h"
/* [한국어] CRC32C (Castagnoli 다항식, iSCSI/SCTP/ext4 metadata) — SSE4.2의
 * CRC32C 명령(PCLMUL)과 ARMv8 CRC32 extension으로 HW 가속 가능. 이 파일의
 * fio_verify_init이 crc32c_intel_probe/arm64_probe로 런타임 탐지. */
#include "crc/crc32c.h"
/* [한국어] CRC16 (CCITT/XMODEM) — 16비트 체크섬. vhdr_crc16는 2바이트. */
#include "crc/crc16.h"
/* [한국어] CRC7 (MMC/SD 카드 표준) — 7비트를 1바이트에 넣는다. vhdr_crc7는 1B. */
#include "crc/crc7.h"
/* [한국어] SHA-256 (FIPS 180-4) — 256비트 해시. fio_sha256_ctx API. */
#include "crc/sha256.h"
/* [한국어] SHA-512 (FIPS 180-4) — 512비트 해시. 64비트 워드 연산 기반이라 64비트
 * 아키텍처에서 SHA-256보다도 빠를 수 있다. */
#include "crc/sha512.h"
/* [한국어] SHA-1 (RFC 3174) — 160비트 해시. 암호학적으로는 취약하나 무결성
 * 용도. vhdr_sha1 = uint32_t[5]. */
#include "crc/sha1.h"
/* [한국어] xxHash v1 (Yann Collet) — 비암호학적 초고속 해시. GB/s 단위 처리량.
 * fio에서는 검증 오버헤드를 최소화하고 싶을 때 선택. */
#include "crc/xxhash.h"
/* [한국어] SHA-3 (Keccak, FIPS 202) — 224/256/384/512 4개 변종. SHA-2와 다른
 * 스폰지 구조. fio_sha3_ctx 공용 컨텍스트에 sha 포인터로 출력 버퍼 지정. */
#include "crc/sha3.h"

/* [한국어] 전방 선언 — 파일 내에서 상호 재귀/전방 참조되는 2개 함수.
 * populate_hdr는 fill_pattern_headers → populate_hdr 경로에서 호출되며, 같은
 * 파일 내에 본체가 뒤에 정의되어 있으므로 여기서 시그니처만 선언해둔다.
 * __fill_hdr는 dump_verify_buffers(VERIFY_PATTERN_NO_HDR 특수 처리)에서 임시
 * 가짜 헤더를 채우기 위해 참조하므로 먼저 선언. */
static void populate_hdr(struct thread_data *td, struct io_u *io_u,
			 struct verify_header *hdr, unsigned int header_num,
			 unsigned int header_len);
/* [한국어] 헤더 필드를 직접 채우는 내부 함수. populate_hdr는 VERIFY_PATTERN_NO_HDR
 * 모드에서 헤더를 생략하지만, 덤프 시에는 비교를 위해 임시로 헤더를 재구성해야
 * 하므로 이 "무조건 채우기" 버전을 따로 제공한다. */
static void __fill_hdr(struct thread_data *td, struct io_u *io_u,
		       struct verify_header *hdr, unsigned int header_num,
		       unsigned int header_len, uint64_t rand_seed);

/*
 * [한국어]
 * fill_buffer_pattern - buffer_pattern 옵션의 바이트 시퀀스로 임의 길이 버퍼를 채움
 *
 * @td  : 잡 컨텍스트. td->o.buffer_pattern(사용자 지정 패턴)과
 *        td->o.buffer_pattern_bytes(패턴 길이)를 읽는다.
 * @p   : 채울 버퍼의 시작 주소. 잡 스레드 소유라 별도 락 불필요.
 * @len : 채울 길이(바이트). 패턴보다 길면 패턴이 반복되며 tile된다.
 *
 * buffer_pattern은 --verify와 별개로 "모든 I/O 버퍼의 기본 채움 패턴"을 지정하는
 * 옵션이다. populate_verify_io_u 이전에 fill_io_buffer에서 이 함수로 채운 뒤,
 * 검증 헤더가 나중에 덮어쓰게 된다. cpy_pattern()은 lib/pattern.c에 있으며
 * memcpy/SIMD 기반 빠른 타일링을 수행한다.
 *
 * 호출 체인:
 *   io_u.c::fill_io_buffer() → [이 함수] fill_buffer_pattern() → lib/pattern.c::cpy_pattern()
 */
void fill_buffer_pattern(struct thread_data *td, void *p, unsigned int len)
{
	/* [한국어] cpy_pattern 반환값(실제 채워진 바이트)을 무시 — 호출자는 len만큼의
	 * 채움이 성공했다고 가정. void 캐스트는 warn_unused_result 억제 관례. */
	(void)cpy_pattern(td->o.buffer_pattern, td->o.buffer_pattern_bytes, p, len);
}

/*
 * [한국어]
 * __fill_buffer - 시드 기반 유사 랜덤 데이터로 버퍼를 채움 (압축률 조절 가능)
 *
 * @o    : thread_options. compress_percentage(0~100)와 buffer_pattern,
 *         buffer_pattern_bytes를 참조.
 * @seed : 64비트 난수 시드. 같은 시드 + 같은 옵션은 항상 같은 결과를 생성하므로
 *         쓰기 시 시드를 기록해 두면 읽기 시 재현 가능하다. verify_pattern_bytes=0
 *         경로에서 핵심.
 * @p    : 버퍼 시작.
 * @len  : 채울 길이.
 *
 * __fill_random_buf_percentage는 (100 - compress_percentage)% 영역만 실제 랜덤으로
 * 채우고 나머지는 buffer_pattern(없으면 0)으로 채운다. 이를 통해 압축 가능한
 * 데이터 비율을 모사해 dedup/compression 스토리지 벤치마크를 가능하게 한다.
 * compress_chunk 단위로 이 "랜덤/패턴" 스위치가 반복된다.
 *
 * 호출 체인: fill_verify_pattern() → [이 함수] → lib/rand.c::__fill_random_buf_percentage()
 */
static void __fill_buffer(struct thread_options *o, uint64_t seed, void *p,
			  unsigned int len)
{
	/* [한국어] 6번째 인자는 "chunk_size" — compress_chunk가 0이면 len 전체를
	 * 하나의 chunk로 처리(현재 호출은 len==chunk_size로 동일 값 전달). */
	__fill_random_buf_percentage(seed, p, o->compress_percentage, len, len, o->buffer_pattern, o->buffer_pattern_bytes);
}

/*
 * [한국어]
 * fill_verify_pattern - 검증 가능한 데이터로 버퍼를 채우는 핵심 함수 (두 모드)
 *
 * @td       : 잡 컨텍스트. verify_pattern, verify_pattern_bytes, verify_fmt,
 *             verify_fmt_sz, verify_pattern_interval, verify_offset,
 *             compress_percentage, buffer_pattern, verify_state(RNG)를 읽음.
 * @p        : 채울 버퍼 시작 주소 (io_u->buf 내의 임의 지점 — 블록별 호출 가능).
 * @len      : 채울 길이.
 * @io_u     : 대상 I/O 유닛. io_u->offset과 io_u->buf_filled_len, io_u->rand_seed을
 *             읽고 쓴다. 오프셋은 패턴 format specifier(%o 등)에 사용되며, 함수
 *             종료 시 원래 값으로 복원된다.
 * @seed     : use_seed=1일 때 사용할 시드. 주로 dump 시 원본 재생성에 쓰임.
 * @use_seed : 1이면 @seed를 그대로 사용, 0이면 td->verify_state에서 새 시드 생성.
 *
 * 두 가지 채움 모드:
 *   (1) verify_pattern_bytes == 0 : PRNG 랜덤 데이터 모드. seed로 재현 가능.
 *       io_u->rand_seed에 사용된 시드를 저장하여 읽기 시 재생성에 사용.
 *   (2) verify_pattern_bytes != 0 : 사용자 패턴 모드. 패턴을 interval 간격으로
 *       반복 tile하며, verify_fmt(%o 등)의 format specifier도 paste_format()으로
 *       해석하여 각 반복에 블록 오프셋 등을 삽입한다.
 *
 * "이미 채워진 버퍼 재사용" 최적화:
 *   buf_filled_len >= len 이면서 verify_fmt가 없고 verify_offset이 없으면 패턴을
 *   다시 그릴 필요가 없으므로 즉시 return. verify_fmt가 있으면 블록별로 오프셋을
 *   다시 기록해야 하고, verify_offset이 있으면 헤더-페이로드 바이트 교환으로
 *   패턴이 부분적으로 섞였으므로 반드시 재생성해야 한다.
 *
 * 호출 체인:
 *   [쓰기]   populate_verify_io_u → fill_pattern_headers → [이 함수]
 *   [덤프]   __dump_verify_buffers → fill_pattern_headers → [이 함수] (use_seed=1)
 */
void fill_verify_pattern(struct thread_data *td, void *p, unsigned int len,
			 struct io_u *io_u, uint64_t seed, int use_seed)
{
	struct thread_options *o = &td->o;             /* [한국어] 옵션 포인터 캐시 (가독성). */
	unsigned int interval = o->verify_pattern_interval;  /* [한국어] 패턴 반복 간격 (0이면 len 전체를 단일 반복으로). */
	unsigned long long offset = io_u->offset;      /* [한국어] 원래 io_u->offset 저장 — 함수 종료 시 복원. */

	/* [한국어] 모드 1: 사용자 지정 패턴이 없으면 PRNG 기반 랜덤 데이터로 채움. */
	if (!o->verify_pattern_bytes) {
		dprint(FD_VERIFY, "fill random bytes len=%u\n", len);

		/* [한국어] use_seed=0 (일반 쓰기 경로) — 새 시드를 검증 RNG에서 생성.
		 * use_seed=1 (덤프 경로)은 원본 hdr->rand_seed를 사용하므로 건너뜀. */
		if (!use_seed) {
			seed = __rand(&td->verify_state);
			/* [한국어] 64비트 ABI(sizeof(long*)==8)에서 seed의 상위 엔트로피를
			 * 확보하기 위해 __rand 결과를 한 번 더 곱한다. 32비트 ABI에서는
			 * seed가 이미 full-width이므로 생략. */
			if (sizeof(int) != sizeof(long *))
				seed *= (unsigned long)__rand(&td->verify_state);
		}
		io_u->rand_seed = seed;         /* [한국어] hdr->rand_seed에 저장되어 읽기 시 재현의 기준이 됨. */
		__fill_buffer(o, seed, p, len); /* [한국어] compress_percentage 반영 랜덤 채움. */
		return;
	}

	/* Skip if we were here and we do not need to patch pattern with
	 * format. However, we cannot skip if verify_offset is set because we
	 * have swapped the header with pattern bytes */
	/* [한국어] 최적화: 이미 이 버퍼가 패턴으로 채워졌고(buf_filled_len 추적),
	 * format specifier(%o 등)가 없어 재기록이 불필요하며, verify_offset으로
	 * 헤더-패턴이 섞이지도 않았다면 재채움을 건너뛴다. populate_hdr가 나중에
	 * 각 블록 시작부를 헤더로 덮어쓰므로 패턴 영역만 보존하면 된다. */
	if (!td->o.verify_fmt_sz && io_u->buf_filled_len >= len && !td->o.verify_offset) {
		dprint(FD_VERIFY, "using already filled verify pattern b=%d len=%u\n",
			o->verify_pattern_bytes, len);
		return;
	}

	/* [한국어] interval==0이면 "전체 버퍼를 한 덩어리로 취급" — 패턴이 len
	 * 길이에 맞춰 단 한 번 반복(또는 짧으면 타일). */
	if (!interval)
		interval = len;

	/* [한국어] 모드 2: 사용자 패턴을 interval 간격으로 반복 tile하면서 채움.
	 * io_u->offset을 각 interval 경계로 정렬해 paste_format이 블록별로 오프셋을
	 * 올바르게 삽입할 수 있게 한다. 첫 시작점에서 p가 io_u->buf 경계에 없으면
	 * 현재 interval 내부의 남은 바이트부터 채운다. */
	io_u->offset += (p - io_u->buf) - (p - io_u->buf) % interval;  /* [한국어] p가 놓인 interval의 시작 경계 offset 계산. */
	for (unsigned int bytes_done = 0, bytes_todo = 0; bytes_done < len;
			bytes_done += bytes_todo, p += bytes_todo, io_u->offset += interval) {
		/* [한국어] 현재 p가 interval 경계 기준 얼마나 떨어졌는지를 mod 계산으로
		 * 구해, interval 경계까지의 거리를 이번 라운드 처리량으로 사용. */
		bytes_todo = (p - io_u->buf) % interval;
		if (!bytes_todo)
			bytes_todo = interval;       /* [한국어] 경계에 정확히 놓이면 interval 전체를 처리. */
		bytes_todo = min(bytes_todo, len - bytes_done);  /* [한국어] 남은 len을 초과하지 않도록 클램프. */

		/* [한국어] paste_format: 패턴 바이트를 복사하며 verify_fmt specifier(%o 등)를
		 * 치환. 예를 들어 %o는 paste_blockoff()를 호출해 io_u->offset을 LE64로 삽입.
		 * 반환값(채워진 바이트)은 현재 사용하지 않음. */
		(void)paste_format(td->o.verify_pattern, td->o.verify_pattern_bytes,
				   td->o.verify_fmt, td->o.verify_fmt_sz,
				   p, bytes_todo, io_u);
	}

	io_u->buf_filled_len = len;   /* [한국어] "이 버퍼는 len까지 유효한 패턴" 마커 — 상술한 skip 최적화에서 참조. */
	io_u->offset = offset;        /* [한국어] 진입 시 저장했던 원래 offset으로 복원. */
}

/*
 * [한국어]
 * get_hdr_inc - 하나의 io_u 버퍼 내에서 "검증 블록" 크기(헤더 간격)를 계산
 *
 * @td   : 잡 컨텍스트. verify_interval, bs_unaligned 옵션 참조.
 * @io_u : 대상 I/O. io_u->buflen 참조.
 * @return: 헤더 사이의 간격(바이트). 기본값은 buflen 전체, verify_interval
 *          옵션이 유효하면 그 값을 사용.
 *
 * fio는 큰 I/O 블록(예: 1MiB)을 작은 검증 단위(예: 4KiB)로 쪼개어 각 단위마다
 * 독립적으로 검증할 수 있다. 이를 "verify_interval"이라 하며, populate_hdr가
 * 0, hdr_inc, 2*hdr_inc, ... 위치마다 verify_header+vhdr을 삽입한다.
 *
 * 사용 처 — fill_pattern_headers (쓰기) / verify_io_u (읽기) / verify_io_u_pattern /
 * __dump_verify_buffers. 쓰기 시와 읽기 시 반드시 동일한 값이 나와야 검증이 성립한다.
 *
 * bs_unaligned 특례: bs_unaligned=1이면 buflen이 verify_interval의 배수가 아닐 수
 * 있고 검증 블록이 깨끗하게 정렬되지 않으므로 buflen 전체를 단일 검증 블록으로
 * 취급 (verify_interval 무시).
 *
 * 호출 체인: 쓰기/읽기 양쪽에서 다수 경로가 이 함수를 호출해 동일 값을 공유.
 */
static unsigned int get_hdr_inc(struct thread_data *td, struct io_u *io_u)
{
	unsigned int hdr_inc;

	/*
	 * If we use bs_unaligned, buflen can be larger than the verify
	 * interval (which just defaults to the smallest blocksize possible).
	 */
	/* [한국어] 기본은 buflen 전체를 하나의 검증 블록으로 사용. verify_interval이
	 * 설정되어 있고 buflen에 수용 가능하며 bs_unaligned가 아닐 때만 interval로
	 * 덮어쓴다. 세 조건 모두 만족해야 안전하게 세분화 가능. */
	hdr_inc = io_u->buflen;
	if (td->o.verify_interval && td->o.verify_interval <= io_u->buflen &&
	    !td->o.bs_unaligned)
		hdr_inc = td->o.verify_interval;

	return hdr_inc;
}

/*
 * [한국어]
 * fill_pattern_headers - 버퍼 바디 채움 + 블록별 검증 헤더 삽입 (쓰기의 최상위 워커)
 *
 * @td       : 잡 컨텍스트.
 * @io_u     : 쓰기 대상 io_u. io_u->buf / buflen 사용.
 * @seed     : use_seed=1일 때 채움 시드. 덤프 경로(원본 재생성)에서 hdr->rand_seed를
 *             넘긴다.
 * @use_seed : 1이면 @seed 고정, 0이면 td->verify_state에서 새로 추첨.
 *
 * 2단계 작업:
 *   (1) fill_verify_pattern()이 io_u->buf 전체를 랜덤/패턴으로 채움.
 *   (2) get_hdr_inc() 간격으로 버퍼를 순회하며 각 블록 시작부(첫 sizeof(verify_header)
 *       + sizeof(vhdr_<alg>) 바이트)를 populate_hdr()로 덮어쓴다.
 * 그 결과 각 블록은 [hdr][vhdr_alg][데이터...] 레이아웃이 된다.
 *
 * 실행 컨텍스트: 쓰기 경로의 잡 스레드 단독. 검증 상태 저장은 필요 없고,
 * 오프로드 워커와 상호작용 없음.
 *
 * 호출 체인:
 *   populate_verify_io_u() → [이 함수] → fill_verify_pattern() + populate_hdr()*N
 *   __dump_verify_buffers() → [이 함수] (use_seed=1로 원본 재생성)
 */
static void fill_pattern_headers(struct thread_data *td, struct io_u *io_u,
				 uint64_t seed, int use_seed)
{
	unsigned int hdr_inc, header_num;
	struct verify_header *hdr;
	void *p = io_u->buf;        /* [한국어] 버퍼 시작 포인터 — 루프에서 hdr_inc씩 전진. */

	/* [한국어] 1단계: 전체 버퍼를 먼저 패턴/랜덤 데이터로 채운다. 이후 헤더가
	 * 각 블록의 앞부분을 덮어쓰므로 헤더 영역의 패턴은 사라지지만, 데이터 영역은
	 * 검증 시 hdr->len - hdr_size 만큼 해시되므로 그대로 보존된다. */
	fill_verify_pattern(td, p, io_u->buflen, io_u, seed, use_seed);

	/* [한국어] 2단계: 검증 블록 간격 결정 후 헤더를 순차적으로 삽입. */
	hdr_inc = get_hdr_inc(td, io_u);
	header_num = 0;
	for (; p < io_u->buf + io_u->buflen; p += hdr_inc) {
		hdr = p;      /* [한국어] 현재 블록 시작을 verify_header*로 캐스트. */
		populate_hdr(td, io_u, hdr, header_num, hdr_inc);  /* [한국어] 헤더 메타+체크섬 채움. */
		header_num++; /* [한국어] hdr->offset 계산에 사용되는 블록 인덱스 증가. */
	}
}

/*
 * [한국어]
 * memswp - 두 메모리 영역의 내용을 교환 (verify_offset의 헤더/페이로드 스왑용)
 *
 * @buf1 : 첫 번째 영역.
 * @buf2 : 두 번째 영역.
 * @len  : 교환할 바이트 수. 반드시 스택 버퍼 200B 이하여야 한다 (assert).
 *
 * verify_offset 옵션은 "블록 시작이 아닌 임의 오프셋에 헤더를 두고 싶다"는
 * 요구를 충족한다. 예를 들어 512B 섹터의 마지막 64B에 헤더를 넣어 FS의
 * 4K 블록 선두 패턴 분석과 독립적인 검증을 하는 등. 구현은 "시작부에 헤더를
 * 채운 뒤 verify_offset 위치의 len 바이트와 스왑"하여 헤더를 원하는 자리로
 * 옮긴다. 스택 임시 버퍼 200B는 사용 중 가장 큰 헤더(verify_header +
 * vhdr_sha512 ≈ 44 + 128 = 172B)를 수용하기 위한 보수적 상한.
 *
 * 사용처: populate_hdr (쓰기 후 스왑) / verify_io_u (읽은 데이터 역스왑).
 */
static void memswp(void *buf1, void *buf2, unsigned int len)
{
	char swap[200];                /* [한국어] 최대 헤더 크기 수용용 스택 임시 버퍼. */

	assert(len <= sizeof(swap));   /* [한국어] 버퍼 초과 방지 — 초과 시 즉시 abort. */

	memcpy(&swap, buf1, len);      /* [한국어] buf1 → swap */
	memcpy(buf1, buf2, len);       /* [한국어] buf2 → buf1 */
	memcpy(buf2, &swap, len);      /* [한국어] swap → buf2, 결과적으로 buf1↔buf2 */
}

/*
 * [한국어]
 * hexdump - 메모리 영역을 log_err로 16진 출력 (CRC mismatch 진단용)
 *
 * @buffer : 출력할 메모리 시작.
 * @len    : 바이트 수.
 *
 * 검증 실패 시 expected CRC와 received CRC를 16진으로 나란히 출력하여
 * 사용자가 시각적으로 차이를 비교할 수 있게 한다. log_err는 stderr로 나가며
 * 한 문자열을 여러 번 불러 붙여쓴 뒤 줄바꿈으로 마무리.
 *
 * 사용처: log_verify_failure()에서 good_crc/bad_crc 덤프.
 */
static void hexdump(void *buffer, int len)
{
	unsigned char *p = buffer;     /* [한국어] 1바이트 단위 접근을 위해 unsigned char*로 재해석. */
	int i;

	for (i = 0; i < len; i++)
		log_err("%02x", p[i]);  /* [한국어] 각 바이트를 2자리 16진 (대문자 없음)으로 출력. */
	log_err("\n");                 /* [한국어] 한 줄로 완결 — stderr 버퍼링 이슈를 줄이기 위함. */
}

/*
 * Prepare for separation of verify_header and checksum header
 */
/*
 * [한국어]
 * __hdr_size - verify_type에 대응하는 총 헤더 크기(고정 + 알고리즘별) 반환
 *
 * @verify_type : enum verify_type 값.
 * @return     : sizeof(struct verify_header) + sizeof(struct vhdr_<alg>).
 *               VERIFY_PATTERN_NO_HDR은 헤더 없이 순수 패턴만 저장하므로 0 반환.
 *               VERIFY_NONE/HDR_ONLY/NULL/PATTERN은 vhdr_*가 없어 verify_header
 *               크기만 반환.
 *
 * 이 함수는 "검증 블록 바디 = hdr->len - __hdr_size" 계산의 기반이 된다.
 * 체크섬 계산/검증 루프가 전체 len에서 헤더 영역을 건너뛰고 데이터만 해싱하게
 * 만드는 핵심 값. 알 수 없는 verify_type이 오면 구성 버그이므로 assert로 중단.
 *
 * 주의: vhdr_md5(16B) + verify_header(~44B) = ~60B이며, 가장 큰 조합은
 * verify_header + vhdr_sha512(128B) ≈ 172B (memswp의 200B 버퍼 상한 근거).
 *
 * 호출 체인: hdr_size (아래) / verify_io_u_pattern / populate_hdr 등.
 */
static inline unsigned int __hdr_size(int verify_type)
{
	unsigned int len = 0;

	switch (verify_type) {
	case VERIFY_NONE:      /* [한국어] 검증 없음 — vhdr 없음. */
	case VERIFY_HDR_ONLY:  /* [한국어] 헤더만 검증 (META 호환) — vhdr 없음. */
	case VERIFY_NULL:      /* [한국어] pretend verify — 실제 vhdr 없음. */
	case VERIFY_PATTERN:   /* [한국어] 패턴 비교 — 헤더는 있되 체크섬 vhdr 없음. */
		len = 0;
		break;
	case VERIFY_MD5:       /* [한국어] MD5 16B */
		len = sizeof(struct vhdr_md5);
		break;
	case VERIFY_CRC64:     /* [한국어] CRC64 8B */
		len = sizeof(struct vhdr_crc64);
		break;
	case VERIFY_CRC32C:         /* [한국어] CRC32C 4B — 소프트웨어 Castagnoli */
	case VERIFY_CRC32:          /* [한국어] CRC32 4B — 이더넷 다항식 */
	case VERIFY_CRC32C_INTEL:   /* [한국어] CRC32C 4B — Intel SSE4.2 PCLMUL HW */
		len = sizeof(struct vhdr_crc32);
		break;
	case VERIFY_CRC16:     /* [한국어] CRC16 2B */
		len = sizeof(struct vhdr_crc16);
		break;
	case VERIFY_CRC7:      /* [한국어] CRC7 1B */
		len = sizeof(struct vhdr_crc7);
		break;
	case VERIFY_SHA256:    /* [한국어] SHA-256 64B (버퍼 상한, 실제 다이제스트 32B). */
		len = sizeof(struct vhdr_sha256);
		break;
	case VERIFY_SHA512:    /* [한국어] SHA-512 128B 버퍼. */
		len = sizeof(struct vhdr_sha512);
		break;
	case VERIFY_SHA3_224:  /* [한국어] SHA3-224 28B */
		len = sizeof(struct vhdr_sha3_224);
		break;
	case VERIFY_SHA3_256:  /* [한국어] SHA3-256 32B */
		len = sizeof(struct vhdr_sha3_256);
		break;
	case VERIFY_SHA3_384:  /* [한국어] SHA3-384 48B */
		len = sizeof(struct vhdr_sha3_384);
		break;
	case VERIFY_SHA3_512:  /* [한국어] SHA3-512 64B */
		len = sizeof(struct vhdr_sha3_512);
		break;
	case VERIFY_XXHASH:    /* [한국어] xxHash 32비트 4B */
		len = sizeof(struct vhdr_xxhash);
		break;
	case VERIFY_SHA1:      /* [한국어] SHA-1 20B (5 * uint32_t). */
		len = sizeof(struct vhdr_sha1);
		break;
	case VERIFY_PATTERN_NO_HDR:   /* [한국어] 헤더 없이 순수 패턴 — 0 반환으로 조기 탈출. */
		return 0;
	default:
		/* [한국어] 알 수 없는 값은 초기화 버그 — 조기 abort로 진단. */
		log_err("fio: unknown verify header!\n");
		assert(0);
	}

	/* [한국어] vhdr_<alg> 크기 + 고정 verify_header 크기 = 총 헤더 영역 크기. */
	return len + sizeof(struct verify_header);
}

/*
 * [한국어]
 * hdr_size - 런타임 헤더 크기 반환 (VERIFY_PATTERN_NO_HDR 특례 포함)
 *
 * @td  : verify 옵션 확인용.
 * @hdr : 실제 헤더 포인터 — 옵션이 NONE이면 hdr->verify_type으로 자동 판별.
 * @return: 헤더 영역 바이트 수 (0 또는 __hdr_size 값).
 *
 * __hdr_size와 달리 런타임 분기(td->o.verify)를 추가하여 PATTERN_NO_HDR 모드를
 * 우선 처리한다. verify_io_u_<alg>/io_u_verify_off 등 "데이터 영역 시작"을
 * 계산해야 하는 모든 지점에서 사용된다.
 */
static inline unsigned int hdr_size(struct thread_data *td,
				    struct verify_header *hdr)
{
	if (td->o.verify == VERIFY_PATTERN_NO_HDR)
		return 0;       /* [한국어] 헤더를 삽입하지 않는 모드 — 데이터가 곧 블록 시작. */

	return __hdr_size(hdr->verify_type);  /* [한국어] hdr에 기록된 타입으로 자동 분기. */
}

/*
 * [한국어]
 * hdr_priv - struct verify_header 바로 뒤에 붙은 알고리즘별 vhdr 포인터 반환
 *
 * @hdr : 블록 시작에 놓인 verify_header 포인터.
 * @return: hdr + sizeof(verify_header) = vhdr_<alg> 시작 주소.
 *
 * 블록 레이아웃이 [verify_header][vhdr_<alg>][data]이므로 vhdr은 고정 오프셋에
 * 위치한다. 헤더 읽기/쓰기 시 (struct vhdr_md5*)hdr_priv(hdr)처럼 캐스트해
 * 접근. struct hack 대신 포인터 산술로 구현되어 있어 헤더 구조 변경 시 이
 * 함수만 손대면 된다.
 */
static void *hdr_priv(struct verify_header *hdr)
{
	void *priv = hdr;

	return priv + sizeof(struct verify_header);   /* [한국어] 고정 바이트 오프셋 (+40~+44B 가정). */
}

/*
 * Verify container, pass info to verify handlers and allow them to
 * pass info back in case of error
 */
/*
 * [한국어] struct vcont — 한 번의 검증 콜의 "입/출력 컨테이너"
 *
 * 왜 필요한가: verify_io_u_<alg>() 함수 13종이 동일한 시그니처로 호출되어야
 * 하고, 실패 시에는 (알고리즘 이름, 기대 CRC, 실제 CRC, CRC 길이)를 공통으로
 * 보고해야 하므로 하나의 구조체로 묶어 전달한다. 성공 시에는 출력 필드가
 * 의미 없고, 실패 시에만 log_verify_failure에서 참조된다.
 *
 * 생명주기: verify_io_u()에서 스택 변수로 할당되어 하나의 io_u(여러 검증 블록)
 * 루프 동안 동일 인스턴스가 재사용된다. 알고리즘 콜 하나마다 hdr_num만 갱신.
 *
 * 동기화: 스택 지역 변수 + 단일 스레드 호출이라 락 불필요.
 */
struct vcont {
	/*
	 * Input
	 */
	struct io_u *io_u;
	/* [한국어] 검증 대상 I/O 유닛. io_u->buf (읽어온 데이터), io_u->file,
	 * io_u->verify_offset, io_u->buflen, io_u->flags를 사용.
	 * 설정자: verify_io_u 진입 시. 읽는 자: verify_io_u_<alg> 전부 + 실패 시
	 * log_verify_failure/__dump_verify_buffers.
	 * 값 범위: 항상 유효한 io_u 포인터 (NULL 아님).
	 * 동기화: 단일 스레드 내 스택 변수 — 락 불필요. */

	unsigned int hdr_num;
	/* [한국어] io_u 버퍼 안에서 몇 번째 검증 블록인지(0-based). hdr->offset과
	 * io_u_verify_off 계산에 사용.
	 * 설정자: verify_io_u의 for 루프가 hdr_num++로 갱신.
	 * 값 범위: 0 ≤ hdr_num < io_u->buflen / hdr_inc. */

	struct thread_data *td;
	/* [한국어] 잡 스레드 데이터 — verify 옵션과 RNG 상태 읽기.
	 * 설정자: verify_io_u. 읽는 자: 모든 verify_* 및 덤프 함수.
	 * 값 범위: 비-NULL 잡 컨텍스트. */

	/*
	 * Output, only valid in case of error
	 */
	const char *name;
	/* [한국어] 실패한 알고리즘 이름 문자열 ("md5", "crc32c" 등). 로그에 출력.
	 * 설정자: verify_io_u_<alg>가 불일치 시 "<알고리즘>"으로 설정.
	 * 읽는 자: log_verify_failure().
	 * 값 범위: 정적 문자열 리터럴 포인터 또는 NULL(성공 시). */

	void *good_crc;
	/* [한국어] 헤더에 저장되어 있던 "기대" 체크섬 값. hexdump 출력용.
	 * 설정자: verify_io_u_<alg>이 vh->digest 주소를 저장.
	 * 값 범위: vhdr_* 내부 필드 포인터 (크기는 crc_len). */

	void *bad_crc;
	/* [한국어] 현재 데이터에서 재계산한 "실제" 체크섬 값. 불일치 증거.
	 * 설정자: verify_io_u_<alg>이 스택 로컬 버퍼 주소를 저장.
	 * 값 범위: 스택 버퍼 주소 (함수 반환 후 무효). */

	unsigned int crc_len;
	/* [한국어] good_crc/bad_crc 버퍼의 길이(바이트). hexdump 길이 인자.
	 * 설정자: verify_io_u_<alg> — 예: crc32 = 4, md5 = 16, sha512 = 128 등. */
};

#define DUMP_BUF_SZ	255  /* [한국어] 덤프 관련 경로/메시지 버퍼의 관례적 상한.
			      * 현재 파일에서 직접 참조되지 않으나 상징적으로 유지 -
			      * dump_buf 경로에서 임시 버퍼가 이 크기를 넘지 않도록 설계. */

/*
 * [한국어]
 * dump_buf - 검증 실패한 데이터를 사후 분석용 파일로 덤프
 *
 * @buf    : 덤프할 데이터 시작 주소.
 * @len    : 바이트 수 (부분 write 반복으로 완전 기록).
 * @offset : 파일 내 블록 오프셋 — 파일명에 포함되어 식별자 역할.
 * @type   : 덤프 종류 문자열 — "received"(디스크에서 읽은 실제 데이터),
 *           "expected"(원본 재생성), "hdr_fail"(헤더 검증 실패 덤프).
 * @f      : 원본 fio_file — file_name에서 basename을 추출해 파일명에 사용.
 *
 * 파일명 형식: "<aux_path>/<basename>.<offset>.<type>" 또는 aux_path 미설정 시
 * "<basename>.<offset>.<type>". aux_path는 --aux-path CLI 옵션으로 지정.
 * O_CREAT|O_TRUNC|O_WRONLY로 열어 write(2) 반복으로 완전 기록.
 * asprintf 실패(ENOMEM)는 한 번만 경고하고 조용히 리턴(fio_did_warn으로 재발 방지).
 *
 * 에러 처리: asprintf 실패 → free_ptr 분기 / open 실패 → free_fname 분기로 자원을
 * 순차 해제. 짧은 write(0 바이트)는 break로 무한루프 방지.
 *
 * 호출 체인:
 *   verify_header()(헤더 실패) → [이 함수] (type="hdr_fail")
 *   __dump_verify_buffers()(데이터 실패) → [이 함수] x2 (received + expected)
 */
static void dump_buf(char *buf, unsigned int len, unsigned long long offset,
		     const char *type, struct fio_file *f)
{
	char *ptr, *fname;
	/* [한국어] OS별 경로 구분자(Linux '/', Windows '\\')로 2바이트 문자열 생성
	 * ({sep_char, '\0'}). asprintf 인자로 사용. */
	char sep[2] = { FIO_OS_PATH_SEPARATOR, 0 };
	int ret, fd;

	/* [한국어] basename(3)이 입력을 수정할 수 있으므로 file_name을 복사. */
	ptr = strdup(f->file_name);

	/* [한국어] asprintf: "<aux_path><sep><basename>.<offset>.<type>" 또는
	 * aux_path=NULL 시 "<basename>.<offset>.<type>". ? : GCC 확장 "elvis operator"
	 * 로 aux_path가 NULL이면 빈 문자열을 사용. 실패(<0) 시 에러 로그 후 정리. */
	if (asprintf(&fname, "%s%s%s.%llu.%s", aux_path ? : "",
		     aux_path ? sep : "", basename(ptr), offset, type) < 0) {
		/* [한국어] 같은 경고를 반복 출력하지 않도록 FIO_WARN_VERIFY_BUF 비트를
		 * 세우고 체크 — 첫 번째 발생만 보고. */
		if (!fio_did_warn(FIO_WARN_VERIFY_BUF))
			log_err("fio: not enough memory for dump buffer filename\n");
		goto free_ptr;
	}

	/* [한국어] 0644 = rw-r--r-- (소유자 read/write, 그 외 read only). */
	fd = open(fname, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0) {
		perror("open verify buf file");
		goto free_fname;
	}

	/* [한국어] write(2) 부분 성공 처리 루프. 큰 버퍼는 한 번에 전부 쓰이지
	 * 않을 수 있으므로 남은 바이트를 반복 기록. ret==0은 FS 포화 상황으로
	 * 더 이상 진전 없으므로 break. */
	while (len) {
		ret = write(fd, buf, len);
		if (!ret)
			break;                          /* [한국어] 0바이트 write = 포화. 조용히 포기. */
		else if (ret < 0) {
			perror("write verify buf file");
			break;                          /* [한국어] 에러 — errno 표시 후 중단. */
		}
		len -= ret;
		buf += ret;
	}

	close(fd);
	/* [한국어] 덤프 성공 알림 — 사용자가 나중에 hexdump/diff로 분석 가능. */
	log_err("       %s data dumped as %s\n", type, fname);

free_fname:
	free(fname);   /* [한국어] asprintf 할당 해제 */

free_ptr:
	free(ptr);     /* [한국어] strdup 할당 해제 */
}

/*
 * Dump the contents of the read block and re-generate the correct data
 * and dump that too.
 */
/*
 * [한국어]
 * __dump_verify_buffers - 검증 실패 블록의 "실제" + "기대" 데이터를 한 쌍으로 덤프
 *
 * @hdr : 실패한 블록의 verify_header (디스크에서 읽힌 값). rand_seed가 원본
 *        재생성의 시드로 사용됨.
 * @vc  : 검증 컨테이너 — td, io_u, hdr_num을 참조.
 *
 * verify_dump=1 옵션이 켜져 있을 때만 동작. 두 단계:
 *   (1) io_u->buf + (hdr_num * hdr->len)부터 hdr->len 바이트를 "received"로 덤프.
 *       이는 디스크/네트워크로부터 실제 받은 데이터.
 *   (2) io_u를 스택에 복사한 dummy + 새 malloc 버퍼로 fill_pattern_headers를
 *       use_seed=1, seed=hdr->rand_seed로 호출 → 원본 쓰기 당시의 데이터를
 *       동일한 PRNG 시퀀스로 재생성. 해당 블록을 "expected"로 덤프.
 *
 * 이를 통해 사용자는 두 파일을 xxd/diff 등으로 비교해 어느 바이트부터 어떻게
 * 다른지 파악할 수 있다(예: ECC 소프트 에러 vs DMA 미스 vs 페이지 오염).
 *
 * 실행 컨텍스트: verify_io_u 루프의 실패 경로(잡 스레드) 또는 verify_async_thread.
 * 어느 쪽이든 동일 io_u에 대해 직렬로 호출되므로 동기화 불필요.
 *
 * 호출 체인: log_verify_failure → dump_verify_buffers → [이 함수] → dump_buf x2.
 */
static void __dump_verify_buffers(struct verify_header *hdr, struct vcont *vc)
{
	struct thread_data *td = vc->td;
	struct io_u *io_u = vc->io_u;
	unsigned long hdr_offset;
	struct io_u dummy;
	void *buf;

	/* [한국어] verify_dump 옵션이 꺼져 있으면 덤프 생성 스킵 (디스크 공간/시간 절약). */
	if (!td->o.verify_dump)
		return;

	/*
	 * Dump the contents we just read off disk
	 */
	/* [한국어] 실패 블록의 바이트 오프셋 = 블록 번호 * 블록 크기. */
	hdr_offset = vc->hdr_num * hdr->len;

	/* [한국어] "received": 실제 읽어온 데이터를 파일로 내보냄. */
	dump_buf(io_u->buf + hdr_offset, hdr->len, io_u->verify_offset + hdr_offset,
			"received", vc->io_u->file);

	/*
	 * Allocate a new buf and re-generate the original data
	 */
	/* [한국어] 원본 재생성용 임시 버퍼. io_u 전체 크기로 할당 — 여러 블록이
	 * 들어있을 수 있으므로 한 번에 재생성하고 실패 블록 부분만 덤프. */
	buf = malloc(io_u->buflen);
	dummy = *io_u;                       /* [한국어] io_u 전체 내용 복사 (offset/file 등 포함). */
	dummy.buf = buf;                     /* [한국어] 새 버퍼로 전환. */
	dummy.rand_seed = hdr->rand_seed;    /* [한국어] 원본 쓰기 때의 시드 재사용. */
	dummy.buf_filled_len = 0;            /* [한국어] "이미 채워진 패턴" 최적화 방지. */
	dummy.buflen = io_u->buflen;

	/* [한국어] use_seed=1 경로로 패턴+헤더 재생성 — 원본 쓰기와 동일한 내용이
	 * 나오도록 보장. __fill_buffer가 seed로 deterministic한 PRNG 전개. */
	fill_pattern_headers(td, &dummy, hdr->rand_seed, 1);

	/* [한국어] "expected": 재생성된 원본 데이터를 파일로 내보냄. */
	dump_buf(buf + hdr_offset, hdr->len, io_u->verify_offset + hdr_offset,
			"expected", vc->io_u->file);
	free(buf);                           /* [한국어] 임시 버퍼 즉시 해제. */
}

/*
 * [한국어]
 * dump_verify_buffers - __dump_verify_buffers의 래퍼 (PATTERN_NO_HDR 모드 특수 처리)
 *
 * @hdr : 원래 헤더 (있으면 그대로, 없으면 교체됨).
 * @vc  : 검증 컨테이너.
 *
 * VERIFY_PATTERN_NO_HDR 모드에서는 디스크에 헤더가 없어 hdr 포인터가 실제로는
 * 데이터 영역을 가리킨다. __dump_verify_buffers는 hdr->len/rand_seed를 참조하므로
 * 임시 가짜 헤더 shdr을 스택에 만들어 버퍼 크기로 채운 뒤 전달한다. rand_seed=0
 * 은 패턴 모드에서 무시되므로 무관.
 *
 * 사용처: log_verify_failure 단일.
 */
static void dump_verify_buffers(struct verify_header *hdr, struct vcont *vc)
{
	struct thread_data *td = vc->td;
	struct verify_header shdr;

	/* [한국어] 헤더 없는 모드 — 덤프용 임시 헤더를 0번 블록 + 전체 buflen으로 구성. */
	if (td->o.verify == VERIFY_PATTERN_NO_HDR) {
		__fill_hdr(td, vc->io_u, &shdr, 0, vc->io_u->buflen, 0);
		hdr = &shdr;
	}

	__dump_verify_buffers(hdr, vc);
}

/*
 * [한국어]
 * log_verify_failure - 검증 실패 1건을 stderr에 구조화된 형태로 보고 + 덤프 트리거
 *
 * @hdr : 실패 블록의 verify_header (또는 PATTERN_NO_HDR 시 더미).
 * @vc  : 검증 컨테이너 — name/good_crc/bad_crc/crc_len에 실패 정보 기록됨.
 *
 * 출력 형식:
 *   "<alg>: verify failed at file <name> offset <off> length <len>
 *    (requested block: offset=<io_off> length=<io_len> flags=<hex>)"
 *   [옵션] "Expected CRC: <hex>" / "Received CRC: <hex>"
 *
 * %.8s로 알고리즘 이름을 8자로 제한(커다란 출력 방지)한다. PATTERN_NO_HDR
 * 모드는 헤더가 없어 io_u 전체가 하나의 블록이므로 len = buflen으로 처리.
 * good/bad CRC 쌍이 모두 있으면 hexdump로 나란히 출력해 시각적 비교 가능.
 * 마지막에 dump_verify_buffers로 "received"/"expected" 파일 덤프를 위임.
 *
 * 호출 체인: verify_io_u_<alg>(실패) → [이 함수] → dump_verify_buffers → dump_buf.
 */
static void log_verify_failure(struct verify_header *hdr, struct vcont *vc)
{
	unsigned long long offset;
	uint32_t len;
	struct thread_data *td = vc->td;

	/* [한국어] 블록의 파일 내 절대 오프셋 계산. PATTERN_NO_HDR이면 io_u 전체가
	 * 한 블록이므로 verify_offset 그대로 + 전체 길이. 일반 모드에서는 hdr->len
	 * 간격으로 블록이 배치되어 있으므로 hdr_num * hdr->len을 더한다. */
	offset = vc->io_u->verify_offset;
	if (td->o.verify != VERIFY_PATTERN_NO_HDR) {
		len = hdr->len;
		offset += (unsigned long long) vc->hdr_num * len;
	} else {
		len = vc->io_u->buflen;
	}

	/* [한국어] 실패 요약 한 줄. %.8s = 최대 8자 문자열(알고리즘 이름), %x = io_u
	 * 플래그 비트맵. "requested block"은 원래 I/O 전체 범위(복수 블록 포함). */
	log_err("%.8s: verify failed at file %s offset %llu, length %u"
			" (requested block: offset=%llu, length=%llu, flags=%x)\n",
			vc->name, vc->io_u->file->file_name, offset, len,
			vc->io_u->verify_offset, vc->io_u->buflen, vc->io_u->flags);

	/* [한국어] 체크섬 비교가 가능한 알고리즘(CRC/해시)은 양쪽 값을 16진 출력.
	 * 패턴/헤더 실패에서는 good_crc/bad_crc가 NULL이라 이 블록은 건너뜀. */
	if (vc->good_crc && vc->bad_crc) {
		log_err("       Expected CRC: ");
		hexdump(vc->good_crc, vc->crc_len);
		log_err("       Received CRC: ");
		hexdump(vc->bad_crc, vc->crc_len);
	}

	dump_verify_buffers(hdr, vc);   /* [한국어] 실제/기대 버퍼를 파일로 덤프. */
}

/*
 * Return data area 'header_num'
 */
/*
 * [한국어]
 * io_u_verify_off - 주어진 블록 번호의 "데이터 영역 시작 포인터" 계산
 *
 * @hdr : 블록 헤더.
 * @vc  : 컨테이너 (td, io_u, hdr_num).
 * @return: io_u->buf + hdr_num*hdr->len + hdr_size — 헤더를 건너뛴 데이터 영역 시작.
 *
 * verify_io_u_<alg>가 체크섬 재계산에 사용하는 시작 포인터. hdr->len이 블록
 * 전체 크기이고 hdr_size가 헤더(고정 + vhdr) 크기이므로 그 차이가 데이터 길이.
 * hdr_size는 PATTERN_NO_HDR이면 0을 반환해 헤더 없이 바로 데이터로 이동.
 */
static inline void *io_u_verify_off(struct verify_header *hdr, struct vcont *vc)
{
	return vc->io_u->buf + vc->hdr_num * hdr->len + hdr_size(vc->td, hdr);
}

/*
 * [한국어]
 * check_pattern - 버퍼의 바이트 시퀀스가 주어진 패턴 타일과 일치하는지 검사
 *
 * @buf          : 검사할 버퍼 시작.
 * @len          : 검사 길이.
 * @mod          : pattern[0..pattern_size-1]에서 buf의 첫 바이트가 매칭될 인덱스
 *                 (패턴이 블록 중간부터 정렬되는 경우 시작 오프셋).
 * @pattern_size : 패턴 원본 크기.
 * @pattern      : 기대 패턴 바이트 시퀀스.
 * @header_size  : 헤더 크기 — 에러 오프셋 출력 시 "블록 내 몇 번째 바이트"인지
 *                 사용자가 보기 쉽도록 더해 출력.
 * @return       : 0 = 일치, EILSEQ = 불일치.
 *
 * 빠른 경로 + 느린 경로 2단계:
 *   (1) lib/pattern.c::cmp_pattern() — SIMD/memcmp 기반 대량 비교. 일치하면 즉시 0.
 *   (2) 빠른 경로가 실패하면 바이트 단위로 재스캔해 첫 불일치를 찾고,
 *       XOR + hweight8로 "몇 비트가 뒤집혔는가"를 보고. 단일 비트 플립은
 *       보통 DRAM ECC 소프트 에러 / 케이블 노이즈를, 여러 바이트 플립은
 *       DMA 미스/페이지 오염을 시사한다. 첫 불일치만 보고 후 EILSEQ 반환.
 *
 * mod 갱신: 매 바이트마다 mod++, pattern_size에 도달하면 0으로 래핑 → 패턴이
 * 반복된다는 가정.
 */
static int check_pattern(char *buf, unsigned int len, unsigned int mod,
		unsigned int pattern_size, char *pattern, unsigned int header_size)
{
	unsigned int i;
	int rc;

	/* [한국어] 1단계: SIMD 벡터 비교. 대부분의 성공 케이스는 여기서 종결. */
	rc = cmp_pattern(pattern, pattern_size, mod, buf, len);
	if (!rc)
		goto done;

	/* Slow path, compare each byte */
	/* [한국어] 2단계: 실패가 확인되면 정확한 위치를 찾기 위해 바이트 단위로 재비교.
	 * 성능 hit이 크지만 실패 시에만 실행되므로 OK. */
	for (i = 0; i < len; i++) {
		if (buf[i] != pattern[mod]) {
			unsigned int bits;

			/* [한국어] XOR로 다른 비트를 추출해 hweight8로 1 비트 개수 계산.
			 * 1-2 비트는 소프트 에러 의심, 많으면 버퍼 오염 의심. */
			bits = hweight8(buf[i] ^ pattern[mod]);
			log_err("fio: got pattern '%02x', wanted '%02x'. Bad bits %d\n",
				(unsigned char)buf[i],
				(unsigned char)pattern[mod],
				bits);
			/* [한국어] header_size를 더해 "사용자가 보는 블록 내 오프셋"을
			 * 일관되게 출력 (헤더 영역까지 포함한 상대 위치). */
			log_err("fio: bad pattern block offset %u\n",
				i + header_size);
			rc = EILSEQ;
			goto done;                /* [한국어] 첫 불일치만 보고 — 무한 로그 회피. */
		}
		mod++;
		if (mod == pattern_size)
			mod = 0;                  /* [한국어] 패턴 끝에 도달하면 wrap — 패턴 반복 규약. */
	}

done:
	return rc;
}

/*
 *  The current thread will need its own buffer if there are multiple threads
 *  and the pattern contains the offset. Fio currently only has one pattern
 *  format specifier so we only need to check that one, but this may need to be
 *  changed if fio ever gains more pattern format specifiers.
 */
/*
 * [한국어]
 * pattern_need_buffer - 검증 스레드가 패턴 버퍼의 사본을 자체 소유해야 하는지 판정
 *
 * @td  : 잡 컨텍스트.
 * @return: true = verify_io_u_pattern 내부에서 pattern을 malloc + memcpy로 복제.
 *          false = td->o.verify_pattern 원본을 직접 사용 가능.
 *
 * 조건:
 *   (1) 비동기 검증(verify_async>0) 또는 잡이 스레드 모드(use_thread=1) —
 *       동일 td 객체를 여러 스레드가 공유.
 *   (2) verify_fmt이 비어있지 않음(format specifier 존재).
 *   (3) 첫 format specifier가 paste_blockoff — "블록 오프셋 삽입" 포맷.
 *
 * 왜: paste_format_inplace()가 원본 패턴 버퍼의 일부를 런타임에 "현재 io_u의
 * 오프셋"으로 덮어쓰므로, 여러 스레드가 동시에 다른 오프셋을 쓰면 race로
 * 잘못된 기대값이 생긴다. 각 스레드가 사본을 소유하면 격리가 보장된다.
 * fio는 현재 paste_blockoff만 사용하므로 desc->paste 포인터 비교만 확인.
 * 추후 다른 specifier 추가 시 이 함수의 비교 로직도 확장 필요.
 *
 * 사용처: verify_io_u_pattern 진입/종료 지점의 대칭 분기.
 */
static inline bool pattern_need_buffer(struct thread_data *td)
{
	return (td->o.verify_async || td->o.use_thread) &&
		td->o.verify_fmt_sz &&
		td->o.verify_fmt[0].desc->paste == paste_blockoff;
}

/*
 * [한국어]
 * verify_io_u_pattern - 패턴 기반 검증 함수 (verify=pattern / pattern_no_hdr / hdr_only+pattern)
 *
 * @hdr : 블록 헤더 (PATTERN_NO_HDR이면 실제로는 데이터 시작).
 * @vc  : 검증 컨테이너.
 * @return: 0=일치, EILSEQ=불일치(logged).
 *
 * 읽어온 데이터 영역이 쓰기 시 채운 verify_pattern과 같은지 확인한다.
 * 블록 시작 오프셋을 고려해 pattern[0..size-1]의 어느 인덱스부터 매칭해야
 * 하는지(mod) 계산하고, format specifier(%o 등)가 있으면 현재 io_u 오프셋을
 * 패턴에 재삽입한 뒤 check_pattern()으로 바이트 비교.
 *
 * 3가지 케이스 (주석에 설명된 대로):
 *   (1) verify_interval 미설정 + verify_pattern_interval 미설정 → 블록 전체 비교.
 *   (2) verify_interval 설정 + verify_pattern_interval 미설정 → interval 단위 비교.
 *   (3) verify_pattern_interval 설정 → 그 세그먼트 단위로 잘라 반복 비교 (각
 *       반복마다 paste_format_inplace로 오프셋 재삽입).
 *
 * 스레드 안전: pattern_need_buffer()가 true면 pattern 사본을 malloc으로 할당하고
 * 함수 종료 시 free한다 — paste_format_inplace의 in-place 수정이 다른 스레드와
 * race하지 않도록 격리.
 *
 * 실패 경로: check_pattern이 EILSEQ를 반환하면 vc->name="pattern"으로 표시하고
 * log_verify_failure로 덤프 트리거 후 루프 중단.
 *
 * 호출 체인: verify_io_u() (switch VERIFY_PATTERN/PATTERN_NO_HDR/HDR_ONLY+pattern_bytes)
 *   → [이 함수] → paste_format_inplace / check_pattern / log_verify_failure.
 */
static int verify_io_u_pattern(struct verify_header *hdr, struct vcont *vc)
{
	struct thread_data *td = vc->td;
	struct io_u *io_u = vc->io_u;
	char *buf, *pattern;
	unsigned int header_size = __hdr_size(td->o.verify);  /* [한국어] 블록 내 헤더 영역 크기(PATTERN_NO_HDR=0). */
	unsigned int len, mod, pattern_size, pattern_interval_mod, bytes_done = 0, bytes_todo;
	int rc;
	unsigned long long offset = io_u->offset;  /* [한국어] 원래 offset 저장 — 루프에서 io_u->offset을 임시 이동시킨다. */

	pattern = td->o.verify_pattern;            /* [한국어] 기본은 옵션 원본 패턴 포인터. */
	pattern_size = td->o.verify_pattern_bytes;
	assert(pattern_size != 0);                 /* [한국어] 패턴 검증에 진입했는데 크기 0이면 구성 버그 — 조기 abort. */

	/*
	 * Make this thread safe when verify_async is set and the verify
	 * pattern includes the offset.
	 */
	/* [한국어] 비동기 검증 + offset-포함 패턴 조건에서는 자체 사본을 사용. */
	if (pattern_need_buffer(td)) {
		pattern = malloc(pattern_size);
		assert(pattern);                   /* [한국어] 메모리 부족은 fatal. */
		memcpy(pattern, td->o.verify_pattern, pattern_size);
	}

	/* [한국어] 케이스 (1)(2): pattern_interval이 없으면 여기서 한 번만 format
	 * specifier 주입. 케이스 (3)은 각 세그먼트마다 매번 재주입해야 하므로 건너뜀. */
	if (!td->o.verify_pattern_interval) {
		(void)paste_format_inplace(pattern, pattern_size,
					   td->o.verify_fmt, td->o.verify_fmt_sz, io_u);
	}

	/*
	 * We have 3 cases here:
	 * 1. Compare the entire buffer if (1) verify_interval is not set and
	 * (2) verify_pattern_interval is not set
	 * 2. Compare the entire *verify_interval* if (1) verify_interval *is*
	 * set and (2) verify_pattern_interval is not set
	 * 3. Compare *verify_pattern_interval* segments or subsets thereof if
	 * (2) verify_pattern_interval is set
	 */

	/* [한국어] 데이터 영역 시작 = hdr 포인터 + 헤더 크기 (PATTERN_NO_HDR이면 header_size=0으로 hdr 그대로). */
	buf = (char *) hdr + header_size;
	len = get_hdr_inc(td, io_u) - header_size;  /* [한국어] 비교할 데이터 길이. */

	if (td->o.verify_pattern_interval) {
		/* [한국어] 케이스 (3): 세그먼트 방식. 블록 전체 오프셋(extent)을 구하고
		 * interval 안에서의 위치 mod를 구한 뒤 pattern_size 기준 mod도 별도 계산. */
		unsigned int extent = get_hdr_inc(td, io_u) * vc->hdr_num + header_size;
		pattern_interval_mod = extent % td->o.verify_pattern_interval;
		mod = pattern_interval_mod % pattern_size;
		/* [한국어] 이번 세그먼트에 남은 공간과 len 중 작은 값 비교. */
		bytes_todo = min(len, td->o.verify_pattern_interval - pattern_interval_mod);
		/* [한국어] io_u->offset을 현재 세그먼트 시작으로 이동 — paste_format_inplace
		 * 가 %o(=io->offset)를 이 값으로 주입. */
		io_u->offset += extent / td->o.verify_pattern_interval * td->o.verify_pattern_interval;
	} else {
		/* [한국어] 케이스 (1)(2): 블록 전체를 한 번에 비교. 패턴 인덱스는
		 * 파일 내 현재 오프셋 mod pattern_size. */
		mod = (get_hdr_inc(td, io_u) * vc->hdr_num + header_size) % pattern_size;
		bytes_todo = len;
		pattern_interval_mod = 0;
	}

	/* [한국어] 비교 루프: 블록 전체 또는 세그먼트별로 check_pattern 호출. */
	while (bytes_done < len) {
		if (td->o.verify_pattern_interval) {
			/* [한국어] 세그먼트마다 오프셋이 달라지므로 매번 재주입. */
			(void)paste_format_inplace(pattern, pattern_size,
					td->o.verify_fmt, td->o.verify_fmt_sz,
					io_u);
		}

		rc = check_pattern(buf, bytes_todo, mod, pattern_size, pattern, header_size);
		if (rc) {
			/* [한국어] 실패 보고 — 알고리즘 이름을 "pattern"으로 설정하고 중단. */
			vc->name = "pattern";
			log_verify_failure(hdr, vc);
			break;
		}

		/* [한국어] 다음 세그먼트 준비. 새 세그먼트는 패턴 정렬(mod=0)에서 시작 가정. */
		mod = 0;
		bytes_done += bytes_todo;
		buf += bytes_todo;
		io_u->offset += td->o.verify_pattern_interval;  /* [한국어] %o 재주입을 위해 오프셋 이동. */
		bytes_todo = min(len - bytes_done, td->o.verify_pattern_interval);
	}

	io_u->offset = offset;                /* [한국어] io_u->offset 복원 — 이후 경로에 부작용 없도록. */
	if (pattern_need_buffer(td))
		free(pattern);                /* [한국어] 사본을 만들었으면 해제. */
	return rc;
}

/*
 * [한국어]
 * verify_io_u_xxhash - xxHash(32비트) 기반 데이터 무결성 검증
 *
 * @hdr/@vc : 표준 검증 인자.
 * @return  : 0=일치, EILSEQ=불일치.
 *
 * xxHash는 Yann Collet의 비암호학적 초고속 해시(GB/s 처리). 암호 강도는 없지만
 * 충돌 저항은 CRC32보다 강하고 해시 함수로서의 분포가 고르다. vhdr_xxhash 4B.
 * XXH32_init(seed=1) → update → digest 3단계 API. 불일치 시 good/bad 4바이트
 * 쌍으로 vc에 기록.
 */
static int verify_io_u_xxhash(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);         /* [한국어] 데이터 영역 시작. */
	struct vhdr_xxhash *vh = hdr_priv(hdr);     /* [한국어] xxhash 다이제스트가 저장된 vhdr. */
	uint32_t hash;
	void *state;

	dprint(FD_VERIFY, "xxhash verify io_u %p, len %u\n", vc->io_u, hdr->len);

	state = XXH32_init(1);                      /* [한국어] seed=1 고정 — 쓰기 시와 동일해야 일치. */
	XXH32_update(state, p, hdr->len - hdr_size(vc->td, hdr));  /* [한국어] 데이터 영역 전체 해시. */
	hash = XXH32_digest(state);                 /* [한국어] 최종 32비트 해시 반환 (state 자동 free). */

	if (vh->hash == hash)
		return 0;                           /* [한국어] 일치 — 성공 반환. */

	/* [한국어] 불일치 — vc에 알고리즘 이름/기대/실제/길이 기록 후 실패 로그 + 덤프. */
	vc->name = "xxhash";
	vc->good_crc = &vh->hash;
	vc->bad_crc = &hash;
	vc->crc_len = sizeof(hash);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_sha3 - SHA-3 계열 검증의 공용 헬퍼 (224/256/384/512에서 공유)
 *
 * @hdr/@vc  : 표준 검증 인자.
 * @sha3_ctx : 호출자가 적절한 fio_sha3_<N>_init 으로 초기화한 컨텍스트 (sha 포인터
 *             세팅된 상태).
 * @sha      : 헤더에 저장되어 있던 "기대" 다이제스트.
 * @sha_size : 다이제스트 크기 (28/32/48/64 바이트).
 * @name     : 로그용 알고리즘 이름 ("sha3-256" 등).
 * @return   : 0=일치, EILSEQ=불일치.
 *
 * 4개 wrapper(_224/_256/_384/_512)가 컨텍스트 초기화와 기대값 포인터만 채우고
 * 공통 update+final+memcmp 로직을 이 함수에서 처리한다. SHA3는 SHA-2와 달리
 * Keccak(sponge) 기반이며 초기화 상수만 변종별로 다르다.
 */
static int verify_io_u_sha3(struct verify_header *hdr, struct vcont *vc,
			    struct fio_sha3_ctx *sha3_ctx, uint8_t *sha,
			    unsigned int sha_size, const char *name)
{
	void *p = io_u_verify_off(hdr, vc);

	dprint(FD_VERIFY, "%s verify io_u %p, len %u\n", name, vc->io_u, hdr->len);

	fio_sha3_update(sha3_ctx, p, hdr->len - hdr_size(vc->td, hdr));  /* [한국어] 데이터 영역 흡수. */
	fio_sha3_final(sha3_ctx);                                        /* [한국어] squeeze → ctx->sha 채움. */

	if (!memcmp(sha, sha3_ctx->sha, sha_size))
		return 0;                                                /* [한국어] 바이트 완전 일치. */

	vc->name = name;
	vc->good_crc = sha;
	vc->bad_crc = sha3_ctx->sha;
	vc->crc_len = sha_size;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_sha3_224 - SHA3-224 검증 (28바이트 다이제스트).
 * @return: 0=일치, EILSEQ=불일치.
 * init 상수는 fio_sha3_224_init에 hardcoded. 공용 verify_io_u_sha3가 나머지 수행.
 */
static int verify_io_u_sha3_224(struct verify_header *hdr, struct vcont *vc)
{
	struct vhdr_sha3_224 *vh = hdr_priv(hdr);     /* [한국어] SHA3-224 전용 vhdr. */
	uint8_t sha[SHA3_224_DIGEST_SIZE];            /* [한국어] 로컬 출력 버퍼 (스택). */
	struct fio_sha3_ctx sha3_ctx = {
		.sha = sha,                           /* [한국어] ctx의 출력 포인터 연결. */
	};

	fio_sha3_224_init(&sha3_ctx);                 /* [한국어] SHA3-224 초기화 (rate/capacity 설정). */

	return verify_io_u_sha3(hdr, vc, &sha3_ctx, vh->sha,
				SHA3_224_DIGEST_SIZE, "sha3-224");
}

/*
 * [한국어]
 * verify_io_u_sha3_256 - SHA3-256 검증 (32바이트 다이제스트).
 */
static int verify_io_u_sha3_256(struct verify_header *hdr, struct vcont *vc)
{
	struct vhdr_sha3_256 *vh = hdr_priv(hdr);
	uint8_t sha[SHA3_256_DIGEST_SIZE];
	struct fio_sha3_ctx sha3_ctx = {
		.sha = sha,
	};

	fio_sha3_256_init(&sha3_ctx);

	return verify_io_u_sha3(hdr, vc, &sha3_ctx, vh->sha,
				SHA3_256_DIGEST_SIZE, "sha3-256");
}

/*
 * [한국어]
 * verify_io_u_sha3_384 - SHA3-384 검증 (48바이트 다이제스트).
 */
static int verify_io_u_sha3_384(struct verify_header *hdr, struct vcont *vc)
{
	struct vhdr_sha3_384 *vh = hdr_priv(hdr);
	uint8_t sha[SHA3_384_DIGEST_SIZE];
	struct fio_sha3_ctx sha3_ctx = {
		.sha = sha,
	};

	fio_sha3_384_init(&sha3_ctx);

	return verify_io_u_sha3(hdr, vc, &sha3_ctx, vh->sha,
				SHA3_384_DIGEST_SIZE, "sha3-384");
}

/*
 * [한국어]
 * verify_io_u_sha3_512 - SHA3-512 검증 (64바이트 다이제스트).
 */
static int verify_io_u_sha3_512(struct verify_header *hdr, struct vcont *vc)
{
	struct vhdr_sha3_512 *vh = hdr_priv(hdr);
	uint8_t sha[SHA3_512_DIGEST_SIZE];
	struct fio_sha3_ctx sha3_ctx = {
		.sha = sha,
	};

	fio_sha3_512_init(&sha3_ctx);

	return verify_io_u_sha3(hdr, vc, &sha3_ctx, vh->sha,
				SHA3_512_DIGEST_SIZE, "sha3-512");
}

/*
 * [한국어]
 * verify_io_u_sha512 - SHA-512 (FIPS 180-4) 데이터 무결성 검증
 *
 * 64비트 워드 연산 기반으로 64비트 CPU에서 SHA-256보다 빠른 경향.
 * vhdr_sha512의 sha512 필드는 128B 버퍼(해시 실제 크기 64B + 여유). memcmp
 * 비교는 sizeof(sha512)=128 전체를 대상으로 한다.
 */
static int verify_io_u_sha512(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_sha512 *vh = hdr_priv(hdr);
	uint8_t sha512[128];                         /* [한국어] 출력 버퍼 (vhdr와 동일 크기). */
	struct fio_sha512_ctx sha512_ctx = {
		.buf = sha512,                       /* [한국어] 결과가 기록될 버퍼 포인터. */
	};

	dprint(FD_VERIFY, "sha512 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	fio_sha512_init(&sha512_ctx);                 /* [한국어] 초기 해시 값 H0..H7(64비트 8개) 로드. */
	fio_sha512_update(&sha512_ctx, p, hdr->len - hdr_size(vc->td, hdr));  /* [한국어] 메시지 블록 흡수. */
	fio_sha512_final(&sha512_ctx);                /* [한국어] 패딩 + 마지막 블록 처리 → buf에 기록. */

	if (!memcmp(vh->sha512, sha512_ctx.buf, sizeof(sha512)))
		return 0;

	vc->name = "sha512";
	vc->good_crc = vh->sha512;
	vc->bad_crc = sha512_ctx.buf;
	vc->crc_len = sizeof(vh->sha512);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_sha256 - SHA-256 (FIPS 180-4) 데이터 무결성 검증.
 * vhdr_sha256의 sha256 필드는 64B 버퍼(해시 실제 32B + 여유).
 */
static int verify_io_u_sha256(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_sha256 *vh = hdr_priv(hdr);
	uint8_t sha256[64];
	struct fio_sha256_ctx sha256_ctx = {
		.buf = sha256,
	};

	dprint(FD_VERIFY, "sha256 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	fio_sha256_init(&sha256_ctx);
	fio_sha256_update(&sha256_ctx, p, hdr->len - hdr_size(vc->td, hdr));
	fio_sha256_final(&sha256_ctx);

	if (!memcmp(vh->sha256, sha256_ctx.buf, sizeof(sha256)))
		return 0;

	vc->name = "sha256";
	vc->good_crc = vh->sha256;
	vc->bad_crc = sha256_ctx.buf;
	vc->crc_len = sizeof(vh->sha256);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_sha1 - SHA-1 (RFC 3174) 데이터 무결성 검증.
 * vhdr_sha1의 sha1은 uint32_t[5] (160비트). 암호학적으로는 취약하지만 무결성
 * 검증(우발적 손상 감지) 용도로는 아직 실용적.
 */
static int verify_io_u_sha1(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_sha1 *vh = hdr_priv(hdr);
	uint32_t sha1[5];                             /* [한국어] 160비트 다이제스트 = 32비트 5개. */
	struct fio_sha1_ctx sha1_ctx = {
		.H = sha1,                            /* [한국어] SHA-1 내부 상태 배열 H0..H4. */
	};

	dprint(FD_VERIFY, "sha1 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	fio_sha1_init(&sha1_ctx);
	fio_sha1_update(&sha1_ctx, p, hdr->len - hdr_size(vc->td, hdr));
	fio_sha1_final(&sha1_ctx);

	if (!memcmp(vh->sha1, sha1_ctx.H, sizeof(sha1)))
		return 0;

	vc->name = "sha1";
	vc->good_crc = vh->sha1;
	vc->bad_crc = sha1_ctx.H;
	vc->crc_len = sizeof(vh->sha1);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_crc7 - CRC7 검증 (MMC/SD 카드 CRC 표준, 7비트).
 * vhdr_crc7 = 1바이트. 매우 작은 검증 오버헤드. fio_crc7은 테이블 룩업 구현.
 */
static int verify_io_u_crc7(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc7 *vh = hdr_priv(hdr);
	unsigned char c;                              /* [한국어] CRC7 결과 (하위 7비트 유효). */

	dprint(FD_VERIFY, "crc7 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc7(p, hdr->len - hdr_size(vc->td, hdr));  /* [한국어] 데이터 영역 CRC7 계산. */

	if (c == vh->crc7)
		return 0;

	vc->name = "crc7";
	vc->good_crc = &vh->crc7;
	vc->bad_crc = &c;
	vc->crc_len = 1;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_crc16 - CRC16 검증 (CCITT 또는 XMODEM 다항식).
 * vhdr_crc16 = 2바이트. 짧은 블록에 적당하나 대용량 검증에는 충돌 확률 상승.
 */
static int verify_io_u_crc16(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc16 *vh = hdr_priv(hdr);
	unsigned short c;

	dprint(FD_VERIFY, "crc16 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc16(p, hdr->len - hdr_size(vc->td, hdr));

	if (c == vh->crc16)
		return 0;

	vc->name = "crc16";
	vc->good_crc = &vh->crc16;
	vc->bad_crc = &c;
	vc->crc_len = 2;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_crc64 - CRC64 검증 (ISO/ECMA 계열 다항식).
 * vhdr_crc64 = 8바이트. 큰 블록에서 CRC32의 충돌 확률을 줄이고 싶을 때 선택.
 */
static int verify_io_u_crc64(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc64 *vh = hdr_priv(hdr);
	unsigned long long c;

	dprint(FD_VERIFY, "crc64 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc64(p, hdr->len - hdr_size(vc->td, hdr));

	if (c == vh->crc64)
		return 0;

	vc->name = "crc64";
	vc->good_crc = &vh->crc64;
	vc->bad_crc = &c;
	vc->crc_len = 8;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_crc32 - CRC32 검증 (IEEE 802.3 이더넷 다항식).
 * vhdr_crc32 = 4바이트. 클래식 CRC32 (zlib/PNG에서 사용). CRC32C와는 다항식이 다름.
 */
static int verify_io_u_crc32(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc32 *vh = hdr_priv(hdr);
	uint32_t c;

	dprint(FD_VERIFY, "crc32 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc32(p, hdr->len - hdr_size(vc->td, hdr));

	if (c == vh->crc32)
		return 0;

	vc->name = "crc32";
	vc->good_crc = &vh->crc32;
	vc->bad_crc = &c;
	vc->crc_len = 4;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_crc32c - CRC32C (Castagnoli) 검증 (iSCSI/SCTP/ext4 메타데이터 표준).
 *
 * fio_crc32c()는 fio_verify_init()에서 crc32c_intel_probe/crc32c_arm64_probe를
 * 통해 SSE4.2 CRC32C 명령(Intel) 또는 ARMv8 CRC32 확장(AArch64)을 감지하면
 * 하드웨어 가속 경로를, 아니면 소프트웨어 테이블 룩업을 사용하도록 디스패치되어
 * 있다. VERIFY_CRC32C와 VERIFY_CRC32C_INTEL은 현재 같은 함수로 수렴 (둘 다
 * 런타임 탐지를 이용하므로 구분 의미는 레거시). vhdr_crc32 = 4바이트 (CRC32과 공유).
 */
static int verify_io_u_crc32c(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc32 *vh = hdr_priv(hdr);
	uint32_t c;

	dprint(FD_VERIFY, "crc32c verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc32c(p, hdr->len - hdr_size(vc->td, hdr));  /* [한국어] HW/SW 자동 디스패치. */

	if (c == vh->crc32)
		return 0;

	vc->name = "crc32c";
	vc->good_crc = &vh->crc32;
	vc->bad_crc = &c;
	vc->crc_len = 4;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u_md5 - MD5 (RFC 1321) 해시 검증.
 *
 * vhdr_md5는 uint32_t[4] (128비트). MD5_HASH_WORDS = 4 (crc/md5.h 정의). 암호학적
 * 으로 완전히 깨졌지만, 우발적 손상 감지 용도(스토리지 ECC 보조)로는 여전히 유효.
 * fio_md5_ctx.hash가 결과를 받을 버퍼 포인터.
 */
static int verify_io_u_md5(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_md5 *vh = hdr_priv(hdr);
	uint32_t hash[MD5_HASH_WORDS];                /* [한국어] 128비트 다이제스트 로컬 버퍼. */
	struct fio_md5_ctx md5_ctx = {
		.hash = hash,
	};

	dprint(FD_VERIFY, "md5 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	fio_md5_init(&md5_ctx);                                   /* [한국어] A/B/C/D 초기값 로드. */
	fio_md5_update(&md5_ctx, p, hdr->len - hdr_size(vc->td, hdr));
	fio_md5_final(&md5_ctx);                                  /* [한국어] 패딩 + 최종 블록 처리. */

	if (!memcmp(vh->md5_digest, md5_ctx.hash, sizeof(hash)))
		return 0;

	vc->name = "md5";
	vc->good_crc = vh->md5_digest;
	vc->bad_crc = md5_ctx.hash;
	vc->crc_len = sizeof(hash);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * Push IO verification to a separate thread
 */
/*
 * [한국어]
 * verify_io_u_async - 읽은 데이터의 검증을 비동기 워커 스레드에 오프로드
 *
 * @td       : 잡 컨텍스트.
 * @io_u_ptr : 검증 대상 io_u의 더블 포인터. 함수 종료 시 *io_u_ptr=NULL로 설정해
 *             호출자(do_verify)가 이 io_u를 put_io_u 하지 않도록 신호. 실제
 *             put_io_u는 verify_async_thread 내부에서 수행.
 * @return   : 항상 0 (성공). 오프로드 자체는 실패할 일이 없음.
 *
 * 메인 잡 스레드는 무거운 해시(SHA/MD5 등) 계산을 본인이 직접 하면 그 사이 I/O
 * 제출이 정체된다. verify_async=N > 0이면 N개의 워커가 미리 만들어지고, 이
 * 함수는 io_u를 단순히 td->verify_list 뒤에 추가 + 조건변수로 워커 1개 깨움.
 * 메인 스레드는 즉시 다음 io_u로 진행 → 처리량 향상.
 *
 * 주의:
 *   (1) IO_U_F_IN_CUR_DEPTH 플래그가 붙은 io_u는 "현재 in-flight depth에 포함됨"을
 *       뜻한다. 오프로드 이후에는 io_u가 워커 소유로 넘어가므로 메인 스레드의
 *       in-flight 카운터에서 빼야 한다 (cur_depth--).
 *   (2) put_file_log는 파일 참조 카운트를 줄이고 필요 시 로그 기록. 워커가 검증
 *       이후 put_io_u를 별도로 수행하므로 여기서 로그만 남김.
 *   (3) pthread_cond_signal 1회는 1개 워커만 깨움 — 워커가 flist_splice_init로
 *       전체 큐를 꺼내가므로 이 정도로 충분. 긴급 broadcast는 종료 시에만.
 *
 * 실행 컨텍스트: 잡 스레드 단독 (do_verify 경로). td->io_u_lock으로 verify_list
 * 생산자-소비자 동기화.
 *
 * 호출 체인: backend.c::do_verify() → [이 함수] → verify_async_thread (깨어남).
 */
int verify_io_u_async(struct thread_data *td, struct io_u **io_u_ptr)
{
	struct io_u *io_u = *io_u_ptr;

	pthread_mutex_lock(&td->io_u_lock);     /* [한국어] verify_list 및 cur_depth 보호. */

	if (io_u->file)
		put_file_log(td, io_u->file);   /* [한국어] 파일 참조 로그 기록 (ref dec는 워커가 수행). */

	/* [한국어] 오프로드 시점에서 io_u가 cur_depth에 집계되어 있었다면 빼줌 —
	 * 그렇지 않으면 in-flight 카운팅이 영구히 틀어진다. */
	if (io_u->flags & IO_U_F_IN_CUR_DEPTH) {
		td->cur_depth--;
		io_u_clear(td, io_u, IO_U_F_IN_CUR_DEPTH);
	}
	flist_add_tail(&io_u->verify_list, &td->verify_list);  /* [한국어] FIFO 큐 꼬리에 추가. */
	*io_u_ptr = NULL;                      /* [한국어] 호출자가 이 io_u를 이중 해제하지 않도록 무효화. */

	pthread_cond_signal(&td->verify_cond); /* [한국어] 워커 1명 깨움 — 큐에 항목이 있음을 알림. */
	pthread_mutex_unlock(&td->io_u_lock);
	return 0;
}

/*
 * Thanks Rusty, for spending the time so I don't have to.
 *
 * http://rusty.ozlabs.org/?p=560
 */
/*
 * [한국어]
 * mem_is_zero - 메모리 영역 전체가 0인지 빠르게 확인 (Rusty Russell 트릭)
 *
 * @data   : 검사할 버퍼 시작.
 * @length : 바이트 수.
 * @return : 1 = 모두 0, 0 = 어딘가 비영 바이트 존재.
 *
 * 트릭: 앞 16바이트만 직접 비교해 0임을 확인한 뒤, 나머지 (length-16) 바이트를
 * 자기 자신의 [0..16] 영역과 memcmp한다. 만약 나머지가 모두 0이면 두 영역은
 * 같은 "0 바이트 반복"이 되어 memcmp가 0을 반환. memcmp는 SIMD 최적화가 되어
 * 있어 순차 바이트 루프보다 매우 빠르다. 첫 16B는 워밍업 + 임의 정렬 문제 회피.
 *
 * 사용처: verify_trimmed_io_u — TRIM 후 읽기가 0으로 돌아오는지 확인.
 */
static int mem_is_zero(const void *data, size_t length)
{
	const unsigned char *p = data;
	size_t len;

	/* Check first 16 bytes manually */
	/* [한국어] 1단계: 앞 16바이트는 손수 루프로 확인 — memcmp에 넘기기 전
	 * "0으로 채워진 비교 기준 영역"을 확보. length가 16보다 작으면 전부 여기서 판정. */
	for (len = 0; len < 16; len++) {
		if (!length)
			return 1;       /* [한국어] 버퍼 소진 — 남은 건 전혀 없고 지금까지 모두 0. */
		if (*p)
			return 0;       /* [한국어] 0이 아닌 바이트 발견 — 즉시 실패. */
		p++;
		length--;
	}

	/* Now we know that's zero, memcmp with self. */
	/* [한국어] 2단계: data[0..15]가 0임을 아는 상태에서 나머지 length 바이트를
	 * data(=같은 영역)와 비교. 완벽히 같으면(=전부 0) memcmp==0 → return 1. */
	return memcmp(data, p, length) == 0;
}

/*
 * [한국어]
 * mem_is_zero_slow - 느린 바이트별 0 검사. 첫 비영 바이트의 오프셋도 반환.
 *
 * @data   : 버퍼.
 * @length : 길이.
 * @offset : [out] 첫 비영 바이트의 오프셋. 전부 0이면 length와 같음.
 * @return : 1 = 전부 0, 0 = 비영 발견.
 *
 * mem_is_zero가 "거짓"을 반환한 뒤 "정확히 어디서 틀어졌는지"를 사용자에게
 * 보여주기 위해 호출. 성능보다 진단 정보가 우선이므로 단순 바이트 루프.
 */
static int mem_is_zero_slow(const void *data, size_t length, size_t *offset)
{
	const unsigned char *p = data;

	*offset = 0;
	while (length) {
		if (*p)
			break;                  /* [한국어] 첫 비영 바이트 발견 — 현재 offset이 위치. */
		(*offset)++;
		length--;
		p++;
	}

	return !length;                         /* [한국어] length==0으로 끝났으면 전부 0. */
}

/*
 * [한국어]
 * verify_trimmed_io_u - TRIM된 영역이 정말 0으로 읽히는지 검증
 *
 * @td   : 잡 컨텍스트 — trim_zero 옵션 확인용.
 * @io_u : 검증 대상 (io_u->flags에 IO_U_F_TRIMMED 세팅되어 있음).
 * @return: 0 = 전부 0 (정상) 또는 trim_zero 비활성 시, EILSEQ = 비영 발견.
 *
 * TRIM(DISCARD/UNMAP) 이후 대부분의 SSD는 DRAT/RZAT 플래그에 따라 해당 영역을
 * 읽으면 0을 반환해야 한다(NVMe DLFEAT, SATA RZAT). trim_zero=1이 설정되면
 * fio가 이를 강제 검증한다. 검증 실패 시 블록 오프셋과 첫 비영 위치를 로그.
 *
 * 호출 체인: verify_io_u() → (io_u->flags에 IO_U_F_TRIMMED) → [이 함수].
 */
static int verify_trimmed_io_u(struct thread_data *td, struct io_u *io_u)
{
	size_t offset;

	if (!td->o.trim_zero)
		return 0;               /* [한국어] 옵션 꺼짐 — 검증 생략. */

	if (mem_is_zero(io_u->buf, io_u->buflen))
		return 0;               /* [한국어] 빠른 검사 통과 — 전부 0. */

	/* [한국어] 실패 — 느린 함수로 정확한 비영 위치 찾고 보고. */
	mem_is_zero_slow(io_u->buf, io_u->buflen, &offset);

	log_err("trim: verify failed at file %s offset %llu, length %llu"
		", block offset %lu\n",
			io_u->file->file_name, io_u->verify_offset, io_u->buflen,
			(unsigned long) offset);
	return EILSEQ;
}

/*
 * [한국어]
 * verify_header - 검증 헤더(고정 44바이트 영역)의 7가지 항목을 순차 검사
 *
 * @io_u    : 대상 io_u. 기대값(verify_offset, rand_seed, numberio) 참조.
 * @td      : 잡 컨텍스트 — 옵션과 워크로드 특성 검사.
 * @hdr     : 검사할 verify_header (io_u->buf 내부).
 * @hdr_num : 이 io_u 내에서 몇 번째 블록의 헤더인지 (0-based).
 * @hdr_len : 기대 헤더/블록 전체 길이 (get_hdr_inc 값).
 * @return  : 0 = 정상, EILSEQ = 검증 실패 (상세 로그 + 덤프).
 *
 * 검증 순서 (실패 시 즉시 err로 점프):
 *   (1) magic == FIO_HDR_MAGIC (0xacca) — 이 영역이 실제로 fio 헤더인지.
 *   (2) version == VERIFY_HEADER_VERSION (0x81) — 크로스 버전 포맷 불일치 감지.
 *       최상위 비트를 세운 것은 구 버전(비 versioned) 헤더와 구별하려고.
 *   (3) hdr->len == hdr_len — 블록 크기 일치.
 *   (4) [옵션] verify_header_seed 설정 시 rand_seed 일치 (데이터 재현 시드 보호).
 *   (5) hdr->offset == io_u->verify_offset + hdr_num*verify_interval — 블록이
 *       원래 쓰여진 위치가 지금 읽는 위치와 같은지 (읽기 오정렬 감지).
 *   (6) [조건부] numberio 일치 — td_write AND bs 고정 AND time_based=0 AND
 *       verify_write_sequence=1일 때만 유효. 읽기 전용/가변 bs/시간 기반에서는
 *       "이 블록을 마지막으로 어떤 순서에 썼는지" 추적 불가.
 *   (7) CRC32C(헤더 바이트 0..crc32 오프셋) == hdr->crc32 — 헤더 자체의
 *       무결성. offsetof()로 crc32 직전까지 해시해 자기 자신을 포함하지 않음.
 *
 * 에러 경로: err 라벨로 공통 로그(파일명, 오프셋, 길이) + verify_dump 시
 * "hdr_fail" 접미사의 덤프 파일 생성 후 EILSEQ 반환.
 *
 * 실행 컨텍스트: verify_io_u의 블록 루프 내에서 호출. 잡 스레드 또는 검증 워커.
 *
 * 호출 체인: verify_io_u() → [이 함수] → (실패 시) dump_buf().
 */
static int verify_header(struct io_u *io_u, struct thread_data *td,
			 struct verify_header *hdr, unsigned int hdr_num,
			 unsigned int hdr_len)
{
	void *p = hdr;       /* [한국어] CRC 계산용 raw 포인터. */
	uint32_t crc;

	/* [한국어] (1) 매직 넘버 확인. 전혀 fio 헤더가 아닌 영역(0 패턴/쓰레기)을
	 * 일찍 감지 — 가장 흔한 실패 케이스. */
	if (hdr->magic != FIO_HDR_MAGIC) {
		log_err("verify: bad magic header %x, wanted %x",
			hdr->magic, FIO_HDR_MAGIC);
		goto err;
	}
	/* [한국어] (2) 헤더 버전 — 구 버전과 신 버전 교차 검증 방지. */
	if (hdr->version != VERIFY_HEADER_VERSION) {
		log_err("verify: unsupported header version %x, wanted %x. Are you trying to verify across versions of fio?",
			hdr->version, VERIFY_HEADER_VERSION);
		goto err;
	}
	/* [한국어] (3) 블록 길이 일치. 쓰기 시 bs와 읽기 시 bs가 다르면 발생. */
	if (hdr->len != hdr_len) {
		log_err("verify: bad header length %u, wanted %u",
			hdr->len, hdr_len);
		goto err;
	}
	/* [한국어] (4) verify_header_seed 옵션 시 RNG 시드 일치. 랜덤 모드에서는
	 * 데이터 재생성 기준이 시드이므로 이 필드 보호가 중요. */
	if (td->o.verify_header_seed && (hdr->rand_seed != io_u->rand_seed)) {
		log_err("verify: bad header rand_seed %"PRIu64
			", wanted %"PRIu64,
			hdr->rand_seed, io_u->rand_seed);
		goto err;
	}
	/* [한국어] (5) 블록 오프셋 일치 — misplaced write 감지 (FS/LBA 매핑 오류 등). */
	if (hdr->offset != io_u->verify_offset + hdr_num * td->o.verify_interval) {
		log_err("verify: bad header offset %"PRIu64
			", wanted %llu",
			hdr->offset, io_u->verify_offset);
		goto err;
	}

	/*
	 * For read-only workloads, the program cannot be certain of the
	 * last numberio written to a block. Checking of numberio will be
	 * done only for workloads that write data.  For verify_only or
	 * any mode de-selecting verify_write_sequence, numberio check is
	 * skipped.
	 */
	/* [한국어] (6) numberio 검증 — 3중 조건 AND: 쓰기 잡이고, 블록 크기가
	 * 고정(min==max)이며, 시간 기반(time_based=1)이 아닐 때만 수행.
	 * verify_write_sequence=1 추가 조건 — 사용자가 명시 opt-in 시에만.
	 * 시간 기반/가변 bs에서는 "마지막 쓰기 순번"이 예측 불가. */
	if (td_write(td) && (td_min_bs(td) == td_max_bs(td)) &&
	    !td->o.time_based)
		if (td->o.verify_write_sequence)
			if (hdr->numberio != io_u->numberio) {
				log_err("verify: bad header numberio %"PRIu64
					", wanted %"PRIu64,
					hdr->numberio, io_u->numberio);
				goto err;
			}

	/* [한국어] (7) 헤더 자체 무결성 — crc32 필드 직전까지의 바이트에 CRC32C 적용.
	 * offsetof(struct verify_header, crc32) = 헤더 내 crc32 필드의 오프셋 ≈ 40.
	 * 헤더 자체가 변조되었거나 비트 플립이 발생했으면 여기서 감지. */
	crc = fio_crc32c(p, offsetof(struct verify_header, crc32));
	if (crc != hdr->crc32) {
		log_err("verify: bad header crc %x, calculated %x",
			hdr->crc32, crc);
		goto err;
	}
	return 0;

err:
	/* [한국어] 공통 실패 로그 — 어느 파일의 어느 오프셋에서 실패했는지.
	 * 각 검사에서 세부 로그를 먼저 찍었으므로 여기서는 "장소" 정보만 추가. */
	log_err(" at file %s offset %llu, length %u"
		" (requested block: offset=%llu, length=%llu)\n",
		io_u->file->file_name,
		io_u->verify_offset + hdr_num * hdr_len, hdr_len,
		io_u->verify_offset, io_u->buflen);

	/* [한국어] verify_dump 시 실패 헤더 블록을 "hdr_fail" 접미사 파일로 저장.
	 * 데이터 영역은 __dump_verify_buffers에서 "received"/"expected"로 별도 덤프. */
	if (td->o.verify_dump)
		dump_buf(p, hdr_len, io_u->verify_offset + hdr_num * hdr_len,
				"hdr_fail", io_u->file);

	return EILSEQ;
}

/*
 * [한국어]
 * verify_io_u - 메인 검증 디스패처. 읽어온 io_u 데이터의 무결성을 블록별로 검증
 *
 * @td       : 잡 컨텍스트.
 * @io_u_ptr : 검증 대상 io_u의 더블 포인터. 성공/실패 관계없이 io_u 자체는 해제하지 않음
 *             (호출자가 put_io_u 수행).
 * @return   : 0 = 모든 블록 일치, EILSEQ/EINVAL = 실패 (verify_fatal 시 td 종료 마킹).
 *
 * 조기 종료 4경로:
 *   (1) verify=NULL (pretend) 또는 io_u가 READ가 아닌 경우 — 검증 건너뜀.
 *   (2) FIO_FAKEIO 엔진 (null.c 등) — 실제 데이터가 없으니 "가짜 성공".
 *   (3) io_u->flags & IO_U_F_VER_IN_DEV — 디바이스가 이미 PI/T10 DIF 등으로
 *       하드웨어 검증을 완료한 상태. 소프트 검증 불필요.
 *   (4) io_u->flags & IO_U_F_TRIMMED — verify_trimmed_io_u로 위임 (0 채움 확인).
 *
 * 메인 루프: io_u->buf를 hdr_inc 간격으로 순회하며 각 블록마다:
 *   (a) verify_offset 설정 시 memswp로 헤더를 원위치로 복원 (쓰기 시 스왑 대응).
 *   (b) VERIFY_PATTERN_NO_HDR 제외하고 verify_header() 호출 — 헤더 7항목 점검.
 *   (c) 검증 알고리즘 결정: verify_type = (옵션이 NONE이면 hdr의 타입 / 아니면
 *       옵션 값). 즉 verify=none이지만 데이터에 헤더가 있으면 헤더가 알려주는
 *       알고리즘으로 검증하는 autodetect 기능.
 *   (d) switch(verify_type) → 해당 verify_io_u_<alg> 호출.
 *   (e) 실패 + verify_fatal=1 이면 루프 탈출.
 *
 * 미디어-요청 알고리즘 불일치: (ret && verify_type != hdr->verify_type) 시
 * 사용자에게 경고 — 예를 들어 CRC32로 썼는데 MD5로 검증하는 경우.
 *
 * verify_fatal 처리: done 라벨에서 실패 + verify_fatal 시 fio_mark_td_terminate
 * 로 잡 종료 플래그 설정 — backend.c 루프가 다음 iteration에서 인식.
 *
 * 실행 컨텍스트: 동기 검증 경로는 잡 스레드, 오프로드 경로는 verify_async_thread.
 *
 * 호출 체인:
 *   backend.c::do_verify() → io_u_sync_complete → [이 함수]  (인라인)
 *   verify_async_thread → [이 함수]                          (오프로드)
 */
int verify_io_u(struct thread_data *td, struct io_u **io_u_ptr)
{
	struct verify_header *hdr;
	struct io_u *io_u = *io_u_ptr;
	unsigned int header_size, hdr_inc, hdr_num = 0;
	void *p;
	int ret;

	/* [한국어] (1) VERIFY_NULL(무-검증 모드) 또는 READ가 아닌 io_u(WRITE의 완료 등)는 검증 대상 아님. */
	if (td->o.verify == VERIFY_NULL || io_u->ddir != DDIR_READ)
		return 0;
	/*
	 * If the IO engine is faking IO (like null), then just pretend
	 * we verified everything.
	 */
	/* [한국어] (2) FIO_FAKEIO 플래그 엔진(null, cpu, filestat) — 실제 데이터 전송이 없어 검증 무의미. */
	if (td_ioengine_flagged(td, FIO_FAKEIO))
		return 0;

	/*
	 * If data has already been verified from the device, we can skip
	 * the actual verification phase here.
	 */
	/* [한국어] (3) 디바이스가 PI/T10 DIF 등으로 이미 검증을 끝냈음을 표시하면 재검증 불필요. */
	if (io_u->flags & IO_U_F_VER_IN_DEV)
		return 0;

	/* [한국어] (4) TRIM된 블록 — verify_trimmed_io_u로 "0 반환" 여부 확인. */
	if (io_u->flags & IO_U_F_TRIMMED) {
		ret = verify_trimmed_io_u(td, io_u);
		goto done;
	}

	hdr_inc = get_hdr_inc(td, io_u);        /* [한국어] io_u 내 블록 간격 결정. */

	ret = 0;
	/* [한국어] io_u 버퍼를 블록 단위로 순회. hdr_num은 현재 블록 인덱스. */
	for (p = io_u->buf; p < io_u->buf + io_u->buflen;
	     p += hdr_inc, hdr_num++) {
		/* [한국어] 이 블록 검증용 컨테이너를 스택에 구성. */
		struct vcont vc = {
			.io_u		= io_u,
			.hdr_num	= hdr_num,
			.td		= td,
		};
		unsigned int verify_type;

		/* [한국어] 이전 블록이 실패했고 verify_fatal=1이면 나머지 블록 스킵. */
		if (ret && td->o.verify_fatal)
			break;

		/* [한국어] verify_offset 설정 시, 쓰기 때 헤더와 패턴을 교환해 두었던 것을
		 * 검증 전에 원위치로 되돌린다. memswp 양방향 대칭으로 역스왑 성립. */
		header_size = __hdr_size(td->o.verify);
		if (td->o.verify_offset)
			memswp(p, p + td->o.verify_offset, header_size);
		hdr = p;                        /* [한국어] 이제 p는 (역스왑 후의) 헤더 시작. */

		/* [한국어] PATTERN_NO_HDR 제외하고 헤더 7항목 점검. 헤더 실패는 즉시 반환. */
		if (td->o.verify != VERIFY_PATTERN_NO_HDR) {
			ret = verify_header(io_u, td, hdr, hdr_num, hdr_inc);
			if (ret)
				return ret;
		}

		/* [한국어] verify 옵션이 NONE이면 헤더가 알려주는 타입으로 자동 분기.
		 * "어떤 알고리즘으로 쓰였는지 모를 때" 유용 — 디스크 전체 스캔 등. */
		if (td->o.verify != VERIFY_NONE)
			verify_type = td->o.verify;
		else
			verify_type = hdr->verify_type;

		/* [한국어] 알고리즘별 검증 디스패치. 각 case는 해당 verify_io_u_<alg>
		 * 래퍼 호출. 반환값(ret)은 0 또는 EILSEQ. */
		switch (verify_type) {
		case VERIFY_HDR_ONLY:
			/* Header is always verified, check if pattern is left
			 * for verification. */
			/* [한국어] HDR_ONLY는 헤더만 보장하되 verify_pattern_bytes가
			 * 있으면 패턴도 부가 검증. 쓰기 시 pattern을 삽입했다면
			 * 읽기 시에도 일관성 확보. */
			if (td->o.verify_pattern_bytes)
				ret = verify_io_u_pattern(hdr, &vc);
			break;
		case VERIFY_MD5:
			ret = verify_io_u_md5(hdr, &vc);
			break;
		case VERIFY_CRC64:
			ret = verify_io_u_crc64(hdr, &vc);
			break;
		case VERIFY_CRC32C:
		case VERIFY_CRC32C_INTEL:
			/* [한국어] 두 타입 모두 fio_crc32c 공용 — 런타임에 HW/SW 분기. */
			ret = verify_io_u_crc32c(hdr, &vc);
			break;
		case VERIFY_CRC32:
			ret = verify_io_u_crc32(hdr, &vc);
			break;
		case VERIFY_CRC16:
			ret = verify_io_u_crc16(hdr, &vc);
			break;
		case VERIFY_CRC7:
			ret = verify_io_u_crc7(hdr, &vc);
			break;
		case VERIFY_SHA256:
			ret = verify_io_u_sha256(hdr, &vc);
			break;
		case VERIFY_SHA512:
			ret = verify_io_u_sha512(hdr, &vc);
			break;
		case VERIFY_SHA3_224:
			ret = verify_io_u_sha3_224(hdr, &vc);
			break;
		case VERIFY_SHA3_256:
			ret = verify_io_u_sha3_256(hdr, &vc);
			break;
		case VERIFY_SHA3_384:
			ret = verify_io_u_sha3_384(hdr, &vc);
			break;
		case VERIFY_SHA3_512:
			ret = verify_io_u_sha3_512(hdr, &vc);
			break;
		case VERIFY_XXHASH:
			ret = verify_io_u_xxhash(hdr, &vc);
			break;
		case VERIFY_SHA1:
			ret = verify_io_u_sha1(hdr, &vc);
			break;
		case VERIFY_PATTERN:
		case VERIFY_PATTERN_NO_HDR:
			/* [한국어] 두 패턴 모드 모두 동일 함수로 처리 — 내부에서 hdr_size 분기. */
			ret = verify_io_u_pattern(hdr, &vc);
			break;
		default:
			/* [한국어] 알 수 없는 타입 — 구성/전송 오류. EINVAL로 즉시 종료. */
			log_err("Bad verify type %u\n", hdr->verify_type);
			ret = EINVAL;
		}

		/* [한국어] 실패 시 미디어에 기록된 타입과 요청 타입이 달랐다면 사용자에게
		 * 재확인 유도. PATTERN_NO_HDR은 hdr에 타입 정보가 없으므로 제외. */
		if (ret && verify_type != hdr->verify_type && verify_type != VERIFY_PATTERN_NO_HDR)
			log_err("fio: verify type mismatch (%u media, %u given)\n",
					hdr->verify_type, verify_type);
	}

done:
	/* [한국어] 치명적 실패 처리: verify_fatal=1이면 잡 전체를 종료 예약.
	 * backend.c의 runstate 루프가 다음 iteration에서 TD_EXITED로 전이. */
	if (ret && td->o.verify_fatal)
		fio_mark_td_terminate(td);

	return ret;
}

/* ================================================================
 * [한국어] 체크섬 "쓰기" 경로 — 블록 쓰기 시 데이터 영역의 체크섬을 계산해
 * 해당 vhdr_<alg>에 저장. verify_io_u_<alg>와 쌍을 이루며, 동일 알고리즘이
 * 동일 입력에 동일 결과를 내야 검증이 성립.
 * ================================================================ */

/*
 * [한국어]
 * fill_xxhash - 데이터 영역의 xxHash(32b)를 계산해 vhdr_xxhash에 저장.
 * 검증 시 verify_io_u_xxhash와 동일한 seed=1로 재계산해야 일치.
 */
static void fill_xxhash(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_xxhash *vh = hdr_priv(hdr);
	void *state;

	state = XXH32_init(1);                     /* [한국어] seed 고정 (검증과 동일). */
	XXH32_update(state, p, len);
	vh->hash = XXH32_digest(state);            /* [한국어] 결과를 vhdr에 저장, state 자동 해제. */
}

/*
 * [한국어]
 * fill_sha3 - SHA-3 공용 update+final 헬퍼. init/ctx 준비는 호출자 몫.
 * 4개 wrapper(fill_sha3_{224,256,384,512})가 각 init 함수를 고르고 이 함수를 호출.
 */
static void fill_sha3(struct fio_sha3_ctx *sha3_ctx, void *p, unsigned int len)
{
	fio_sha3_update(sha3_ctx, p, len);
	fio_sha3_final(sha3_ctx);
}

/*
 * [한국어]
 * fill_sha3_224 - SHA3-224 계산 후 vhdr_sha3_224.sha에 저장.
 * ctx.sha 포인터를 vh->sha로 바로 연결해 final이 직접 기록.
 */
static void fill_sha3_224(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha3_224 *vh = hdr_priv(hdr);
	struct fio_sha3_ctx sha3_ctx = {
		.sha = vh->sha,            /* [한국어] 출력 포인터를 vhdr 내부로 직결. */
	};

	fio_sha3_224_init(&sha3_ctx);
	fill_sha3(&sha3_ctx, p, len);
}

/* [한국어] fill_sha3_256 - SHA3-256 계산. vh->sha에 직접 기록. */
static void fill_sha3_256(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha3_256 *vh = hdr_priv(hdr);
	struct fio_sha3_ctx sha3_ctx = {
		.sha = vh->sha,
	};

	fio_sha3_256_init(&sha3_ctx);
	fill_sha3(&sha3_ctx, p, len);
}

/* [한국어] fill_sha3_384 - SHA3-384 계산. vh->sha에 직접 기록. */
static void fill_sha3_384(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha3_384 *vh = hdr_priv(hdr);
	struct fio_sha3_ctx sha3_ctx = {
		.sha = vh->sha,
	};

	fio_sha3_384_init(&sha3_ctx);
	fill_sha3(&sha3_ctx, p, len);
}

/* [한국어] fill_sha3_512 - SHA3-512 계산. vh->sha에 직접 기록. */
static void fill_sha3_512(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha3_512 *vh = hdr_priv(hdr);
	struct fio_sha3_ctx sha3_ctx = {
		.sha = vh->sha,
	};

	fio_sha3_512_init(&sha3_ctx);
	fill_sha3(&sha3_ctx, p, len);
}

/*
 * [한국어]
 * fill_sha512 - SHA-512 계산 후 vh->sha512(128B 버퍼)에 저장.
 * ctx.buf를 vh->sha512로 직결해 복사 생략.
 */
static void fill_sha512(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha512 *vh = hdr_priv(hdr);
	struct fio_sha512_ctx sha512_ctx = {
		.buf = vh->sha512,
	};

	fio_sha512_init(&sha512_ctx);
	fio_sha512_update(&sha512_ctx, p, len);
	fio_sha512_final(&sha512_ctx);
}

/*
 * [한국어]
 * fill_sha256 - SHA-256 계산 후 vh->sha256(64B 버퍼)에 저장.
 */
static void fill_sha256(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha256 *vh = hdr_priv(hdr);
	struct fio_sha256_ctx sha256_ctx = {
		.buf = vh->sha256,
	};

	fio_sha256_init(&sha256_ctx);
	fio_sha256_update(&sha256_ctx, p, len);
	fio_sha256_final(&sha256_ctx);
}

/*
 * [한국어]
 * fill_sha1 - SHA-1 계산 후 vh->sha1(uint32_t[5])에 저장.
 */
static void fill_sha1(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha1 *vh = hdr_priv(hdr);
	struct fio_sha1_ctx sha1_ctx = {
		.H = vh->sha1,            /* [한국어] H0..H4 상태 배열을 vhdr에 연결. */
	};

	fio_sha1_init(&sha1_ctx);
	fio_sha1_update(&sha1_ctx, p, len);
	fio_sha1_final(&sha1_ctx);
}

/* [한국어] fill_crc7 - CRC7 계산 후 vh->crc7(1B)에 저장. */
static void fill_crc7(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc7 *vh = hdr_priv(hdr);

	vh->crc7 = fio_crc7(p, len);
}

/* [한국어] fill_crc16 - CRC16 계산 후 vh->crc16(2B)에 저장. */
static void fill_crc16(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc16 *vh = hdr_priv(hdr);

	vh->crc16 = fio_crc16(p, len);
}

/* [한국어] fill_crc32 - CRC32(이더넷) 계산 후 vh->crc32(4B)에 저장. */
static void fill_crc32(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc32 *vh = hdr_priv(hdr);

	vh->crc32 = fio_crc32(p, len);
}

/*
 * [한국어]
 * fill_crc32c - CRC32C(Castagnoli) 계산 후 vh->crc32에 저장. HW 가속 자동 디스패치.
 * vhdr_crc32 구조를 CRC32와 공유하므로 같은 필드에 저장되지만 다항식이 다르다.
 */
static void fill_crc32c(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc32 *vh = hdr_priv(hdr);

	vh->crc32 = fio_crc32c(p, len);   /* [한국어] SSE4.2 또는 ARM crc32 확장 자동 선택. */
}

/* [한국어] fill_crc64 - CRC64 계산 후 vh->crc64(8B)에 저장. */
static void fill_crc64(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc64 *vh = hdr_priv(hdr);

	vh->crc64 = fio_crc64(p, len);
}

/*
 * [한국어]
 * fill_md5 - MD5 해시 계산 후 vh->md5_digest(16B)에 저장.
 * ctx.hash를 vh->md5_digest로 직결해 추가 복사 없이 final이 기록.
 * (uint32_t *) 캐스트는 md5_digest가 바이트 배열일 때 워드 정렬 접근 허용.
 */
static void fill_md5(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_md5 *vh = hdr_priv(hdr);
	struct fio_md5_ctx md5_ctx = {
		.hash = (uint32_t *) vh->md5_digest,
	};

	fio_md5_init(&md5_ctx);
	fio_md5_update(&md5_ctx, p, len);
	fio_md5_final(&md5_ctx);
}

/*
 * [한국어]
 * __fill_hdr - verify_header의 모든 고정 필드를 채우는 내부 함수 (always-fill)
 *
 * @td         : 잡 컨텍스트.
 * @io_u       : 대상 I/O — verify_offset/start_time/numberio를 소스로 사용.
 * @hdr        : 채울 헤더 (io_u->buf 또는 스택 더미).
 * @header_num : 블록 인덱스 — hdr->offset 계산에 사용.
 * @header_len : 블록 전체 길이.
 * @rand_seed  : 데이터 생성에 사용된 64비트 시드 (verify_pattern_bytes=0일 때 의미).
 *
 * 동작: 매직/버전/타입/길이/시드/오프셋/시간/스레드/numberio 9개 필드를 채운 뒤
 * 맨 마지막에 CRC32C(헤더 바이트 0..crc32_offset)를 계산해 crc32 필드에 저장.
 * 즉 CRC는 자기 자신을 제외한 헤더 앞부분만 커버 — 검증 측도 같은 offsetof로 계산.
 *
 * 왜 __fill 버전이 따로 있는가: VERIFY_PATTERN_NO_HDR 모드는 정상 흐름에선 헤더
 * 를 찍지 않지만, dump_verify_buffers에서 임시 가짜 헤더를 만들어 비교해야 하므로
 * 모드 검사를 건너뛰는 버전이 필요. 정상 경로는 fill_hdr(래퍼)가 사용.
 *
 * 호출 체인:
 *   populate_hdr → fill_hdr → [이 함수] (정상)
 *   dump_verify_buffers → [이 함수] (PATTERN_NO_HDR 더미)
 */
static void __fill_hdr(struct thread_data *td, struct io_u *io_u,
		       struct verify_header *hdr, unsigned int header_num,
		       unsigned int header_len, uint64_t rand_seed)
{
	void *p = hdr;

	hdr->magic = FIO_HDR_MAGIC;              /* [한국어] 0xacca — 헤더 식별자. */
	hdr->version = VERIFY_HEADER_VERSION;    /* [한국어] 0x81 — 상위 비트로 구-신 버전 구별. */
	hdr->verify_type = td->o.verify;         /* [한국어] 이 블록의 해시/CRC 알고리즘 기록. */
	hdr->len = header_len;                   /* [한국어] 블록 총 크기(헤더+vhdr+데이터). */
	hdr->rand_seed = rand_seed;              /* [한국어] 데이터 영역 PRNG 시드 — 랜덤 모드 재현 기준. */
	hdr->offset = io_u->verify_offset + header_num * td->o.verify_interval;  /* [한국어] 파일 내 절대 오프셋. */
	hdr->time_sec = io_u->start_time.tv_sec;    /* [한국어] I/O 시작 시각(초) — 진단용 메타데이터. */
	hdr->time_nsec = io_u->start_time.tv_nsec;  /* [한국어] 나노초 부분. */
	hdr->thread = td->thread_number;             /* [한국어] 어느 잡이 썼는지 — 멀티잡 디버깅용. */
	hdr->numberio = io_u->numberio;              /* [한국어] 이 잡 내 몇 번째 I/O인지 — verify_write_sequence 검사용. */
	/* [한국어] 마지막: crc32 필드 직전까지의 바이트에 CRC32C 적용. 자신을 포함하지
	 * 않도록 offsetof(verify_header, crc32)로 길이 제한. 검증 시 같은 계산으로 비교. */
	hdr->crc32 = fio_crc32c(p, offsetof(struct verify_header, crc32));
}


/*
 * [한국어]
 * fill_hdr - __fill_hdr의 래퍼. VERIFY_PATTERN_NO_HDR 모드에서는 no-op.
 *
 * PATTERN_NO_HDR 모드는 디스크에 헤더 바이트를 삽입하지 않고 순수 패턴만 기록
 * (드라이브 내부 PI/DIF 검증과 병행하는 시나리오). 이 래퍼가 모드 검사 단일
 * 책임을 가져 populate_hdr의 흐름을 깔끔하게 유지.
 */
static void fill_hdr(struct thread_data *td, struct io_u *io_u,
		     struct verify_header *hdr, unsigned int header_num,
		     unsigned int header_len, uint64_t rand_seed)
{
	if (td->o.verify != VERIFY_PATTERN_NO_HDR)
		__fill_hdr(td, io_u, hdr, header_num, header_len, rand_seed);
}

/*
 * [한국어]
 * populate_hdr - 쓰기 시 단일 검증 블록의 헤더 + vhdr 체크섬 + (옵션) 스왑을 완성
 *
 * @td         : 잡 컨텍스트.
 * @io_u       : 소스 io_u — rand_seed/start_time/numberio를 헤더로 복사.
 * @hdr        : 블록 시작 포인터 (io_u->buf 내부).
 * @header_num : 이 io_u 내 블록 인덱스.
 * @header_len : 블록 총 크기.
 *
 * 3단계:
 *   (1) fill_hdr()로 고정 헤더 필드(매직/버전/타입/길이/시드/오프셋/시간/CRC32C) 기록.
 *   (2) data_len = header_len - hdr_size 계산 → 데이터 영역(p + hdr_size 이후)에
 *       td->o.verify로 선택된 알고리즘의 체크섬을 계산해 vhdr_<alg>에 기록.
 *   (3) verify_offset 옵션이 설정되어 있으면 헤더 hdr_size 바이트를 p..p+hdr_size
 *       영역에서 p+verify_offset..p+verify_offset+hdr_size 영역과 스왑. 이는
 *       "헤더가 블록 시작이 아닌 지정된 오프셋에 나타나게 하는" 효과 — 검증 시
 *       동일 스왑을 다시 하면 원위치로 복원됨 (verify_io_u에서 수행).
 *
 * 에러 처리: header_len <= hdr_size 이면 블록 크기가 헤더만으로도 꽉 차 데이터
 * 영역이 음수가 되므로 td_verror(EINVAL)로 잡 실패 기록 + 조기 반환.
 *
 * 알고리즘 분기: VERIFY_HDR_ONLY / PATTERN / PATTERN_NO_HDR 세 경우는 vhdr가
 * 없으므로 no-op. default는 unknown type으로 assert.
 *
 * 호출 체인: fill_pattern_headers → [이 함수] (io_u 내 블록 개수만큼 반복).
 */
static void populate_hdr(struct thread_data *td, struct io_u *io_u,
			 struct verify_header *hdr, unsigned int header_num,
			 unsigned int header_len)
{
	unsigned int data_len;
	void *data;
	char *p;

	p = (char *) hdr;                 /* [한국어] 바이트 단위 포인터 연산을 위해 char*로 캐스트. */

	/* [한국어] (1) 헤더 고정 필드 채움 (CRC32C 포함). rand_seed는 io_u의 값 그대로 — 이 시드가
	 * 검증 시 PRNG 재생성에 사용된다. */
	fill_hdr(td, io_u, hdr, header_num, header_len, io_u->rand_seed);

	/* [한국어] 블록이 헤더 영역보다 작으면 데이터 영역이 없는 것이므로 설정 오류. */
	if (header_len <= hdr_size(td, hdr)) {
		td_verror(td, EINVAL, "Blocksize too small");
		return;
	}
	data_len = header_len - hdr_size(td, hdr);  /* [한국어] 해시/체크섬을 적용할 데이터 영역 길이. */

	/* [한국어] 데이터 영역 시작 = 블록 시작 + 헤더 크기. */
	data = p + hdr_size(td, hdr);
	/* [한국어] (2) 검증 유형에 따라 알고리즘별 fill_<alg> 호출 — vhdr_<alg>에 체크섬 저장. */
	switch (td->o.verify) {
	case VERIFY_MD5:
		dprint(FD_VERIFY, "fill md5 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_md5(hdr, data, data_len);
		break;
	case VERIFY_CRC64:
		dprint(FD_VERIFY, "fill crc64 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc64(hdr, data, data_len);
		break;
	case VERIFY_CRC32C:
	case VERIFY_CRC32C_INTEL:
		dprint(FD_VERIFY, "fill crc32c io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc32c(hdr, data, data_len);   /* [한국어] HW/SW 자동 분기 적용. */
		break;
	case VERIFY_CRC32:
		dprint(FD_VERIFY, "fill crc32 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc32(hdr, data, data_len);
		break;
	case VERIFY_CRC16:
		dprint(FD_VERIFY, "fill crc16 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc16(hdr, data, data_len);
		break;
	case VERIFY_CRC7:
		dprint(FD_VERIFY, "fill crc7 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc7(hdr, data, data_len);
		break;
	case VERIFY_SHA256:
		dprint(FD_VERIFY, "fill sha256 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha256(hdr, data, data_len);
		break;
	case VERIFY_SHA512:
		dprint(FD_VERIFY, "fill sha512 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha512(hdr, data, data_len);
		break;
	case VERIFY_SHA3_224:
		dprint(FD_VERIFY, "fill sha3-224 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha3_224(hdr, data, data_len);
		break;
	case VERIFY_SHA3_256:
		dprint(FD_VERIFY, "fill sha3-256 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha3_256(hdr, data, data_len);
		break;
	case VERIFY_SHA3_384:
		dprint(FD_VERIFY, "fill sha3-384 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha3_384(hdr, data, data_len);
		break;
	case VERIFY_SHA3_512:
		dprint(FD_VERIFY, "fill sha3-512 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha3_512(hdr, data, data_len);
		break;
	case VERIFY_XXHASH:
		dprint(FD_VERIFY, "fill xxhash io_u %p, len %u\n",
						io_u, hdr->len);
		fill_xxhash(hdr, data, data_len);
		break;
	case VERIFY_SHA1:
		dprint(FD_VERIFY, "fill sha1 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha1(hdr, data, data_len);
		break;
	case VERIFY_HDR_ONLY:
	case VERIFY_PATTERN:
	case VERIFY_PATTERN_NO_HDR:
		/* nothing to do here */
		/* [한국어] 헤더만 / 패턴 / NO_HDR은 vhdr_<alg>가 없어 추가 체크섬 기록 불필요. */
		break;
	default:
		/* [한국어] 알 수 없는 값은 fixup_options에서 걸러져야 — 여기까지 왔으면 버그. */
		log_err("fio: bad verify type: %d\n", td->o.verify);
		assert(0);
	}

	/* [한국어] (3) verify_offset 설정 시 헤더 영역을 지정 위치로 이동 (스왑).
	 * hdr_size==0 (NO_HDR)이면 아무것도 하지 않음. 검증 단계에서 같은 스왑이
	 * 수행되어 대칭적으로 원위치 복원된다. */
	if (td->o.verify_offset && hdr_size(td, hdr))
		memswp(p, p + td->o.verify_offset, hdr_size(td, hdr));
}

/*
 * fill body of io_u->buf with random data and add a header with the
 * checksum of choice
 */
/*
 * [한국어]
 * populate_verify_io_u - 쓰기 제출 직전 io_u의 버퍼를 검증 가능하게 준비
 *
 * @td   : 잡 컨텍스트.
 * @io_u : 쓰기 대상 io_u (io_u->buf, io_u->buflen 사용/수정).
 *
 * VERIFY_NULL(검증 비활성)에서는 no-op (패턴도 체크섬도 필요 없음). 그 외에는
 * fill_pattern_headers(seed=0, use_seed=0)로 위임 — 내부에서 verify_state RNG로
 * 새 시드를 추첨하고 버퍼 바디를 채운 뒤 populate_hdr로 블록별 헤더 + vhdr을
 * 기록한다. 호출 후 io_u는 그대로 td_io_queue → 엔진 queue()로 제출 가능 상태.
 *
 * 실행 컨텍스트: 잡 스레드의 쓰기 경로 단일 호출 지점 — io_u.c::get_io_u ->
 * io_u.c::fill_io_buffer 이후 do_io()의 쓰기 분기에서.
 *
 * 호출 체인:
 *   backend.c::do_io() → io_u.c::fill_io_buffer() → [이 함수]
 *     → fill_pattern_headers() → fill_verify_pattern() + populate_hdr()
 */
void populate_verify_io_u(struct thread_data *td, struct io_u *io_u)
{
	if (td->o.verify == VERIFY_NULL)
		return;                   /* [한국어] 검증 없음 — 버퍼 준비 불필요. */

	fill_pattern_headers(td, io_u, 0, 0);
	/* [한국어] seed=0 + use_seed=0 → fill_verify_pattern이 verify_state에서
	 * 새 시드를 뽑아 io_u->rand_seed에 저장. 그 시드는 hdr->rand_seed로도 저장
	 * 되어 검증 시 데이터 재생성에 사용된다. */
}

/*
 * [한국어]
 * get_next_verify - 다음에 검증할 I/O 블록을 io_hist에서 꺼내 io_u에 바인딩
 *
 * @td   : 잡 컨텍스트. td->io_hist_tree/io_hist_list/io_hist_len을 읽고 수정.
 * @io_u : 채울 대상 io_u (free 상태). offset/buflen/file/numberio/rand_seed/
 *         ddir/xfer_buf/xfer_buflen/flags를 설정.
 * @return: 0 = 성공(io_u 채워짐, ddir=DDIR_READ), 1 = 검증할 것이 없음(또는
 *          아직 in-flight)/파일 열기 실패.
 *
 * io_hist는 과거 쓰기 완료된 블록의 레코드(struct io_piece) 집합으로
 * iolog.c가 관리한다. 랜덤 쓰기였다면 RB 트리에 정렬 삽입되어 있고, 순차였다면
 * flist로 유지된다. 이 함수는 트리 우선으로 "첫" 엔트리를 꺼내고 없으면 리스트
 * 첫 엔트리를 꺼낸다. 꺼낸 io_piece의 offset/len/file/numberio 정보를 io_u에
 * 옮기고 struct 자체는 free.
 *
 * in-flight 체크: atomic_load_acquire(&ipo->flags) & IP_F_IN_FLIGHT —
 * 이 블록이 아직 장치에 커밋되지 않았으면 검증 불가. 꺼내지 않고 "nothing" 반환.
 * acquire 의미: 이 로드가 성공한 뒤의 모든 읽기는 IN_FLIGHT 비트 clear가 관찰된
 * 이후의 메모리 효과를 본다 (완료 핸들러가 release로 clear).
 *
 * 파일 상태: ipo->file이 아직 열려있지 않으면 td_io_open_file로 재개방. 검증이
 * 쓰기 단계보다 훨씬 뒤에 일어날 수 있어 파일이 닫혔을 수 있기 때문.
 *
 * 시드 복원 (랜덤 모드): verify_pattern_bytes==0이면 쓰기 때 사용한 시드와 같은
 * 시퀀스를 검증에서도 재생성해야 하므로 td->verify_state에서 __rand를 한 번(또는
 * 64비트 ABI에서 두 번) 뽑아 io_u->rand_seed로 넣는다. 이 시드는 이후
 * verify_header의 rand_seed와 일치해야 한다 (verify_header_seed 옵션 시 검사).
 *
 * 실행 컨텍스트: 잡 스레드 단독 (do_verify 경로).
 *
 * 호출 체인: backend.c::do_verify() → [이 함수] → td_io_queue() → engines[READ].
 */
int get_next_verify(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo = NULL;

	/*
	 * this io_u is from a requeue, we already filled the offsets
	 */
	/* [한국어] 재큐된 io_u (이전에 queue 시 FIO_Q_BUSY로 되돌려진 것)는 이미
	 * 메타데이터가 세팅되어 있으므로 그대로 재제출. */
	if (io_u->file)
		return 0;

	/* [한국어] 우선순위: RB 트리(랜덤 쓰기 기록) > flist(순차 쓰기 기록). */
	if (!RB_EMPTY_ROOT(&td->io_hist_tree)) {
		struct fio_rb_node *n = rb_first(&td->io_hist_tree);

		ipo = rb_entry(n, struct io_piece, rb_node);

		/*
		 * Ensure that the associated IO has completed
		 */
		/* [한국어] 아직 장치에 반영되지 않은 쓰기는 검증 대상 아님. acquire load로
		 * 완료 핸들러의 release store와 happens-before 관계 보장. */
		if (atomic_load_acquire(&ipo->flags) & IP_F_IN_FLIGHT)
			goto nothing;

		rb_erase(n, &td->io_hist_tree);          /* [한국어] 트리에서 제거 — 중복 검증 방지. */
		assert(ipo->flags & IP_F_ONRB);          /* [한국어] 꺼낸 엔트리가 실제로 "트리에 있음" 표시가 되어 있어야. */
		ipo->flags &= ~IP_F_ONRB;                /* [한국어] 소유권 전환 — 더 이상 트리 소속 아님. */
	} else if (!flist_empty(&td->io_hist_list)) {
		ipo = flist_first_entry(&td->io_hist_list, struct io_piece, list);

		/*
		 * Ensure that the associated IO has completed
		 */
		/* [한국어] 리스트 측도 동일한 in-flight 검사. */
		if (atomic_load_acquire(&ipo->flags) & IP_F_IN_FLIGHT)
			goto nothing;

		flist_del(&ipo->list);
		assert(ipo->flags & IP_F_ONLIST);
		ipo->flags &= ~IP_F_ONLIST;
	}

	/* [한국어] ipo를 꺼냈으면 io_u에 복사하고 검증용 read 제출을 준비. */
	if (ipo) {
		td->io_hist_len--;                    /* [한국어] 남은 기록 수 감소 (do_verify 진행 추적용). */

		io_u->offset = ipo->offset;           /* [한국어] 검증 대상 블록의 파일 내 오프셋. */
		io_u->verify_offset = ipo->offset;    /* [한국어] 검증 로그 출력용 — 원래 쓰기 오프셋 보존. */
		io_u->buflen = ipo->len;              /* [한국어] 블록 길이 (버퍼 사용 크기). */
		io_u->numberio = ipo->numberio;       /* [한국어] 쓰기 때 부여된 순번 — verify_header 에서 비교. */
		io_u->file = ipo->file;               /* [한국어] 대상 파일 포인터. */
		io_u_set(td, io_u, IO_U_F_VER_LIST);  /* [한국어] "verify 경로로 생성된 io_u" 플래그. */

		/* [한국어] TRIM된 블록이었으면 플래그 전파 — verify_io_u가 verify_trimmed_io_u로 분기. */
		if (ipo->flags & IP_F_TRIMMED)
			io_u_set(td, io_u, IO_U_F_TRIMMED);

		/* [한국어] 파일이 닫혀 있으면 재개방. 실패하면 이 io_u 포기하고 재시도 시그널(1). */
		if (!fio_file_open(io_u->file)) {
			int r = td_io_open_file(td, io_u->file);

			if (r) {
				dprint(FD_VERIFY, "failed file %s open\n",
						io_u->file->file_name);
				return 1;
			}
		}

		get_file(ipo->file);                  /* [한국어] fio_file 참조 카운트 증가 — 검증 완료 시 put_file_log로 감소. */
		assert(fio_file_open(io_u->file));
		io_u->ddir = DDIR_READ;               /* [한국어] 검증은 반드시 읽기 방향. */
		io_u->xfer_buf = io_u->buf;           /* [한국어] 실제 전송 버퍼 = io_u 자체 버퍼. */
		io_u->xfer_buflen = io_u->buflen;     /* [한국어] 전송 길이 = 블록 길이. */

		remove_trim_entry(td, ipo);           /* [한국어] trim 트래킹에서도 제거 (중복 방지). */
		free(ipo);                            /* [한국어] io_piece는 더 이상 필요 없음. */
		dprint(FD_VERIFY, "get_next_verify: ret io_u %p\n", io_u);

		/* [한국어] 랜덤 데이터 모드(패턴 없음)에서는 쓰기 때 사용한 시드를 그대로
		 * 재현해 기대 데이터를 생성한다. 쓰기와 검증 양쪽이 같은 verify_state RNG
		 * 를 같은 순서로 소비하므로 __rand가 같은 값을 돌려준다. */
		if (!td->o.verify_pattern_bytes) {
			io_u->rand_seed = __rand(&td->verify_state);
			/* [한국어] 64비트 ABI 상위 엔트로피 보강 (fill_verify_pattern과 대칭). */
			if (sizeof(int) != sizeof(long *))
				io_u->rand_seed *= __rand(&td->verify_state);
		}
		return 0;
	}

nothing:
	/* [한국어] 트리/리스트에서 꺼낼 것이 없거나 모두 in-flight 상태 — 재시도 필요. */
	dprint(FD_VERIFY, "get_next_verify: empty\n");
	return 1;
}

/*
 * [한국어]
 * fio_verify_init - 잡 시작 시 검증 서브시스템 초기화 (현재는 CRC32C HW 가속 탐지)
 *
 * @td : 잡 컨텍스트. td->o.verify만 읽음.
 *
 * verify=crc32c 또는 crc32c_intel 일 때만 의미 있는 일을 한다:
 *   - crc32c_arm64_probe() : AArch64에서 HWCAP_CRC32 확인 후 HW 경로 활성화.
 *     (ARMv8 Crypto Extensions의 CRC32 명령 사용)
 *   - crc32c_intel_probe() : CPUID ECX bit 20 (SSE4.2의 CRC32)를 확인 후 활성화.
 *     (PCLMUL 기반 128비트 폴딩으로 수 GB/s 처리)
 * 두 probe는 각각 자기 아키텍처에서만 의미 있고 다른 쪽에서는 no-op.
 *
 * 탐지 결과는 fio_crc32c 함수 포인터를 업데이트 — fill_crc32c/verify_io_u_crc32c는
 * 그대로 fio_crc32c를 호출하되 런타임에 HW/SW 디스패치.
 *
 * 실행 컨텍스트: 잡 스레드 시작 시 1회. fio_crc32c 포인터 업데이트는 idempotent.
 *
 * 호출 체인: backend.c::thread_main() → [이 함수] (init_io 블록).
 */
void fio_verify_init(struct thread_data *td)
{
	if (td->o.verify == VERIFY_CRC32C_INTEL ||
	    td->o.verify == VERIFY_CRC32C) {
		crc32c_arm64_probe();   /* [한국어] AArch64 HWCAP_CRC32 탐지. 다른 아키텍처는 no-op. */
		crc32c_intel_probe();   /* [한국어] CPUID SSE4.2 CRC32 탐지. 다른 아키텍처는 no-op. */
	}
}

/*
 * [한국어]
 * verify_async_thread - 비동기 검증 워커 스레드의 메인 루프
 *
 * @data : 잡 스레드 데이터(thread_data*). pthread_create 시 전달.
 * @return: 항상 NULL. 종료는 thread exit 플래그 또는 fatal 에러로.
 *
 * 한 잡당 verify_async N명 워커가 이 함수를 실행. 모든 워커가 같은 td를
 * 공유하며 td->verify_list에서 경쟁적으로 io_u를 꺼낸다.
 *
 * 라이프사이클:
 *   (1) 생성 직후 (옵션) CPU affinity 설정 — verify_cpumask가 주어진 경우
 *       스케줄링 고정으로 캐시 지역성/NUMA 최적화.
 *   (2) 무한 루프:
 *       (a) read_barrier() 후 exit 플래그 검사 — 빠른 탈출 경로.
 *       (b) io_u_lock 획득 → verify_list가 비어있으면 verify_cond에서 wait.
 *       (c) wake 후 flist_splice_init로 전체 큐를 로컬 FIFO로 이동 — 락 최소화.
 *       (d) 락 해제 후 로컬 리스트의 각 io_u에 대해 IO_U_F_NO_FILE_PUT 세팅
 *           (워커가 파일 put을 생략 — 이미 verify_io_u_async가 put_file_log 처리),
 *           verify_io_u() 호출, put_io_u().
 *       (e) non-fatal 에러(ERROR_TYPE_VERIFY_BIT)는 update_error_count 후 계속,
 *           fatal은 루프 탈출.
 *   (3) 종료 시 td_verror + (verify_fatal이면) fio_mark_td_terminate, 그리고
 *       nr_verify_threads-- + free_cond 시그널로 verify_async_exit의 조인 웨이트
 *       해제.
 *
 * 실행 컨텍스트: detach된 pthread — join 없음. 종료 동기화는 free_cond로.
 *
 * 호출 체인: verify_async_init 가 pthread_create(이 함수)로 기동 → 메시지 루프.
 */
static void *verify_async_thread(void *data)
{
	struct thread_data *td = data;
	struct io_u *io_u;
	int ret = 0;

	/* [한국어] verify_cpumask 옵션이 설정되어 있으면 이 워커를 해당 CPU set으로
	 * 바인딩. 잡 스레드와 다른 CPU에 배치해 캐시/전력을 분리 관리하는 용도.
	 * 실패 시 로그만 남기고 워커 조기 종료. */
	if (fio_option_is_set(&td->o, verify_cpumask) &&
	    fio_setaffinity(td->pid, td->o.verify_cpumask)) {
		log_err("fio: failed setting verify thread affinity\n");
		goto done;
	}

	do {
		FLIST_HEAD(list);    /* [한국어] 루프마다 새 로컬 리스트. splice로 큐 덤프 수신. */

		read_barrier();      /* [한국어] 메인 스레드의 td->verify_thread_exit=1 기록을 관찰하기 전 barrier. */
		if (td->verify_thread_exit)
			break;       /* [한국어] 이미 종료 요청됐으면 루프 탈출 → done. */

		/* [한국어] verify_list 보호 락 획득. 이 락은 io_u.c의 free list와 공용이지만,
		 * 여기서는 verify_list + cur_depth 동시성만 관심. */
		pthread_mutex_lock(&td->io_u_lock);

		/* [한국어] 큐가 비어있으면 verify_cond에서 wait. exit 플래그도 같이 확인해
		 * spurious wake 또는 종료 broadcast에 모두 반응. pthread_cond_wait은 락을
		 * atomic하게 풀고 대기하다가 시그널 오면 다시 잡는다. */
		while (flist_empty(&td->verify_list) &&
		       !td->verify_thread_exit) {
			ret = pthread_cond_wait(&td->verify_cond,
							&td->io_u_lock);
			if (ret) {
				break;  /* [한국어] 조건변수 오류 — 잡 종료로 처리. */
			}
		}

		/* [한국어] 대기 큐 전체를 로컬 리스트로 이동. 이후 실제 검증은 락 없이 수행
		 * 가능해 메인 스레드의 새 enqueue를 블록하지 않음. */
		flist_splice_init(&td->verify_list, &list);
		pthread_mutex_unlock(&td->io_u_lock);

		if (flist_empty(&list))
			continue;      /* [한국어] spurious wake — 다시 루프 맨 위로. */

		/* [한국어] 로컬 리스트의 모든 io_u를 하나씩 꺼내 검증. */
		while (!flist_empty(&list)) {
			io_u = flist_first_entry(&list, struct io_u, verify_list);
			flist_del_init(&io_u->verify_list);  /* [한국어] 리스트에서 분리. */

			io_u_set(td, io_u, IO_U_F_NO_FILE_PUT);  /* [한국어] put_io_u 시 파일 put 생략 — verify_io_u_async가 이미 처리. */
			ret = verify_io_u(td, &io_u);            /* [한국어] 실제 무결성 검사 — 이 함수가 heavy lifting. */

			put_io_u(td, io_u);                      /* [한국어] 검증 끝난 io_u를 free 리스트로. */
			if (!ret)
				continue;       /* [한국어] 성공 — 다음 항목. */
			/* [한국어] 실패지만 non-fatal(EILSEQ 등)이면 카운터만 올리고 계속 진행.
			 * update_error_count가 td->error_<type>을 증가시키고, td_clear_error로
			 * td->error를 리셋해 다음 io_u에서 재사용 가능하게. */
			if (td_non_fatal_error(td, ERROR_TYPE_VERIFY_BIT, ret)) {
				update_error_count(td, ret);
				td_clear_error(td);
				ret = 0;
			}
		}
	} while (!ret);

	/* [한국어] 치명적 에러로 탈출한 경우: td_verror로 잡에 에러 기록 + verify_fatal
	 * 시 잡 전체 종료 표시. 여기까지 오면 다른 워커들도 다음 루프에서 exit 체크로
	 * 자연스럽게 정리. */
	if (ret) {
		td_verror(td, ret, "async_verify");
		if (td->o.verify_fatal)
			fio_mark_td_terminate(td);
	}

done:
	/* [한국어] 종료 확정 — nr_verify_threads-- + verify_async_exit의 join-wait를
	 * 깨우기 위해 free_cond 시그널. 락 내부에서 수행해 경쟁 방지. */
	pthread_mutex_lock(&td->io_u_lock);
	td->nr_verify_threads--;
	pthread_cond_signal(&td->free_cond);
	pthread_mutex_unlock(&td->io_u_lock);

	return NULL;
}

/*
 * [한국어]
 * verify_async_init - 비동기 검증 워커 풀을 생성하고 기동
 *
 * @td     : 잡 컨텍스트. td->o.verify_async 개수만큼 워커 생성.
 * @return : 0=성공, 1=일부 또는 전부 생성 실패 (이미 만들어진 워커는 exit 플래그로 회수).
 *
 * 동작:
 *   (1) pthread 속성 attr 초기화 후 stack size를 PTHREAD_STACK_MIN의 2배로 설정 —
 *       최소 스택을 살짝 늘려 안전 여유 확보 (보통 32~64KiB).
 *   (2) verify_thread_exit=0 리셋, verify_threads 배열 할당.
 *   (3) 각 워커를 pthread_create + pthread_detach (join 필요 없음 — 종료는
 *       free_cond로 동기화).
 *   (4) 일부 실패 시 verify_thread_exit=1 + pthread_cond_broadcast로 이미 만든
 *       워커들을 일괄 종료 유도.
 *
 * detach 사용 이유: 워커 종료 시점이 잡 스레드 종료 시점과 완전히 독립적이라
 * join 대기 대신 조건 변수로 동기화하는 패턴이 더 유연. 리소스 누수 없이
 * OS가 자동 회수.
 *
 * 실행 컨텍스트: 잡 스레드의 초기화 단계(backend.c::thread_main). 단일 호출.
 *
 * 호출 체인: thread_main() → [이 함수] → (잡 종료 시) verify_async_exit().
 */
int verify_async_init(struct thread_data *td)
{
	int i, ret;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	/* [한국어] 스택 크기 = PTHREAD_STACK_MIN*2. 기본 8MiB보다 훨씬 작게 설정해
	 * N개 워커 합계 메모리를 아끼되, 작은 해시/CRC 컨텍스트엔 충분. */
	pthread_attr_setstacksize(&attr, 2 * PTHREAD_STACK_MIN);

	td->verify_thread_exit = 0;   /* [한국어] 새 세션이므로 exit 플래그 리셋. */

	/* [한국어] pthread_t 핸들 배열 할당. nr_verify_threads와는 별도로 관리. */
	td->verify_threads = malloc(sizeof(pthread_t) * td->o.verify_async);
	for (i = 0; i < td->o.verify_async; i++) {
		/* [한국어] 각 워커를 verify_async_thread 엔트리로 생성. td 포인터를 인자로. */
		ret = pthread_create(&td->verify_threads[i], &attr,
					verify_async_thread, td);
		if (ret) {
			log_err("fio: async verify creation failed: %s\n",
					strerror(ret));
			break;
		}
		/* [한국어] detach로 전환 — 리소스 자동 회수, join 의존 제거. */
		ret = pthread_detach(td->verify_threads[i]);
		if (ret) {
			log_err("fio: async verify thread detach failed: %s\n",
					strerror(ret));
			break;
		}
		td->nr_verify_threads++;   /* [한국어] 생성 성공 건만 카운트. 워커가 종료 시 --. */
	}

	pthread_attr_destroy(&attr);   /* [한국어] attr 자원 해제 — 핸들은 이미 pthread_create에 복사됨. */

	/* [한국어] 일부/전부 실패 처리: 이미 기동된 워커들에게 exit 신호. */
	if (i != td->o.verify_async) {
		log_err("fio: only %d verify threads started, exiting\n", i);

		pthread_mutex_lock(&td->io_u_lock);
		td->verify_thread_exit = 1;                  /* [한국어] 워커 루프에서 체크. */
		pthread_cond_broadcast(&td->verify_cond);    /* [한국어] 전원을 깨움 — 다음 루프에서 exit 체크로 종료. */
		pthread_mutex_unlock(&td->io_u_lock);

		return 1;
	}

	return 0;
}

/*
 * [한국어]
 * verify_async_exit - 비동기 검증 워커 풀을 질서 있게 종료하고 자원 해제
 *
 * @td : 잡 컨텍스트.
 *
 * 프로토콜:
 *   (1) verify_thread_exit=1 설정 (모든 워커가 루프 체크 시 관찰).
 *   (2) pthread_cond_broadcast(verify_cond) — 대기 중인 모든 워커를 깨워
 *       큐 비어있음 + exit 플래그 조합으로 종료 경로 진입.
 *   (3) 각 워커가 종료 시 nr_verify_threads-- + pthread_cond_signal(free_cond)
 *       을 수행 — 이 함수는 nr_verify_threads==0 될 때까지 free_cond wait.
 *   (4) 모두 종료되면 verify_threads 배열 해제.
 *
 * detach된 스레드이므로 pthread_join 대신 조건 변수로 "완전 종료" 감지.
 *
 * 실행 컨텍스트: 잡 스레드 종료 단계 (backend.c::thread_main의 cleanup).
 *
 * 호출 체인: thread_main() 마지막 부분 → [이 함수].
 */
void verify_async_exit(struct thread_data *td)
{
	pthread_mutex_lock(&td->io_u_lock);
	td->verify_thread_exit = 1;                    /* [한국어] 모든 워커의 루프 체크가 관찰하게 세팅. */
	pthread_cond_broadcast(&td->verify_cond);      /* [한국어] wait 중인 전체 워커 wake-up. */

	/* [한국어] 마지막 워커까지 종료 완료를 기다림. 워커가 free_cond 시그널을
	 * 보내므로 여기서 wait. 스퓨리어스 wake 방어를 위해 while 루프. */
	while (td->nr_verify_threads)
		pthread_cond_wait(&td->free_cond, &td->io_u_lock);

	pthread_mutex_unlock(&td->io_u_lock);
	free(td->verify_threads);
	td->verify_threads = NULL;                     /* [한국어] 댕글링 방지 — 재호출 안전성. */
}

/*
 * [한국어]
 * paste_blockoff - verify_pattern의 "%o" 포맷 specifier 콜백 (블록 오프셋 삽입)
 *
 * @buf  : 패턴 버퍼 내 삽입 위치 (paste_format이 specifier 위치를 가리킴).
 * @len  : 삽입 가능한 최대 바이트 수 (specifier 폭).
 * @priv : lib/pattern.c가 전달한 컨텍스트 — 여기서는 (struct io_u *).
 * @return: 항상 0.
 *
 * 역할: io_u->offset(64비트)을 리틀엔디안으로 직렬화해 len 바이트만큼 buf에 기록.
 * sizeof(off)=8보다 len이 작으면 하위 바이트만 기록 (short integer-like truncation).
 *
 * 사용 맥락: 사용자가 verify_pattern="ABC%o"처럼 지정하면 lib/pattern.c가
 * "%o" specifier를 만날 때 이 콜백을 호출, 현재 io_u의 offset을 실시간으로
 * 주입. 블록마다 다른 패턴이 나오므로 "패턴 + 오프셋 = 예측 불가능한 무결성 검증"이 된다.
 *
 * typecheck는 io->offset이 unsigned long long과 호환되는 타입인지 컴파일 타임 검사
 * (타입 변경 시 경고 유도).
 *
 * 호출 체인: lib/pattern.c::paste_format → format_desc[%o].paste = [이 함수].
 */
int paste_blockoff(char *buf, unsigned int len, void *priv)
{
	struct io_u *io = priv;
	unsigned long long off;

	typecheck(__typeof__(off), io->offset);     /* [한국어] 타입 안전성 컴파일 시간 보장. */
	off = cpu_to_le64((uint64_t)io->offset);    /* [한국어] CPU 엔디안→리틀엔디안 강제. */
	len = min(len, (unsigned int)sizeof(off));  /* [한국어] specifier 폭을 8B로 클램프. */
	memcpy(buf, &off, len);                     /* [한국어] 바이트 복사 — 정렬 무시. */
	return 0;
}

/*
 * [한국어]
 * get_all_io_list - 모든(또는 선택된) 잡 스레드의 I/O 상태 스냅샷을 직렬화 버퍼로 수집
 *
 * @save_mask : IO_LIST_ALL(-1) 또는 특정 스레드 번호(1-based). 후자인 경우
 *              해당 스레드만 수집.
 * @sz        : [out] 반환된 버퍼의 바이트 크기.
 * @return    : calloc로 할당된 all_io_list 포인터 (호출자가 free). nr=0이면 NULL.
 *
 * 반환 구조 레이아웃 (리틀엔디안 직렬화):
 *   struct all_io_list {
 *     __le64 threads;                       // 포함된 잡 수 N
 *     struct thread_io_list state[];        // 가변 개수, 각 thread_io_list도 가변 크기
 *   }
 *   thread_io_list {
 *     __le32 depth, __le64 numberio, __le64 index, rand_state, char name[],
 *     inflight_write inflight[depth];        // in-flight numberio 각 슬롯
 *   }
 *
 * 동작:
 *   (1) 첫 번째 순회로 총 크기 계산: nr 스레드 * sizeof(thread_io_list) +
 *       (iodepth * nr_files 합계) * sizeof(inflight_write) + 헤더 8바이트.
 *       이 과정에서 td->stop_io=1, TD_F_VSTATE_SAVED 플래그 설정 — 이후 해당
 *       스레드가 더 이상 쓰기 진행하지 않도록 backend.c가 관찰.
 *   (2) calloc으로 할당, threads 필드 기록.
 *   (3) 두 번째 순회로 각 스레드의 depth/numberio/index/rand state/name/inflight를
 *       채움. rand 상태는 32/64비트 모드 분기(use64 비트). 모든 정수는 LE 변환.
 *   (4) io_list_next(s)로 가변 크기 thread_io_list 다음 주소 계산 (depth에 따라).
 *
 * compiletime_assert: all_io_list 고정 헤더가 정확히 8바이트여야 — 포맷 ABI 불변성.
 *
 * 동기화: inflight_numberio[], inflight_issued는 atomic_load_acquire로 잡 스레드가
 * 쓴 최신 값을 보장 읽기. stop_io=1 설정 후에는 해당 잡 스레드가 관측 시 스스로 I/O를
 * 멈추므로 스냅샷이 안정된다.
 *
 * 호출 체인: verify_save_state → [이 함수] → (write_thread_list_state 반복).
 */
struct all_io_list *get_all_io_list(int save_mask, size_t *sz)
{
	struct all_io_list *rep;
	size_t depth;
	void *next;
	int nr;

	/* [한국어] 포맷 ABI 잠금: all_io_list 헤더(threads 필드만)는 정확히 8바이트. */
	compiletime_assert(sizeof(struct all_io_list) == 8, "all_io_list");

	/*
	 * Calculate reply space needed. We need one 'io_state' per thread,
	 * and the size will vary depending on depth.
	 */
	/* [한국어] 1단계: 크기 계산 루프. 대상 잡 수(nr)와 in-flight 총합(depth) 집계. */
	depth = 0;
	nr = 0;
	for_each_td(td) {
		/* [한국어] save_mask가 특정 td 인덱스면 그것만 수집. IO_LIST_ALL이면 전부. */
		if (save_mask != IO_LIST_ALL && (__td_index + 1) != save_mask)
			continue;
		td->stop_io = 1;               /* [한국어] 잡 스레드에 I/O 중단 신호. */
		td->flags |= TD_F_VSTATE_SAVED;/* [한국어] "상태 저장됨" 마커 — 중복 저장 방지. */
		depth += (td->o.iodepth * td->o.nr_files);  /* [한국어] 파일마다 iodepth만큼의 in-flight 슬롯 필요. */
		nr++;
	} end_for_each();

	if (!nr)
		return NULL;                   /* [한국어] 대상 없음 — 호출자가 파일도 만들지 않음. */

	/* [한국어] 총 버퍼 크기 = 고정 헤더 + thread_io_list*N + inflight_write*총depth. */
	*sz = sizeof(*rep);
	*sz += nr * sizeof(struct thread_io_list);
	*sz += depth * sizeof(struct inflight_write);
	rep = calloc(1, *sz);              /* [한국어] 0-clear 할당 — 패딩도 결정적. */

	rep->threads = cpu_to_le64((uint64_t) nr);    /* [한국어] 헤더 기록. */

	/* [한국어] 2단계: 내용 채우기. next는 "다음 thread_io_list 시작 주소" 포인터. */
	next = &rep->state[0];
	for_each_td(td) {
		struct thread_io_list *s = next;

		/* [한국어] 크기 계산 때와 같은 필터 — 순회 일관성 보장. */
		if (save_mask != IO_LIST_ALL && (__td_index + 1) != save_mask)
			continue;

		/* [한국어] in-flight 슬롯의 numberio 각각을 원자적으로 읽고 LE로 저장. */
		for (int i = 0; i < td->o.iodepth; i++)
			s->inflight[i].numberio = cpu_to_le64(atomic_load_acquire(&td->inflight_numberio[i]));

		s->depth = cpu_to_le32((uint32_t) td->o.iodepth);    /* [한국어] 슬롯 개수. */
		s->numberio = cpu_to_le64((uint64_t) atomic_load_acquire(&td->inflight_issued));  /* [한국어] 마지막 발행 numberio. */
		s->index = cpu_to_le64((uint64_t) __td_index);       /* [한국어] 글로벌 잡 인덱스. */

		/* [한국어] 오프셋 생성용 RNG 상태 직렬화. use64 분기: 64비트 Tausworthe(5 words)
		 * vs 32비트 Tausworthe(3 words)를 선택. 마지막 슬롯은 0 패딩(포맷 정렬). */
		if (td->offset_state.use64) {
			s->rand.state64.s[0] = cpu_to_le64(td->offset_state.state64.s1);
			s->rand.state64.s[1] = cpu_to_le64(td->offset_state.state64.s2);
			s->rand.state64.s[2] = cpu_to_le64(td->offset_state.state64.s3);
			s->rand.state64.s[3] = cpu_to_le64(td->offset_state.state64.s4);
			s->rand.state64.s[4] = cpu_to_le64(td->offset_state.state64.s5);
			s->rand.state64.s[5] = 0;
			s->rand.use64 = cpu_to_le64((uint64_t)1);
		} else {
			s->rand.state32.s[0] = cpu_to_le32(td->offset_state.state32.s1);
			s->rand.state32.s[1] = cpu_to_le32(td->offset_state.state32.s2);
			s->rand.state32.s[2] = cpu_to_le32(td->offset_state.state32.s3);
			s->rand.state32.s[3] = 0;
			s->rand.use64 = 0;
		}
		/* [한국어] 잡 이름을 인라인 이름 필드에 복사 — 로드 시 잡 매칭에 사용. */
		snprintf((char *) s->name, sizeof(s->name), "%s", td->o.name);
		next = io_list_next(s);       /* [한국어] s의 가변 꼬리(inflight[])를 건너뛴 다음 시작 위치. */
	} end_for_each();

	return rep;
}

/*
 * [한국어]
 * open_state_file - 검증 상태 파일을 모드별로 열기 (경로는 verify_state_gen_name 생성)
 *
 * @name      : 잡 이름(td->o.name).
 * @prefix    : 파일명 접두어 (예: "local", aux_path/local 등).
 * @num       : 잡 인덱스(0-based).
 * @for_write : 1 = 쓰기(O_CREAT|O_TRUNC|O_WRONLY|O_SYNC), 0 = 읽기(O_RDONLY).
 * @return    : 파일 디스크립터, 실패 시 -1.
 *
 * O_SYNC 사용: 쓰기 모드에서 상태 파일이 동기적으로 디스크에 내려가야 재시작
 * 안전성이 보장된다(크래시 후 로드 시 해당 파일이 최신 상태여야).
 *
 * _WIN32에서는 O_BINARY를 추가해 CRLF 변환 방지 (바이너리 포맷 보호).
 *
 * 파일명 형식은 verify_state_gen_name이 담당 — 통상 "<prefix>-<name>-<num>.state"
 * 또는 유사 패턴. 경로 전체는 PATH_MAX 버퍼에 담는다.
 *
 * 호출 체인: write_thread_list_state / verify_load_state → [이 함수] → open(2).
 */
static int open_state_file(const char *name, const char *prefix, int num,
			   int for_write)
{
	char out[PATH_MAX];
	int flags;
	int fd;

	if (for_write)
		flags = O_CREAT | O_TRUNC | O_WRONLY | O_SYNC;   /* [한국어] 쓰기: 동기화 + 초기화. */
	else
		flags = O_RDONLY;                                 /* [한국어] 읽기: 단순 open. */

#ifdef _WIN32
	/* [한국어] Windows는 텍스트 모드에서 CRLF 변환을 수행해 바이너리 CRC를 깰 수 있다.
	 * O_BINARY로 강제 바이너리 경로. POSIX에는 O_BINARY가 없어 Linux에선 정의 자체가 없음. */
	flags |= O_BINARY;
#endif

	/* [한국어] 파일명 생성 — verify-state.c가 <prefix>.<num>.<name>.state 등 형식으로. */
	verify_state_gen_name(out, sizeof(out), name, prefix, num);

	fd = open(out, flags, 0644);
	if (fd == -1) {
		perror("fio: open state file");
		log_err("fio: state file: %s (for_write=%d)\n", out, for_write);
		return -1;
	}

	return fd;
}

/*
 * [한국어]
 * write_thread_list_state - 단일 thread_io_list 스냅샷을 상태 파일로 내보냄
 *
 * @s      : 직렬화된 thread_io_list 포인터 (get_all_io_list가 생성).
 * @prefix : 파일명 접두어.
 * @return : 0=성공, 1=실패.
 *
 * 파일 레이아웃:
 *   [verify_state_hdr : 24B]  {__le64 version, __le64 size, __le64 crc}
 *   [thread_io_list   : 가변 = thread_io_list_sz(s)]
 *
 * CRC32C는 thread_io_list 전체를 대상으로 계산. 로드 시 같은 계산으로 검증.
 * write(2)가 부분 성공을 반환하지 않는다고 가정 — 파일시스템 짧은 쓰기는 실패로 취급.
 * O_SYNC로 열려 있어 write 반환 시 디스크에 내려간 상태 (crash-safe).
 *
 * 호출 체인: __verify_save_state → [이 함수] → open_state_file → write.
 */
static int write_thread_list_state(struct thread_io_list *s,
				   const char *prefix)
{
	struct verify_state_hdr hdr;
	uint64_t crc;
	ssize_t ret;
	int fd;

	/* [한국어] 잡 인덱스(s->index)와 이름으로 파일 open. 이 함수 진입 전 이미
	 * cpu_to_le64된 상태가 아니라 s는 아직 호스트 바이트 순서 — 주의: get_all_io_list가
	 * 이미 cpu_to_le64(index)를 했기 때문에 여기서 그냥 전달. */
	fd = open_state_file((const char *) s->name, prefix, s->index, 1);
	if (fd == -1)
		return 1;

	crc = fio_crc32c((void *)s, thread_io_list_sz(s));   /* [한국어] 전체 바이트 CRC32C. */

	/* [한국어] 헤더 3필드 세팅 — LE 직렬화. VSTATE_HDR_VERSION은 포맷 ABI 버전. */
	hdr.version = cpu_to_le64((uint64_t) VSTATE_HDR_VERSION);
	hdr.size = cpu_to_le64((uint64_t) thread_io_list_sz(s));
	hdr.crc = cpu_to_le64(crc);
	ret = write(fd, &hdr, sizeof(hdr));
	if (ret != sizeof(hdr))
		goto write_fail;               /* [한국어] 헤더 일부만 써졌어도 실패 처리. */

	ret = write(fd, s, thread_io_list_sz(s));
	if (ret != thread_io_list_sz(s)) {
write_fail:
		if (ret < 0)
			perror("fio: write state file");
		log_err("fio: failed to write state file\n");
		ret = 1;
	} else
		ret = 0;

	close(fd);
	return ret;
}

/*
 * [한국어]
 * __verify_save_state - 수집된 all_io_list를 개별 스레드 파일 N개로 분할 저장
 *
 * 각 thread_io_list는 하나의 상태 파일로 나가므로 재시작 시 잡별로 독립 로드 가능.
 * io_list_next로 가변 길이 thread_io_list를 차례로 진행.
 */
void __verify_save_state(struct all_io_list *state, const char *prefix)
{
	struct thread_io_list *s = &state->state[0];
	unsigned int i;

	/* [한국어] state->threads는 LE로 저장된 값 — 호스트로 변환해 루프 카운트로. */
	for (i = 0; i < le64_to_cpu(state->threads); i++) {
		write_thread_list_state(s,  prefix);
		s = io_list_next(s);           /* [한국어] 다음 thread_io_list 시작으로 이동. */
	}
}

/*
 * [한국어]
 * verify_save_state - 검증 상태 저장 최상위 진입점 (SIGUSR2 트리거 등)
 *
 * @mask : IO_LIST_ALL 또는 특정 잡 인덱스. get_all_io_list에 전달.
 *
 * 동작:
 *   (1) get_all_io_list(mask)로 스냅샷 수집.
 *   (2) aux_path 설정 시 "<aux_path>/local" 접두어, 아니면 "local".
 *   (3) __verify_save_state로 파일 저장 후 버퍼 해제.
 *
 * SIGUSR2 핸들러나 잡 종료 직전에 호출해 재시작 시 검증을 이어갈 수 있게 한다.
 *
 * 호출 체인: signal handler / backend.c cleanup → [이 함수] →
 *   get_all_io_list → __verify_save_state → write_thread_list_state.
 */
void verify_save_state(int mask)
{
	struct all_io_list *state;
	size_t sz;

	state = get_all_io_list(mask, &sz);
	if (state) {
		char prefix[PATH_MAX];

		/* [한국어] aux_path 설정 여부에 따라 저장 경로 접두어 결정. FIO_OS_PATH_SEPARATOR
		 * 는 Linux '/' / Windows '\\'. */
		if (aux_path)
			sprintf(prefix, "%s%clocal", aux_path, FIO_OS_PATH_SEPARATOR);
		else
			strcpy(prefix, "local");

		__verify_save_state(state, prefix);
		free(state);                  /* [한국어] get_all_io_list calloc 해제. */
	}
}

/*
 * [한국어]
 * verify_free_state - 잡에 로드되어 있던 검증 상태 버퍼 해제
 *
 * @td : 잡 컨텍스트. td->vstate이 NULL이 아니면 free.
 *
 * 잡 종료 시 cleanup 경로에서 호출. verify_load_state가 malloc한 버퍼를 해제.
 */
void verify_free_state(struct thread_data *td)
{
	if (td->vstate)
		free(td->vstate);
}

/*
 * [한국어]
 * verify_assign_state - 파일에서 읽어온 thread_io_list를 호스트 바이트순으로 변환하고 td에 연결
 *
 * @td : 목표 잡 컨텍스트. td->vstate에 p 저장.
 * @p  : 파일 본문 버퍼 (malloc 소유권 이 함수에 이전).
 *
 * 파일의 모든 필드는 LE 직렬화되어 있으므로 호스트로 변환 후 다시 구조체에 기록
 * (in-place). rand.use64는 "RNG가 32비트인지 64비트인지" 플래그로, 이 값에 따라
 * state32/state64 분기하여 바이트 변환. inflight 배열의 각 numberio도 변환.
 *
 * 변환 후 포인터 p를 td->vstate로 이관 — 이후 free는 verify_free_state가 담당.
 *
 * 호출 체인: verify_load_state → [이 함수].
 */
void verify_assign_state(struct thread_data *td, void *p)
{
	struct thread_io_list *s = p;
	int i;

	/* [한국어] 스칼라 필드 3개 먼저 변환. use64는 이후 분기에 필요. */
	s->depth = le32_to_cpu(s->depth);
	s->numberio = le64_to_cpu(s->numberio);
	s->rand.use64 = le64_to_cpu(s->rand.use64);

	/* [한국어] RNG 상태: 64비트 모드면 state64 6슬롯, 32비트 모드면 state32 4슬롯 변환. */
	if (s->rand.use64) {
		for (i = 0; i < 6; i++)
			s->rand.state64.s[i] = le64_to_cpu(s->rand.state64.s[i]);
	} else {
		for (i = 0; i < 4; i++)
			s->rand.state32.s[i] = le32_to_cpu(s->rand.state32.s[i]);
	}

	/* [한국어] in-flight 슬롯 numberio 각각 LE→호스트. depth개 순회. */
	for (i = 0; i < s->depth; i++) {
		s->inflight[i].numberio = le64_to_cpu(s->inflight[i].numberio);
		dprint(FD_VERIFY, "verify_assign_state numberio=%"PRIu64", inflight[%d]=%"PRIu64"\n", s->numberio, i, s->inflight[i].numberio);
	}

	td->vstate = p;    /* [한국어] 이제 td 소유 — verify_state_should_stop이 참조. */
}

/*
 * [한국어]
 * verify_state_hdr - 상태 파일 헤더의 버전/CRC 유효성 검사
 *
 * @hdr : 상태 파일에서 읽은 24바이트 헤더 (in-place LE→host 변환).
 * @s   : 헤더 뒤에 따라오는 thread_io_list 본문 포인터.
 * @return: 0 = 유효, 1 = 버전 불일치 또는 CRC 실패.
 *
 * 헤더 3필드를 LE→호스트로 변환한 뒤 버전 매칭 검사, 이후 본문 크기만큼 CRC32C
 * 재계산해 저장된 CRC와 비교. 크로스 버전/부분 쓰여짐/비트 플립을 감지한다.
 */
int verify_state_hdr(struct verify_state_hdr *hdr, struct thread_io_list *s)
{
	uint64_t crc;

	hdr->version = le64_to_cpu(hdr->version);
	hdr->size = le64_to_cpu(hdr->size);
	hdr->crc = le64_to_cpu(hdr->crc);

	if (hdr->version != VSTATE_HDR_VERSION)
		return 1;          /* [한국어] 포맷 ABI 불일치 — 거부. */

	crc = fio_crc32c((void *)s, hdr->size);
	if (crc != hdr->crc)
		return 1;          /* [한국어] CRC 실패 — 파일 손상. */

	return 0;
}

/*
 * [한국어]
 * verify_load_state - 디스크의 상태 파일을 읽어 td->vstate에 설치 (재시작 경로)
 *
 * @td     : 잡 컨텍스트. td->o.verify_state 옵션으로 활성 여부 결정.
 *           실패 시 td_verror로 에러 기록.
 * @prefix : 파일명 접두어 (보통 "local" 또는 "<aux>/local").
 * @return : 0=성공(또는 옵션 미설정), 1=실패.
 *
 * 단계:
 *   (1) 옵션 미설정 시 즉시 0 반환 (no-op).
 *   (2) open_state_file로 읽기 전용 오픈 — 파일명은 (name, prefix, td_idx)로 생성.
 *   (3) verify_state_hdr(24B) 읽기, LE→호스트 변환, 버전 일치 확인.
 *   (4) hdr.size 만큼 본문 malloc 후 read, CRC32C 재계산 비교.
 *   (5) 성공 시 verify_assign_state(td, s)로 소유권 이관.
 *
 * 에러 경로: err 라벨에서 s가 할당되었으면 free, fd close, 1 반환. td_verror로
 * 상세 errno 기록 (read 실패 경로에서만).
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계.
 *
 * 호출 체인: init.c::setup_random_seeds 이후 → fio_verify_load_state (backend.c) →
 *            [이 함수] → verify_assign_state.
 */
int verify_load_state(struct thread_data *td, const char *prefix)
{
	struct verify_state_hdr hdr;
	void *s = NULL;
	uint64_t crc;
	ssize_t ret;
	int fd;

	/* [한국어] 옵션 꺼짐 — 재시작 로드 불필요. */
	if (!td->o.verify_state)
		return 0;

	/* [한국어] td->thread_number는 1-based, open_state_file num은 0-based라 -1. */
	fd = open_state_file(td->o.name, prefix, td->thread_number - 1, 0);
	if (fd == -1)
		return 1;

	/* [한국어] (3) 헤더 24B 읽기. 부분 성공 허용 없음 — 크기 불일치는 실패. */
	ret = read(fd, &hdr, sizeof(hdr));
	if (ret != sizeof(hdr)) {
		if (ret < 0)
			td_verror(td, errno, "read verify state hdr");
		log_err("fio: failed reading verify state header\n");
		goto err;
	}

	/* [한국어] 헤더 필드 3개 LE→호스트 변환 (in-place). */
	hdr.version = le64_to_cpu(hdr.version);
	hdr.size = le64_to_cpu(hdr.size);
	hdr.crc = le64_to_cpu(hdr.crc);

	if (hdr.version != VSTATE_HDR_VERSION) {
		log_err("fio: unsupported (%d) version in verify state header\n",
				(unsigned int) hdr.version);
		goto err;
	}

	/* [한국어] (4) 본문 버퍼 할당 + 읽기. 크기는 헤더가 알려준 값. */
	s = malloc(hdr.size);
	ret = read(fd, s, hdr.size);
	if (ret != hdr.size) {
		if (ret < 0)
			td_verror(td, errno, "read verify state");
		log_err("fio: failed reading verity state\n");
		goto err;
	}

	/* [한국어] CRC 재계산 → 파일 손상 감지. */
	crc = fio_crc32c(s, hdr.size);
	if (crc != hdr.crc) {
		log_err("fio: verify state is corrupt\n");
		goto err;
	}

	close(fd);

	/* [한국어] (5) td에 소유권 이관. 이후 s free는 verify_free_state가 처리. */
	verify_assign_state(td, s);
	return 0;
err:
	if (s)
		free(s);
	close(fd);
	return 1;
}

/*
 * Use the loaded verify state to know when to stop doing verification
 */
/*
 * [한국어]
 * verify_state_should_stop - 재시작 모드에서 특정 numberio를 검증해도 되는지 판정
 *
 * @td       : 잡 컨텍스트. td->vstate (verify_load_state로 로드된) 참조.
 * @numberio : 현재 io_u가 가진 I/O 시퀀스 번호.
 * @return   : 0 = 계속 검증, 1 = 이 블록부터는 중단.
 *
 * 재시작 시나리오: 이전 실행이 SIGUSR2나 크래시로 중단되어 상태가 저장됨. 새
 * 실행에서 그 상태를 로드한 뒤 검증을 시도한다. 하지만 저장된 상태 기준으로
 * 두 가지 경우는 "실제로 디스크에 반영되지 않은" 쓰기이므로 검증해서는 안 됨:
 *
 *   (a) numberio >= s->numberio (저장 시점의 최대 발행 번호) — 이 번호는
 *       이전 실행이 아직 발행하지도 못한 쓰기. 디스크에 없으니 검증 실패 확정.
 *   (b) numberio < s->numberio 이지만 s->inflight[] 배열에 이 번호가 있음 —
 *       발행은 했으나 완료 이벤트가 도착하기 전에 종료된 "in-flight write".
 *       실제로 기록됐는지 불확실하므로 안전하게 건너뜀.
 *
 * td->vstate가 NULL이면 재시작 모드가 아니거나 로드 실패 — 항상 0 반환(일반 검증).
 *
 * 실행 컨텍스트: do_verify 경로에서 각 io_u에 대해 호출. 잡 스레드 단일.
 *
 * 호출 체인: backend.c::do_verify → [이 함수] → (true 반환 시) 해당 io_u 스킵.
 */
int verify_state_should_stop(struct thread_data *td, uint64_t numberio)
{
	struct thread_io_list *s = td->vstate;
	int i;

	dprint(FD_VERIFY, "verify_state_should_stop numberio=%"PRIu64"\n", numberio);
	if (!s)
		return 0;       /* [한국어] 재시작 상태 없음 — 일반 경로. */

	/* If the current seq is lower than the max issued seq, check to make sure
	 * the write was not inflight.
	 */
	/* [한국어] 경우 (b): 저장된 최대 발행 번호보다 작은 numberio라도 in-flight
	 * 슬롯에 실제 존재하면 검증 중단. */
	if (numberio < s->numberio) {
		for (i = 0; i < s->depth; i++) {
			if (s->inflight[i].numberio == numberio) {
				log_info("Stop verify because seq %"PRIu64" was an inflight write\n",
					numberio);
				return 1;
			}
		}
	} else {
		/* [한국어] 경우 (a): 저장 시점보다 미래의 번호 — 존재할 수 없는 데이터. */
		log_info("Stop verify because seq %"PRIu64" >= %"PRIu64"\n",
			numberio, s->numberio);
		return 1;
	}

	return 0;
}
