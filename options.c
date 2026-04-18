/*
 * [한국어 설명] fio 옵션 정의 및 파싱 콜백 구현 (options.c) — 전체 옵션 정의 허브
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 **모든** 옵션을 단일 fio_options[] 배열로 정의하는 핵심 허브다.
 * 각 옵션은 struct fio_option 엔트리로 표현되며 `--name=value` 형식의 CLI 플래그와
 * `name=value` 형식의 잡 파일(INI) 라인으로 동일하게 파싱된다. 옵션 수는 약 300여 개로
 * I/O 기본(filename/rw/bs/size/iodepth), 엔진 선택, 버퍼 관리, verify, log, runtime,
 * random 분포, rate limit, trim, ZBD, FDP, dedupe, compress, cgroup, CPU affinity,
 * steady state, stats, error handling, network 등 거의 모든 런타임 동작을 제어한다.
 * 단순 INT/BOOL 옵션은 파서가 자동 저장하지만, 복합 문자열(bssplit, rw, cpumask,
 * verify_pattern, random_distribution 등)은 이 파일에서 제공하는 str_*_cb 콜백이
 * 직접 파싱하여 thread_options에 결과를 채운다.
 *
 * 추가로 이 파일은 **키워드 치환 시스템**(fio_keywords: $pagesize/$mb_memory/$ncpus),
 * **환경 변수 치환**(fio_option_dup_subs: ${VAR}), **bc(1) 기반 산술 계산**(bc_calc),
 * **유사 옵션명 추천**(show_closest_option — 레벤슈타인 거리), **옵션 설정 여부 비트맵**
 * (set_options[]), **엔진별 동적 옵션 등록**(add_option/add_opt_posval)까지 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *                 CLI (argv)         잡 파일 (.fio / --name= blocks)
 *                     │                       │
 *                     ▼                       ▼
 *              init.c::parse_cmd_line    init.c::__parse_jobs_ini
 *                          \                 /
 *                           ▼               ▼
 *                        init.c::add_job(td, opts[])
 *                                    │
 *                                    ▼
 *                     ┌── options.c::fio_options_parse(td, opts, n)
 *                     │        1) sort_options() ─ prio 순 정렬 (filename이 directory 뒤)
 *                     │        2) dup_and_sub_options() ─ ${ENV}, $pagesize 치환 + bc 계산
 *                     │        3) for each: parse.c::parse_option(opt, fio_options, ...)
 *                     │              └─ 매칭된 fio_option 엔트리의 type/off1/cb 참조해
 *                     │                 파싱 엔진이 값을 thread_options에 저장
 *                     │        4) 인식 못한 옵션 → ioengine_load() → td->io_ops->options 재시도
 *                     │        5) 여전히 인식 못하면 show_closest_option() 추천
 *                     ▼
 *              td->o (struct thread_options) 필드들이 모두 채워진 상태
 *                                    │
 *                                    ▼
 *              init.c::fixup_options()  ← 교차 검증, 기본값 보정
 *                                    │
 *                                    ▼
 *              backend.c::thread_main()  ← 실제 I/O 실행
 *
 * 호출 체인 (파싱 경로):
 *   fio_options_parse [이 파일]
 *     └→ parse_option [parse.c]
 *          ├→ (단순 타입) thread_options 필드에 직접 저장
 *          └→ (cb 지정) str_*_cb() [이 파일] — 문자열 분해/검증/변환 후 저장
 *
 * 호출 체인 (값 조회 경로):
 *   add_job/fixup_options/thread_main/io_u.c/...
 *     └→ td->o.<field>  또는  fio_option_is_set(&td->o, <field>)
 *
 * === 타 모듈과의 연결 ===
 * - **parse.c** (파싱 엔진): fio_option 엔트리의 type/off1~off6/minval/maxval/posval/
 *   def/cb/verify/parent/hide/prio/alias/exclusive_group 필드를 해석해 문자열→이진값
 *   변환과 thread_options 저장을 수행. 이 파일은 엔트리 **정의**, parse.c는 **해석**.
 * - **thread_options.h**: fio_option.off1 = offsetof(struct thread_options, field) 로
 *   저장 대상 필드를 정적 지정. thread_options는 잡 1개의 모든 옵션 스냅샷.
 * - **options.h**: 이 파일 외부 API 선언(fio_options_parse/fio_cmd_option_parse/
 *   fio_option_find/add_option/add_opt_posval/fio_option_mark_set/fio_options_free 등).
 * - **optgroup.h**: FIO_OPT_C_{IO,ENGINE,GENERAL,FILE,STAT,LOG,VERIFY,PROFILE} 카테고리와
 *   FIO_OPT_G_* (수십 개 세부 그룹: IO_BASIC, IO_BUF, IO_FLOW, FILENAME, RANDOM, ZONE,
 *   LATPROF, RATE, VERIFY, LOG, CRED, TRIM, STEADYSTATE, ZBD, DEDUPE, ...) 분류 식별자 정의.
 *   `fio --enghelp=<name>` 등 도움말 필터링/GUI 옵션 트리에 사용.
 * - **ioengines.c**: ioengine_load() 에서 td->io_ops->options(엔진별 fio_option 배열)를
 *   얻어 엔진 전용 옵션을 파싱. 엔진이 add_opt_posval() 로 ioengine 옵션의 posval에
 *   자기 이름을 등록한다.
 * - **init.c**: parse_options → parse_cmd_line/__parse_jobs_ini 에서 이 파일의 API 사용.
 * - **filesetup.c** (add_file, add_dir_files): str_filename_cb/str_opendir_cb에서 호출.
 * - **lib/pattern.c** (parse_and_fill_pattern_alloc): buffer_pattern/verify_pattern 파싱.
 * - **verify.h**: VERIFY_* 매크로(VERIFY_NONE/CRC32/MD5/PATTERN/...) posval 매핑에 사용.
 * - **zbd.h**: zoned_mode 관련 옵션의 posval/cb.
 * - **lib/rand.h** / **lib/zipf.c** / **lib/gauss.c**: random_distribution의 실제 구현.
 * - **cmdprio.c / cmdprio.h**: libaio/io_uring/sg 공유 cmdprio_* 옵션(percentage/bssplit).
 * - 데이터 흐름: **잡파일/CLI 문자열** → parse.c 어휘 분석 → fio_options[].cb 호출 →
 *   str_*_cb 가 strsep/strtok/sscanf로 분해 → add_file/fio_cpuset_init/pattern_alloc/
 *   bssplit_ddir 등으로 thread_options 내부 자료구조 구축 → backend가 소비.
 *
 * === 주요 함수/구조체 요약 ===
 * [최상위 배열]
 * - fio_options[FIO_MAX_OPTS]:  ~300개 fio_option 엔트리. NULL sentinel(.name=NULL)로 종료.
 * - fio_keywords[]:  $pagesize/$mb_memory/$ncpus 치환 테이블. init 시 값 채움.
 *
 * [복합 옵션 파서 콜백 (본 파일의 정수)]
 * - str_rw_cb():          rw=write,4k 형식의 ddir + 시퀀스 오프셋/주기 파싱.
 * - str_bssplit_cb():     bs=4k/50:8k/30:16k/20 형식의 블록크기 확률 분포 파싱.
 * - str_fst_cb():         file_service_type = random/roundrobin/zipf/pareto/gauss 파싱.
 * - str_random_distribution_cb(): random 오프셋 분포(zipf/pareto/gauss/zoned/zoned_abs).
 * - str_mem_cb():         iomem = malloc/shm/mmap/mmaphuge/... 및 ":" 뒤 경로 추출.
 * - str_verify_pattern_cb() / str_buffer_pattern_cb(): 패턴 문자열/16진수/"%o" 파싱.
 * - str_cpumask_cb() / str_cpus_allowed_cb(): CPU affinity 마스크 / 범위 표기 파싱.
 * - str_numa_cpunodes_cb() / str_numa_mpol_cb(): NUMA 노드/정책 파싱 (libnuma).
 * - str_fdp_pli_cb() / str_dp_scheme_cb(): FDP placement ID 리스트/scheme 파일 검증.
 * - str_filename_cb() / str_directory_cb() / str_opendir_cb(): 대상 파일/디렉토리 등록.
 * - str_ignore_error_cb() / str_replay_skip_cb(): 에러 무시 / blktrace 재생 필터.
 * - str_steadystate_cb(): iops:10%, bw:5%, lat:1ms 등 정상상태 판정 기준 파싱.
 * - str_ioengine_external_cb(): ioengine=external:/path/to/engine.so 분리 + stat 검증.
 * - str_size_cb() / str_io_size_cb() / str_offset_cb() / str_offset_increment_cb() /
 *   str_zoneskip_cb(): 백분율/존 단위/절대값 3-way 인코딩 파싱.
 * - str_write_bw_log_cb / str_write_lat_log_cb / str_write_iops_log_cb /
 *   str_write_hist_log_cb: 성능 로그 활성화 + 파일명 저장.
 *
 * [파싱/유틸 외부 API]
 * - fio_options_parse():       잡의 옵션 배열을 파싱하는 메인 드라이버.
 * - fio_cmd_option_parse():    커맨드라인 --opt val 형식 파싱.
 * - fio_options_dup_and_init(): CLI용 struct option[] 테이블 생성(getopt_long).
 * - fio_fill_default_options(): 모든 옵션의 기본값 적용.
 * - fio_show_option_help():    fio --cmdhelp=<opt> 출력.
 * - fio_options_mem_dupe() / fio_options_free(): fork/종료 시 문자열 복제/해제.
 * - fio_option_find():         이름으로 fio_options[] 조회 (외부 노출).
 * - fio_option_mark_set() / __fio_option_is_set(): 설정 여부 비트맵 get/set.
 * - add_option() / add_opt_posval() / del_opt_posval(): 엔진이 동적으로 옵션/posval 등록.
 * - invalidate_profile_options(): 프로파일 unload 시 해당 옵션들을 INVALID로 무효화.
 *
 * [키워드/환경 치환]
 * - fio_keywords_init() / fio_keywords_exit(): $pagesize/$mb_memory/$ncpus 초기화·해제.
 * - fio_option_dup_subs(): ${ENV} → getenv() 값 치환.
 * - fio_keyword_replace(): $keyword → 값 치환. 치환 후 산술 연산 포함 시 bc_calc().
 * - bc_calc(): popen("bc") 로 size=1024*1024 같은 표현식 계산.
 *
 * [저수준 헬퍼]
 * - split_parse_ddir():       "값/퍼센트:값/퍼센트:..." 범용 분할 파서.
 * - str_split_parse():        ddir 3-way("읽기,쓰기,트림") 분리 드라이버.
 * - parse_cmdprio_bssplit_entry() / split_parse_prio_ddir(): cmdprio_bssplit 파싱.
 * - split_parse_distr():      "값:중심" 형식의 분포 파라미터 파싱.
 * - str2error():              "EINVAL" → 22 같은 errno 이름 → 번호 변환.
 * - get_next_str() / get_max_str_idx() / set_name_idx() / get_name_by_idx():
 *   콜론 구분 파일 리스트 순회 (escape '\:' 지원).
 * - rw_verify() / gtod_cpu_verify(): .verify 콜백 (추가 사후 검증).
 * - is_valid_steadystate() / parse_zoned_distribution() / zone_split_ddir(): 보조 파서.
 *
 * === fio_option 엔트리 필드 핸드북 ===
 * 각 fio_options[] 엔트리는 다음 필드를 갖는다 (parse.h 정의):
 * - .name:    옵션 이름 (CLI/INI에서 사용). 필수.
 * - .lname:   긴 설명(long name). GUI 및 --cmdhelp 출력에 사용.
 * - .alias:   옵션의 별칭 (예: "readwrite" → "rw").
 * - .type:    값의 파싱 타입:
 *             FIO_OPT_STR / STR_STORE / STR_VAL(숫자+단위 k/m/g) / STR_VAL_INT / INT /
 *             ULL / BOOL / FLOAT_LIST / RANGE / STR_MULTI / STR_SET / STR_VAL_ZONE /
 *             DEPRECATED / UNSUPPORTED / INVALID / STR_ULL / ...
 * - .off1~.off6: thread_options 내 대상 필드 오프셋 (offsetof). 최대 6개 필드에 분산 저장.
 * - .def:     기본값 문자열. parse.c 가 매 잡 초기화 시 적용.
 * - .help:    짧은 도움말 (--cmdhelp=opt, fio --help 에 출력).
 * - .cb:      커스텀 콜백 함수. 단순 타입으로 표현 불가한 문자열을 직접 파싱.
 *             시그니처 2종: int (*)(void *data, const char *str)  /  int (*)(void *data, long long *val).
 *             data = &td->o 이므로 cb_data_to_td(data) 매크로로 td 역산.
 * - .verify:  파싱 후 의존성 검증 콜백 (예: read_only 모드에서 write 잡 금지 — rw_verify).
 * - .minval / .maxval: INT/ULL 타입 상한/하한. parse.c가 클램프하거나 에러.
 * - .minlen / .maxlen: STR_STORE 문자열 길이 제한.
 * - .interval:  GUI 슬라이더 단계 값 (예: 1).
 * - .posval:    STR/STR_MULTI 타입의 허용 값 테이블. PARSE_MAX_VP=32 슬롯.
 *               각 엔트리 { .ival = "문자열", .oval = 정수값, .help = "설명", .cb = 옵셔널 콜백 }.
 *               엔진 이름 posval은 런타임에 add_opt_posval 로 동적 추가됨.
 * - .parent:   상위 옵션명. 이 옵션의 효과는 parent가 활성일 때만 의미 있음.
 * - .hide:     `fio --help` 출력에서 숨김 (기본값일 때 출력 안 함).
 * - .hide_on_set: 다른 옵션 설정 시 숨김 처리.
 * - .exclusive_group: 같은 그룹의 옵션들은 상호배타 (하나만 설정 가능).
 * - .prio:     파싱 우선순위. 음수=나중 파싱, 양수=먼저 파싱.
 *              예: filename.prio=-1 → directory 먼저 처리되도록 강제.
 * - .category / .group: FIO_OPT_C_*/FIO_OPT_G_* (optgroup.h). 도움말 분류 및 GUI.
 * - .prof_name: 이 옵션이 속한 프로파일명. invalidate_profile_options()가 사용.
 * - .is_seconds / .is_time: STR_VAL_TIME 타입의 단위 해석 힌트.
 *
 * === 키워드/치환 치트시트 ===
 * - $pagesize, $mb_memory, $ncpus: 시스템 값 (fio_keywords_init).
 * - ${ENV_VAR}: 환경 변수 (fio_option_dup_subs).
 * - $jobname, $jobnum, $filenum: 파일명 포맷 치환 (filesetup.c 에서 처리).
 * - $pid, $clientuid: 런타임 식별자.
 * - 산술 연산(+-  /): bc(1) 유틸리티로 계산. 예: size=$mb_memory*1024.
 *
 * === 실행 컨텍스트 ===
 * - 모든 파싱은 **메인 프로세스의 init 단계**에서 수행. 잡 스레드 생성 전.
 * - fio_clock_source_cb 같은 몇몇 콜백은 시간 초기화를 바로 수행 (즉시 효과).
 * - parse_dryrun() == true 면 부수효과(파일 추가/문자열 복제) 생략 — 단순 검증 모드.
 * - 파싱 중 오류는 log_err()+td_verror() 로 보고, 콜백은 0=성공/!=0=실패 반환.
 */
#include <stdio.h>            /* [한국어] popen/pclose/fread/sprintf — bc_calc 에서 bc(1) 서브프로세스 파이프. FILE*/fopen 계열. */
#include <stdlib.h>           /* [한국어] malloc/calloc/realloc/free/strtol/strtoull/atoi/getenv — 파싱 콜백 전반에서 필수. */
#include <unistd.h>           /* [한국어] 간접 POSIX 정의(pid_t 등). 일부 os 헤더 체인에서 요구. */
#include <ctype.h>            /* [한국어] isdigit — str_numa_mpol_cb 의 nodelist 유효성 검사. */
#include <string.h>           /* [한국어] strcmp/strncmp/strstr/strchr/strsep/strdup/strlen/memcpy/memset/memmove — 거의 모든 콜백이 사용. */
#include <assert.h>           /* [한국어] parse_and_fill_pattern_alloc 반환 이후 패턴 바이트 수 > 0 보증. */
#include <fcntl.h>            /* [한국어] open(2) 관련 플래그 — 일부 옵션 cb가 stat/lstat 계열과 함께 사용. */
#include <sys/stat.h>         /* [한국어] stat/lstat/struct stat/S_ISREG/S_ISDIR — 파일명·디렉토리·외부 엔진 경로 유효성 검증. */
#include <netinet/in.h>       /* [한국어] INET6_ADDRSTRLEN — client_sockaddr_str 버퍼 크기. */

#include "fio.h"             /* [한국어] fio 핵심 — struct thread_data/thread_options, td_verror, log_err,
                              *         dprint(FD_PARSE), td_random/td_read/td_write 매크로, FIO_MAX_OPTS,
                              *         cpus_configured, os_phys_mem, page_size, fio_clock_source 등
                              *         이 파일의 거의 모든 심볼이 여기서 공급된다. */
#include "verify.h"          /* [한국어] VERIFY_NONE/CRC32/MD5/PATTERN/... posval 매핑과 verify_pattern_cb
                              *         내부의 verify 기본값 설정에 사용. */
#include "parse.h"           /* [한국어] struct fio_option/parse_option/parse_cmd_option/options_init/
                              *         options_to_lopts/find_option/fill_default_options/options_free/
                              *         options_mem_dupe/sort_options/string_distance 등 파싱 프레임워크 API.
                              *         이 파일은 정의, parse.c는 해석. */
#include "lib/pattern.h"     /* [한국어] parse_and_fill_pattern_alloc/struct pattern_fmt_desc/paste_blockoff —
                              *         buffer_pattern/verify_pattern 옵션에서 %o(오프셋) 치환 및
                              *         16진수/문자열 패턴 컴파일에 사용. */
#include "options.h"         /* [한국어] 이 파일이 외부에 노출하는 함수 프로토타입 선언. */
#include "optgroup.h"        /* [한국어] FIO_OPT_C_{GENERAL,FILE,IO,ENGINE,STAT,LOG,VERIFY,PROFILE} 카테고리와
                              *         FIO_OPT_G_* 수십 개 세부 그룹. 각 엔트리의 .category/.group 필드에 사용. */
#include "zbd.h"             /* [한국어] Zoned Block Device 관련 매크로/열거형 — zonemode, zone_size 등 옵션의 posval 값. */

/* [한국어] --client 옵션에 사용되는 서버 소켓 주소 문자열.
 * 설정자: 클라이언트 모드 진입 시(server.c) IPv4/IPv6 주소를 INET_ADDRSTRLEN/INET6_ADDRSTRLEN 포맷으로 저장.
 * 읽는 자: set_name_idx() 에서 unique_filename 옵션이 켜졌을 때 파일명 prefix로 사용.
 * 값 범위: 빈 문자열("") 또는 유효한 IP 문자열 (IPv6 최장 45자 + 널 종단).
 * 동기화: 단일 프로세스에서 시작 시 1회 세팅되고 이후 읽기 전용이라 별도 락 불필요. */
char client_sockaddr_str[INET6_ADDRSTRLEN] = { 0 };

/*
 * [한국어] 콜백 데이터(thread_options 포인터) → thread_data 포인터 변환 매크로.
 *
 * 배경: parse.c 의 파서가 cb에 data로 전달하는 값은 &td->o 이다(struct thread_options 포인터).
 * 그러나 콜백은 td->flags, td->file_service_nr, td->zipf_theta 등 thread_data 자체의 필드에
 * 접근해야 할 때가 많다. 이 매크로는 container_of 이디엄으로 thread_options 포인터에서
 * thread_data 포인터를 역산한다 (struct thread_data 안에 struct thread_options o 가 임베드되어 있음).
 *
 * 사용 예: 거의 모든 str_*_cb 콜백 첫 줄에 `struct thread_data *td = cb_data_to_td(data);`
 */
#define cb_data_to_td(data)	container_of(data, struct thread_data, o)

/*
 * [한국어] 버퍼 패턴 포맷 디스크립터 — verify_pattern 에서 "%o" 매크로를 해석.
 *
 * 구조체 배경: struct pattern_fmt_desc {const char *fmt; size_t len; paste_fn *paste;} 는
 * lib/pattern.h 가 제공하는 포맷 디스크립터 테이블 엔트리 타입이다. parse_and_fill_pattern_alloc()
 * 가 패턴 문자열을 파싱하면서 fmt 문자열을 만나면 해당 엔트리의 paste 콜백으로 런타임에 값을 삽입.
 *
 * "%o"  → I/O 오프셋(8바이트)을 버퍼 패턴 슬롯에 삽입. paste_blockoff 가 io_u->offset을 LE로 기록.
 *         예: verify_pattern="%o" → 각 블록 첫 8바이트가 해당 블록의 offset 값 → 잘못된 위치로
 *         쓴 경우 검증 단계에서 offset 불일치로 손상 탐지.
 *
 * 마지막 { } 는 sentinel로 빈 엔트리(fmt=NULL)를 의미해 배열 순회 종료 조건.
 * FIO_FIELD_SIZE(io_u *, offset) 는 sizeof(io_u->offset) = 8 바이트(uint64_t)이므로
 * 패턴 내 "%o" 자리표시자는 8바이트 슬롯을 차지한다.
 */
static const struct pattern_fmt_desc fmt_desc[] = {
	{
		.fmt   = "%o",                                      /* [한국어] 치환할 포맷 문자열 (printf 스타일). */
		.len   = FIO_FIELD_SIZE(struct io_u *, offset),     /* [한국어] 치환 결과가 차지할 바이트 수 = sizeof(uint64_t). */
		.paste = paste_blockoff                             /* [한국어] 런타임에 오프셋을 LE 바이트로 기록하는 콜백. */
	},
	{ }                                                         /* [한국어] sentinel — fmt=NULL 로 순회 종료. */
};

/*
 * [한국어]
 * get_opt_postfix() — 콜론으로 구분된 옵션 문자열의 ":뒤" 부분을 strdup으로 복제해 반환.
 *
 * @str: 원본 옵션 문자열 (예: "mmap:/tmp/hugefile", "rw=write,4k", "zipf:1.2").
 * @return: 콜론 뒤 문자열의 독립 복사본 (호출자가 free 책임). 콜론이 없으면 NULL.
 *
 * 왜 필요한가: fio의 많은 옵션은 "주값:파라미터" 형식을 사용한다. 예를 들어 iomem=mmap:/tmp/xxx,
 * file_service_type=zipf:1.2, ioengine=external:/path/to/so 등. 파서는 먼저 주값(mmap, zipf,
 * external)으로 엔트리를 매칭하고, cb 콜백이 이 함수로 ":뒤" 파라미터를 추출한다.
 *
 * 동작 단계:
 * 1) strstr로 ":" 위치 탐색.
 * 2) 없으면 NULL 반환 (파라미터 없음 — 호출자는 기본 동작).
 * 3) ":" 다음 문자부터 시작해 앞뒤 공백 제거.
 * 4) strdup으로 독립 문자열 반환.
 *
 * 호출 체인: str_mem_cb / str_fst_cb / str_rw_cb / str_sfr_cb / str_random_distribution_cb /
 *           str_steadystate_cb → [get_opt_postfix] → caller uses returned string + frees it.
 *
 * 주의: 반환값은 반드시 free() 해야 함.
 */
static char *get_opt_postfix(const char *str)
{
	char *p = strstr(str, ":");        /* [한국어] 구분자 위치 탐색. */

	if (!p)                            /* [한국어] 콜론 없음 → 파라미터 없음을 NULL로 신호. */
		return NULL;

	p++;                               /* [한국어] 콜론 다음 문자로 이동 (실제 파라미터 시작). */
	strip_blank_front(&p);             /* [한국어] 앞쪽 공백 제거 (parse.h 매크로, p 포인터를 전진). */
	strip_blank_end(p);                /* [한국어] 뒤쪽 공백을 NUL로 치환. */
	return strdup(p);                  /* [한국어] 원본 str이 곧 해제될 수 있으므로 독립 복사본을 반환. */
}

/*
 * [한국어]
 * split_parse_distr() — "값:중심" 형식의 분포 파라미터 문자열을 파싱.
 *
 * @str:    원본 문자열. 예: "1.2", "1.2:0.5". 콜론 뒤는 중심점 (선택적).
 * @val:    [out] 주값 (double). 예: zipf theta, pareto input, gauss dev.
 * @center: [out] 분포 중심점 (0.0~1.0). 콜론이 없으면 변경되지 않음 (호출자가 -1.0 기본 세팅).
 * @return: true=성공, false=파싱 실패 (str_to_float 에러 또는 OOM).
 *
 * 왜 필요한가: random_distribution=zipf:1.2 같은 옵션에서 zipf는 이미 posval로 분리되고,
 * cb가 ":뒤" 부분 "1.2" 만 받는다. 그런데 fio는 추가로 "zipf:1.2:0.5" 형식으로 분포의
 * 중심점을 지정할 수 있다 (0~1 범위, 기본 0.5 = 영역 중앙). 이 함수가 그 2-way 분리를 한다.
 *
 * 동작 단계:
 * 1) str 복제 (strsep이 원본을 수정하므로).
 * 2) ":" 찾기 → 있으면 앞뒤 분리 후 뒤쪽을 center로 파싱.
 * 3) 앞쪽을 val로 파싱.
 * 4) 복제본 free 후 결과 반환.
 *
 * 호출 체인: str_fst_cb / str_random_distribution_cb → [split_parse_distr] → str_to_float.
 */
static bool split_parse_distr(const char *str, double *val, double *center)
{
	char *cp, *p;
	bool r;

	p = strdup(str);                   /* [한국어] 원본 보존을 위해 복제 (이 함수는 로컬 버퍼로 작업). */
	if (!p)                            /* [한국어] OOM 방어. */
		return false;

	cp = strstr(p, ":");               /* [한국어] 중심점 구분자 탐색. */
	r = true;
	if (cp) {                          /* [한국어] ":" 발견 — 뒤쪽이 중심점. */
		*cp = '\0';                    /* [한국어] 주값 부분을 NUL 종단으로 자른다. */
		cp++;                          /* [한국어] 중심점 시작 위치로 진행. */
		r = str_to_float(cp, center, 0); /* [한국어] 문자열 → double. */
	}
	r = r && str_to_float(p, val, 0);  /* [한국어] 중심 파싱 성공 시에만 주값 파싱 (단락 평가). */
	free(p);                           /* [한국어] 복제 버퍼 해제. */
	return r;
}

/*
 * [한국어]
 * bs_cmp() — qsort() 콜백: bssplit 배열을 퍼센트(perc) 오름차순으로 정렬.
 *
 * @p1, @p2: struct bssplit* (void* 캐스트). perc 필드 비교.
 * @return:  p1<p2 → 음수, p1>p2 → 양수, 같으면 0. (qsort 관례)
 *
 * 왜 필요한가: 런타임에 io_u.c::get_next_buflen()가 0~100 난수를 뽑아 어느 bssplit
 * 엔트리에 속하는지 선형/이분 탐색으로 매핑한다. 퍼센트 오름차순 정렬이 되어 있으면
 * 누적 합을 만들며 첫 번째 cumperc >= 난수 인 엔트리를 바로 선택할 수 있어 로직이 간결.
 *
 * 호출 체인: bssplit_ddir() → qsort(o->bssplit[ddir], nr, sizeof(bssplit), [bs_cmp]).
 */
static int bs_cmp(const void *p1, const void *p2)
{
	const struct bssplit *bsp1 = p1;               /* [한국어] void* → struct bssplit* 재해석. */
	const struct bssplit *bsp2 = p2;

	return (int) bsp1->perc - (int) bsp2->perc;    /* [한국어] 단순 차이 — perc가 작은 unsigned라 int 캐스팅. */
}

/*
 * [한국어]
 * split_parse_ddir() — "값/퍼센트:값/퍼센트:..." 형식의 범용 분할 문자열 파서.
 *
 * @o:          thread_options 포인터 (str_to_decimal이 kb_base 같은 맥락을 참조하기 위함).
 * @split:      [out] struct split{ nr, val1[], val2[] } 채울 구조체. val1=주값(bs),
 *              val2=퍼센트 또는 절대값. nr=파싱된 엔트리 수.
 * @str:        원본 문자열(수정됨 — strsep이 NUL을 삽입하므로 호출자가 strdup 필요).
 * @absolute:   true → "/" 뒤를 절대값(바이트 등)으로 파싱. false → 0~100 퍼센트로 해석.
 * @max_splits: BSSPLIT_MAX 등 배열 상한. 초과 시 경고 후 중단.
 * @return:     0=성공, 1=실패 (str_to_decimal 에러 등).
 *
 * 왜 필요한가: bssplit("4k/50:8k/30:16k/20"), zonesplit, cmdprio_bssplit 등 여러 옵션이
 * 동일한 "토큰/파라미터:토큰/파라미터:..." 문법을 공유한다. 공통 파서로 코드 중복 방지.
 *
 * 문법 규칙:
 * - ':' 가 엔트리 구분자. 빈 엔트리는 종료 신호.
 * - '/' 뒤가 없으면 퍼센트 미지정(-1U 마커) — 호출자가 나머지 퍼센트를 균등 분배.
 * - 퍼센트 0이면 -1U로 마킹 (이후 "미지정"으로 재해석됨).
 * - 퍼센트 >100 은 100으로 클램프.
 *
 * 동작 단계:
 * 1) strsep(&str, ":") 로 엔트리 하나씩 꺼냄.
 * 2) 엔트리 내 "/" 위치 찾아 앞=주값, 뒤=퍼센트/절대값 분리.
 * 3) str_to_decimal 로 크기 단위(k/m/g) 포함 숫자 파싱.
 * 4) split->val1[i]=주값, split->val2[i]=퍼센트 저장.
 * 5) max_splits 도달 시 경고 후 중단.
 *
 * 호출 체인: bssplit_ddir / zone_split_ddir → [split_parse_ddir] → strsep / str_to_decimal.
 *
 * 에러 경로: str_to_decimal 실패 → log_err + return 1. 호출자가 전체 파싱을 중단.
 */
int split_parse_ddir(struct thread_options *o, struct split *split,
			    char *str, bool absolute, unsigned int max_splits)
{
	unsigned long long perc;
	unsigned int i;
	long long val;
	char *fname;

	split->nr = 0;

	i = 0;
	while ((fname = strsep(&str, ":")) != NULL) {
		char *perc_str;

		if (!strlen(fname))
			break;

		perc_str = strstr(fname, "/");
		if (perc_str) {
			*perc_str = '\0';
			perc_str++;
			if (absolute) {
				if (str_to_decimal(perc_str, &val, 1, o, 0, 0)) {
					log_err("fio: split conversion failed\n");
					return 1;
				}
				perc = val;
			} else {
				perc = atoi(perc_str);
				if (perc > 100)
					perc = 100;
				else if (!perc)
					perc = -1U;
			}
		} else {
			if (absolute)
				perc = 0;
			else
				perc = -1U;
		}

		if (str_to_decimal(fname, &val, 1, o, 0, 0)) {
			log_err("fio: split conversion failed\n");
			return 1;
		}

		split->val1[i] = val;
		split->val2[i] = perc;
		i++;
		if (i == max_splits) {
			log_err("fio: hit max of %d split entries\n", i);
			break;
		}
	}

	split->nr = i;
	return 0;
}

/*
 * [한국어]
 * bssplit_ddir() — 특정 방향(DDIR_READ/WRITE/TRIM) 에 대한 bssplit 배열 구축.
 *
 * @o:    thread_options (결과를 o->bssplit[ddir] 에 저장).
 * @eo:   unused.
 * @ddir: DDIR_READ / DDIR_WRITE / DDIR_TRIM.
 * @str:  방향별 문자열. 예: "4k/50:8k/30:16k/20". str_split_parse 가 쉼표로 분리한 결과.
 * @data: unused (str_split_parse 시그니처 준수).
 * @return: 0=성공, 1=실패.
 *
 * 왜 필요한가: "bs" 옵션은 단일 값이지만 bssplit 은 여러 블록 크기의 확률 분포. 예: 50% 확률로 4K,
 * 30%로 8K, 20%로 16K. 런타임에 io_u.c::get_next_buflen 이 난수 뽑아 이 분포에서 블록 크기 추첨.
 *
 * 동작 단계:
 * 1) split_parse_ddir 로 "값/퍼센트" 리스트 파싱 (일반 헬퍼 재사용).
 * 2) o->bssplit[ddir] 에 struct bssplit[.nr] 할당.
 * 3) 각 엔트리를 .bs/.perc 로 복사 + max_bs/min_bs 추적.
 * 4) 퍼센트 합계 검증 (>100% 거부) + 미지정(-1U) 엔트리에 나머지 균등 분배.
 * 5) o->min_bs[ddir]/max_bs[ddir] 세팅 — io_u.c 버퍼 크기 결정에 사용.
 * 6) qsort(bs_cmp) — 퍼센트 오름차순 정렬. 런타임 누적합 탐색 간소화.
 *
 * 호출 체인: str_bssplit_cb → str_split_parse → [bssplit_ddir] → split_parse_ddir → qsort.
 */
static int bssplit_ddir(struct thread_options *o, void *eo,
			enum fio_ddir ddir, char *str, bool data)
{
	unsigned int i, perc, perc_missing;
	unsigned long long max_bs, min_bs;
	struct split split;

	memset(&split, 0, sizeof(split));

	if (split_parse_ddir(o, &split, str, data, BSSPLIT_MAX))
		return 1;
	if (!split.nr)
		return 0;

	max_bs = 0;
	min_bs = -1;
	o->bssplit[ddir] = malloc(split.nr * sizeof(struct bssplit));
	o->bssplit_nr[ddir] = split.nr;
	for (i = 0; i < split.nr; i++) {
		if (split.val1[i] > max_bs)
			max_bs = split.val1[i];
		if (split.val1[i] < min_bs)
			min_bs = split.val1[i];

		o->bssplit[ddir][i].bs = split.val1[i];
		o->bssplit[ddir][i].perc =split.val2[i];
	}

	/*
	 * Now check if the percentages add up, and how much is missing
	 */
	perc = perc_missing = 0;
	for (i = 0; i < o->bssplit_nr[ddir]; i++) {
		struct bssplit *bsp = &o->bssplit[ddir][i];

		if (bsp->perc == -1U)
			perc_missing++;
		else
			perc += bsp->perc;
	}

	if (perc > 100 && perc_missing > 1) {
		log_err("fio: bssplit percentages add to more than 100%%\n");
		free(o->bssplit[ddir]);
		o->bssplit[ddir] = NULL;
		return 1;
	}

	/*
	 * If values didn't have a percentage set, divide the remains between
	 * them.
	 */
	if (perc_missing) {
		if (perc_missing == 1 && o->bssplit_nr[ddir] == 1)
			perc = 100;
		for (i = 0; i < o->bssplit_nr[ddir]; i++) {
			struct bssplit *bsp = &o->bssplit[ddir][i];

			if (bsp->perc == -1U)
				bsp->perc = (100 - perc) / perc_missing;
		}
	}

	o->min_bs[ddir] = min_bs;
	o->max_bs[ddir] = max_bs;

	/*
	 * now sort based on percentages, for ease of lookup
	 */
	qsort(o->bssplit[ddir], o->bssplit_nr[ddir], sizeof(struct bssplit), bs_cmp);
	return 0;
}

/*
 * [한국어]
 * str_split_parse() — 읽기/쓰기/트림 방향 3-way 분할 파싱 드라이버.
 *
 * @td:   thread_data.
 * @str:  "읽기값,쓰기값,트림값" 형식의 문자열 (수정됨).
 * @fn:   각 방향별로 호출될 콜백 (bssplit_ddir 또는 zone_split_ddir).
 * @eo:   unused (콜백 시그니처 유지용).
 * @data: fn 에 그대로 전달 (예: zone_split에서 absolute 여부 flag).
 * @return: fn 의 반환값 (하나라도 실패하면 1).
 *
 * 왜 필요한가: bssplit/zonesplit 같은 옵션은 3-way 쉼표 분리 후 각 방향별로 내부에 ":" 기반
 * 분포 문법을 가진다. 공통 패턴을 이 드라이버가 캡슐화.
 *
 * 문법 규칙:
 *   "읽기부분"                     → 3방향 모두에 동일 적용.
 *   "읽기부분,쓰기부분"             → 트림은 쓰기와 동일.
 *   "읽기부분,쓰기부분,트림부분"    → 3방향 명시.
 *
 * 예: bssplit=4k/50:8k/50,16k/100
 *     → READ: 4k(50%)+8k(50%), WRITE: 16k(100%), TRIM: 16k(100%).
 *
 * 동작 단계:
 * 1) 첫 ',' 위치 찾기. 없으면 strdup 복사본을 3방향 모두에 적용.
 * 2) ',' 있으면 두 번째 ',' 도 검색 → 1개 또는 2개에 따라 2/3 분리.
 * 3) 역순 호출: TRIM → WRITE → READ (nested NULL 삽입 편의).
 * 4) strdup 복사본 사용하여 strsep 이 원본 훼손하지 않도록.
 *
 * 호출 체인: str_bssplit_cb / parse_zoned_distribution → [str_split_parse] → bssplit_ddir | zone_split_ddir.
 */
int str_split_parse(struct thread_data *td, char *str,
		    split_parse_fn *fn, void *eo, bool data)
{
	char *odir, *ddir;
	int ret = 0;

	odir = strchr(str, ',');
	if (odir) {
		ddir = strchr(odir + 1, ',');
		if (ddir) {
			ret = fn(&td->o, eo, DDIR_TRIM, ddir + 1, data);
			if (!ret)
				*ddir = '\0';
		} else {
			char *op;

			op = strdup(odir + 1);
			ret = fn(&td->o, eo, DDIR_TRIM, op, data);

			free(op);
		}
		if (!ret)
			ret = fn(&td->o, eo, DDIR_WRITE, odir + 1, data);
		if (!ret) {
			*odir = '\0';
			ret = fn(&td->o, eo, DDIR_READ, str, data);
		}
	} else {
		char *op;

		op = strdup(str);
		ret = fn(&td->o, eo, DDIR_WRITE, op, data);
		free(op);

		if (!ret) {
			op = strdup(str);
			ret = fn(&td->o, eo, DDIR_TRIM, op, data);
			free(op);
		}
		if (!ret)
			ret = fn(&td->o, eo, DDIR_READ, str, data);
	}

	return ret;
}

/*
 * [한국어]
 * fio_fdp_cmp() — qsort 비교자: uint16_t FDP Placement ID 오름차순 정렬.
 *
 * @p1, @p2: uint16_t* (void*로 전달).
 * @return: 차이값.
 *
 * 왜 필요한가: str_fdp_pli_cb 가 "1,2,5-7" 같은 입력을 ID 배열로 확장한 후 이 비교자로 정렬.
 * 런타임에 fio가 FDP write 시 라운드로빈 또는 랜덤 선택 시 일관된 순서 보장.
 */
static int fio_fdp_cmp(const void *p1, const void *p2)
{
	const uint16_t *t1 = p1;
	const uint16_t *t2 = p2;

	return *t1 - *t2;
}

/*
 * [한국어]
 * str_fdp_pli_cb() — fdp_pli (Flexible Data Placement — Placement Identifier List) 옵션 콜백.
 *
 * @data:  &td->o.
 * @input: "1,2,3-5" 형식. 콤마 구분 ID와 '-' 범위 혼용 허용.
 * @return: 0=성공, 1=실패(범위 초과, 잘못된 순서).
 *
 * 왜 필요한가: NVMe FDP (TP 4146) 는 SSD 내부의 Reclaim Unit Handle (RUH) 에 대응되는 Placement ID
 * (PI) 를 호스트가 명시 제공하여 쓰기 데이터를 특정 NAND 블록 그룹에 배치. Data Placement 제어로
 * GC 효율/수명 향상. 이 옵션으로 사용할 PI 집합 지정.
 *
 * 동작 단계:
 * 1) 입력 복제 + 공백 스트립.
 * 2) strsep(',') 로 항목 분리.
 * 3) 각 항목을 strsep('-') 로 시작/끝 ID 분리 (단일 ID는 끝=-1).
 * 4) strtoull 로 정수 변환. start>end 검증.
 * 5) FIO_MAX_DP_IDS / 0xFFFF 상한 검사.
 * 6) td->o.dp_ids[] 에 평탄화해 저장.
 * 7) 정렬: qsort(fio_fdp_cmp) — 런타임 선택 시 결정적 순서.
 *
 * 호출 체인: parse.c → [str_fdp_pli_cb] → strtoull/qsort.
 * 읽는 자: engines/io_uring.c / xnvme.c 등이 FDP 쓰기 SQE 작성 시 dp_ids[] 참조.
 */
static int str_fdp_pli_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	char *str, *p, *id1;
	int i = 0, ret = 0;

	if (!input)
		return 1;

	p = str = strdup(input);
	strip_blank_front(&str);
	strip_blank_end(str);

	while ((id1 = strsep(&str, ",")) != NULL) {
		char *str2, *id2;
		unsigned int start, end;

		if (!strlen(id1))
			break;

		str2 = id1;
		end = -1;
		while ((id2 = strsep(&str2, "-")) != NULL) {
			if (!strlen(id2))
				break;

			end = strtoull(id2, NULL, 0);
		}

		start = strtoull(id1, NULL, 0);
		if (end == -1)
			end = start;
		if (start > end) {
			ret = 1;
			break;
		}

		while (start <= end) {
			if (i >= FIO_MAX_DP_IDS) {
				log_err("fio: only %d IDs supported\n", FIO_MAX_DP_IDS);
				ret = 1;
				break;
			}
			if (start > 0xFFFF) {
				log_err("Placement IDs cannot exceed 0xFFFF\n");
				ret = 1;
				break;
			}
			td->o.dp_ids[i++] = start++;
		}

		if (ret)
			break;
	}

	free(p);

	qsort(td->o.dp_ids, i, sizeof(*td->o.dp_ids), fio_fdp_cmp);
	td->o.dp_nr_ids = i;

	return ret;
}

/*
 * [한국어]
 * str_dp_scheme_cb() — dp_scheme 옵션의 파일 경로 유효성 검증.
 *
 * @data:  &td->o. dp_scheme_file 은 이미 FIO_OPT_STR_STORE 로 저장됨.
 * @input: 원본 옵션 문자열 (사용하지 않음 — td->o.dp_scheme_file 직접 접근).
 * @return: 0=성공, errno=lstat 실패 또는 정규 파일 아님.
 *
 * 왜 필요한가: Data Placement scheme 을 JSON 파일로 전달받아 복잡한 placement 정책을 기술.
 * 파싱 단계에서 파일 존재+정규파일임을 조기 검증해 나중에 I/O 중 실패하지 않도록.
 *
 * 동작 단계:
 * 1) parse_dryrun 방어.
 * 2) dp_scheme_file 복사 후 공백 스트립 → 원본 덮어쓰기 (strcpy 는 같은 길이라 안전).
 * 3) lstat 실패 → td_verror 반영.
 * 4) S_ISREG 아님 → 에러 (디렉토리/심볼릭 거부).
 */
static int str_dp_scheme_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	struct stat sb;
	char *filename;
	int ret = 0;

	if (parse_dryrun())
		return 0;

	filename = strdup(td->o.dp_scheme_file);
	strip_blank_front(&filename);
	strip_blank_end(filename);

	strcpy(td->o.dp_scheme_file, filename);

	if (lstat(filename, &sb) < 0){
		ret = errno;
		log_err("fio: lstat() error related to %s\n", filename);
		td_verror(td, ret, "lstat");
		goto out;
	}

	if (!S_ISREG(sb.st_mode)) {
		ret = errno;
		log_err("fio: %s is not a file\n", filename);
		td_verror(td, ret, "S_ISREG");
		goto out;
	}

out:
	free(filename);
	return ret;
}

/*
 * [한국어]
 * str_bssplit_cb() — bssplit 옵션 콜백: 블록 크기 확률 분포 파싱 진입점.
 *
 * @data:  &td->o.
 * @input: "4k/50:8k/30:16k/20" 형식의 ":" 구분 분포 또는 "읽기분포,쓰기분포,트림분포" 3-way.
 * @return: 0=성공, 1=실패.
 *
 * 왜 필요한가: 실제 워크로드는 단일 블록 크기가 아니라 여러 크기의 혼합. bssplit 은
 * "50% 확률로 4K, 30%로 8K, 20%로 16K" 같은 분포를 지정. io_u.c::get_next_buflen 이 매 I/O
 * 마다 이 분포에서 난수 뽑아 블록 크기 결정.
 *
 * 동작: str_split_parse 로 3-way 분리 → 각 방향은 bssplit_ddir 으로 위임.
 * dryrun 시 결과 배열 해제 (부수효과 제거).
 *
 * 호출 체인: parse.c → [str_bssplit_cb] → str_split_parse → bssplit_ddir → split_parse_ddir.
 */
static int str_bssplit_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	char *str, *p;
	int ret = 0;

	p = str = strdup(input);

	strip_blank_front(&str);
	strip_blank_end(str);

	ret = str_split_parse(td, str, bssplit_ddir, NULL, false);

	if (parse_dryrun()) {
		int i;

		for (i = 0; i < DDIR_RWDIR_CNT; i++) {
			free(td->o.bssplit[i]);
			td->o.bssplit[i] = NULL;
			td->o.bssplit_nr[i] = 0;
		}
	}

	free(p);
	return ret;
}

/*
 * [한국어]
 * parse_cmdprio_bssplit_entry() — cmdprio_bssplit 의 개별 엔트리 파싱.
 *
 * @o:     thread_options (str_to_decimal 컨텍스트).
 * @entry: [out] 결과 채워짐. {bs, perc, prio}.
 * @str:   "bs/perc/class/level/hint" 형식. 뒤쪽 필드는 선택적.
 * @return: 0=성공, 1=실패.
 *
 * 왜 필요한가: libaio/io_uring/sg 엔진에서 블록 크기별로 다른 I/O 우선순위 적용. 예: "4k/50/1/2/0"
 * 는 4K 블록의 50%에 대해 IOPRIO_CLASS=1(RT), level=2, hint=0 으로 ioprio_set.
 *
 * 입력 형식 (%m[^/]/%u/%u/%u/%u):
 *   bs/                        (perc=0, prio=-1)
 *   bs/perc                    (prio=-1)
 *   bs/perc/class/level
 *   bs/perc/class/level/hint
 *
 * 동작 단계:
 * 1) sscanf 로 bs_str / perc / class / level / hint 추출. matches 변수로 파싱된 필드 수 확인.
 * 2) bs_str → str_to_decimal (k/m/g 단위 처리).
 * 3) perc min(perc, 100) 으로 클램프.
 * 4) matches 에 따라 class/level/hint 해석:
 *    - matches=1 or 2: prio=-1 (우선순위 미적용).
 *    - matches=4: class+level만. hint=0.
 *    - matches=5: class+level+hint.
 *    - 기타: 형식 오류.
 * 5) ioprio_value(class, level, hint) 로 16비트 ioprio 값 인코딩.
 *
 * 호출 체인: split_parse_prio_ddir → [parse_cmdprio_bssplit_entry] → str_to_decimal / ioprio_value.
 */
static int parse_cmdprio_bssplit_entry(struct thread_options *o,
				       struct split_prio *entry, char *str)
{
	int matches = 0;
	char *bs_str = NULL;
	long long bs_val;
	unsigned int perc = 0, class, level, hint;

	/*
	 * valid entry formats:
	 * bs/ - %s/ - set perc to 0, prio to -1.
	 * bs/perc - %s/%u - set prio to -1.
	 * bs/perc/class/level - %s/%u/%u/%u
	 * bs/perc/class/level/hint - %s/%u/%u/%u/%u
	 */
	matches = sscanf(str, "%m[^/]/%u/%u/%u/%u",
			 &bs_str, &perc, &class, &level, &hint);
	if (matches < 1) {
		log_err("fio: invalid cmdprio_bssplit format\n");
		return 1;
	}

	if (str_to_decimal(bs_str, &bs_val, 1, o, 0, 0)) {
		log_err("fio: split conversion failed\n");
		free(bs_str);
		return 1;
	}
	free(bs_str);

	entry->bs = bs_val;
	entry->perc = min(perc, 100u);
	entry->prio = -1;
	switch (matches) {
	case 1: /* bs/ case */
	case 2: /* bs/perc case */
		break;
	case 4: /* bs/perc/class/level case */
	case 5: /* bs/perc/class/level/hint case */
		class = min(class, (unsigned int) IOPRIO_MAX_PRIO_CLASS);
		level = min(level, (unsigned int) IOPRIO_MAX_PRIO);
		if (matches == 5)
			hint = min(hint, (unsigned int) IOPRIO_MAX_PRIO_HINT);
		else
			hint = 0;
		entry->prio = ioprio_value(class, level, hint);
		break;
	default:
		log_err("fio: invalid cmdprio_bssplit format\n");
		return 1;
	}

	return 0;
}

/*
 * Returns a negative integer if the first argument should be before the second
 * argument in the sorted list. A positive integer if the first argument should
 * be after the second argument in the sorted list. A zero if they are equal.
 */
/*
 * [한국어]
 * fio_split_prio_cmp() — qsort 비교자: split_prio.bs 오름차순.
 *
 * 왜 필요한가: split_parse_prio_ddir 결과 정렬에 사용. 런타임에 block size → priority 이진 탐색 가능.
 */
static int fio_split_prio_cmp(const void *p1, const void *p2)
{
	const struct split_prio *tmp1 = p1;
	const struct split_prio *tmp2 = p2;

	if (tmp1->bs > tmp2->bs)
		return 1;
	if (tmp1->bs < tmp2->bs)
		return -1;
	return 0;
}

/*
 * [한국어]
 * split_parse_prio_ddir() — cmdprio_bssplit 의 방향별 파싱 드라이버.
 *
 * @o:          thread_options.
 * @entries:    [out] calloc 된 split_prio 배열.
 * @nr_entries: [out] 엔트리 수.
 * @str:        ":" 구분 엔트리 리스트.
 * @return:     0=성공, 1=실패 (parse_cmdprio_bssplit_entry 실패 또는 OOM).
 *
 * 왜 필요한가: cmdprio_bssplit=4k/50/1/2:8k/30/2/1 의 "4k/50/1/2", "8k/30/2/1" 각 엔트리를
 * parse_cmdprio_bssplit_entry 로 파싱하고 bs 오름차순 정렬.
 *
 * 동작 단계 (2-pass):
 * 1) 1st pass (복제본 소모) — 엔트리 개수 세기 (BSSPLIT_MAX 상한 검증).
 * 2) calloc 으로 배열 할당.
 * 3) 2nd pass — 실제 파싱. perc=0 엔트리는 무의미하므로 skip.
 * 4) qsort(fio_split_prio_cmp) — bs 오름차순.
 *
 * 호출 체인: cmdprio.c::fio_cmdprio_parse_and_gen_bssplit → [split_parse_prio_ddir] →
 *            parse_cmdprio_bssplit_entry.
 */
int split_parse_prio_ddir(struct thread_options *o, struct split_prio **entries,
			  int *nr_entries, char *str)
{
	struct split_prio *tmp_entries;
	unsigned int nr_bssplits;
	char *str_cpy, *p, *fname;

	/* strsep modifies the string, dup it so that we can use strsep twice */
	p = str_cpy = strdup(str);
	if (!p)
		return 1;

	nr_bssplits = 0;
	while ((fname = strsep(&str_cpy, ":")) != NULL) {
		if (!strlen(fname))
			break;
		nr_bssplits++;
	}
	free(p);

	if (nr_bssplits > BSSPLIT_MAX) {
		log_err("fio: too many cmdprio_bssplit entries\n");
		return 1;
	}

	tmp_entries = calloc(nr_bssplits, sizeof(*tmp_entries));
	if (!tmp_entries)
		return 1;

	nr_bssplits = 0;
	while ((fname = strsep(&str, ":")) != NULL) {
		struct split_prio *entry;

		if (!strlen(fname))
			break;

		entry = &tmp_entries[nr_bssplits];

		if (parse_cmdprio_bssplit_entry(o, entry, fname)) {
			log_err("fio: failed to parse cmdprio_bssplit entry\n");
			free(tmp_entries);
			return 1;
		}

		/* skip zero perc entries, they provide no useful information */
		if (entry->perc)
			nr_bssplits++;
	}

	qsort(tmp_entries, nr_bssplits, sizeof(*tmp_entries),
	      fio_split_prio_cmp);

	*entries = tmp_entries;
	*nr_entries = nr_bssplits;

	return 0;
}

/*
 * [한국어]
 * str2error() — POSIX/Linux errno 이름 문자열을 정수 errno 값으로 변환.
 *
 * @str: "EINVAL" / "ENOENT" / "EWOULDBLOCK" 등의 심볼 이름.
 * @return: 해당 errno 정수 (1..134). 미매칭 시 0.
 *
 * 왜 필요한가: ignore_error 옵션이 "EIO,EAGAIN" 처럼 errno 이름을 받게 하려면 이름→숫자 매핑 필요.
 * <errno.h> 는 심볼 이름→번호 런타임 매핑을 제공하지 않으므로 직접 테이블 유지.
 *
 * 동작: err[] 배열 선형 탐색 (약 130개). 매칭되면 인덱스+1 이 errno 값 (POSIX/Linux 번호 순서 일치 가정).
 *       2개 별칭 예외: EWOULDBLOCK → EAGAIN, EDEADLOCK → EDEADLK.
 *
 * 주의: 테이블 순서가 Linux <asm-generic/errno.h> / <asm-generic/errno-base.h> 의 번호 순서와 일치해야 함.
 *       플랫폼 간 번호가 다를 수 있지만 fio는 주로 Linux 타겟.
 */
static int str2error(char *str)
{
	const char *err[] = { "EPERM", "ENOENT", "ESRCH", "EINTR", "EIO",
			    "ENXIO", "E2BIG", "ENOEXEC", "EBADF",
			    "ECHILD", "EAGAIN", "ENOMEM", "EACCES",
			    "EFAULT", "ENOTBLK", "EBUSY", "EEXIST",
			    "EXDEV", "ENODEV", "ENOTDIR", "EISDIR",
			    "EINVAL", "ENFILE", "EMFILE", "ENOTTY",
			    "ETXTBSY","EFBIG", "ENOSPC", "ESPIPE",
			    "EROFS","EMLINK", "EPIPE", "EDOM", "ERANGE",
			    "EDEADLK", "ENAMETOOLONG", "ENOLCK", "ENOSYS", "ENOTEMPTY",
			    "ELOOP", "EWOULDBLOCK", "ENOMSG", "EIDRM", "ECHRNG",
			    "EL2NSYNC", "EL3HLT", "EL3RST", "ELNRNG", "EUNATCH",
			    "ENOCSI", "EL2HLT", "EBADE", "EBADR", "EXFULL",
			    "ENOANO", "EBADRQC", "EBADSLT", "EDEADLOCK", "EBFONT",
			    "ENOSTR", "ENODATA", "ETIME", "ENOSR", "ENONET",
			    "ENOPKG", "EREMOTE", "ENOLINK", "EADV", "ESRMNT",
			    "ECOMM", "EPROTO", "EMULTIHOP", "EDOTDOT", "EBADMSG",
			    "EOVERFLOW", "ENOTUNIQ", "EBADFD", "EREMCHG", "ELIBACC",
			    "ELIBBAD", "ELIBSCN", "ELIBMAX", "ELIBEXEC", "EILSEQ",
			    "ERESTART", "ESTRPIPE", "EUSERS", "ENOTSOCK", "EDESTADDRREQ",
			    "EMSGSIZE", "EPROTOTYPE", "ENOPROTOOPT", "EPROTONOSUPPORT", "ESOCKTNOSUPPORT",
			    "EOPNOTSUPP", "EPFNOSUPPORT", "EAFNOSUPPORT", "EADDRINUSE", "EADDRNOTAVAIL",
			    "ENETDOWN", "ENETUNREACH", "ENETRESET", "ECONNABORTED", "ECONNRESET",
			    "ENOBUFS", "EISCONN", "ENOTCONN", "ESHUTDOWN", "ETOOMANYREFS",
			    "ETIMEDOUT", "ECONNREFUSED", "EHOSTDOWN", "EHOSTUNREACH", "EALREADY",
			    "EINPROGRESS", "ESTALE", "EUCLEAN", "ENOTNAM", "ENAVAIL",
			    "EISNAM", "EREMOTEIO", "EDQUOT", "ENOMEDIUM", "EMEDIUMTYPE",
			    "ECANCELED", "ENOKEY", "EKEYEXPIRED", "EKEYREVOKED", "EKEYREJECTED",
			    "EOWNERDEAD", "ENOTRECOVERABLE", "ERFKILL", "EHWPOISON" };
	int i = 0, num = sizeof(err) / sizeof(char *);
	int retval;

	while (i < num) {
		if (!strcmp(err[i], str)) {
			retval = i + 1;
			/* Handle errno aliases that should map to actual errno values */
			if (!strcmp(str, "EWOULDBLOCK"))
				retval = EAGAIN;
			else if (!strcmp(str, "EDEADLOCK"))
				retval = EDEADLK;
			return retval;
		}
		i++;
	}
	return 0;
}

/*
 * [한국어]
 * ignore_error_type() — str_ignore_error_cb 의 방향별 errno 리스트 파싱 헬퍼.
 *
 * @td:    thread_data.
 * @etype: 방향 enum (ERROR_TYPE_READ_BIT/WRITE_BIT/TRIM_BIT).
 * @str:   ":" 구분 errno 리스트 문자열 (수정됨).
 * @return: 0=성공, 1=알 수 없는 토큰/범위 초과.
 *
 * 왜 필요한가: "EIO:EAGAIN:EINVAL" 같은 리스트를 파싱해 td->o.ignore_error[etype] 정수 배열 구축.
 * "E" 접두사 문자열은 str2error로 변환, 그 외는 decimal/hex 정수로 해석 (0x 접두사 지원).
 *
 * 동작 단계:
 * 1) 초기 배열 크기 4 할당 (calloc). 4 초과 시 동적으로 << 1 (배가) 재할당.
 * 2) strsep(':') 로 토큰화.
 * 3) 토큰 첫 문자 'E' 면 str2error → errno 번호, 아니면 strtol (base 10 or 16).
 * 4) 음수는 절대값으로 정규화.
 * 5) 0 (알 수 없음) 은 에러.
 * 6) 성공하면 continue_on_error 비트마스크에 1<<etype 비트 세팅.
 *
 * 호출 체인: str_ignore_error_cb → [ignore_error_type] → str2error / strtol.
 */
static int ignore_error_type(struct thread_data *td, enum error_type_bit etype,
				char *str)
{
	unsigned int i;
	int *error;
	char *fname;

	if (etype >= ERROR_TYPE_CNT) {
		log_err("Illegal error type\n");
		return 1;
	}

	td->o.ignore_error_nr[etype] = 4;
	error = calloc(4, sizeof(int));

	i = 0;
	while ((fname = strsep(&str, ":")) != NULL) {

		if (!strlen(fname))
			break;

		/*
		 * grow struct buffer, if needed
		 */
		if (i == td->o.ignore_error_nr[etype]) {
			td->o.ignore_error_nr[etype] <<= 1;
			error = realloc(error, td->o.ignore_error_nr[etype]
						  * sizeof(int));
		}
		if (fname[0] == 'E') {
			error[i] = str2error(fname);
		} else {
			int base = 10;
			if (!strncmp(fname, "0x", 2) ||
					!strncmp(fname, "0X", 2))
				base = 16;
			error[i] = strtol(fname, NULL, base);
			if (error[i] < 0)
				error[i] = -error[i];
		}
		if (!error[i]) {
			log_err("Unknown error %s, please use number value\n",
				  fname);
			td->o.ignore_error_nr[etype] = 0;
			free(error);
			return 1;
		}
		i++;
	}
	if (i) {
		td->o.continue_on_error |= 1 << etype;
		td->o.ignore_error_nr[etype] = i;
		td->o.ignore_error[etype] = error;
	} else {
		td->o.ignore_error_nr[etype] = 0;
		free(error);
	}

	return 0;

}

/*
 * [한국어]
 * str_replay_skip_cb() — replay_skip 옵션 콜백.
 *
 * @data:  parse.c 가 전달한 &td->o (cb_data_to_td 로 td 역산).
 * @input: 콤마 구분 방향 리스트. 예: "read", "write,trim", "read,write,trim,sync".
 * @return: 0=성공, 1=알 수 없는 토큰 또는 NULL input.
 *
 * 왜 필요한가: blktrace 재생(read_iolog_file) 모드에서 특정 방향의 I/O 이벤트를
 * 건너뛸 때 사용한다. 디바이스 특성상 TRIM 을 지원하지 않거나 READ 만 측정하려는
 * 경우, 원본 trace의 해당 방향 엔트리를 무시하도록 마킹한다.
 *
 * 동작 단계:
 * 1) parse_dryrun() 이면 부수효과 없이 0 반환.
 * 2) input 복제 후 공백 스트립.
 * 3) strchr(',') 로 한 토큰씩 분리.
 * 4) read/write/trim/sync 매칭 시 td->o.replay_skip 비트마스크에 1u<<DDIR_* 설정.
 * 5) 알 수 없는 토큰이면 에러 후 break.
 *
 * 호출 체인: parse.c::parse_option → [str_replay_skip_cb] → iolog.c::read_iolog_get()
 *            가 이 마스크를 보고 해당 ddir 을 스킵.
 * 읽는 자: iolog.c 의 replay 루프.
 */
static int str_replay_skip_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	char *str, *p, *n;
	int ret = 0;

	if (parse_dryrun())
		return 0;
	if (!input)
		return 1;

	p = str = strdup(input);

	strip_blank_front(&str);
	strip_blank_end(str);

	while (p) {
		n = strchr(p, ',');
		if (n)
			*n++ = '\0';
		if (!strcmp(p, "read"))
			td->o.replay_skip |= 1u << DDIR_READ;
		else if (!strcmp(p, "write"))
			td->o.replay_skip |= 1u << DDIR_WRITE;
		else if (!strcmp(p, "trim"))
			td->o.replay_skip |= 1u << DDIR_TRIM;
		else if (!strcmp(p, "sync"))
			td->o.replay_skip |= 1u << DDIR_SYNC;
		else {
			log_err("Unknown skip type: %s\n", p);
			ret = 1;
			break;
		}
		p = n;
	}
	free(str);
	return ret;
}

/*
 * [한국어]
 * str_ignore_error_cb() — ignore_error 옵션 콜백.
 *
 * @data:  &td->o.
 * @input: "읽기errno:...,쓰기errno:...,트림errno:..." 형식. 쉼표 3-way 분리.
 *         예: "EIO:EAGAIN,ENOSPC,ENODEV" — READ는 EIO/EAGAIN, WRITE는 ENOSPC, TRIM은 ENODEV 무시.
 * @return: 0=성공, 1=실패.
 *
 * 왜 필요한가: 벤치마크 진행 중 특정 errno를 만나도 잡을 중단하지 않고 계속 진행할 때
 * 사용. 예: 용량 초과(ENOSPC) 시 에러 대신 재시도, 또는 간헐 장애 시 통계 누적만.
 *
 * 동작 단계:
 * 1) parse_dryrun() 방어.
 * 2) input 복제+공백 스트립.
 * 3) strchr(',') 로 방향 단위 분리 — type 카운터로 DDIR_READ→WRITE→TRIM 매핑.
 * 4) 각 방향 토큰을 ignore_error_type() 에 넘겨 ":" 내부 errno 이름/번호 파싱.
 * 5) ignore_error_type 이 td->o.ignore_error[type]/nr 채움 + continue_on_error 비트 세팅.
 *
 * 호출 체인: parse.c → [str_ignore_error_cb] → ignore_error_type → str2error.
 * 읽는 자: backend.c::do_io()가 I/O 에러 시 td->o.ignore_error[ddir] 검사.
 */
static int str_ignore_error_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	char *str, *p, *n;
	int ret = 1;
	enum error_type_bit type = 0;

	if (parse_dryrun())
		return 0;
	if (!input)
		return 1;

	p = str = strdup(input);

	strip_blank_front(&str);
	strip_blank_end(str);

	while (p) {
		n = strchr(p, ',');
		if (n)
			*n++ = '\0';
		ret = ignore_error_type(td, type, p);
		if (ret)
			break;
		p = n;
		type++;
	}
	free(str);
	return ret;
}

/*
 * [한국어]
 * str_rw_cb() — rw(readwrite) 옵션의 ",후위 파라미터" 처리 콜백.
 *
 * @data: &td->o. parse.c 는 이미 `rw=randread` 의 주값(posval)을 td_ddir 에 매핑 완료.
 * @str:  원본 전체 옵션 문자열. 예: "rw=write,4k" 또는 "rw=randrw,16" 또는 단순 "rw=read".
 * @return: 0=성공, 1=후위 파싱 실패.
 *
 * 왜 필요한가: fio rw 옵션은 주값 뒤에 콤마로 2가지 파라미터를 더 받는다:
 *   (a) 순차(read/write/rw) 잡: offset — 각 I/O 사이의 고정 오프셋 증분(ddir_seq_add).
 *       예: "rw=write,4k" → 오프셋 0, 4K, 8K, 12K 순으로 blk 단위 +4K 점프.
 *   (b) 랜덤(randread/randwrite/randrw) 잡: nr — 같은 블록을 몇 번 반복 I/O 할지(ddir_seq_nr).
 *       예: "rw=randrw,16" → 랜덤 위치 하나 뽑고 16번 반복 후 다음 랜덤 위치.
 *
 * 동작 단계:
 * 1) parse_dryrun() 방어 — 부수효과 없이 0 반환.
 * 2) ddir_seq_nr=1, ddir_seq_add=0 기본값 설정.
 * 3) get_opt_postfix(str) 로 ':' 뒤의 후위 문자열 추출(실제로는 get_opt_postfix가 콜론 기반이지만
 *    여기서는 parse.c 가 "rw=" 제거 후 콤마로 재변환한 결과를 받는다).
 * 4) td_random(td)이면 nr(반복 횟수)로 해석, 아니면 add(오프셋 증분)로 해석.
 * 5) str_to_decimal 로 k/m/g 단위 처리.
 *
 * 호출 체인: parse.c::parse_option(rw posval 매핑 후) → [str_rw_cb] → str_to_decimal.
 * 읽는 자: io_u.c::get_next_offset() 가 ddir_seq_add/nr 을 사용해 다음 I/O 위치 계산.
 */
static int str_rw_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);
	struct thread_options *o = &td->o;
	char *nr;

	if (parse_dryrun())
		return 0;

	o->ddir_seq_nr = 1;
	o->ddir_seq_add = 0;

	nr = get_opt_postfix(str);
	if (!nr)
		return 0;

	if (td_random(td)) {
		long long val;

		if (str_to_decimal(nr, &val, 1, o, 0, 0)) {
			log_err("fio: randrw postfix parsing failed\n");
			free(nr);
			return 1;
		}
		if ((val <= 0) || (val > UINT_MAX)) {
			log_err("fio: randrw postfix parsing out of range\n");
			free(nr);
			return 1;
		}
		o->ddir_seq_nr = (unsigned int) val;
	} else {
		long long val;

		if (str_to_decimal(nr, &val, 1, o, 0, 0)) {
			log_err("fio: rw postfix parsing failed\n");
			free(nr);
			return 1;
		}

		o->ddir_seq_add = val;
	}

	free(nr);
	return 0;
}

/*
 * [한국어]
 * str_mem_cb() — iomem(mem) 옵션의 ":경로" 후위 처리 콜백.
 *
 * @data: &td->o. mem_type 은 이미 parse.c 가 posval(malloc/shm/shmhuge/mmap/mmaphuge/mmapshared/cudamalloc)
 *        매핑으로 설정 완료.
 * @mem:  원본 옵션 문자열. 예: "iomem=mmap:/mnt/huge/fio.mem", "iomem=malloc".
 * @return: 항상 0.
 *
 * 왜 필요한가: mmap 계열 iomem(MMAPHUGE, MMAP, MMAPSHARED)은 특정 파일을 백엔드로 사용할 수 있다.
 * 예: hugetlbfs에 생성된 파일(/mnt/huge/fio)이나 공유 메모리 파일. 이 콜백은 ":뒤" 경로를 추출해
 * td->o.mmapfile에 strdup으로 저장. init.c::init_io_u() 에서 io_u 버퍼 할당 시 이 경로를 mmap 한다.
 *
 * 동작: MMAPHUGE/MMAP/MMAPSHARED 면 get_opt_postfix()로 콜론 뒤 경로 복제 저장.
 *       다른 mem_type(malloc/shm/shmhuge)은 mmapfile 의미 없음 — NULL 유지.
 *
 * 호출 체인: parse.c → [str_mem_cb] → get_opt_postfix → strdup.
 * 읽는 자: io_u.c::fio_iomem_*, init.c::init_io_u_buffers 에서 mmap(fd, ...) 경로로 사용.
 */
static int str_mem_cb(void *data, const char *mem)
{
	struct thread_data *td = cb_data_to_td(data);

	if (td->o.mem_type == MEM_MMAPHUGE || td->o.mem_type == MEM_MMAP ||
	    td->o.mem_type == MEM_MMAPSHARED)
		td->o.mmapfile = get_opt_postfix(mem);

	return 0;
}

/*
 * [한국어]
 * fio_clock_source_cb() — clocksource 옵션 콜백. 파싱 즉시 전역 시계 재초기화.
 *
 * @data: &td->o. clocksource 는 이미 posval 매핑으로 설정 (gettimeofday/clock_gettime/cpu).
 * @str:  원본 "clocksource=cpu" 문자열 (사용하지 않음).
 * @return: 0.
 *
 * 왜 필요한가: 잡이 시작되기 전에 전역 fio_clock_source 를 바꿔야 fio_gettime() 이후 호출이
 * 새 클록으로 작동한다. 특히 CPU(TSC) 클록은 초기 보정(fio_clock_init)이 필요 — ns/tick 변환
 * 상수 측정을 위한 1회성 벤치마크 루프를 돈다.
 *
 * 동작 단계:
 * 1) td->o.clocksource 값을 전역 fio_clock_source 에 복사.
 * 2) fio_clock_source_set = 1 마킹 (후속 잡이 덮어쓰지 않도록).
 * 3) fio_clock_init() — TSC 보정 또는 clock_gettime 해상도 측정.
 *
 * 호출 체인: parse.c → [fio_clock_source_cb] → fio_clock_init (gettime.c).
 * 실행 컨텍스트: 메인 프로세스의 옵션 파싱 단계. 잡 스레드 생성 전.
 */
static int fio_clock_source_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	fio_clock_source = td->o.clocksource;
	fio_clock_source_set = 1;
	fio_clock_init();
	return 0;
}

/*
 * [한국어]
 * str_rwmix_read_cb() — rwmixread 콜백: 읽기 비율 설정 시 쓰기 비율을 자동 보완.
 *
 * @data: &td->o.
 * @val:  [in] 파싱된 정수값 (0~100, 읽기 퍼센트).
 * @return: 0.
 *
 * 왜 필요한가: rwmixread 와 rwmixwrite 는 같은 것을 다르게 표현 (한쪽 설정 시 나머지는 100-x).
 * 사용자가 둘 중 하나만 지정해도 자동으로 쌍이 완성되도록 cb가 양쪽을 한번에 채운다.
 *
 * 호출 체인: parse.c → [str_rwmix_read_cb] — td->o.rwmix[DDIR_READ/WRITE] 채움.
 * 읽는 자: io_u.c::get_rand_ddir() 가 rwmix[] 로 매 io_u 의 ddir 추첨.
 */
static int str_rwmix_read_cb(void *data, long long *val)
{
	struct thread_data *td = cb_data_to_td(data);

	td->o.rwmix[DDIR_READ] = *val;
	td->o.rwmix[DDIR_WRITE] = 100 - *val;
	return 0;
}

/*
 * [한국어]
 * str_rwmix_write_cb() — rwmixwrite 콜백: str_rwmix_read_cb 의 대칭 쌍.
 *
 * @data: &td->o.
 * @val:  [in] 쓰기 퍼센트(0~100).
 * @return: 0.
 *
 * 동일 로직의 대칭 버전. 둘 다 지정되면 나중에 파싱된 쪽의 cb가 양쪽을 덮어쓴다.
 * 우선순위 규칙은 없으며, parse.c 가 옵션 등장 순서대로 호출한다.
 */
static int str_rwmix_write_cb(void *data, long long *val)
{
	struct thread_data *td = cb_data_to_td(data);

	td->o.rwmix[DDIR_WRITE] = *val;
	td->o.rwmix[DDIR_READ] = 100 - *val;
	return 0;
}

/*
 * [한국어]
 * str_exitall_cb() — exitall CLI/잡 옵션 콜백.
 *
 * @return: 0.
 *
 * 왜 필요한가: fio는 여러 잡을 병렬 실행할 수 있다. 기본적으로 각 잡은 runtime/size/loops
 * 조건에 따라 독립적으로 종료한다. 이 플래그를 켜면 가장 먼저 끝난 잡 하나를 신호로
 * 모든 잡이 즉시 종료된다(terminate_threads 전파). 복합 벤치마크에서 "먼저 끝내는 자가 기준"
 * 같은 조건 테스트에 유용.
 *
 * 호출 체인: parse.c → [str_exitall_cb] (인자 없는 STR_SET 타입).
 * 읽는 자: backend.c::reap_threads() 가 exitall_on_terminate 플래그로 전역 종료 결정.
 *
 * 특이점: 인자가 없는(void) 콜백 — FIO_OPT_STR_SET 타입 전용 시그니처.
 */
static int str_exitall_cb(void)
{
	exitall_on_terminate = true;
	return 0;
}

#ifdef FIO_HAVE_CPU_AFFINITY
/*
 * [한국어]
 * fio_cpus_split() — CPU 마스크에서 cpu_index 번째 비트만 남기고 나머지 클리어.
 *
 * @mask:      [in, out] CPU 마스크. 수정됨.
 * @cpu_index: 남길 CPU의 인덱스 (마스크 내 순서, 모듈로 순환).
 * @return: 최종 마스크의 set 비트 수 (항상 0 또는 1).
 *
 * 왜 필요한가: cpus_allowed_policy=split 모드에서 numjobs 잡이 각각 다른 CPU 하나만 사용하도록.
 * 예: cpus_allowed=0-3, numjobs=4, split → 잡0=CPU0, 잡1=CPU1, 잡2=CPU2, 잡3=CPU3.
 *
 * 동작 단계:
 * 1) 마스크의 set 비트 수 조회.
 * 2) cpu_index %= 비트수 (모듈로 순환).
 * 3) 선형 스캔 — cpu_index 번째 set 비트를 제외한 나머지 모두 clear.
 *
 * 호출 체인: backend.c::thread_main → [fio_cpus_split] → sched_setaffinity.
 */
int fio_cpus_split(os_cpu_mask_t *mask, unsigned int cpu_index)
{
	unsigned int i, index, cpus_in_mask;
	const long max_cpu = cpus_configured();

	cpus_in_mask = fio_cpu_count(mask);
	if (!cpus_in_mask)
		return 0;

	cpu_index = cpu_index % cpus_in_mask;

	index = 0;
	for (i = 0; i < max_cpu; i++) {
		if (!fio_cpu_isset(mask, i))
			continue;

		if (cpu_index != index)
			fio_cpu_clear(mask, i);

		index++;
	}

	return fio_cpu_count(mask);
}

/*
 * [한국어]
 * str_cpumask_cb() — cpumask 옵션 콜백: 비트 마스크 형식의 CPU 친화성 설정.
 *
 * @data: &td->o.
 * @val:  [in] 정수로 파싱된 비트마스크. 각 비트 i → CPU i 허용. 예: 0x5 → CPU 0,2.
 * @return: 0=성공, 1=범위 초과/cpuset_init 실패.
 *
 * 왜 필요한가: cpumask 는 cpus_allowed ("0,2-4" 범위 표기) 의 이진 표현 변종. 32비트 정수라
 * CPU 0~31 만 표현 가능 — 그 이상은 cpus_allowed 옵션을 써야 한다.
 *
 * 동작 단계:
 * 1) parse_dryrun() 방어.
 * 2) fio_cpuset_init() — sched_setaffinity(2) 호환 os_cpu_mask_t 준비 (Linux: cpu_set_t).
 * 3) cpus_configured() 로 현재 시스템 최대 CPU 수 조회.
 * 4) 32비트 루프 — val의 비트 i가 1이면 i >= max_cpu 검사 후 fio_cpu_set(&mask, i).
 * 5) td->o.cpumask 에 완성된 마스크 저장.
 *
 * 호출 체인: parse.c → [str_cpumask_cb] → fio_cpuset_init/fio_cpu_set (os/os.h 매크로).
 * 읽는 자: backend.c::thread_main() 가 sched_setaffinity(2) 로 잡 스레드 고정.
 * 실행 컨텍스트: CONFIG_HAVE_CPU_AFFINITY 빌드 시에만 제공. 미지원 플랫폼에서는 엔트리 자체가
 *                 UNSUPPORTED로 마킹되어 이 cb 호출되지 않음.
 */
static int str_cpumask_cb(void *data, unsigned long long *val)
{
	struct thread_data *td = cb_data_to_td(data);
	unsigned int i;
	long max_cpu;
	int ret;

	if (parse_dryrun())
		return 0;

	ret = fio_cpuset_init(&td->o.cpumask);
	if (ret < 0) {
		log_err("fio: cpuset_init failed\n");
		td_verror(td, ret, "fio_cpuset_init");
		return 1;
	}

	max_cpu = cpus_configured();

	for (i = 0; i < sizeof(int) * 8; i++) {
		if ((1 << i) & *val) {
			if (i >= max_cpu) {
				log_err("fio: CPU %d too large (max=%ld)\n", i,
								max_cpu - 1);
				return 1;
			}
			dprint(FD_PARSE, "set cpu allowed %d\n", i);
			fio_cpu_set(&td->o.cpumask, i);
		}
	}

	return 0;
}

/*
 * [한국어]
 * set_cpus_allowed() — "0,2-4,6" 형식의 CPU 리스트를 os_cpu_mask_t 에 반영.
 *
 * @td:    thread_data (에러 보고용).
 * @mask:  [out] 대상 CPU 마스크 (&td->o.cpumask 또는 verify_cpumask 또는 log_gz_cpumask).
 * @input: CPU 리스트 문자열.
 * @return: 0=성공, 1=실패 (잘못된 CPU 번호, cpuset_init 실패).
 *
 * 왜 필요한가: str_cpus_allowed_cb / str_verify_cpus_allowed_cb / str_log_cpus_allowed_cb 세
 * 콜백이 공통으로 사용. 마스크 대상만 다르고 파싱 로직은 동일.
 *
 * 문법:
 *   "0"            → CPU 0만.
 *   "0,2,4"        → CPU 0, 2, 4.
 *   "0-3"          → CPU 0, 1, 2, 3.
 *   "0-3,7,10-12"  → 혼합 범위와 단일.
 *
 * 동작 단계:
 * 1) fio_cpuset_init — 마스크를 zero 로 초기화 (CPU_ZERO).
 * 2) strsep(',') 로 토큰 분리.
 * 3) 각 토큰을 strsep('-') 로 시작/끝 분리 (단일은 끝=-1).
 * 4) atoi 로 정수 변환.
 * 5) 범위 [icpu..icpu2] 를 순회하며 FIO_MAX_CPUS / cpus_configured() 상한 검사 후 fio_cpu_set.
 *
 * 호출 체인: str_cpus_allowed_cb / 유사 → [set_cpus_allowed] → fio_cpu_set.
 */
static int set_cpus_allowed(struct thread_data *td, os_cpu_mask_t *mask,
			    const char *input)
{
	char *cpu, *str, *p;
	long max_cpu;
	int ret = 0;

	ret = fio_cpuset_init(mask);
	if (ret < 0) {
		log_err("fio: cpuset_init failed\n");
		td_verror(td, ret, "fio_cpuset_init");
		return 1;
	}

	p = str = strdup(input);

	strip_blank_front(&str);
	strip_blank_end(str);

	max_cpu = cpus_configured();

	while ((cpu = strsep(&str, ",")) != NULL) {
		char *str2, *cpu2;
		int icpu, icpu2;

		if (!strlen(cpu))
			break;

		str2 = cpu;
		icpu2 = -1;
		while ((cpu2 = strsep(&str2, "-")) != NULL) {
			if (!strlen(cpu2))
				break;

			icpu2 = atoi(cpu2);
		}

		icpu = atoi(cpu);
		if (icpu2 == -1)
			icpu2 = icpu;
		while (icpu <= icpu2) {
			if (icpu >= FIO_MAX_CPUS) {
				log_err("fio: your OS only supports up to"
					" %d CPUs\n", (int) FIO_MAX_CPUS);
				ret = 1;
				break;
			}
			if (icpu >= max_cpu) {
				log_err("fio: CPU %d too large (max=%ld)\n",
							icpu, max_cpu - 1);
				ret = 1;
				break;
			}

			dprint(FD_PARSE, "set cpu allowed %d\n", icpu);
			fio_cpu_set(mask, icpu);
			icpu++;
		}
		if (ret)
			break;
	}

	free(p);
	return ret;
}

/*
 * [한국어]
 * str_cpus_allowed_cb() — cpus_allowed 옵션 콜백.
 *
 * @data:  &td->o.
 * @input: "0,2-4,6" 형식의 CPU 리스트 문자열.
 * @return: set_cpus_allowed() 의 반환값 (0=성공, 1=실패).
 *
 * 왜 필요한가: cpumask의 32비트 제약을 넘어 임의 CPU 조합을 지정. 최신 서버(수백 코어)에서 필수.
 *
 * 동작: set_cpus_allowed() 헬퍼에 td->o.cpumask 전달하여 파싱 및 세팅 위임.
 * 호출 체인: parse.c → [str_cpus_allowed_cb] → set_cpus_allowed → fio_cpuset_init/fio_cpu_set.
 */
static int str_cpus_allowed_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);

	if (parse_dryrun())
		return 0;
	if (!input)
		return 1;

	return set_cpus_allowed(td, &td->o.cpumask, input);
}

/*
 * [한국어]
 * str_verify_cpus_allowed_cb() — verify_cpus_allowed 옵션 콜백.
 *
 * @data:  &td->o.
 * @input: "0-3" 등 CPU 리스트.
 * @return: 0=성공, 1=실패.
 *
 * 왜 필요한가: verify_async>0 으로 별도 검증 스레드가 생성되면 그 스레드를 I/O 스레드와
 * 다른 CPU 에 고정할 수 있다. 메인 I/O 스레드와 캐시 경쟁을 피하거나 특정 NUMA 노드에 격리.
 *
 * 호출 체인: parse.c → [str_verify_cpus_allowed_cb] → set_cpus_allowed(td, &verify_cpumask, ...).
 * 읽는 자: verify.c::verify_async_thread 가 sched_setaffinity 로 사용.
 */
static int str_verify_cpus_allowed_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);

	if (parse_dryrun())
		return 0;
	if (!input)
		return 1;

	return set_cpus_allowed(td, &td->o.verify_cpumask, input);
}

#ifdef CONFIG_ZLIB
/*
 * [한국어]
 * str_log_cpus_allowed_cb() — log_compression_cpus 옵션 콜백 (CONFIG_ZLIB 한정).
 *
 * @data:  &td->o.
 * @input: CPU 리스트.
 * @return: 0=성공, 1=실패.
 *
 * 왜 필요한가: log_compression > 0 이면 iolog.c::gz_work 압축 스레드가 zlib deflate를 돌린다.
 * CPU 소모가 커서 잡 스레드와 별 CPU 에 고정해야 I/O 측정 오염을 줄인다.
 *
 * 호출 체인: parse.c → [str_log_cpus_allowed_cb] → set_cpus_allowed(td, &log_gz_cpumask, ...).
 * 읽는 자: iolog.c::gz_init_worker 가 압축 스레드 생성 시 affinity 적용.
 */
static int str_log_cpus_allowed_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);

	if (parse_dryrun())
		return 0;
	if (!input)
		return 1;

	return set_cpus_allowed(td, &td->o.log_gz_cpumask, input);
}
#endif /* CONFIG_ZLIB */

#endif /* FIO_HAVE_CPU_AFFINITY */

#ifdef CONFIG_LIBNUMA
/*
 * [한국어]
 * str_numa_cpunodes_cb() — numa_cpu_nodes 옵션 콜백 (CONFIG_LIBNUMA 한정).
 *
 * @data:  &td->o.
 * @input: NUMA 노드 문자열. 예: "0", "0-1", "0,2". libnuma가 해석.
 * @return: 0=성공, 1=파싱 실패.
 *
 * 왜 필요한가: 다중 소켓 NUMA 시스템에서 잡 스레드를 특정 메모리 노드와 짝지어 실행.
 * cpus_allowed 가 CPU 친화성이라면 이 옵션은 NUMA 메모리 친화성. 둘을 함께 쓰면 CPU 와
 * 메모리 양쪽이 한 NUMA 노드에 고정되어 원격 노드 접근 지연을 제거.
 *
 * 동작 단계:
 * 1) parse_dryrun() 방어.
 * 2) numa_parse_nodestring(input) — libnuma가 "0-1,3" 같은 리스트를 bitmask로 변환.
 * 3) bitmask 즉시 free (검증만 목적 — 실제 적용은 strdup된 문자열로).
 * 4) td->o.numa_cpunodes = strdup(input) — 실제 적용은 post_init 단계에서.
 *
 * 호출 체인: parse.c → [str_numa_cpunodes_cb] → numa_parse_nodestring.
 * 읽는 자: backend.c::thread_main() 이 numa_run_on_node_mask() 호출.
 */
static int str_numa_cpunodes_cb(void *data, char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	struct bitmask *verify_bitmask;

	if (parse_dryrun())
		return 0;

	/* numa_parse_nodestring() parses a character string list
	 * of nodes into a bit mask. The bit mask is allocated by
	 * numa_allocate_nodemask(), so it should be freed by
	 * numa_free_nodemask().
	 */
	verify_bitmask = numa_parse_nodestring(input);
	if (verify_bitmask == NULL) {
		log_err("fio: numa_parse_nodestring failed\n");
		td_verror(td, 1, "str_numa_cpunodes_cb");
		return 1;
	}
	numa_free_nodemask(verify_bitmask);

	td->o.numa_cpunodes = strdup(input);
	return 0;
}

/*
 * [한국어]
 * str_numa_mpol_cb() — numa_mem_policy 옵션 콜백 (CONFIG_LIBNUMA 한정).
 *
 * @data:  &td->o.
 * @input: "정책:노드리스트" 형식. 예: "interleave:0-1", "bind:2", "prefer:1", "local", "default".
 * @return: 0=성공, 1=잘못된 정책/노드리스트.
 *
 * 왜 필요한가: NUMA 메모리 할당 정책은 mbind(2)/set_mempolicy(2) 로 매핑되는 5가지 모드가 있다:
 *   - MPOL_DEFAULT:    커널 기본 (로컬 우선, 실패시 다른 노드) — 노드리스트 금지.
 *   - MPOL_PREFERRED:  지정 노드 우선, 부족하면 다른 노드 — 노드 1개만 허용.
 *   - MPOL_BIND:       지정 노드에만 할당, 부족하면 OOM — 리스트 필수.
 *   - MPOL_INTERLEAVE: 리스트 노드들에 라운드로빈 분배 — 리스트 없으면 "all".
 *   - MPOL_LOCAL:      실행 중인 CPU의 로컬 노드 — 노드리스트 금지.
 *
 * 동작 단계:
 * 1) parse_dryrun() 방어.
 * 2) strchr(':')로 정책 vs 노드리스트 분리, 분리자 자리에 NUL 삽입.
 * 3) policy_types[] 루프로 정책 문자열을 MPOL_* enum에 매핑.
 * 4) 정책별 노드리스트 유효성 검사 (위 5가지 규칙).
 * 5) PREFERRED → atoi(nodelist) 로 numa_mem_prefer_node 저장.
 *    INTERLEAVE/BIND → numa_parse_nodestring 으로 검증 후 strdup.
 *    LOCAL/DEFAULT → 노드리스트 없음 확인만.
 *
 * 호출 체인: parse.c → [str_numa_mpol_cb] → numa_parse_nodestring (libnuma).
 * 읽는 자: backend.c::thread_main() 이 set_mempolicy(2) 호출.
 */
static int str_numa_mpol_cb(void *data, char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	const char * const policy_types[] =
		{ "default", "prefer", "bind", "interleave", "local", NULL };
	int i;
	char *nodelist;
	struct bitmask *verify_bitmask;

	if (parse_dryrun())
		return 0;
	if (!input)
		return 1;

	nodelist = strchr(input, ':');
	if (nodelist) {
		/* NUL-terminate mode */
		*nodelist++ = '\0';
	}

	for (i = 0; i <= MPOL_LOCAL; i++) {
		if (!strcmp(input, policy_types[i])) {
			td->o.numa_mem_mode = i;
			break;
		}
	}
	if (i > MPOL_LOCAL) {
		log_err("fio: memory policy should be: default, prefer, bind, interleave, local\n");
		goto out;
	}

	switch (td->o.numa_mem_mode) {
	case MPOL_PREFERRED:
		/*
		 * Insist on a nodelist of one node only
		 */
		if (nodelist) {
			char *rest = nodelist;
			while (isdigit(*rest))
				rest++;
			if (*rest) {
				log_err("fio: one node only for \'prefer\'\n");
				goto out;
			}
		} else {
			log_err("fio: one node is needed for \'prefer\'\n");
			goto out;
		}
		break;
	case MPOL_INTERLEAVE:
		/*
		 * Default to online nodes with memory if no nodelist
		 */
		if (!nodelist)
			nodelist = strdup("all");
		break;
	case MPOL_LOCAL:
	case MPOL_DEFAULT:
		/*
		 * Don't allow a nodelist
		 */
		if (nodelist) {
			log_err("fio: NO nodelist for \'local\'\n");
			goto out;
		}
		break;
	case MPOL_BIND:
		/*
		 * Insist on a nodelist
		 */
		if (!nodelist) {
			log_err("fio: a nodelist is needed for \'bind\'\n");
			goto out;
		}
		break;
	}


	/* numa_parse_nodestring() parses a character string list
	 * of nodes into a bit mask. The bit mask is allocated by
	 * numa_allocate_nodemask(), so it should be freed by
	 * numa_free_nodemask().
	 */
	switch (td->o.numa_mem_mode) {
	case MPOL_PREFERRED:
		td->o.numa_mem_prefer_node = atoi(nodelist);
		break;
	case MPOL_INTERLEAVE:
	case MPOL_BIND:
		verify_bitmask = numa_parse_nodestring(nodelist);
		if (verify_bitmask == NULL) {
			log_err("fio: numa_parse_nodestring failed\n");
			td_verror(td, 1, "str_numa_memnodes_cb");
			return 1;
		}
		td->o.numa_memnodes = strdup(nodelist);
		numa_free_nodemask(verify_bitmask);

		break;
	case MPOL_LOCAL:
	case MPOL_DEFAULT:
	default:
		break;
	}

	return 0;
out:
	return 1;
}
#endif

/*
 * [한국어]
 * str_fst_cb() — file_service_type 옵션 콜백.
 *
 * @data: &td->o. file_service_type 은 이미 parse.c posval 매핑 완료 (RANDOM/RR/SEQ/ZIPF/PARETO/GAUSS).
 * @str:  원본 "file_service_type=random:16" 또는 "zipf:1.2:0.5" 형식. 콜론 뒤 파라미터 추출.
 * @return: 0=성공, 1=파라미터 범위 오류.
 *
 * 왜 필요한가: 다중 파일(filename=f1:f2:f3)에서 I/O가 어느 파일을 쓸지 결정. RR(round robin)과
 * SEQUENTIAL 은 "한 파일에서 몇 번 I/O 후 다음" 의 반복 횟수(file_service_nr) 를 받는다.
 * 분포형(ZIPF/PARETO/GAUSS)은 파일 인덱스를 분포 함수로 추첨 — 파라미터는 분포 모양을 결정.
 *
 * 파라미터 의미:
 *   - RANDOM/RR/SEQ:     nr = 같은 파일에서 연속 I/O 횟수 (기본 1).
 *   - ZIPF:              theta = 지프 분포의 편향. theta→0 균등, theta↑ 편향 심함. 1.0 금지(수학적 특이점).
 *   - PARETO:            input ∈ (0, 1). 분포 tail 특성.
 *   - GAUSS:             dev = 표준편차 (0~100). 0이면 중앙 결정론적.
 *   - 추가 ":center"     분포 중심 위치 ∈ [0, 1]. 기본 0.5 (전체 파일 수의 절반 지점).
 *
 * 동작 단계:
 * 1) td->file_service_nr = 1 기본값.
 * 2) switch(file_service_type): RANDOM/RR/SEQ는 get_opt_postfix + atoi(nr).
 *    ZIPF/PARETO/GAUSS는 FIO_DEF_* 로 val 초기화 후 공통 경로로 진입.
 * 3) split_parse_distr(nr, &val, &center) 로 "파라미터:중심" 2-way 분리.
 * 4) center 범위 [0, 1] 검증 → td->random_center 저장.
 * 5) 분포별 val 범위 검증 → td->zipf_theta / pareto_h / gauss_dev 저장.
 *
 * 호출 체인: parse.c → [str_fst_cb] → split_parse_distr → str_to_float.
 * 읽는 자: io_u.c::get_next_file() 이 분포별 함수로 파일 인덱스 추첨.
 */
static int str_fst_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);
	double val;
	double center = -1;
	bool done = false;
	char *nr;

	td->file_service_nr = 1;

	switch (td->o.file_service_type) {
	case FIO_FSERVICE_RANDOM:
	case FIO_FSERVICE_RR:
	case FIO_FSERVICE_SEQ:
		nr = get_opt_postfix(str);
		if (nr) {
			td->file_service_nr = atoi(nr);
			free(nr);
		}
		done = true;
		break;
	case FIO_FSERVICE_ZIPF:
		val = FIO_DEF_ZIPF;
		break;
	case FIO_FSERVICE_PARETO:
		val = FIO_DEF_PARETO;
		break;
	case FIO_FSERVICE_GAUSS:
		val = 0.0;
		break;
	default:
		log_err("fio: bad file service type: %d\n", td->o.file_service_type);
		return 1;
	}

	if (done)
		return 0;

	nr = get_opt_postfix(str);
	if (nr && !split_parse_distr(nr, &val, &center)) {
		log_err("fio: file service type random postfix parsing failed\n");
		free(nr);
		return 1;
	}

	free(nr);

	if (center != -1 && (center < 0.00 || center > 1.00)) {
		log_err("fio: distribution center out of range (0 <= center <= 1.0)\n");
		return 1;
	}
	td->random_center = center;

	switch (td->o.file_service_type) {
	case FIO_FSERVICE_ZIPF:
		if (val == 1.00) {
			log_err("fio: zipf theta must be different than 1.0\n");
			return 1;
		}
		if (parse_dryrun())
			return 0;
		td->zipf_theta = val;
		break;
	case FIO_FSERVICE_PARETO:
		if (val <= 0.00 || val >= 1.00) {
                          log_err("fio: pareto input out of range (0 < input < 1.0)\n");
                          return 1;
		}
		if (parse_dryrun())
			return 0;
		td->pareto_h = val;
		break;
	case FIO_FSERVICE_GAUSS:
		if (val < 0.00 || val >= 100.00) {
                          log_err("fio: normal deviation out of range (0 <= input < 100.0)\n");
                          return 1;
		}
		if (parse_dryrun())
			return 0;
		td->gauss_dev = val;
		break;
	}

	return 0;
}

#ifdef CONFIG_SYNC_FILE_RANGE
/*
 * [한국어]
 * str_sfr_cb() — sync_file_range 옵션 콜백 (CONFIG_SYNC_FILE_RANGE 한정).
 *
 * @data: &td->o. sync_file_range mode(WAIT_BEFORE/WRITE/WAIT_AFTER) 는 이미 posval로 설정.
 * @str:  원본 옵션 문자열. 콜론 뒤에 "매 몇 번 I/O 마다 sync_file_range 호출" 주기 지정.
 *        예: "sync_file_range=wait_before,write:8" → 매 8 I/O마다 호출.
 * @return: 0.
 *
 * 왜 필요한가: sync_file_range(2) 는 Linux 전용 — 파일 특정 범위만 디스크 동기화(fsync보다 세밀).
 * 이를 매 I/O마다 호출하면 오버헤드가 크므로 주기(nr) 를 받아 "nr 번에 한 번" 호출하도록 제어.
 *
 * 호출 체인: parse.c → [str_sfr_cb] → get_opt_postfix → atoi.
 * 읽는 자: backend.c::do_io() 가 sync_file_range_nr 을 카운터로 사용.
 */
static int str_sfr_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);
	char *nr = get_opt_postfix(str);

	td->sync_file_range_nr = 1;
	if (nr) {
		td->sync_file_range_nr = atoi(nr);
		free(nr);
	}

	return 0;
}
#endif

/*
 * [한국어]
 * zone_split_ddir() — zoned / zoned_abs random_distribution의 영역별 분할 파싱.
 *
 * @o:        thread_options.
 * @eo:       unused.
 * @ddir:     방향.
 * @str:      "접근퍼센트/영역퍼센트:..." 또는 "접근퍼센트/영역크기:..." 포맷.
 * @absolute: false=영역을 파일 전체의 퍼센트로, true=절대 바이트 수로.
 * @return:   0=성공, 1=실패.
 *
 * 왜 필요한가: zoned 분포는 "파일을 N 영역으로 나누어 각 영역에 X% 확률로 접근". 예:
 *   random_distribution=zoned:60/10:30/20:10/70
 *   → 첫 번째 영역(전체의 10%)에 60% 접근, 두 번째(20%)에 30%, 세 번째(70%)에 10%.
 * 실제 워크로드의 "핫/콜드" 분리(자주 쓰는 10%에 90% 접근) 모사.
 *
 * 동작 단계:
 * 1) split_parse_ddir로 val1=접근퍼센트, val2=영역퍼센트|크기 리스트화.
 * 2) o->zone_split[ddir] 에 struct zone_split[] 할당 후 필드 복사.
 * 3) 접근퍼센트 합 == 100 검증 (접근은 반드시 완전 분배).
 * 4) 영역퍼센트 합 ≤ 100 검증 (<100 이면 나머지는 접근 불가 영역).
 * 5) 미지정(-1U) 엔트리에 나머지 균등 분배.
 *
 * 호출 체인: parse_zoned_distribution → str_split_parse → [zone_split_ddir].
 */
static int zone_split_ddir(struct thread_options *o, void *eo,
			   enum fio_ddir ddir, char *str, bool absolute)
{
	unsigned int i, perc, perc_missing, sperc, sperc_missing;
	struct split split;

	memset(&split, 0, sizeof(split));

	if (split_parse_ddir(o, &split, str, absolute, ZONESPLIT_MAX))
		return 1;
	if (!split.nr)
		return 0;

	o->zone_split[ddir] = malloc(split.nr * sizeof(struct zone_split));
	o->zone_split_nr[ddir] = split.nr;
	for (i = 0; i < split.nr; i++) {
		o->zone_split[ddir][i].access_perc = split.val1[i];
		if (absolute)
			o->zone_split[ddir][i].size = split.val2[i];
		else
			o->zone_split[ddir][i].size_perc = split.val2[i];
	}

	/*
	 * Now check if the percentages add up, and how much is missing
	 */
	perc = perc_missing = 0;
	sperc = sperc_missing = 0;
	for (i = 0; i < o->zone_split_nr[ddir]; i++) {
		struct zone_split *zsp = &o->zone_split[ddir][i];

		if (zsp->access_perc == (uint8_t) -1U)
			perc_missing++;
		else
			perc += zsp->access_perc;

		if (!absolute) {
			if (zsp->size_perc == (uint8_t) -1U)
				sperc_missing++;
			else
				sperc += zsp->size_perc;
		}
	}

	if (perc > 100 || sperc > 100) {
		log_err("fio: zone_split percentages add to more than 100%%\n");
		free(o->zone_split[ddir]);
		o->zone_split[ddir] = NULL;
		return 1;
	}
	if (perc < 100) {
		log_err("fio: access percentage don't add up to 100 for zoned "
			"random distribution (got=%u)\n", perc);
		free(o->zone_split[ddir]);
		o->zone_split[ddir] = NULL;
		return 1;
	}

	/*
	 * If values didn't have a percentage set, divide the remains between
	 * them.
	 */
	if (perc_missing) {
		if (perc_missing == 1 && o->zone_split_nr[ddir] == 1)
			perc = 100;
		for (i = 0; i < o->zone_split_nr[ddir]; i++) {
			struct zone_split *zsp = &o->zone_split[ddir][i];

			if (zsp->access_perc == (uint8_t) -1U)
				zsp->access_perc = (100 - perc) / perc_missing;
		}
	}
	if (sperc_missing) {
		if (sperc_missing == 1 && o->zone_split_nr[ddir] == 1)
			sperc = 100;
		for (i = 0; i < o->zone_split_nr[ddir]; i++) {
			struct zone_split *zsp = &o->zone_split[ddir][i];

			if (zsp->size_perc == (uint8_t) -1U)
				zsp->size_perc = (100 - sperc) / sperc_missing;
		}
	}

	return 0;
}

/*
 * [한국어]
 * parse_zoned_distribution() — random_distribution=zoned 또는 zoned_abs 전체 파싱 진입점.
 *
 * @td:       thread_data.
 * @input:    원본 옵션 문자열 (prefix "zoned:" 또는 "zoned_abs:" 포함).
 * @absolute: false=퍼센트 해석, true=절대 바이트 해석.
 * @return:   0=성공, 1=실패.
 *
 * 왜 필요한가: str_random_distribution_cb 가 FIO_RAND_DIST_ZONED/ZONED_ABS 선택 시 이 함수에 위임.
 * zoned 는 다른 분포와 달리 내부에 추가 문법(영역/퍼센트 리스트)을 갖는다.
 *
 * 동작 단계:
 * 1) 접두사 "zoned:" / "zoned_abs:" 검증 후 스킵.
 * 2) str_split_parse 로 3-way 방향 분할 → 각 방향별 zone_split_ddir 호출.
 * 3) dprint(FD_PARSE) 로 파싱 결과 덤프 (디버그 편의).
 * 4) parse_dryrun 이면 부수효과 제거.
 * 5) ret != 0 이면 zone_split_nr 리셋.
 *
 * 호출 체인: str_random_distribution_cb → [parse_zoned_distribution] → str_split_parse → zone_split_ddir.
 */
static int parse_zoned_distribution(struct thread_data *td, const char *input,
				    bool absolute)
{
	const char *pre = absolute ? "zoned_abs:" : "zoned:";
	char *str, *p;
	int i, ret = 0;

	p = str = strdup(input);

	strip_blank_front(&str);
	strip_blank_end(str);

	/* We expect it to start like that, bail if not */
	if (strncmp(str, pre, strlen(pre))) {
		log_err("fio: mismatch in zoned input <%s>\n", str);
		free(p);
		return 1;
	}
	str += strlen(pre);

	ret = str_split_parse(td, str, zone_split_ddir, NULL, absolute);

	free(p);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		int j;

		dprint(FD_PARSE, "zone ddir %d (nr=%u): \n", i, td->o.zone_split_nr[i]);

		for (j = 0; j < td->o.zone_split_nr[i]; j++) {
			struct zone_split *zsp = &td->o.zone_split[i][j];

			if (absolute) {
				dprint(FD_PARSE, "\t%d: %u/%llu\n", j,
						zsp->access_perc,
						(unsigned long long) zsp->size);
			} else {
				dprint(FD_PARSE, "\t%d: %u/%u\n", j,
						zsp->access_perc,
						zsp->size_perc);
			}
		}
	}

	if (parse_dryrun()) {
		for (i = 0; i < DDIR_RWDIR_CNT; i++) {
			free(td->o.zone_split[i]);
			td->o.zone_split[i] = NULL;
			td->o.zone_split_nr[i] = 0;
		}

		return ret;
	}

	if (ret) {
		for (i = 0; i < DDIR_RWDIR_CNT; i++)
			td->o.zone_split_nr[i] = 0;
	}

	return ret;
}

/*
 * [한국어]
 * str_random_distribution_cb() — random_distribution 옵션 콜백.
 *
 * @data: &td->o. random_distribution 은 이미 posval로 FIO_RAND_DIST_* enum 설정 완료.
 * @str:  원본 "random_distribution=zipf:1.2:0.5" 형식.
 * @return: 0=성공, 1=파라미터 범위 오류.
 *
 * 왜 필요한가: 랜덤 I/O 오프셋 선택의 **확률 분포**를 결정. 실제 워크로드 모방의 핵심:
 *   - random(기본): 균등 분포 (uniform) — 모든 블록 동등 확률. LFSR 기반.
 *   - zipf(theta):  지프 분포 — 핫 스폿 편향(소수 블록이 대부분 접근). 웹/DB 워크로드 모사.
 *                    theta → 0 균등, theta ↑ 편향 심함. theta=1.0 수학적 특이점(금지).
 *   - pareto(input): 파레토 분포 — 80/20 법칙 변종. input ∈ (0, 1).
 *   - gauss(dev):    정규 분포 — 중심 주변 집중. dev = 표준편차 퍼센트 (0~100).
 *   - zoned:         영역별 비율. "60/10:30/20:10/70" → 영역의 60% 접근이 전체 10%에 집중 등.
 *   - zoned_abs:     zoned와 동일하나 크기를 바이트 절대값으로.
 *
 * 선택적 ":center" 파라미터 (zipf/pareto/gauss 공통): 분포 중심 위치 ∈ [0, 1].
 * 기본 0.5 (전체의 절반 지점이 핫 스폿 중심). 0 → 파일 시작, 1 → 파일 끝.
 *
 * 동작 단계:
 * 1) random_distribution enum 에 따라 val 기본값 세팅 (ZIPF=FIO_DEF_ZIPF 등).
 * 2) ZONED/ZONED_ABS 는 parse_zoned_distribution 으로 위임 (복잡한 영역 파싱).
 * 3) get_opt_postfix → split_parse_distr 로 "val:center" 2-way 분리.
 * 4) center 범위 [0, 1] 검증 → td->o.random_center.u.f 저장 (fio_fp64_t 사용).
 * 5) 분포별 val 범위 검증 → zipf_theta/pareto_h/gauss_dev fio_fp64_t 저장.
 *
 * 호출 체인: parse.c → [str_random_distribution_cb] → split_parse_distr | parse_zoned_distribution.
 * 읽는 자: io_u.c::__get_next_rand_offset_{zipf,pareto,gauss,zoned,zoned_abs}().
 */
static int str_random_distribution_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);
	double val;
	double center = -1;
	char *nr;

	if (td->o.random_distribution == FIO_RAND_DIST_ZIPF)
		val = FIO_DEF_ZIPF;
	else if (td->o.random_distribution == FIO_RAND_DIST_PARETO)
		val = FIO_DEF_PARETO;
	else if (td->o.random_distribution == FIO_RAND_DIST_GAUSS)
		val = 0.0;
	else if (td->o.random_distribution == FIO_RAND_DIST_ZONED)
		return parse_zoned_distribution(td, str, false);
	else if (td->o.random_distribution == FIO_RAND_DIST_ZONED_ABS)
		return parse_zoned_distribution(td, str, true);
	else
		return 0;

	nr = get_opt_postfix(str);
	if (nr && !split_parse_distr(nr, &val, &center)) {
		log_err("fio: random postfix parsing failed\n");
		free(nr);
		return 1;
	}

	free(nr);

	if (center != -1 && (center < 0.00 || center > 1.00)) {
		log_err("fio: distribution center out of range (0 <= center <= 1.0)\n");
		return 1;
	}
	td->o.random_center.u.f = center;

	if (td->o.random_distribution == FIO_RAND_DIST_ZIPF) {
		if (val == 1.00) {
			log_err("fio: zipf theta must different than 1.0\n");
			return 1;
		}
		if (parse_dryrun())
			return 0;
		td->o.zipf_theta.u.f = val;
	} else if (td->o.random_distribution == FIO_RAND_DIST_PARETO) {
		if (val <= 0.00 || val >= 1.00) {
			log_err("fio: pareto input out of range (0 < input < 1.0)\n");
			return 1;
		}
		if (parse_dryrun())
			return 0;
		td->o.pareto_h.u.f = val;
	} else {
		if (val < 0.00 || val >= 100.0) {
			log_err("fio: normal deviation out of range (0 <= input < 100.0)\n");
			return 1;
		}
		if (parse_dryrun())
			return 0;
		td->o.gauss_dev.u.f = val;
	}

	return 0;
}

/*
 * [한국어]
 * is_valid_steadystate() — ss_state enum 이 유효한 FIO_SS_* 값 중 하나인지 검증.
 *
 * @state: 검증 대상.
 * @return: true=유효, false=비유효 (파싱 실패 또는 버그).
 *
 * 왜 필요한가: str_steadystate_cb 진입 시 방어적 검증. parse.c posval 매핑이 정상이면 항상 유효해야 함.
 */
static bool is_valid_steadystate(unsigned int state)
{
	return (state == FIO_SS_IOPS || state == FIO_SS_IOPS_SLOPE ||
		state == FIO_SS_BW || state == FIO_SS_BW_SLOPE ||
		state == FIO_SS_LAT || state == FIO_SS_LAT_SLOPE);
}

/*
 * [한국어]
 * str_steadystate_cb() — steadystate 옵션 콜백.
 *
 * @data: &td->o. ss_state 는 이미 posval로 FIO_SS_{IOPS,IOPS_SLOPE,BW,BW_SLOPE,LAT,LAT_SLOPE} 설정.
 * @str:  원본 "steadystate=iops:10%" 또는 "iops_slope:0.1%" 또는 "bw:5M" 등.
 * @return: 0=성공, 1=파싱 실패.
 *
 * 왜 필요한가: Steady State(정상 상태) 모드는 측정치가 충분히 안정화 되었는지 판단하고
 * 지정 기간(ss_dur) 동안 유지되면 잡을 자동 종료. SSD preconditioning 같은 산업 표준
 * 벤치마크(SNIA SSS)에서 필수. 시간 기반 런타임 대신 "안정화될 때까지" 조건부 종료.
 *
 * 기준 종류(6가지):
 *   - iops:          IOPS 평균 대비 표준편차 (절대/퍼센트).
 *   - iops_slope:    IOPS의 선형 회귀 기울기 (변동이 아닌 추세).
 *   - bw, bw_slope:  대역폭 버전.
 *   - lat, lat_slope: 레이턴시 버전.
 *
 * 값 형식:
 *   - "기준:X%" → 퍼센트 편차 (FIO_SS_PCT 플래그 세팅, ss_limit에 % 수치).
 *   - "기준:숫자" → 절대값 (IOPS는 float, BW는 k/m/g 포함 bytes/s, LAT는 시간 단위 ns/us/ms).
 *
 * 동작 단계:
 * 1) is_valid_steadystate() 로 ss_state enum 확인.
 * 2) get_opt_postfix(str) 로 ":뒤" 파라미터 추출.
 * 3) "%" 감지 → FIO_SS_PCT 비트 세팅, str_to_float 로 퍼센트.
 *    "%" 없음 + IOPS → str_to_float 로 절대 IOPS 값.
 *    "%" 없음 + LAT → check_str_time 로 ns 변환.
 *    "%" 없음 + BW → str_to_decimal 로 bytes/s.
 * 4) td->o.ss_limit.u.f (fio_fp64_t) 에 저장.
 * 5) td->ss.state 에 복사 — steadystate.c 가 주기적 검사 시 사용.
 *
 * 호출 체인: parse.c → [str_steadystate_cb] → str_to_float/check_str_time/str_to_decimal.
 * 읽는 자: steadystate.c::steadystate_check() 가 ss_limit/ss_state 로 수렴 판정.
 */
static int str_steadystate_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);
	double val;
	char *nr;
	char *pct;
	long long ll;

	if (!is_valid_steadystate(td->o.ss_state)) {
		/* should be impossible to get here */
		log_err("fio: unknown steady state criterion\n");
		return 1;
	}

	nr = get_opt_postfix(str);
	if (!nr) {
		log_err("fio: steadystate threshold must be specified in addition to criterion\n");
		free(nr);
		return 1;
	}

	/* ENHANCEMENT Allow fio to understand size=10.2% and use here */
	pct = strstr(nr, "%");
	if (pct) {
		*pct = '\0';
		strip_blank_end(nr);
		if (!str_to_float(nr, &val, 0))	{
			log_err("fio: could not parse steadystate threshold percentage\n");
			free(nr);
			return 1;
		}

		dprint(FD_PARSE, "set steady state threshold to %f%%\n", val);
		free(nr);
		if (parse_dryrun())
			return 0;

		td->o.ss_state |= FIO_SS_PCT;
		td->o.ss_limit.u.f = val;
	} else if (td->o.ss_state & FIO_SS_IOPS) {
		if (!str_to_float(nr, &val, 0)) {
			log_err("fio: steadystate IOPS threshold postfix parsing failed\n");
			free(nr);
			return 1;
		}

		dprint(FD_PARSE, "set steady state IOPS threshold to %f\n", val);
		free(nr);
		if (parse_dryrun())
			return 0;

		td->o.ss_limit.u.f = val;
        } else if (td->o.ss_state & FIO_SS_LAT) {
                long long tns;
                if (check_str_time(nr, &tns, 0)) {
                        log_err("fio: steadystate latency threshold parsing failed\n");
                        free(nr);
                        return 1;
                }

                dprint(FD_PARSE, "set steady state latency threshold to %lld nsec\n", tns);
                free(nr);
                if (parse_dryrun())
                        return 0;

                td->o.ss_limit.u.f = (double) tns;

	} else {	/* bandwidth criterion */
		if (str_to_decimal(nr, &ll, 1, td, 0, 0)) {
			log_err("fio: steadystate BW threshold postfix parsing failed\n");
			free(nr);
			return 1;
		}

		dprint(FD_PARSE, "set steady state BW threshold to %lld\n", ll);
		free(nr);
		if (parse_dryrun())
			return 0;

		td->o.ss_limit.u.f = (double) ll;
	}

	td->ss.state = td->o.ss_state;
	return 0;
}

/*
 * Return next name in the string. Files are separated with ':'. If the ':'
 * is escaped with a '\', then that ':' is part of the filename and does not
 * indicate a new file.
 */
/*
 * [한국어]
 * get_next_str() — 콜론 구분 문자열에서 다음 토큰 추출 (이스케이프 '\:' 지원).
 *
 * @ptr: [in, out] 진행 포인터. 호출 후 다음 토큰 시작 또는 NULL.
 * @return: 현재 토큰 시작 (NUL 종단). NULL이면 끝.
 *
 * 왜 필요한가: filename 옵션에 "/dev/sda:/dev/sdb" 형식으로 여러 대상 지정. Windows 드라이브
 * 문자 "C:\path" 같이 콜론이 이름 일부인 경우도 지원 위해 '\:' 이스케이프. 일반 strsep 과 달리
 * 이스케이프를 인식하고 제거한다.
 *
 * 동작 단계:
 * 1) 현재 위치에서 ':' 탐색.
 * 2) 없으면 전체 나머지를 토큰으로 반환, *ptr=NULL.
 * 3) ':' 가 시작 위치면 스킵 (연속 ':' 처리).
 * 4) ':' 앞 문자가 '\'이면 이스케이프 — memmove로 '\'제거 후 ':'뒤에서 다시 탐색.
 * 5) 순수 ':' 발견 시 그 자리에 NUL 삽입, *ptr 을 다음으로.
 *
 * 호출 체인: str_filename_cb / str_directory_cb / set_name_idx → [get_next_str].
 */
char *get_next_str(char **ptr)
{
	char *str = *ptr;
	char *p, *start;

	if (!str || !strlen(str))
		return NULL;

	start = str;
	do {
		/*
		 * No colon, we are done
		 */
		p = strchr(str, ':');
		if (!p) {
			*ptr = NULL;
			break;
		}

		/*
		 * We got a colon, but it's the first character. Skip and
		 * continue
		 */
		if (p == start) {
			str = ++start;
			continue;
		}

		if (*(p - 1) != '\\') {
			*p = '\0';
			*ptr = p + 1;
			break;
		}

		memmove(p - 1, p, strlen(p) + 1);
		str = p;
	} while (1);

	return start;
}


/*
 * [한국어]
 * get_max_str_idx() — 입력 문자열의 콜론 구분 토큰 개수 반환.
 *
 * @input: "/a:/b:/c" 같은 리스트.
 * @return: 토큰 수 (예: 3).
 *
 * 왜 필요한가: filename_format 등에서 여러 디렉토리를 잡 번호 모듈로로 라운드로빈할 때
 * 총 개수를 알아야 % 연산 가능.
 *
 * 동작: 복제본에 get_next_str 반복 호출해 NULL까지 카운팅.
 */
int get_max_str_idx(char *input)
{
	unsigned int cur_idx;
	char *str, *p;

	p = str = strdup(input);
	for (cur_idx = 0; ; cur_idx++)
		if (get_next_str(&str) == NULL)
			break;

	free(p);
	return cur_idx;
}

/*
 * Returns the directory at the index, indexes > entries will be
 * assigned via modulo division of the index
 */
/*
 * [한국어]
 * set_name_idx() — 콜론 구분 리스트에서 index 번째 항목 + 경로 구분자 조합해 target에 저장.
 *
 * @target:          [out] 결과 문자열 버퍼.
 * @tlen:            버퍼 크기.
 * @input:           "/dir1:/dir2:/dir3" 등 디렉토리 리스트.
 * @index:           조회 인덱스. 항목 수 초과 시 모듈로 순환.
 * @unique_filename: true이고 client_sockaddr_str 세팅됨 → "<dir>/<client_addr>." 형식.
 *                   false → "<dir>/" (플랫폼 경로 구분자 포함).
 * @return:          snprintf 결과 길이.
 *
 * 왜 필요한가: numjobs 와 directory 리스트가 같이 있을 때 각 잡이 다른 디렉토리를 쓰도록.
 * 서버 모드에서는 클라이언트 주소를 파일명에 포함해 서버가 여러 클라 동시 처리 시 충돌 방지.
 */
int set_name_idx(char *target, size_t tlen, char *input, int index,
		 bool unique_filename)
{
	unsigned int cur_idx;
	int len;
	char *fname, *str, *p;

	p = str = strdup(input);

	index %= get_max_str_idx(input);
	for (cur_idx = 0; cur_idx <= index; cur_idx++)
		fname = get_next_str(&str);

	if (client_sockaddr_str[0] && unique_filename) {
		len = snprintf(target, tlen, "%s/%s.", fname,
				client_sockaddr_str);
	} else
		len = snprintf(target, tlen, "%s%c", fname,
				FIO_OS_PATH_SEPARATOR);

	target[tlen - 1] = '\0';
	free(p);

	return len;
}

/*
 * [한국어]
 * get_name_by_idx() — set_name_idx 의 단순 버전: 토큰만 strdup으로 반환.
 *
 * @input: 콜론 구분 리스트.
 * @index: 인덱스 (모듈로 순환).
 * @return: strdup된 토큰. 호출자가 free 책임.
 */
char* get_name_by_idx(char *input, int index)
{
	unsigned int cur_idx;
	char *fname, *str, *p;

	p = str = strdup(input);

	index %= get_max_str_idx(input);
	for (cur_idx = 0; cur_idx <= index; cur_idx++)
		fname = get_next_str(&str);

	fname = strdup(fname);
	free(p);

	return fname;
}

/*
 * [한국어]
 * str_filename_cb() — filename 옵션 콜백.
 *
 * @data:  &td->o. directory 는 이미 str_directory_cb 로 처리됨 (prio=-1).
 * @input: 콜론 구분 파일/디바이스 리스트. 예: "/dev/sda:/dev/sdb", "/mnt/f1:/mnt/f2".
 *         이스케이프 '\:' 는 파일명 일부로 해석 (Windows 드라이브 문자 "C\:\path" 지원).
 * @return: 0.
 *
 * 왜 필요한가: fio 잡은 여러 대상을 동시 타격할 수 있다 (nrfiles 이상은 fio가 가상 파일 자동 생성).
 * filename 옵션으로 명시 리스트를 주면 fio가 각 항목을 add_file() 로 td->files[] 에 등록.
 *
 * 동작 단계:
 * 1) input 복제 + 공백 스트립.
 * 2) td->files_index==0 이면 이전에 nrfiles 옵션으로 세팅된 nr_files 를 리셋 — 명시 파일 리스트가 우선.
 * 3) get_next_str() 로 '\:' 이스케이프 고려한 콜론 분리.
 * 4) 각 파일명 add_file(td, fname, 0, 1) — filesetup.c 가 fio_file 할당+stat.
 *
 * 호출 체인: parse.c → [str_filename_cb] → get_next_str → filesetup.c::add_file.
 * 읽는 자: 이후 td->files[] 를 backend.c::thread_main → setup_files → open_files 가 소비.
 * 우선순위: .prio = -1 (directory 등 다른 옵션 파싱 완료 후 실행 — directory/filename 조합 지원).
 */
static int str_filename_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	char *fname, *str, *p;

	if (!input)
		return 1;

	p = str = strdup(input);

	strip_blank_front(&str);
	strip_blank_end(str);

	/*
	 * Ignore what we may already have from nrfiles option.
	 */
	if (!td->files_index)
		td->o.nr_files = 0;

	while ((fname = get_next_str(&str)) != NULL) {
		if (!strlen(fname))
			break;
		add_file(td, fname, 0, 1);
	}

	free(p);
	return 0;
}

/*
 * [한국어]
 * str_directory_cb() — directory 옵션 콜백 (경로 유효성 검증 전용).
 *
 * @data:   &td->o.
 * @unused: parse.c 가 전달한 문자열 — 무시 (td->o.directory 를 직접 읽음).
 * @return: 0=성공, 1=lstat 실패/디렉토리 아님.
 *
 * 왜 필요한가: filename 없이 directory 만 지정 시 fio는 "$jobname.$jobnum.$filenum" 형식으로
 * 파일을 자동 생성. 경로가 유효한 디렉토리인지 lstat + S_ISDIR 로 조기 검증 — 나중에 잡
 * 실행 중 실패하는 대신 파싱 단계에서 즉시 에러.
 *
 * 동작 단계:
 * 1) parse_dryrun() 방어.
 * 2) td->o.directory 복제 후 get_next_str() 로 콜론 구분 여러 디렉토리 순회 지원.
 * 3) 각 경로에 lstat → S_ISDIR 검사.
 * 4) 실패 시 td_verror로 errno 이관.
 *
 * 호출 체인: parse.c → [str_directory_cb] → lstat(2) → td_verror.
 */
static int str_directory_cb(void *data, const char fio_unused *unused)
{
	struct thread_data *td = cb_data_to_td(data);
	struct stat sb;
	char *dirname, *str, *p;
	int ret = 0;

	if (parse_dryrun())
		return 0;

	p = str = strdup(td->o.directory);
	while ((dirname = get_next_str(&str)) != NULL) {
		if (lstat(dirname, &sb) < 0) {
			ret = errno;

			log_err("fio: %s is not a directory\n", dirname);
			td_verror(td, ret, "lstat");
			goto out;
		}
		if (!S_ISDIR(sb.st_mode)) {
			log_err("fio: %s is not a directory\n", dirname);
			ret = 1;
			goto out;
		}
	}

out:
	free(p);
	return ret;
}

/*
 * [한국어]
 * str_opendir_cb() — opendir 옵션 콜백: 디렉토리의 모든 파일을 I/O 대상으로 등록.
 *
 * @data: &td->o.
 * @str:  사용 안 함 (td->o.opendir 을 직접 읽음).
 * @return: add_dir_files 의 반환값.
 *
 * 왜 필요한가: 많은 파일이 있는 디렉토리 전체를 대상으로 할 때 일일이 filename=에 나열하는 대신
 * opendir=/path 로 재귀 등록. filesetup.c::add_dir_files 가 readdir(3) 로 순회하며 S_ISREG만 추가.
 *
 * 동작: parse_dryrun 방어 → nr_files 리셋 → add_dir_files 위임.
 * 호출 체인: parse.c → [str_opendir_cb] → filesetup.c::add_dir_files → readdir(3) → add_file().
 */
static int str_opendir_cb(void *data, const char fio_unused *str)
{
	struct thread_data *td = cb_data_to_td(data);

	if (parse_dryrun())
		return 0;

	if (!td->files_index)
		td->o.nr_files = 0;

	return add_dir_files(td, td->o.opendir);
}

/*
 * [한국어]
 * str_buffer_pattern_cb() — buffer_pattern 옵션 콜백.
 *
 * @data:  &td->o.
 * @input: 패턴 문자열. 형식:
 *         - 리터럴 문자열: "ABCD" — 각 블록을 ABCD 반복으로 채움.
 *         - 16진수: "0xdeadbeef" — 4바이트 패턴.
 *         - 빈 문자열 금지.
 *         (%o 포맷 미지원 — verify_pattern 과 달리 주석에 FIXME).
 * @return: 0=성공, 1=파싱 실패.
 *
 * 왜 필요한가: 쓰기 I/O 버퍼를 특정 패턴으로 채워 디스크에 기록. verify_pattern 과 달리
 * 검증 목적이 아니라 **압축률 제어**(dedupe/compression 테스트)에 주로 사용. 예: 0x00
 * 반복은 SSD 내부 압축에 의해 실제 쓰기량이 크게 감소하는 것을 확인.
 *
 * 동작 단계:
 * 1) parse_and_fill_pattern_alloc 로 패턴 바이트 배열 할당+채움.
 * 2) buffer_pattern_bytes 에 결과 크기 저장.
 * 3) compress_percentage 또는 read 잡이면 refill_buffers=1 (매번 재채움 필요).
 *    그 외(pure write + no compress)는 refill 생략 — 한번 채운 버퍼 재사용으로 CPU 절약.
 * 4) scramble_buffers=0, zero_buffers=0 — 패턴이 우선되므로 스크램블/제로 필.
 *
 * 호출 체인: parse.c → [str_buffer_pattern_cb] → lib/pattern.c::parse_and_fill_pattern_alloc.
 * 읽는 자: io_u.c::fill_io_buffer 가 td->o.buffer_pattern 으로 io_u->buf 채움.
 */
static int str_buffer_pattern_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	int ret;

	if (!input)
		return 1;

	/* FIXME: for now buffer pattern does not support formats */
	ret = parse_and_fill_pattern_alloc(input, strlen(input),
				&td->o.buffer_pattern, NULL, NULL, NULL);
	if (ret < 0)
		return 1;

	assert(ret != 0);
	td->o.buffer_pattern_bytes = ret;

	/*
	 * If this job is doing any reading or has compression set,
	 * ensure that we refill buffers for writes or we could be
	 * invalidating the pattern through reads.
	 */
	if (!td->o.compress_percentage && !td_read(td))
		td->o.refill_buffers = 0;
	else
		td->o.refill_buffers = 1;

	td->o.scramble_buffers = 0;
	td->o.zero_buffers = 0;

	return 0;
}

/*
 * [한국어]
 * str_buffer_compress_cb() — buffer_compress_percentage 옵션 콜백.
 *
 * @data: &td->o.
 * @il:   [in] 압축 가능 퍼센트 (0~100). 버퍼의 몇 %가 압축 가능한 패턴이어야 하는지.
 * @return: 0.
 *
 * 왜 필요한가: 실제 워크로드의 압축률을 시뮬레이션. 압축 SSD/스토리지에서 쓰기 bandwidth
 * 효율을 측정. fio는 지정 퍼센트만큼 "압축 가능"(반복 바이트)으로 채우고 나머지는 랜덤.
 *
 * 효과:
 * - TD_F_COMPRESS 플래그 세팅 → io_u.c 버퍼 채우기 경로가 compress-aware 모드로 전환.
 * - compress_percentage 저장 → io_u.c::fill_random_buf_percentage 에서 참조.
 *
 * 호출 체인: parse.c → [str_buffer_compress_cb].
 * 읽는 자: io_u.c::fill_io_buffer → fill_random_buf_percentage.
 */
static int str_buffer_compress_cb(void *data, unsigned long long *il)
{
	struct thread_data *td = cb_data_to_td(data);

	td->flags |= TD_F_COMPRESS;
	td->o.compress_percentage = *il;
	return 0;
}

/*
 * [한국어]
 * str_dedupe_cb() — dedupe_percentage 옵션 콜백.
 *
 * @data: &td->o.
 * @il:   [in] 중복 퍼센트 (0~100). 전체 쓰기 블록 중 몇 %가 이전 블록과 동일해야 하는지.
 * @return: 0.
 *
 * 왜 필요한가: 중복 제거(deduplication) SSD/스토리지의 실질 용량 절감 효과 측정.
 * fio는 내부적으로 소수의 seed 블록을 유지하고 지정 퍼센트는 그 seed를 재사용.
 *
 * 효과:
 * - TD_F_COMPRESS 플래그 (dedupe도 압축 범주로 분류됨).
 * - dedupe_percentage 저장.
 * - refill_buffers=1 강제 — 매 I/O마다 버퍼 재생성 필요 (이전 버퍼와 의도적 중복 생성).
 *
 * 호출 체인: parse.c → [str_dedupe_cb].
 * 읽는 자: io_u.c::fill_io_buffer 의 dedupe 경로.
 */
static int str_dedupe_cb(void *data, unsigned long long *il)
{
	struct thread_data *td = cb_data_to_td(data);

	td->flags |= TD_F_COMPRESS;
	td->o.dedupe_percentage = *il;
	td->o.refill_buffers = 1;
	return 0;
}

/*
 * [한국어]
 * str_verify_pattern_cb() — verify_pattern 옵션 콜백.
 *
 * @data:  &td->o.
 * @input: 패턴 문자열. buffer_pattern과 달리 "%o"(오프셋 8바이트) 포맷 지원.
 *         예: "%o" — 각 블록 첫 8바이트에 해당 블록의 offset 기록. 오쓰기 탐지.
 * @return: 0=성공, 1=실패.
 *
 * 왜 필요한가: 쓰기/읽기 왕복 후 데이터 무결성 검증. CRC/해시는 계산 오버헤드가 있는데
 * 패턴 비교는 단순 memcmp — 빠르다. "%o" 포맷은 잘못된 위치로 매핑된 쓰기를 잡아냄
 * (드라이브 펌웨어 버그, 잘못된 섹터 원격 매핑 등).
 *
 * 동작 단계:
 * 1) verify_fmt_sz 에 포맷 슬롯 최대값 설정.
 * 2) parse_and_fill_pattern_alloc(input, fmt_desc, verify_fmt, &sz) —
 *    fmt_desc 는 "%o" → paste_blockoff 콜백 매핑. 런타임에 각 블록 offset 자동 삽입.
 * 3) verify_pattern_bytes 에 결과 크기 저장.
 * 4) verify 가 아직 미설정이면 VERIFY_PATTERN 으로 기본 세팅 — verify 옵션 없이
 *    verify_pattern 만 줘도 자동 활성화.
 *
 * 호출 체인: parse.c → [str_verify_pattern_cb] → lib/pattern.c::parse_and_fill_pattern_alloc.
 * 읽는 자: verify.c::verify_io_u_pattern() 이 저장된 패턴으로 읽은 데이터 검증.
 */
static int str_verify_pattern_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);
	int ret;

	if (!input)
		return 1;

	td->o.verify_fmt_sz = FIO_ARRAY_SIZE(td->o.verify_fmt);
	ret = parse_and_fill_pattern_alloc(input, strlen(input),
			&td->o.verify_pattern, fmt_desc, td->o.verify_fmt,
			&td->o.verify_fmt_sz);
	if (ret < 0)
		return 1;

	assert(ret != 0);
	td->o.verify_pattern_bytes = ret;
	/*
	 * VERIFY_* could already be set
	 */
	if (!fio_option_is_set(&td->o, verify))
		td->o.verify = VERIFY_PATTERN;

	return 0;
}

/*
 * [한국어]
 * str_gtod_reduce_cb() — gtod_reduce 옵션 콜백: 고성능 I/O 측정용 통계 축소 모드.
 *
 * @data: &td->o.
 * @il:   [in] 1=활성화, 0=무변경 (비활성화 효과 없음 — 켜려면 1).
 * @return: 0.
 *
 * 왜 필요한가: 초고성능 I/O (예: NVMe io_uring 수백만 IOPS)에서 매 I/O마다 clock_gettime/gettimeofday
 * 호출이 병목이 된다. 상세 레이턴시 통계(slat/clat/lat 분리, percentile 히스토그램)는 각 I/O에서
 * 수 번의 시간 측정을 요구 — 이를 일괄 비활성화하여 순수 IOPS/BW 측정에 집중.
 *
 * 효과 (val != 0 일 때):
 * - disable_lat = disable_clat = disable_slat = disable_bw = 1.
 * - clat_percentiles = lat_percentiles = slat_percentiles = 0 (히스토그램 비활성).
 * - ts_cache_mask = 63 — 타임스탬프 캐시 64회 재사용 (매번 아니라 매 64 I/O마다 갱신).
 *
 * 주의: 일반적으로는 상세 통계가 필요하므로 벤치마크 튜닝 이후 최종 측정 단계에서만 사용.
 *
 * 호출 체인: parse.c → [str_gtod_reduce_cb].
 * 읽는 자: io_u.c/stat.c 의 통계 경로가 disable_* 플래그로 skip 여부 판정.
 */
static int str_gtod_reduce_cb(void *data, int *il)
{
	struct thread_data *td = cb_data_to_td(data);
	int val = *il;

	/*
	 * Only modify options if gtod_reduce==1
	 * Otherwise leave settings alone.
	 */
	if (val) {
		td->o.disable_lat = 1;
		td->o.disable_clat = 1;
		td->o.disable_slat = 1;
		td->o.disable_bw = 1;
		td->o.clat_percentiles = 0;
		td->o.lat_percentiles = 0;
		td->o.slat_percentiles = 0;
		td->ts_cache_mask = 63;
	}

	return 0;
}

/*
 * [한국어]
 * str_offset_cb() — offset 옵션 콜백: 파일 내 시작 오프셋의 3-way 인코딩.
 *
 * @data:  &td->o.
 * @__val: [in] 파싱된 값. parse.c 의 FIO_OPT_STR_VAL_ZONE 이 아래 3가지 중 하나로 인코딩:
 *         (a) 일반 바이트 수 (예: 1048576).
 *         (b) 퍼센트 — -1ULL 근처 상위 비트 (parse_is_percent). 0~100%.
 *         (c) 존 단위 — ZONE_BASE_VAL 오프셋 (parse_is_zone). "1z" 등.
 * @return: 0.
 *
 * 왜 필요한가: offset 은 단일 숫자가 아니라 의미가 달라지는 3가지 표현:
 *   - "offset=1M" → 절대 바이트 (start_offset 에 저장).
 *   - "offset=10%" → 파일 크기의 10% 위치 (start_offset_percent에 저장, 실제값은 setup_files에서 해석).
 *   - "offset=3z" → 존 3개 건너뛴 위치 (ZBD 용, start_offset_nz).
 * parse.c 가 어떤 형식인지 마커 비트로 구분해 전달. 이 cb가 분기해 적절한 필드에 저장.
 *
 * 동작: parse_is_percent / parse_is_zone 판정 후 start_offset / _percent / _nz 3필드 중 하나 세팅.
 *
 * 호출 체인: parse.c → [str_offset_cb].
 * 읽는 자: filesetup.c::setup_files 가 파일 크기 확정 후 3필드 중 설정된 값을 바이트로 해석.
 */
static int str_offset_cb(void *data, long long *__val)
{
	struct thread_data *td = cb_data_to_td(data);
	unsigned long long v = *__val;

	if (parse_is_percent(v)) {
		td->o.start_offset = 0;
		td->o.start_offset_percent = -1ULL - v;
		td->o.start_offset_nz = 0;
		dprint(FD_PARSE, "SET start_offset_percent %d\n",
					td->o.start_offset_percent);
	} else if (parse_is_zone(v)) {
		td->o.start_offset = 0;
		td->o.start_offset_percent = 0;
		td->o.start_offset_nz = v - ZONE_BASE_VAL;
	} else
		td->o.start_offset = v;

	return 0;
}

/*
 * [한국어]
 * str_offset_increment_cb() — offset_increment 옵션 콜백. str_offset_cb 와 동일한 3-way 인코딩.
 *
 * @data: &td->o.
 * @__val: [in] 바이트/퍼센트/존 단위 인코딩.
 * @return: 0.
 *
 * 왜 필요한가: numjobs>1 일 때 각 잡의 시작 위치를 자동 분배. 예: numjobs=4, offset_increment=1G
 * → 잡 0은 0, 잡 1은 1G, 잡 2는 2G, 잡 3은 3G. 파일 겹침 없이 병렬 벤치.
 *
 * 동작: str_offset_cb 와 동일 패턴 — 3필드 중 하나(offset_increment/_percent/_nz) 세팅.
 * 읽는 자: init.c::fixup_options 에서 잡 번호에 곱해 실제 start_offset 계산.
 */
static int str_offset_increment_cb(void *data, long long *__val)
{
	struct thread_data *td = cb_data_to_td(data);
	unsigned long long v = *__val;

	if (parse_is_percent(v)) {
		td->o.offset_increment = 0;
		td->o.offset_increment_percent = -1ULL - v;
		td->o.offset_increment_nz = 0;
		dprint(FD_PARSE, "SET offset_increment_percent %d\n",
					td->o.offset_increment_percent);
	} else if (parse_is_zone(v)) {
		td->o.offset_increment = 0;
		td->o.offset_increment_percent = 0;
		td->o.offset_increment_nz = v - ZONE_BASE_VAL;
	} else
		td->o.offset_increment = v;

	return 0;
}

/*
 * [한국어]
 * str_size_cb() — size 옵션 콜백. str_offset_cb 와 동일한 3-way 인코딩.
 *
 * @data: &td->o.
 * @__val: [in] 바이트/퍼센트/존 인코딩.
 * @return: 0.
 *
 * 왜 필요한가: 잡이 처리할 총 데이터 양. 파일 크기보다 작을 수 있고 (일부만 I/O)
 * 절대 바이트 또는 파일 크기 대비 퍼센트 또는 존 개수로 표현.
 *
 * 동작: size / size_percent / size_nz 3필드 중 하나 세팅.
 * 읽는 자: filesetup.c가 파일 크기 확정 후 해석. io_u.c::keep_running이 진행 상황 비교.
 */
static int str_size_cb(void *data, long long *__val)
{
	struct thread_data *td = cb_data_to_td(data);
	unsigned long long v = *__val;

	if (parse_is_percent(v)) {
		td->o.size = 0;
		td->o.size_percent = -1ULL - v;
		dprint(FD_PARSE, "SET size_percent %d\n",
					td->o.size_percent);
	} else if (parse_is_zone(v)) {
		td->o.size = 0;
		td->o.size_percent = 0;
		td->o.size_nz = v - ZONE_BASE_VAL;
	} else
		td->o.size = v;

	return 0;
}

/*
 * [한국어]
 * str_io_size_cb() — io_size 옵션 콜백.
 *
 * @data:  &td->o.
 * @__val: [in] 바이트/퍼센트/존 인코딩.
 *         size 와 차이: size는 "파일에서 접근할 범위"를, io_size는 "실제 수행할 총 I/O 바이트"를 의미.
 * @return: 0=성공, 1=io_size_percent > 100 (랜덤 I/O는 같은 블록 반복 가능해서 100%+도 유효할 때가 있으나
 *          이 콜백에서는 100% 초과 거부 — uncapped 비트로 미리 걸러짐).
 *
 * 왜 필요한가: 파일이 10G 일 때 size=10G 로 두고 io_size=30G 하면 같은 10G 범위를 3번 반복 I/O.
 * loops=3 과 유사하지만 io_size는 바이트 기준이라 더 정밀.
 *
 * 특이점: parse_is_percent_uncapped 를 사용 — 100% 초과도 파싱 단계에서는 허용 (io_size=200%는
 * size의 2배 의미). 다만 이 함수 내부에서 100% 초과 시 명시적으로 거부.
 *
 * 동작: io_size / io_size_percent / io_size_nz 3필드 중 하나 세팅.
 */
static int str_io_size_cb(void *data, unsigned long long *__val)
{
	struct thread_data *td = cb_data_to_td(data);
	unsigned long long v = *__val;

	if (parse_is_percent_uncapped(v)) {
		td->o.io_size = 0;
		td->o.io_size_percent = -1ULL - v;
		if (td->o.io_size_percent > 100) {
			log_err("fio: io_size values greater than 100%% aren't supported\n");
			return 1;
		}
		dprint(FD_PARSE, "SET io_size_percent %d\n",
					td->o.io_size_percent);
	} else if (parse_is_zone(v)) {
		td->o.io_size = 0;
		td->o.io_size_percent = 0;
		td->o.io_size_nz = v - ZONE_BASE_VAL;
	} else
		td->o.io_size = v;

	return 0;
}

/*
 * [한국어]
 * str_zoneskip_cb() — zoneskip 옵션 콜백. 2-way 인코딩 (바이트 or 존 단위).
 *
 * @data: &td->o.
 * @__val: [in] 바이트 수 또는 ZONE_BASE_VAL+n (n 개 존).
 * @return: 0.
 *
 * 왜 필요한가: zonemode 에서 한 존을 다 쓴 후 다음 존으로 점프. zoneskip=Nz 면 "N 개 존 건너뛰기".
 * 이는 SMR(Shingled Magnetic Recording) 존 간섭 방지 테스트나 ZNS SSD 의 동시 쓰기 존 수 조절에 사용.
 *
 * 동작: zone_skip (바이트) 또는 zone_skip_nz (존 개수) 중 하나 세팅.
 * 읽는 자: io_u.c::setup_strided_zone_mode / zone_boundary() 가 사용.
 */
static int str_zoneskip_cb(void *data, long long *__val)
{
	struct thread_data *td = cb_data_to_td(data);
	unsigned long long v = *__val;

	if (parse_is_zone(v)) {
		td->o.zone_skip = 0;
		td->o.zone_skip_nz = v - ZONE_BASE_VAL;
	} else
		td->o.zone_skip = v;

	return 0;
}

/*
 * [한국어]
 * str_write_bw_log_cb() — write_bw_log 옵션 콜백.
 *
 * @data: &td->o.
 * @str:  로그 파일명 또는 NULL (stdout). NULL이면 기본 파일명 사용.
 * @return: 0.
 *
 * 왜 필요한가: 시계열 성능 분석을 위해 매 log_avg_msec 주기로 대역폭을 파일에 기록. fio_generate_plots
 * 가 이 로그로 그래프 생성. 형식: "<msec> <KB/s> <ddir> <bs> <offset>".
 *
 * 동작: str이 있으면 bw_log_file에 strdup, write_bw_log=1 플래그 세팅.
 * 읽는 자: stat.c::add_bw_sample/finalize_logs → iolog.c::write_bandw_log.
 */
static int str_write_bw_log_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	if (str)
		td->o.bw_log_file = strdup(str);

	td->o.write_bw_log = 1;
	return 0;
}

/*
 * [한국어]
 * str_write_lat_log_cb() — write_lat_log 옵션 콜백.
 *
 * @data: &td->o.
 * @str:  기본 이름. 실제 파일명은 "<name>_slat/clat/lat.log" 3개로 확장됨.
 * @return: 0.
 *
 * 왜 필요한가: 레이턴시는 submission latency(slat), completion latency(clat), total latency(lat) 3분리.
 * 각 항목별로 시계열 로그 생성. 레이턴시 분포 분석, tail latency 추적, CDF 작성에 필수.
 */
static int str_write_lat_log_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	if (str)
		td->o.lat_log_file = strdup(str);

	td->o.write_lat_log = 1;
	return 0;
}

/*
 * [한국어]
 * str_write_iops_log_cb() — write_iops_log 옵션 콜백.
 *
 * @data: &td->o.
 * @str:  로그 파일명.
 * @return: 0.
 *
 * 왜 필요한가: 초당 I/O 수를 시계열로 기록. IOPS 변동, 전력 상태 전환, SSD GC 영향 분석에 필수.
 * 읽는 자: stat.c::add_iops_sample → iolog.c::write_iops_log.
 */
static int str_write_iops_log_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	if (str)
		td->o.iops_log_file = strdup(str);

	td->o.write_iops_log = 1;
	return 0;
}

/*
 * [한국어]
 * str_write_hist_log_cb() — write_hist_log 옵션 콜백.
 *
 * @data: &td->o.
 * @str:  히스토그램 로그 파일명.
 * @return: 0.
 *
 * 왜 필요한가: 일반 lat_log 는 평균/퍼센타일만 기록하지만 hist_log 는 log_hist_msec 주기마다
 * 전체 plat_bucket[] 히스토그램 배열 전체를 덤프. 세부 분포 분석(HDR 히스토그램 재구성)에 사용.
 * 파일 크기가 크므로 통상 특수 분석 시에만 활성화.
 */
static int str_write_hist_log_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	if (str)
		td->o.hist_log_file = strdup(str);

	td->o.write_hist_log = 1;
	return 0;
}

/*
 * [한국어]
 * str_ioengine_external_cb() — ioengine=external:/path/to/engine.so 콜백.
 *
 * @data: &td->o.
 * @str:  콜론 뒤 경로 부분. parse.c 가 "external" posval 매칭 후 ":뒤"를 cb 로 전달.
 *        예: "/opt/custom_fio_engine.so".
 * @return: 0=성공, 1=NULL/잘못된 경로.
 *
 * 왜 필요한가: 외부 .so 엔진을 동적으로 로드 가능 — 사용자 정의 엔진을 fio 재빌드 없이 사용.
 * engines/skeleton_external.c 가 템플릿. dlopen(3) 으로 로드되고 "ioengine" 심볼 찾아 등록.
 *
 * 메모리 레이아웃 (파싱 중 변환):
 *   "external:/path/to/so\0"  ← strdup된 원본
 *   "external\0"              ← parse 후 td->o.ioengine (주값)
 *            "/path/to/so\0"  ← str 인자 = 이 함수가 ioengine_so_path 에 저장
 *
 * 동작 단계:
 * 1) 공백 스트립.
 * 2) stat(2) + S_ISREG — 파일 존재 + 정규 파일 확인 (.so 기대).
 * 3) 기존 ioengine_so_path 해제 후 strdup 으로 저장.
 *
 * 호출 체인: parse.c (posval "external" 매칭) → [str_ioengine_external_cb] → stat(2).
 * 읽는 자: ioengines.c::dlopen_ioengine 이 이 경로로 dlopen(3).
 */
static int str_ioengine_external_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);
	struct stat sb;
	char *p;

	if (!str) {
		log_err("fio: null external ioengine path\n");
		return 1;
	}

	p = (char *)str; /* str is mutable */
	strip_blank_front(&p);
	strip_blank_end(p);

	if (stat(p, &sb) || !S_ISREG(sb.st_mode)) {
		log_err("fio: invalid external ioengine path \"%s\"\n", p);
		return 1;
	}

	if (td->o.ioengine_so_path)
		free(td->o.ioengine_so_path);
	td->o.ioengine_so_path = strdup(p);
	return 0;
}

/*
 * [한국어]
 * rw_verify() — rw 옵션의 .verify 콜백: readonly 모드에서 write/trim 잡 거부.
 *
 * @o:    fio_option 자기 자신 (사용하지 않음).
 * @data: &td->o.
 * @return: 0=통과, 1=거부 (잡 전체 실패).
 *
 * 왜 필요한가: fio 전역 read_only 플래그(--readonly CLI)는 "어떤 잡도 쓰기를 해서는 안 됨"을
 * 강제한다. 실수로 운영 디스크에 쓰기 테스트를 돌리는 것을 방지. 잡 파일에 rw=randwrite가
 * 있더라도 --readonly 가 우선 — 파싱 단계에서 에러로 중단.
 *
 * 호출 시점: parse.c 가 rw 옵션 파싱 완료 후 .verify 콜백을 호출 (파싱과 검증 분리 메커니즘).
 * 호출 체인: parse.c::parse_option → [rw_verify] → log_err + 반환 1.
 */
static int rw_verify(const struct fio_option *o, void *data)
{
	struct thread_data *td = cb_data_to_td(data);

	if (read_only && (td_write(td) || td_trim(td))) {
		log_err("fio: job <%s> has write or trim bit set, but"
			" fio is in read-only mode\n", td->o.name);
		return 1;
	}

	return 0;
}

/*
 * [한국어]
 * gtod_cpu_verify() — gtod_cpu 옵션의 .verify 콜백: CPU affinity 지원 확인.
 *
 * @o:    fio_option 자기 자신 (사용하지 않음).
 * @data: &td->o.
 * @return: 0=통과, 1=거부.
 *
 * 왜 필요한가: gtod_cpu 는 gettimeofday() 오프로드 — 별도 CPU에 전용 스레드를 두고 그 스레드가
 * 계속 시간을 갱신하면 잡 스레드는 rdtsc 캐시만 읽어 clock syscall 회피. 단, 전용 CPU에 스레드를
 * 고정해야 하므로 sched_setaffinity 가 필수. FIO_HAVE_CPU_AFFINITY 미지원 플랫폼에서는 거부.
 *
 * 동작 분기:
 * - #ifdef FIO_HAVE_CPU_AFFINITY: 항상 0 반환 (통과).
 * - 미지원 빌드: gtod_cpu 이 0이 아니면 에러 + 1 반환.
 */
static int gtod_cpu_verify(const struct fio_option *o, void *data)
{
#ifndef FIO_HAVE_CPU_AFFINITY
	struct thread_data *td = cb_data_to_td(data);

	if (td->o.gtod_cpu) {
		log_err("fio: platform must support CPU affinity for"
			"gettimeofday() offloading\n");
		return 1;
	}
#endif

	return 0;
}

/*
 * [한국어] ====== fio_options[] 공통 규약 블록 (모든 엔트리에 적용) ======
 *
 * 이 배열은 약 300여 개의 fio 옵션을 순서대로 정의하는 fio 파싱의 핵심 **데이터베이스**다.
 * 각 엔트리는 struct fio_option (parse.h 정의) 이며, 공통 필드 의미는 다음과 같다.
 * 개별 엔트리의 주석은 필드 고유 의미(특히 posval/cb/parent/hide/minval/maxval/def)에 집중한다.
 *
 * [필드 의미 — 파일 전체에 공통 적용, 각 엔트리에서 반복 설명하지 않음]
 *
 *   .name       CLI/INI 옵션 이름 (필수). 예: "rw", "bs", "iodepth". 첫 번째 매칭 키.
 *   .lname      긴 설명 라벨 (fio --cmdhelp 및 GUI 출력에 사용).
 *   .alias      옵션 별칭. 예: rw 의 alias="readwrite". 둘 다 동일 엔트리로 매칭.
 *   .type       파싱 타입 — 파서가 값을 어떻게 해석할지 결정:
 *                 FIO_OPT_STR        ─ 미리 정의된 posval 중 하나 매칭 (예: rw=randread).
 *                 FIO_OPT_STR_STORE  ─ 임의 문자열을 strdup해서 보관 (예: filename).
 *                 FIO_OPT_STR_SET    ─ 값 없이 플래그로 활성화 (예: --readonly).
 *                 FIO_OPT_STR_VAL    ─ 단위(k/m/g) 포함 숫자를 uint64_t로. 예: size=4M.
 *                 FIO_OPT_STR_VAL_INT ─ 위와 동일하나 int.
 *                 FIO_OPT_STR_VAL_TIME ─ 시간 단위(us/ms/s/m/h) 포함. 예: runtime=30s.
 *                 FIO_OPT_STR_VAL_ZONE ─ 바이트 or 존 단위 (z 접미사).
 *                 FIO_OPT_INT        ─ 일반 정수. minval/maxval 클램프.
 *                 FIO_OPT_ULL        ─ unsigned long long.
 *                 FIO_OPT_BOOL       ─ 0/1 또는 true/false/yes/no.
 *                 FIO_OPT_FLOAT_LIST ─ 콤마 구분 부동소수점 목록 (percentile 등).
 *                 FIO_OPT_RANGE      ─ "min-max" 범위 파싱.
 *                 FIO_OPT_STR_MULTI  ─ 여러 posval의 조합 (비트마스크).
 *                 FIO_OPT_STR_ULL    ─ posval인데 unsigned long long 저장.
 *                 FIO_OPT_DEPRECATED ─ 옵션은 유지하되 경고 출력 후 무시.
 *                 FIO_OPT_UNSUPPORTED ─ 현재 빌드에서 사용 불가 (configure에서 비활성).
 *                 FIO_OPT_INVALID    ─ 사용 금지 (프로파일 unload 시 마킹).
 *   .off1~.off6  thread_options 내 저장 대상 필드 오프셋 (offsetof로 지정).
 *                최대 6개 필드에 분산 저장 (예: 범위 옵션은 min=off1, max=off2).
 *                off1=0 이면서 .cb가 있으면 "콜백에서 직접 처리" 의미 (패치 저장 없음).
 *   .def        기본값 문자열. parse.c 가 잡 생성 시 자동 적용.
 *   .help       --cmdhelp=<opt> 출력 텍스트.
 *   .cb         커스텀 파싱 콜백 (위에서 정의된 str_*_cb 함수들).
 *                시그니처: int (*)(void *data, const char *str|long long *val|unsigned long long *il|int *il).
 *                data는 &td->o 이며 cb_data_to_td(data) 로 td 역산.
 *   .verify     파싱 후 사후 검증 콜백. 실패 시 잡 전체 거부.
 *                예: rw_verify → read_only 모드에서 write/trim 거부.
 *   .minval/.maxval  INT/ULL 타입의 허용 범위. 초과 시 parse.c 가 에러.
 *   .minlen/.maxlen  STR_STORE 타입의 문자열 길이 제한.
 *   .interval   GUI 슬라이더 단계 (단위별 증감).
 *   .posval[PARSE_MAX_VP=32]  STR/STR_MULTI 타입의 허용 값 테이블:
 *                  { .ival="문자열", .oval=정수값, .help="설명", .cb=선택적 콜백 }.
 *                  ioengine 같은 동적 엔트리는 engines/*.c 등록 시점에 add_opt_posval 로 추가됨.
 *   .parent     종속 부모 옵션 이름. 이 옵션은 parent가 활성일 때만 유효.
 *                예: iodepth_batch.parent="iodepth" → iodepth 설정 없으면 의미 없음.
 *   .hide       이 옵션을 `fio --help` 출력에서 숨김 (내부용 / 드물게 쓰는 옵션).
 *   .hide_on_set  다른 옵션 설정 시 자동 숨김 (상호배제 UX).
 *   .exclusive_group  같은 그룹의 옵션들은 상호배타.
 *   .prio       파싱 우선순위. 음수=나중, 양수=먼저. filename.prio=-1 → directory 먼저.
 *   .category   FIO_OPT_C_{GENERAL,FILE,IO,ENGINE,STAT,LOG,VERIFY,PROFILE} — 대분류.
 *   .group      FIO_OPT_G_* — 세부 그룹 (IO_BASIC/IO_BUF/FILENAME/RANDOM/ZONE/VERIFY/...).
 *   .prof_name  이 옵션이 프로파일 전용일 때 프로파일명 기록 (profile unload 시 무효화 대상).
 *   .inverse    불린의 반대 의미 옵션 (예: fallocate=none ↔ nofallocate=1).
 *   .is_seconds 숫자를 초 단위로 해석 (runtime 등).
 *   .is_time    시간 값 여부 (us/ms/s 파싱 힌트).
 *
 * [섹션 구성 — 아래 엔트리들은 대략 이 순서로 배치됨]
 *   1) 일반(General): name, description, wait_for
 *   2) 파일(File): filename, filetype, directory, filename_format, lockfile, opendir
 *   3) I/O 패턴 및 엔진: rw, rw_sequencer, ioengine, ioengine_external
 *   4) I/O 깊이: iodepth, iodepth_batch, iodepth_low, serialize_overlap, io_submit_mode
 *   5) 블록 크기: bs, bs_unaligned, bssplit, bsrange, ba(blockalign)
 *   6) 버퍼링/캐시: direct, atomic, buffered, sync, fadvise_hint
 *   7) 실행 제어: size, io_size, fill_device, offset, offset_increment, numjobs, loops
 *   8) 데이터 검증: verify, verify_interval, verify_pattern, verify_fatal, verify_async
 *   9) 존(Zone): zonemode, zonesize, zonecapacity, zoneskip, zone_split, recovery_mode
 *   10) FDP: fdp, fdp_pli, fdp_pli_select, dataplacement, dp_scheme
 *   11) 속도 제한: rate, rate_min, rate_cycle, rate_process, rate_ignore_thinktime
 *   12) 레이턴시 목표: latency_target, latency_window, latency_percentile, max_latency
 *   13) CPU/NUMA: cpumask, cpus_allowed, cpus_allowed_policy, numa_cpu_nodes, numa_mem_policy
 *   14) 로깅/통계: write_bw_log, write_lat_log, write_iops_log, log_avg_msec, ...
 *
 * [엔트리 해석 예시]
 *   {
 *     .name = "rw",                            → 이 이름으로 잡 파일/CLI에서 매칭
 *     .lname = "Read/write",                   → --cmdhelp 출력 레이블
 *     .alias = "readwrite",                    → 별칭
 *     .type = FIO_OPT_STR,                     → posval 중 하나 매칭
 *     .cb = str_rw_cb,                         → ",시퀀스" 후위 처리를 위한 커스텀 파서
 *     .off1 = offsetof(thread_options, td_ddir), → 기본 매칭 결과 저장 위치
 *     .def = "read",                           → 잡 파일에서 rw 미지정 시 순차 읽기
 *     .verify = rw_verify,                     → readonly 모드 호환성 사후 검증
 *     .category = FIO_OPT_C_IO,                → I/O 카테고리
 *     .group = FIO_OPT_G_IO_BASIC,             → 기본 I/O 그룹
 *     .posval = {                              → 허용 값 목록
 *       { .ival = "read",      .oval = TD_DDIR_READ,      .help = "Sequential read" },
 *       { .ival = "randread",  .oval = TD_DDIR_RANDREAD,  .help = "Random read" },
 *       ...
 *     },
 *   }
 *
 * [sentinel]  배열 끝에는 { .name = NULL } 를 두어 parse.c 가 순회 종료를 감지한다.
 *             FIO_MAX_OPTS 는 런타임에 add_option() 으로 동적 엔진 옵션을 추가할 때의 상한.
 *
 * ==================================================================
 */
struct fio_option fio_options[FIO_MAX_OPTS] = {
	/* ===== [한국어] 일반 옵션 (General) ===== */
	{
		.name	= "description",
		.lname	= "Description of job",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, description),
		.help	= "Text job description",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_DESC,
	},
	{
		.name	= "name",
		.lname	= "Job name",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, name),
		.help	= "Name of this job",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_DESC,
	},
	{
		.name	= "wait_for",
		.lname	= "Waitee name",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, wait_for),
		.help	= "Name of the job this one wants to wait for before starting",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_DESC,
	},
	{
		.name	= "filename",
		.lname	= "Filename(s)",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, filename),
		.maxlen	= PATH_MAX,
		.cb	= str_filename_cb,
		.prio	= -1, /* must come after "directory" */
		.help	= "File(s) to use for the workload",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_FILENAME,
	},
	{
		.name   = "filetype",
		.lname  = "file_type",
		.type   = FIO_OPT_STR,
		.off1   = offsetof(struct thread_options, filetype),
		.help   = "Assume all files defined in a job are of this type",
		.def    = "none",
		.group  = FIO_OPT_G_IO_BASIC,
		.category = FIO_OPT_C_FILE,
		.posval = {
			  { .ival = "none",  .oval = 0 },
			  { .ival = "file",  .oval = FIO_TYPE_FILE },
			  { .ival = "block", .oval = FIO_TYPE_BLOCK },
			  { .ival = "char",  .oval = FIO_TYPE_CHAR },
		},
	},
	{
		.name	= "directory",
		.lname	= "Directory",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, directory),
		.cb	= str_directory_cb,
		.help	= "Directory to store files in",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_FILENAME,
	},
	{
		.name	= "filename_format",
		.lname	= "Filename Format",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, filename_format),
		.prio	= -1, /* must come after "directory" */
		.help	= "Override default $jobname.$jobnum.$filenum naming",
		.def	= "$jobname.$jobnum.$filenum",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_FILENAME,
	},
	{
		.name	= "unique_filename",
		.lname	= "Unique Filename",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, unique_filename),
		.help	= "For network clients, prefix file with source IP",
		.def	= "1",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_FILENAME,
	},
	{
		.name	= "lockfile",
		.lname	= "Lockfile",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, file_lock_mode),
		.help	= "Lock file when doing IO to it",
		.prio	= 1,
		.parent	= "filename",
		.hide	= 0,
		.def	= "none",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_FILENAME,
		.posval = {
			  { .ival = "none",
			    .oval = FILE_LOCK_NONE,
			    .help = "No file locking",
			  },
			  { .ival = "exclusive",
			    .oval = FILE_LOCK_EXCLUSIVE,
			    .help = "Exclusive file lock",
			  },
			  {
			    .ival = "readwrite",
			    .oval = FILE_LOCK_READWRITE,
			    .help = "Read vs write lock",
			  },
		},
	},
	{
		.name	= "opendir",
		.lname	= "Open directory",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, opendir),
		.cb	= str_opendir_cb,
		.help	= "Recursively add files from this directory and down",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_FILENAME,
	},
	/* ===== [한국어] I/O 패턴 및 엔진 옵션 ===== */
	{
		.name	= "rw",        /* I/O 방향: read, write, randread, randwrite, randrw 등 */
		.lname	= "Read/write",
		.alias	= "readwrite",
		.type	= FIO_OPT_STR,
		.cb	= str_rw_cb,
		.off1	= offsetof(struct thread_options, td_ddir),
		.help	= "IO direction",
		.def	= "read",
		.verify	= rw_verify,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
		.posval = {
			  { .ival = "read",
			    .oval = TD_DDIR_READ,
			    .help = "Sequential read",
			  },
			  { .ival = "write",
			    .oval = TD_DDIR_WRITE,
			    .help = "Sequential write",
			  },
			  { .ival = "trim",
			    .oval = TD_DDIR_TRIM,
			    .help = "Sequential trim",
			  },
			  { .ival = "randread",
			    .oval = TD_DDIR_RANDREAD,
			    .help = "Random read",
			  },
			  { .ival = "randwrite",
			    .oval = TD_DDIR_RANDWRITE,
			    .help = "Random write",
			  },
			  { .ival = "randtrim",
			    .oval = TD_DDIR_RANDTRIM,
			    .help = "Random trim",
			  },
			  { .ival = "rw",
			    .oval = TD_DDIR_RW,
			    .help = "Sequential read and write mix",
			  },
			  { .ival = "readwrite",
			    .oval = TD_DDIR_RW,
			    .help = "Sequential read and write mix",
			  },
			  { .ival = "randrw",
			    .oval = TD_DDIR_RANDRW,
			    .help = "Random read and write mix"
			  },
			  { .ival = "trimwrite",
			    .oval = TD_DDIR_TRIMWRITE,
			    .help = "Trim and write mix, trims preceding writes"
			  },
			  { .ival = "randtrimwrite",
			    .oval = TD_DDIR_RANDTRIMWRITE,
			    .help = "Randomly trim and write mix, trims preceding writes"
			  },
		},
	},
	{
		.name	= "rw_sequencer",
		.lname	= "RW Sequencer",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, rw_seq),
		.help	= "IO offset generator modifier",
		.def	= "sequential",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
		.posval = {
			  { .ival = "sequential",
			    .oval = RW_SEQ_SEQ,
			    .help = "Generate sequential offsets",
			  },
			  { .ival = "identical",
			    .oval = RW_SEQ_IDENT,
			    .help = "Generate identical offsets",
			  },
		},
	},

	{
		.name	= "ioengine",
		.lname	= "IO Engine",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, ioengine),
		.help	= "IO engine to use",
		.def	= FIO_PREFERRED_ENGINE,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
		.posval	= {
			  { .ival = "sync",
			    .help = "Use read/write",
			  },
			  { .ival = "psync",
			    .help = "Use pread/pwrite",
			  },
			  { .ival = "vsync",
			    .help = "Use readv/writev",
			  },
#ifdef CONFIG_PWRITEV
			  { .ival = "pvsync",
			    .help = "Use preadv/pwritev",
			  },
#endif
#ifdef FIO_HAVE_PWRITEV2
			  { .ival = "pvsync2",
			    .help = "Use preadv2/pwritev2",
			  },
#endif
#ifdef CONFIG_LIBAIO
			  { .ival = "libaio",
			    .help = "Linux native asynchronous IO",
			  },
#endif
#ifdef ARCH_HAVE_IOURING
			  { .ival = "io_uring",
			    .help = "Fast Linux native aio",
			  },
#endif
#ifdef CONFIG_POSIXAIO
			  { .ival = "posixaio",
			    .help = "POSIX asynchronous IO",
			  },
#endif
#ifdef CONFIG_SOLARISAIO
			  { .ival = "solarisaio",
			    .help = "Solaris native asynchronous IO",
			  },
#endif
#ifdef CONFIG_WINDOWSAIO
			  { .ival = "windowsaio",
			    .help = "Windows native asynchronous IO"
			  },
#endif
#ifdef CONFIG_RBD
			  { .ival = "rbd",
			    .help = "Rados Block Device asynchronous IO"
			  },
#endif
			  { .ival = "mmap",
			    .help = "Memory mapped IO"
			  },
#ifdef CONFIG_LINUX_SPLICE
			  { .ival = "splice",
			    .help = "splice/vmsplice based IO",
			  },
			  { .ival = "netsplice",
			    .help = "splice/vmsplice to/from the network",
			  },
#endif
#ifdef FIO_HAVE_SGIO
			  { .ival = "sg",
			    .help = "SCSI generic v3 IO",
			  },
#endif
			  { .ival = "null",
			    .help = "Testing engine (no data transfer)",
			  },
			  { .ival = "net",
			    .help = "Network IO",
			  },
			  { .ival = "cpuio",
			    .help = "CPU cycle burner engine",
			  },
#ifdef CONFIG_RDMA
			  { .ival = "rdma",
			    .help = "RDMA IO engine",
			  },
#endif
#ifdef CONFIG_LINUX_EXT4_MOVE_EXTENT
			  { .ival = "e4defrag",
			    .help = "ext4 defrag engine",
			  },
#endif
#ifdef CONFIG_LINUX_FALLOCATE
			  { .ival = "falloc",
			    .help = "fallocate() file based engine",
			  },
#endif
#ifdef CONFIG_GFAPI
			  { .ival = "gfapi",
			    .help = "Glusterfs libgfapi(sync) based engine"
			  },
			  { .ival = "gfapi_async",
			    .help = "Glusterfs libgfapi(async) based engine"
			  },
#endif
#ifdef CONFIG_LIBHDFS
			  { .ival = "libhdfs",
			    .help = "Hadoop Distributed Filesystem (HDFS) engine"
			  },
#endif
#ifdef CONFIG_IME
			  { .ival = "ime_psync",
			    .help = "DDN's IME synchronous IO engine",
			  },
			  { .ival = "ime_psyncv",
			    .help = "DDN's IME synchronous IO engine using iovecs",
			  },
			  { .ival = "ime_aio",
			    .help = "DDN's IME asynchronous IO engine",
			  },
#endif
#ifdef CONFIG_LINUX_DEVDAX
			  { .ival = "dev-dax",
			    .help = "DAX Device based IO engine",
			  },
#endif
			  {
			    .ival = "filecreate",
			    .help = "File creation engine",
			  },
			  { .ival = "external",
			    .help = "Load external engine (append name)",
			    .cb = str_ioengine_external_cb,
			  },
#ifdef CONFIG_LIBPMEM
			  { .ival = "libpmem",
			    .help = "PMDK libpmem based IO engine",
			  },
#endif
#ifdef CONFIG_HTTP
			  { .ival = "http",
			    .help = "HTTP (WebDAV/S3) IO engine",
			  },
#endif
			  { .ival = "nbd",
			    .help = "Network Block Device (NBD) IO engine"
			  },
#ifdef CONFIG_DFS
			  { .ival = "dfs",
			    .help = "DAOS File System (dfs) IO engine",
			  },
#endif
#ifdef CONFIG_LIBNFS
			  { .ival = "nfs",
			    .help = "NFS IO engine",
			  },
#endif
#ifdef CONFIG_LIBXNVME
			  { .ival = "xnvme",
			    .help = "XNVME IO engine",
			  },
#endif
		},
	},
	/* ===== [한국어] I/O 깊이(큐잉) 옵션 ===== */
	{
		.name	= "iodepth",   /* 비동기 I/O 큐 깊이 (libaio, io_uring 등에서 핵심) */
		.lname	= "IO Depth",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, iodepth),
		.help	= "Number of IO buffers to keep in flight",
		.minval = 1,
		.interval = 1,
		.def	= "1",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
	},
	{
		.name	= "iodepth_batch",
		.lname	= "IO Depth batch",
		.alias	= "iodepth_batch_submit",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, iodepth_batch),
		.help	= "Number of IO buffers to submit in one go",
		.parent	= "iodepth",
		.hide	= 1,
		.interval = 1,
		.def	= "1",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
	},
	{
		.name	= "iodepth_batch_complete_min",
		.lname	= "Min IO depth batch complete",
		.alias	= "iodepth_batch_complete",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, iodepth_batch_complete_min),
		.help	= "Min number of IO buffers to retrieve in one go",
		.parent	= "iodepth",
		.hide	= 1,
		.minval	= 0,
		.interval = 1,
		.def	= "1",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
	},
	{
		.name	= "iodepth_batch_complete_max",
		.lname	= "Max IO depth batch complete",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, iodepth_batch_complete_max),
		.help	= "Max number of IO buffers to retrieve in one go",
		.parent	= "iodepth",
		.hide	= 1,
		.minval	= 0,
		.interval = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
	},
	{
		.name	= "iodepth_low",
		.lname	= "IO Depth batch low",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, iodepth_low),
		.help	= "Low water mark for queuing depth",
		.parent	= "iodepth",
		.hide	= 1,
		.interval = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
	},
	{
		.name	= "serialize_overlap",
		.lname	= "Serialize overlap",
		.off1	= offsetof(struct thread_options, serialize_overlap),
		.type	= FIO_OPT_BOOL,
		.help	= "Wait for in-flight IOs that collide to complete",
		.parent	= "iodepth",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
	},
	{
		.name	= "io_submit_mode",
		.lname	= "IO submit mode",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, io_submit_mode),
		.help	= "How IO submissions and completions are done",
		.def	= "inline",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BASIC,
		.posval = {
			  { .ival = "inline",
			    .oval = IO_MODE_INLINE,
			    .help = "Submit and complete IO inline",
			  },
			  { .ival = "offload",
			    .oval = IO_MODE_OFFLOAD,
			    .help = "Offload submit and complete to threads",
			  },
		},
	},
	{
		.name	= "size",
		.lname	= "Size",
		.type	= FIO_OPT_STR_VAL_ZONE,
		.cb	= str_size_cb,
		.off1	= offsetof(struct thread_options, size),
		.help	= "Total size of device or files",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "io_size",
		.alias	= "io_limit",
		.lname	= "IO Size",
		.type	= FIO_OPT_STR_VAL_ZONE,
		.cb	= str_io_size_cb,
		.off1	= offsetof(struct thread_options, io_size),
		.help	= "Total size of I/O to be performed",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "fill_device",
		.lname	= "Fill device",
		.alias	= "fill_fs",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, fill_device),
		.help	= "Write until an ENOSPC error occurs",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "filesize",
		.lname	= "File size",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, file_size_low),
		.off2	= offsetof(struct thread_options, file_size_high),
		.minval = 1,
		.help	= "Size of individual files",
		.interval = 1024 * 1024,
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "file_append",
		.lname	= "File append",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, file_append),
		.help	= "IO will start at the end of the file(s)",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "offset",
		.lname	= "IO offset",
		.alias	= "fileoffset",
		.type	= FIO_OPT_STR_VAL_ZONE,
		.cb	= str_offset_cb,
		.off1	= offsetof(struct thread_options, start_offset),
		.help	= "Start IO from this offset",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "offset_align",
		.lname	= "IO offset alignment",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, start_offset_align),
		.help	= "Start IO from this offset alignment",
		.def	= "0",
		.interval = 512,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "offset_increment",
		.lname	= "IO offset increment",
		.type	= FIO_OPT_STR_VAL_ZONE,
		.cb	= str_offset_increment_cb,
		.off1	= offsetof(struct thread_options, offset_increment),
		.help	= "What is the increment from one offset to the next",
		.parent = "offset",
		.hide	= 1,
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "number_ios",
		.lname	= "Number of IOs to perform",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, number_ios),
		.help	= "Force job completion after this number of IOs",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "num_range",
		.lname	= "Number of ranges",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, num_range),
		.maxval	= MAX_TRIM_RANGE,
		.help	= "Number of ranges for trim command",
		.def	= "1",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	/* ===== [한국어] 블록 크기(Block Size) 옵션 ===== */
	{
		.name	= "bs",        /* 블록 크기: 기본 4k. 읽기,쓰기,트림 별도 지정 가능 */
		.lname	= "Block size",
		.alias	= "blocksize",
		.type	= FIO_OPT_ULL,
		.off1	= offsetof(struct thread_options, bs[DDIR_READ]),
		.off2	= offsetof(struct thread_options, bs[DDIR_WRITE]),
		.off3	= offsetof(struct thread_options, bs[DDIR_TRIM]),
		.minval = 1,
		.help	= "Block size unit",
		.def	= "4096",
		.parent = "rw",
		.hide	= 1,
		.interval = 512,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "ba",
		.lname	= "Block size align",
		.alias	= "blockalign",
		.type	= FIO_OPT_ULL,
		.off1	= offsetof(struct thread_options, ba[DDIR_READ]),
		.off2	= offsetof(struct thread_options, ba[DDIR_WRITE]),
		.off3	= offsetof(struct thread_options, ba[DDIR_TRIM]),
		.minval	= 1,
		.help	= "IO block offset alignment",
		.parent	= "rw",
		.hide	= 1,
		.interval = 512,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "bsrange",
		.lname	= "Block size range",
		.alias	= "blocksize_range",
		.type	= FIO_OPT_RANGE,
		.off1	= offsetof(struct thread_options, min_bs[DDIR_READ]),
		.off2	= offsetof(struct thread_options, max_bs[DDIR_READ]),
		.off3	= offsetof(struct thread_options, min_bs[DDIR_WRITE]),
		.off4	= offsetof(struct thread_options, max_bs[DDIR_WRITE]),
		.off5	= offsetof(struct thread_options, min_bs[DDIR_TRIM]),
		.off6	= offsetof(struct thread_options, max_bs[DDIR_TRIM]),
		.minval = 1,
		.help	= "Set block size range (in more detail than bs)",
		.parent = "rw",
		.hide	= 1,
		.interval = 4096,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "bssplit",
		.lname	= "Block size split",
		.type	= FIO_OPT_STR_ULL,
		.cb	= str_bssplit_cb,
		.off1	= offsetof(struct thread_options, bssplit),
		.help	= "Set a specific mix of block sizes",
		.parent	= "rw",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "bs_unaligned",
		.lname	= "Block size unaligned",
		.alias	= "blocksize_unaligned",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, bs_unaligned),
		.help	= "Don't sector align IO buffer sizes",
		.parent = "rw",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "bs_is_seq_rand",
		.lname	= "Block size division is seq/random (not read/write)",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, bs_is_seq_rand),
		.help	= "Consider any blocksize setting to be sequential,random",
		.def	= "0",
		.parent = "blocksize",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "randrepeat",
		.alias	= "allrandrepeat",
		.lname	= "Random repeatable",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, rand_repeatable),
		.help	= "Use repeatable random IO pattern",
		.def	= "1",
		.parent = "rw",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "randseed",
		.lname	= "The random generator seed",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, rand_seed),
		.help	= "Set the random generator seed value",
		.def	= "0x89",
		.parent = "rw",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "norandommap",
		.lname	= "No randommap",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, norandommap),
		.help	= "Accept potential duplicate random blocks",
		.parent = "rw",
		.hide	= 1,
		.hide_on_set = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "softrandommap",
		.lname	= "Soft randommap",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, softrandommap),
		.help	= "Set norandommap if randommap allocation fails",
		.parent	= "norandommap",
		.hide	= 1,
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "sprandom",
		.lname	= "Sandisk Pseudo Random Preconditioning",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, sprandom),
		.help	= "Set up Sandisk Pseudo Random Preconditioning",
		.parent	= "rw",
		.hide	= 1,
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "spr_num_regions",
		.lname	= "SPRandom number of regions",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, spr_num_regions),
		.help	= "Number of regions for sprandom",
		.parent	= "sprandom",
		.hide	= 1,
		.def    = "100",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "spr_op",
		.lname	= "SPRandom Over provisioning",
		.type	= FIO_OPT_FLOAT_LIST,
		.off1	= offsetof(struct thread_options, spr_over_provisioning),
		.help	= "Over provisioning ratio for SPRandom",
		.parent	= "sprandom",
		.maxlen = 1,
		.hide	= 1,
		.def    = "0.15",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "spr_cs",
		.lname	= "SPRandom Device cache size",
		.type	= FIO_OPT_ULL,
		.off1	= offsetof(struct thread_options, spr_cache_size),
		.help	= "Cache Size in bytes for SPRandom",
		.parent	= "sprandom",
		.maxlen = 1,
		.hide	= 1,
		.def    = "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "random_generator",
		.lname	= "Random Generator",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, random_generator),
		.help	= "Type of random number generator to use",
		.def	= "tausworthe",
		.posval	= {
			  { .ival = "tausworthe",
			    .oval = FIO_RAND_GEN_TAUSWORTHE,
			    .help = "Strong Tausworthe generator",
			  },
			  { .ival = "lfsr",
			    .oval = FIO_RAND_GEN_LFSR,
			    .help = "Variable length LFSR",
			  },
			  {
			    .ival = "tausworthe64",
			    .oval = FIO_RAND_GEN_TAUSWORTHE64,
			    .help = "64-bit Tausworthe variant",
			  },
		},
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "random_distribution",
		.lname	= "Random Distribution",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, random_distribution),
		.cb	= str_random_distribution_cb,
		.help	= "Random offset distribution generator",
		.def	= "random",
		.posval	= {
			  { .ival = "random",
			    .oval = FIO_RAND_DIST_RANDOM,
			    .help = "Completely random",
			  },
			  { .ival = "zipf",
			    .oval = FIO_RAND_DIST_ZIPF,
			    .help = "Zipf distribution",
			  },
			  { .ival = "pareto",
			    .oval = FIO_RAND_DIST_PARETO,
			    .help = "Pareto distribution",
			  },
			  { .ival = "normal",
			    .oval = FIO_RAND_DIST_GAUSS,
			    .help = "Normal (Gaussian) distribution",
			  },
			  { .ival = "zoned",
			    .oval = FIO_RAND_DIST_ZONED,
			    .help = "Zoned random distribution",
			  },
			  { .ival = "zoned_abs",
			    .oval = FIO_RAND_DIST_ZONED_ABS,
			    .help = "Zoned absolute random distribution",
			  },
		},
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "percentage_random",
		.lname	= "Percentage Random",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, perc_rand[DDIR_READ]),
		.off2	= offsetof(struct thread_options, perc_rand[DDIR_WRITE]),
		.off3	= offsetof(struct thread_options, perc_rand[DDIR_TRIM]),
		.maxval	= 100,
		.help	= "Percentage of seq/random mix that should be random",
		.def	= "100,100,100",
		.interval = 5,
		.inverse = "percentage_sequential",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "percentage_sequential",
		.lname	= "Percentage Sequential",
		.type	= FIO_OPT_DEPRECATED,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "nrfiles",
		.lname	= "Number of files",
		.alias	= "nr_files",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, nr_files),
		.help	= "Split job workload between this number of files",
		.def	= "1",
		.interval = 1,
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "openfiles",
		.lname	= "Number of open files",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, open_files),
		.help	= "Number of files to keep open at the same time",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "file_service_type",
		.lname	= "File service type",
		.type	= FIO_OPT_STR,
		.cb	= str_fst_cb,
		.off1	= offsetof(struct thread_options, file_service_type),
		.help	= "How to select which file to service next",
		.def	= "roundrobin",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
		.posval	= {
			  { .ival = "random",
			    .oval = FIO_FSERVICE_RANDOM,
			    .help = "Choose a file at random (uniform)",
			  },
			  { .ival = "zipf",
			    .oval = FIO_FSERVICE_ZIPF,
			    .help = "Zipf randomized",
			  },
			  { .ival = "pareto",
			    .oval = FIO_FSERVICE_PARETO,
			    .help = "Pareto randomized",
			  },
			  { .ival = "normal",
			    .oval = FIO_FSERVICE_GAUSS,
			    .help = "Normal (Gaussian) randomized",
			  },
			  { .ival = "gauss",
			    .oval = FIO_FSERVICE_GAUSS,
			    .help = "Alias for normal",
			  },
			  { .ival = "roundrobin",
			    .oval = FIO_FSERVICE_RR,
			    .help = "Round robin select files",
			  },
			  { .ival = "sequential",
			    .oval = FIO_FSERVICE_SEQ,
			    .help = "Finish one file before moving to the next",
			  },
		},
		.parent = "nrfiles",
		.hide	= 1,
	},
	{
		.name	= "fallocate",
		.lname	= "Fallocate",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, fallocate_mode),
		.help	= "Whether pre-allocation is performed when laying out files",
#ifdef FIO_HAVE_DEFAULT_FALLOCATE
		.def	= "native",
#else
		.def	= "none",
#endif
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
		.posval	= {
			  { .ival = "none",
			    .oval = FIO_FALLOCATE_NONE,
			    .help = "Do not pre-allocate space",
			  },
			  { .ival = "native",
			    .oval = FIO_FALLOCATE_NATIVE,
			    .help = "Use native pre-allocation if possible",
			  },
#ifdef CONFIG_POSIX_FALLOCATE
			  { .ival = "posix",
			    .oval = FIO_FALLOCATE_POSIX,
			    .help = "Use posix_fallocate()",
			  },
#endif
#ifdef CONFIG_LINUX_FALLOCATE
			  { .ival = "keep",
			    .oval = FIO_FALLOCATE_KEEP_SIZE,
			    .help = "Use fallocate(..., FALLOC_FL_KEEP_SIZE, ...)",
			  },
#endif
			  { .ival = "truncate",
			    .oval = FIO_FALLOCATE_TRUNCATE,
			    .help = "Truncate file to final size instead of allocating"
			  },
			  /* Compatibility with former boolean values */
			  { .ival = "0",
			    .oval = FIO_FALLOCATE_NONE,
			    .help = "Alias for 'none'",
			  },
#ifdef CONFIG_POSIX_FALLOCATE
			  { .ival = "1",
			    .oval = FIO_FALLOCATE_POSIX,
			    .help = "Alias for 'posix'",
			  },
#endif
		},
	},
	{
		.name	= "fadvise_hint",
		.lname	= "Fadvise hint",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, fadvise_hint),
		.posval	= {
			  { .ival = "0",
			    .oval = F_ADV_NONE,
			    .help = "Don't issue fadvise/madvise",
			  },
			  { .ival = "1",
			    .oval = F_ADV_TYPE,
			    .help = "Advise using fio IO pattern",
			  },
			  { .ival = "random",
			    .oval = F_ADV_RANDOM,
			    .help = "Advise using FADV_RANDOM",
			  },
			  { .ival = "sequential",
			    .oval = F_ADV_SEQUENTIAL,
			    .help = "Advise using FADV_SEQUENTIAL",
			  },
#ifdef POSIX_FADV_NOREUSE
			  { .ival = "noreuse",
			    .oval = F_ADV_NOREUSE,
			    .help = "Advise using FADV_NOREUSE",
			  },
#endif
		},
		.help	= "Use fadvise() to advise the kernel on IO pattern",
		.def	= "1",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "fsync",
		.lname	= "Fsync",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, fsync_blocks),
		.help	= "Issue fsync for writes every given number of blocks",
		.def	= "0",
		.interval = 1,
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "fdatasync",
		.lname	= "Fdatasync",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, fdatasync_blocks),
		.help	= "Issue fdatasync for writes every given number of blocks",
		.def	= "0",
		.interval = 1,
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "write_barrier",
		.lname	= "Write barrier",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, barrier_blocks),
		.help	= "Make every Nth write a barrier write",
		.def	= "0",
		.interval = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
#ifdef CONFIG_SYNC_FILE_RANGE
	{
		.name	= "sync_file_range",
		.lname	= "Sync file range",
		.posval	= {
			  { .ival = "wait_before",
			    .oval = SYNC_FILE_RANGE_WAIT_BEFORE,
			    .help = "SYNC_FILE_RANGE_WAIT_BEFORE",
			    .orval  = 1,
			  },
			  { .ival = "write",
			    .oval = SYNC_FILE_RANGE_WRITE,
			    .help = "SYNC_FILE_RANGE_WRITE",
			    .orval  = 1,
			  },
			  {
			    .ival = "wait_after",
			    .oval = SYNC_FILE_RANGE_WAIT_AFTER,
			    .help = "SYNC_FILE_RANGE_WAIT_AFTER",
			    .orval  = 1,
			  },
		},
		.type	= FIO_OPT_STR_MULTI,
		.cb	= str_sfr_cb,
		.off1	= offsetof(struct thread_options, sync_file_range),
		.help	= "Use sync_file_range()",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
#else
	{
		.name	= "sync_file_range",
		.lname	= "Sync file range",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support sync_file_range",
	},
#endif
	/* ===== [한국어] 버퍼링/캐시 옵션 ===== */
	{
		.name	= "direct",    /* O_DIRECT 사용 여부: 1이면 커널 페이지 캐시 우회 */
		.lname	= "Direct I/O",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, odirect),
		.help	= "Use O_DIRECT IO (negates buffered)",
		.def	= "0",
		.inverse = "buffered",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_TYPE,
	},
#ifdef FIO_HAVE_RWF_ATOMIC
	{
		.name	= "atomic",
		.lname	= "Atomic I/O",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, oatomic),
		.help	= "Use Atomic IO with O_DIRECT (implies O_DIRECT)",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_TYPE,
	},
#endif
	{
		.name	= "buffered",
		.lname	= "Buffered I/O",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, odirect),
		.neg	= 1,
		.help	= "Use buffered IO (negates direct)",
		.def	= "1",
		.inverse = "direct",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_TYPE,
	},
	{
		.name	= "overwrite",
		.lname	= "Overwrite",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, overwrite),
		.help	= "When writing, set whether to overwrite current data",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "loops",
		.lname	= "Loops",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, loops),
		.help	= "Number of times to run the job",
		.def	= "1",
		.interval = 1,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_RUNTIME,
	},
	/* ===== [한국어] 실행 제어 옵션 ===== */
	{
		.name	= "numjobs",   /* 이 잡의 복제 수 (병렬 워커 수) */
		.lname	= "Number of jobs",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, numjobs),
		.help	= "Duplicate this job this many times",
		.def	= "1",
		.interval = 1,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_RUNTIME,
	},
	{
		.name	= "startdelay",
		.lname	= "Start delay",
		.type	= FIO_OPT_STR_VAL_TIME,
		.off1	= offsetof(struct thread_options, start_delay),
		.off2	= offsetof(struct thread_options, start_delay_high),
		.help	= "Only start job when this period has passed",
		.def	= "0",
		.is_seconds = 1,
		.is_time = 1,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_RUNTIME,
	},
	{
		.name	= "runtime",
		.lname	= "Runtime",
		.alias	= "timeout",
		.type	= FIO_OPT_STR_VAL_TIME,
		.off1	= offsetof(struct thread_options, timeout),
		.help	= "Stop workload when this amount of time has passed",
		.def	= "0",
		.is_seconds = 1,
		.is_time = 1,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_RUNTIME,
	},
	{
		.name	= "time_based",
		.lname	= "Time based",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, time_based),
		.help	= "Keep running until runtime/timeout is met",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_RUNTIME,
	},
	{
		.name	= "verify_only",
		.lname	= "Verify only",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, verify_only),
		.help	= "Verifies previously written data is still valid",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_RUNTIME,
	},
	{
		.name	= "ramp_time",
		.lname	= "Ramp time",
		.type	= FIO_OPT_STR_VAL_TIME,
		.off1	= offsetof(struct thread_options, ramp_time),
		.help	= "Ramp up time before measuring performance",
		.is_seconds = 1,
		.is_time = 1,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_RUNTIME,
	},
	{
		.name	= "ramp_size",
		.lname	= "Ramp size",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, ramp_size),
		.minval = 1,
		.help	= "Amount of data transferred before measuring performance",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_RUNTIME,
	},
	{
		.name	= "clocksource",
		.lname	= "Clock source",
		.type	= FIO_OPT_STR,
		.cb	= fio_clock_source_cb,
		.off1	= offsetof(struct thread_options, clocksource),
		.help	= "What type of timing source to use",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CLOCK,
		.posval	= {
#ifdef CONFIG_GETTIMEOFDAY
			  { .ival = "gettimeofday",
			    .oval = CS_GTOD,
			    .help = "Use gettimeofday(2) for timing",
			  },
#endif
			  { .ival = "clock_gettime",
			    .oval = CS_CGETTIME,
			    .help = "Use clock_gettime(2) for timing",
			  },
#ifdef ARCH_HAVE_CPU_CLOCK
			  { .ival = "cpu",
			    .oval = CS_CPUCLOCK,
			    .help = "Use CPU private clock",
			  },
#endif
		},
	},
	{
		.name	= "mem",
		.alias	= "iomem",
		.lname	= "I/O Memory",
		.type	= FIO_OPT_STR,
		.cb	= str_mem_cb,
		.off1	= offsetof(struct thread_options, mem_type),
		.help	= "Backing type for IO buffers",
		.def	= "malloc",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
		.posval	= {
			  { .ival = "malloc",
			    .oval = MEM_MALLOC,
			    .help = "Use malloc(3) for IO buffers",
			  },
#ifndef CONFIG_NO_SHM
			  { .ival = "shm",
			    .oval = MEM_SHM,
			    .help = "Use shared memory segments for IO buffers",
			  },
#ifdef FIO_HAVE_HUGETLB
			  { .ival = "shmhuge",
			    .oval = MEM_SHMHUGE,
			    .help = "Like shm, but use huge pages",
			  },
#endif
#endif
			  { .ival = "mmap",
			    .oval = MEM_MMAP,
			    .help = "Use mmap(2) (file or anon) for IO buffers",
			  },
			  { .ival = "mmapshared",
			    .oval = MEM_MMAPSHARED,
			    .help = "Like mmap, but use the shared flag",
			  },
#ifdef FIO_HAVE_HUGETLB
			  { .ival = "mmaphuge",
			    .oval = MEM_MMAPHUGE,
			    .help = "Like mmap, but use huge pages",
			  },
#endif
#ifdef CONFIG_CUDA
			  { .ival = "cudamalloc",
			    .oval = MEM_CUDA_MALLOC,
			    .help = "Allocate GPU device memory for GPUDirect RDMA",
			  },
#endif
		  },
	},
	{
		.name	= "iomem_align",
		.alias	= "mem_align",
		.lname	= "I/O memory alignment",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, mem_align),
		.minval	= 0,
		.help	= "IO memory buffer offset alignment",
		.def	= "0",
		.parent	= "iomem",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	/* ===== [한국어] 데이터 검증(Verify) 옵션 ===== */
	{
		.name	= "verify",    /* 검증 알고리즘: md5, crc32, sha256 등 */
		.lname	= "Verify",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, verify),
		.help	= "Verify data written",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
		.posval = {
			  { .ival = "0",
			    .oval = VERIFY_NONE,
			    .help = "Don't do IO verification",
			  },
			  { .ival = "md5",
			    .oval = VERIFY_MD5,
			    .help = "Use md5 checksums for verification",
			  },
			  { .ival = "crc64",
			    .oval = VERIFY_CRC64,
			    .help = "Use crc64 checksums for verification",
			  },
			  { .ival = "crc32",
			    .oval = VERIFY_CRC32,
			    .help = "Use crc32 checksums for verification",
			  },
			  { .ival = "crc32c-intel",
			    .oval = VERIFY_CRC32C,
			    .help = "Use crc32c checksums for verification (hw assisted, if available)",
			  },
			  { .ival = "crc32c",
			    .oval = VERIFY_CRC32C,
			    .help = "Use crc32c checksums for verification (hw assisted, if available)",
			  },
			  { .ival = "crc16",
			    .oval = VERIFY_CRC16,
			    .help = "Use crc16 checksums for verification",
			  },
			  { .ival = "crc7",
			    .oval = VERIFY_CRC7,
			    .help = "Use crc7 checksums for verification",
			  },
			  { .ival = "sha1",
			    .oval = VERIFY_SHA1,
			    .help = "Use sha1 checksums for verification",
			  },
			  { .ival = "sha256",
			    .oval = VERIFY_SHA256,
			    .help = "Use sha256 checksums for verification",
			  },
			  { .ival = "sha512",
			    .oval = VERIFY_SHA512,
			    .help = "Use sha512 checksums for verification",
			  },
			  { .ival = "sha3-224",
			    .oval = VERIFY_SHA3_224,
			    .help = "Use sha3-224 checksums for verification",
			  },
			  { .ival = "sha3-256",
			    .oval = VERIFY_SHA3_256,
			    .help = "Use sha3-256 checksums for verification",
			  },
			  { .ival = "sha3-384",
			    .oval = VERIFY_SHA3_384,
			    .help = "Use sha3-384 checksums for verification",
			  },
			  { .ival = "sha3-512",
			    .oval = VERIFY_SHA3_512,
			    .help = "Use sha3-512 checksums for verification",
			  },
			  { .ival = "xxhash",
			    .oval = VERIFY_XXHASH,
			    .help = "Use xxhash checksums for verification",
			  },
			  /* Meta information was included into verify_header,
			   * 'meta' verification is implied by default. */
			  { .ival = "meta",
			    .oval = VERIFY_HDR_ONLY,
			    .help = "Use io information for verification. "
				    "Now is implied by default, thus option is obsolete, "
				    "don't use it",
			  },
			  { .ival = "pattern",
			    .oval = VERIFY_PATTERN_NO_HDR,
			    .help = "Verify strict pattern",
			  },
			  { .ival = "pattern_hdr",
			    .oval = VERIFY_PATTERN,
			    .help = "Verify pattern with header",
			  },
			  {
			    .ival = "null",
			    .oval = VERIFY_NULL,
			    .help = "Pretend to verify",
			  },
		},
	},
	{
		.name	= "do_verify",
		.lname	= "Perform verify step",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, do_verify),
		.help	= "Run verification stage after write",
		.def	= "1",
		.parent = "verify",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verifysort",
		.lname	= "Verify sort",
		.type	= FIO_OPT_SOFT_DEPRECATED,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verifysort_nr",
		.lname	= "Verify Sort Nr",
		.type	= FIO_OPT_SOFT_DEPRECATED,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name   = "verify_interval",
		.lname	= "Verify interval",
		.type   = FIO_OPT_INT,
		.off1   = offsetof(struct thread_options, verify_interval),
		.minval	= 2 * sizeof(struct verify_header),
		.help   = "Store verify buffer header every N bytes",
		.parent	= "verify",
		.hide	= 1,
		.interval = 2 * sizeof(struct verify_header),
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_offset",
		.lname	= "Verify offset",
		.type	= FIO_OPT_INT,
		.help	= "Offset verify header location by N bytes",
		.off1	= offsetof(struct thread_options, verify_offset),
		.minval	= sizeof(struct verify_header),
		.parent	= "verify",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_pattern",
		.lname	= "Verify pattern",
		.type	= FIO_OPT_STR,
		.cb	= str_verify_pattern_cb,
		.off1	= offsetof(struct thread_options, verify_pattern),
		.help	= "Fill pattern for IO buffers",
		.parent	= "verify",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_pattern_interval",
		.lname	= "Running verify pattern",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, verify_pattern_interval),
		.def	= "0",
		.help	= "Re-create verify pattern every N bytes",
		.parent = "verify",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_fatal",
		.lname	= "Verify fatal",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, verify_fatal),
		.def	= "0",
		.help	= "Exit on a single verify failure, don't continue",
		.parent = "verify",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_dump",
		.lname	= "Verify dump",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, verify_dump),
		.def	= "0",
		.help	= "Dump contents of good and bad blocks on failure",
		.parent = "verify",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_async",
		.lname	= "Verify asynchronously",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, verify_async),
		.def	= "0",
		.help	= "Number of async verifier threads to use",
		.parent	= "verify",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_backlog",
		.lname	= "Verify backlog",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, verify_backlog),
		.help	= "Verify after this number of blocks are written",
		.parent	= "verify",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_backlog_batch",
		.lname	= "Verify backlog batch",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, verify_batch),
		.help	= "Verify this number of IO blocks",
		.parent	= "verify",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
#ifdef FIO_HAVE_CPU_AFFINITY
	{
		.name	= "verify_async_cpus",
		.lname	= "Async verify CPUs",
		.type	= FIO_OPT_STR,
		.cb	= str_verify_cpus_allowed_cb,
		.off1	= offsetof(struct thread_options, verify_cpumask),
		.help	= "Set CPUs allowed for async verify threads",
		.parent	= "verify_async",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
#else
	{
		.name	= "verify_async_cpus",
		.lname	= "Async verify CPUs",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support CPU affinities",
	},
#endif
	{
		.name	= "experimental_verify",
		.lname	= "Experimental Verify",
		.off1	= offsetof(struct thread_options, experimental_verify),
		.type	= FIO_OPT_BOOL,
		.help	= "Enable experimental verification",
		.parent	= "verify",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_state_load",
		.lname	= "Load verify state",
		.off1	= offsetof(struct thread_options, verify_state),
		.type	= FIO_OPT_BOOL,
		.help	= "Load verify termination state",
		.parent	= "verify",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_state_save",
		.lname	= "Save verify state",
		.off1	= offsetof(struct thread_options, verify_state_save),
		.type	= FIO_OPT_BOOL,
		.def	= "1",
		.help	= "Save verify state on termination",
		.parent	= "verify",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_write_sequence",
		.lname	= "Verify write sequence number",
		.off1	= offsetof(struct thread_options, verify_write_sequence),
		.type	= FIO_OPT_BOOL,
		.def	= "1",
		.help	= "Verify header write sequence number",
		.parent	= "verify",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "verify_header_seed",
		.lname	= "Verify header seed",
		.off1	= offsetof(struct thread_options, verify_header_seed),
		.type	= FIO_OPT_BOOL,
		.def	= "1",
		.help	= "Verify the header seed used to generate the buffer contents",
		.parent	= "verify",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_VERIFY,
	},
#ifdef FIO_HAVE_TRIM
	{
		.name	= "trim_percentage",
		.lname	= "Trim percentage",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, trim_percentage),
		.minval = 0,
		.maxval = 100,
		.help	= "Number of verify blocks to trim (i.e., discard)",
		.parent	= "verify",
		.def	= "0",
		.interval = 1,
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_TRIM,
	},
	{
		.name	= "trim_verify_zero",
		.lname	= "Verify trim zero",
		.type	= FIO_OPT_BOOL,
		.help	= "Verify that trimmed (i.e., discarded) blocks are returned as zeroes",
		.off1	= offsetof(struct thread_options, trim_zero),
		.parent	= "trim_percentage",
		.hide	= 1,
		.def	= "1",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_TRIM,
	},
	{
		.name	= "trim_backlog",
		.lname	= "Trim backlog",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, trim_backlog),
		.help	= "Trim after this number of blocks are written",
		.parent	= "trim_percentage",
		.hide	= 1,
		.interval = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_TRIM,
	},
	{
		.name	= "trim_backlog_batch",
		.lname	= "Trim backlog batch",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, trim_batch),
		.help	= "Trim this number of IO blocks",
		.parent	= "trim_percentage",
		.hide	= 1,
		.interval = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_TRIM,
	},
#else
	{
		.name	= "trim_percentage",
		.lname	= "Trim percentage",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Fio does not support TRIM on your platform",
	},
	{
		.name	= "trim_verify_zero",
		.lname	= "Verify trim zero",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Fio does not support TRIM on your platform",
	},
	{
		.name	= "trim_backlog",
		.lname	= "Trim backlog",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Fio does not support TRIM on your platform",
	},
	{
		.name	= "trim_backlog_batch",
		.lname	= "Trim backlog batch",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Fio does not support TRIM on your platform",
	},
#endif
	{
		.name	= "write_iolog",
		.lname	= "Write I/O log",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, write_iolog_file),
		.help	= "Store IO pattern to file",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "read_iolog",
		.lname	= "Read I/O log",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, read_iolog_file),
		.help	= "Playback IO pattern from file",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "read_iolog_chunked",
		.lname	= "Read I/O log in parts",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, read_iolog_chunked),
		.def	= "0",
		.parent	= "read_iolog",
		.help	= "Parse IO pattern in chunks",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "replay_no_stall",
		.lname	= "Don't stall on replay",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, no_stall),
		.def	= "0",
		.parent	= "read_iolog",
		.hide	= 1,
		.help	= "Playback IO pattern file as fast as possible without stalls",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "replay_redirect",
		.lname	= "Redirect device for replay",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, replay_redirect),
		.parent	= "read_iolog",
		.hide	= 1,
		.help	= "Replay all I/O onto this device, regardless of trace device",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "replay_scale",
		.lname	= "Replace offset scale factor",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, replay_scale),
		.parent	= "read_iolog",
		.def	= "1",
		.help	= "Align offsets to this blocksize",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "replay_align",
		.lname	= "Replace alignment",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, replay_align),
		.parent	= "read_iolog",
		.help	= "Scale offset down by this factor",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
		.pow2	= 1,
	},
	{
		.name	= "replay_time_scale",
		.lname	= "Replay Time Scale",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, replay_time_scale),
		.def	= "100",
		.minval	= 1,
		.parent	= "read_iolog",
		.hide	= 1,
		.help	= "Scale time for replay events",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "replay_skip",
		.lname	= "Replay Skip",
		.type	= FIO_OPT_STR,
		.cb	= str_replay_skip_cb,
		.off1	= offsetof(struct thread_options, replay_skip),
		.parent	= "read_iolog",
		.help	= "Skip certain IO types (read,write,trim,flush)",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "merge_blktrace_file",
		.lname	= "Merged blktrace output filename",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, merge_blktrace_file),
		.help	= "Merged blktrace output filename",
		.category = FIO_OPT_C_IO,
		.group = FIO_OPT_G_IOLOG,
	},
	{
		.name	= "merge_blktrace_scalars",
		.lname	= "Percentage to scale each trace",
		.type	= FIO_OPT_FLOAT_LIST,
		.off1	= offsetof(struct thread_options, merge_blktrace_scalars),
		.maxlen	= FIO_IO_U_LIST_MAX_LEN,
		.help	= "Percentage to scale each trace",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "merge_blktrace_iters",
		.lname	= "Number of iterations to run per trace",
		.type	= FIO_OPT_FLOAT_LIST,
		.off1	= offsetof(struct thread_options, merge_blktrace_iters),
		.maxlen	= FIO_IO_U_LIST_MAX_LEN,
		.help	= "Number of iterations to run per trace",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "exec_prerun",
		.lname	= "Pre-execute runnable",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, exec_prerun),
		.help	= "Execute this file prior to running job",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "exec_postrun",
		.lname	= "Post-execute runnable",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, exec_postrun),
		.help	= "Execute this file after running job",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
	},
#ifdef FIO_HAVE_IOSCHED_SWITCH
	{
		.name	= "ioscheduler",
		.lname	= "I/O scheduler",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, ioscheduler),
		.help	= "Use this IO scheduler on the backing device",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
#else
	{
		.name	= "ioscheduler",
		.lname	= "I/O scheduler",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support IO scheduler switching",
	},
#endif
	/* ===== [한국어] 존(Zone) 관련 옵션 ===== */
	{
		.name	= "zonemode",  /* 존 모드: none, strided, zbd */
		.lname	= "Zone mode",
		.help	= "Mode for the zonesize, zonerange and zoneskip parameters",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, zone_mode),
		.def	= "none",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_ZONE,
		.posval	= {
			   { .ival = "none",
			     .oval = ZONE_MODE_NONE,
			     .help = "no zoning",
			   },
			   { .ival = "strided",
			     .oval = ZONE_MODE_STRIDED,
			     .help = "strided mode - random I/O is restricted to a single zone",
			   },
			   { .ival = "zbd",
			     .oval = ZONE_MODE_ZBD,
			     .help = "zoned block device mode - random I/O selects one of multiple zones randomly",
			   },
		},
	},
	{
		.name	= "zonesize",
		.lname	= "Zone size",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, zone_size),
		.help	= "Amount of data to read per zone",
		.def	= "0",
		.interval = 1024 * 1024,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_ZONE,
	},
	{
		.name	= "zonecapacity",
		.lname	= "Zone capacity",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, zone_capacity),
		.help	= "Capacity per zone",
		.def	= "0",
		.interval = 1024 * 1024,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_ZONE,
	},
	{
		.name	= "zonerange",
		.lname	= "Zone range",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, zone_range),
		.help	= "Give size of an IO zone",
		.def	= "0",
		.interval = 1024 * 1024,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_ZONE,
	},
	{
		.name	= "zoneskip",
		.lname	= "Zone skip",
		.type	= FIO_OPT_STR_VAL_ZONE,
		.cb	= str_zoneskip_cb,
		.off1	= offsetof(struct thread_options, zone_skip),
		.help	= "Space between IO zones",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_ZONE,
	},
	{
		.name	= "read_beyond_wp",
		.lname	= "Allow reads beyond the zone write pointer",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, read_beyond_wp),
		.help	= "Allow reads beyond the zone write pointer",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "max_open_zones",
		.lname	= "Per device/file maximum number of open zones",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, max_open_zones),
		.maxval	= ZBD_MAX_WRITE_ZONES,
		.help	= "Limit on the number of simultaneously opened sequential write zones with zonemode=zbd",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "job_max_open_zones",
		.lname	= "Job maximum number of open zones",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, job_max_open_zones),
		.maxval	= ZBD_MAX_WRITE_ZONES,
		.help	= "Limit on the number of simultaneously opened sequential write zones with zonemode=zbd by one thread/process",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "ignore_zone_limits",
		.lname	= "Ignore zone resource limits",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, ignore_zone_limits),
		.def	= "0",
		.help	= "Ignore the zone resource limits (max open/active zones) reported by the device",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "zone_reset_threshold",
		.lname	= "Zone reset threshold",
		.help	= "Zoned block device reset threshold",
		.type	= FIO_OPT_FLOAT_LIST,
		.maxlen	= 1,
		.off1	= offsetof(struct thread_options, zrt),
		.minfp	= 0,
		.maxfp	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_ZONE,
	},
	{
		.name	= "zone_reset_frequency",
		.lname	= "Zone reset frequency",
		.help	= "Zoned block device zone reset frequency in HZ",
		.type	= FIO_OPT_FLOAT_LIST,
		.maxlen	= 1,
		.off1	= offsetof(struct thread_options, zrf),
		.minfp	= 0,
		.maxfp	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_ZONE,
	},
	{
		.name	= "recover_zbd_write_error",
		.lname	= "Recover write errors when zonemode=zbd is set",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, recover_zbd_write_error),
		.def	= 0,
		.help	= "Continue writes for sequential write required zones after recovering write errors with care for partial write pointer move",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_ZONE,
	},
	{
		.name	= "write_zone_remainder",
		.lname	= "Fill remainders of zones by write instead of zone finish operion",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, write_zone_remainder),
		.def	= 0,
		.help	= "When block size is unaligned, zones have small remainder write areas at ends. Fill them by write instead of zone finish operations for better performance.",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_ZONE,
	},
	/* ===== [한국어] FDP(Flexible Data Placement) 옵션 ===== */
	{
		.name   = "fdp",       /* FDP 활성화 (NVMe 데이터 배치 기능) */
		.lname  = "Flexible data placement",
		.type   = FIO_OPT_BOOL,
		.off1   = offsetof(struct thread_options, fdp),
		.help   = "Use Data placement directive (FDP)",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group  = FIO_OPT_G_INVALID,
	},
	{
		.name	= "dataplacement",
		.alias	= "data_placement",
		.lname	= "Data Placement interface",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, dp_type),
		.help	= "Data Placement interface to use",
		.def	= "none",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
		.posval	= {
			  { .ival = "none",
			    .oval = FIO_DP_NONE,
			    .help = "Do not specify a data placement interface",
			  },
			  { .ival = "fdp",
			    .oval = FIO_DP_FDP,
			    .help = "Use Flexible Data Placement interface",
			  },
			  { .ival = "streams",
			    .oval = FIO_DP_STREAMS,
			    .help = "Use Streams interface",
			  },
		},
	},
	{
		.name	= "plid_select",
		.alias	= "fdp_pli_select",
		.lname	= "Data Placement ID selection strategy",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, dp_id_select),
		.help	= "Strategy for selecting next Data Placement ID",
		.def	= "roundrobin",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
		.posval	= {
			  { .ival = "random",
			    .oval = FIO_DP_RANDOM,
			    .help = "Choose a Placement ID at random (uniform)",
			  },
			  { .ival = "roundrobin",
			    .oval = FIO_DP_RR,
			    .help = "Round robin select Placement IDs",
			  },
			  { .ival = "scheme",
			    .oval = FIO_DP_SCHEME,
			    .help = "Use a scheme(based on LBA) to select Placement IDs",
			  },
		},
	},
	{
		.name	= "plids",
		.alias	= "fdp_pli",
		.lname	= "Stream IDs/Data Placement ID indices",
		.type	= FIO_OPT_STR,
		.cb	= str_fdp_pli_cb,
		.off1	= offsetof(struct thread_options, dp_ids),
		.help	= "Sets which Data Placement ids to use (defaults to all for FDP)",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "dp_scheme",
		.lname	= "Data Placement Scheme",
		.type	= FIO_OPT_STR_STORE,
		.cb	= str_dp_scheme_cb,
		.off1	= offsetof(struct thread_options, dp_scheme_file),
		.maxlen	= PATH_MAX,
		.help	= "scheme file that specifies offset-RUH mapping",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "lockmem",
		.lname	= "Lock memory",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct thread_options, lockmem),
		.help	= "Lock down this amount of memory (per worker)",
		.def	= "0",
		.interval = 1024 * 1024,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "rwmixread",
		.lname	= "Read/write mix read",
		.type	= FIO_OPT_INT,
		.cb	= str_rwmix_read_cb,
		.off1	= offsetof(struct thread_options, rwmix[DDIR_READ]),
		.maxval	= 100,
		.help	= "Percentage of mixed workload that is reads",
		.def	= "50",
		.interval = 5,
		.inverse = "rwmixwrite",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RWMIX,
	},
	{
		.name	= "rwmixwrite",
		.lname	= "Read/write mix write",
		.type	= FIO_OPT_INT,
		.cb	= str_rwmix_write_cb,
		.off1	= offsetof(struct thread_options, rwmix[DDIR_WRITE]),
		.maxval	= 100,
		.help	= "Percentage of mixed workload that is writes",
		.def	= "50",
		.interval = 5,
		.inverse = "rwmixread",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RWMIX,
	},
	{
		.name	= "rwmixcycle",
		.lname	= "Read/write mix cycle",
		.type	= FIO_OPT_DEPRECATED,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RWMIX,
	},
#ifdef CONFIG_LINUX
	{
		.name	= "comm",
		.lname	= "Job process comm",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, comm),
		.help	= "Process comm of this job",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_DESC,
	},
#endif
	{
		.name	= "nice",
		.lname	= "Nice",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, nice),
		.help	= "Set job CPU nice value",
		.minval	= -20,
		.maxval	= 19,
		.def	= "0",
		.interval = 1,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CRED,
	},
#ifdef FIO_HAVE_IOPRIO
	{
		.name	= "prio",
		.lname	= "I/O nice priority",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, ioprio),
		.help	= "Set job IO priority value",
		.minval	= IOPRIO_MIN_PRIO,
		.maxval	= IOPRIO_MAX_PRIO,
		.interval = 1,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CRED,
	},
#else
	{
		.name	= "prio",
		.lname	= "I/O nice priority",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support IO priorities",
	},
#endif
#ifdef FIO_HAVE_IOPRIO_CLASS
#ifndef FIO_HAVE_IOPRIO
#error "FIO_HAVE_IOPRIO_CLASS requires FIO_HAVE_IOPRIO"
#endif
	{
		.name	= "prioclass",
		.lname	= "I/O nice priority class",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, ioprio_class),
		.help	= "Set job IO priority class",
		.minval	= IOPRIO_MIN_PRIO_CLASS,
		.maxval	= IOPRIO_MAX_PRIO_CLASS,
		.interval = 1,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CRED,
	},
	{
		.name	= "priohint",
		.lname	= "I/O nice priority hint",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, ioprio_hint),
		.help	= "Set job IO priority hint",
		.minval	= IOPRIO_MIN_PRIO_HINT,
		.maxval	= IOPRIO_MAX_PRIO_HINT,
		.interval = 1,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CRED,
	},
#else
	{
		.name	= "prioclass",
		.lname	= "I/O nice priority class",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support IO priority classes",
	},
	{
		.name	= "priohint",
		.lname	= "I/O nice priority hint",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support IO priority hints",
	},
#endif
	{
		.name	= "thinktime",
		.lname	= "Thinktime",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, thinktime),
		.help	= "Idle time between IO buffers (usec)",
		.def	= "0",
		.is_time = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_THINKTIME,
	},
	{
		.name	= "thinktime_spin",
		.lname	= "Thinktime spin",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, thinktime_spin),
		.help	= "Start think time by spinning this amount (usec)",
		.def	= "0",
		.is_time = 1,
		.parent	= "thinktime",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_THINKTIME,
	},
	{
		.name	= "thinkcycles",
		.lname	= "Think cycles",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, thinkcycles),
		.help	= "Spin for a constant amount of cycles between requests",
		.def	= "0",
		.parent	= "thinktime",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_THINKTIME,
	},
	{
		.name	= "thinktime_blocks",
		.lname	= "Thinktime blocks",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, thinktime_blocks),
		.help	= "IO buffer period between 'thinktime'",
		.def	= "1",
		.parent	= "thinktime",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_THINKTIME,
	},
	{
		.name	= "thinktime_blocks_type",
		.lname	= "Thinktime blocks type",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, thinktime_blocks_type),
		.help	= "How thinktime_blocks takes effect",
		.def	= "complete",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_THINKTIME,
		.posval = {
			  { .ival = "complete",
			    .oval = THINKTIME_BLOCKS_TYPE_COMPLETE,
			    .help = "thinktime_blocks takes effect at the completion side",
			  },
			  {
			    .ival = "issue",
			    .oval = THINKTIME_BLOCKS_TYPE_ISSUE,
			    .help = "thinktime_blocks takes effect at the issue side",
			  },
		},
		.parent = "thinktime",
	},
	{
		.name	= "thinktime_iotime",
		.lname	= "Thinktime interval",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, thinktime_iotime),
		.help	= "IO time interval between 'thinktime'",
		.def	= "0",
		.parent	= "thinktime",
		.hide	= 1,
		.is_seconds = 1,
		.is_time = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_THINKTIME,
	},
	/* ===== [한국어] 속도 제한(Rate Limiting) 옵션 ===== */
	{
		.name	= "rate",      /* I/O 속도 상한 (바이트/초) */
		.lname	= "I/O rate",
		.type	= FIO_OPT_ULL,
		.off1	= offsetof(struct thread_options, rate[DDIR_READ]),
		.off2	= offsetof(struct thread_options, rate[DDIR_WRITE]),
		.off3	= offsetof(struct thread_options, rate[DDIR_TRIM]),
		.help	= "Set bandwidth rate",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RATE,
	},
	{
		.name	= "rate_min",
		.alias	= "ratemin",
		.lname	= "I/O min rate",
		.type	= FIO_OPT_ULL,
		.off1	= offsetof(struct thread_options, ratemin[DDIR_READ]),
		.off2	= offsetof(struct thread_options, ratemin[DDIR_WRITE]),
		.off3	= offsetof(struct thread_options, ratemin[DDIR_TRIM]),
		.help	= "Job must meet this rate or it will be shutdown",
		.parent	= "rate",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RATE,
	},
	{
		.name	= "rate_iops",
		.lname	= "I/O rate IOPS",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, rate_iops[DDIR_READ]),
		.off2	= offsetof(struct thread_options, rate_iops[DDIR_WRITE]),
		.off3	= offsetof(struct thread_options, rate_iops[DDIR_TRIM]),
		.help	= "Limit IO used to this number of IO operations/sec",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RATE,
	},
	{
		.name	= "rate_iops_min",
		.lname	= "I/O min rate IOPS",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, rate_iops_min[DDIR_READ]),
		.off2	= offsetof(struct thread_options, rate_iops_min[DDIR_WRITE]),
		.off3	= offsetof(struct thread_options, rate_iops_min[DDIR_TRIM]),
		.help	= "Job must meet this rate or it will be shut down",
		.parent	= "rate_iops",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RATE,
	},
	{
		.name	= "rate_process",
		.lname	= "Rate Process",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, rate_process),
		.help	= "What process controls how rated IO is managed",
		.def	= "linear",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RATE,
		.posval = {
			  { .ival = "linear",
			    .oval = RATE_PROCESS_LINEAR,
			    .help = "Linear rate of IO",
			  },
			  {
			    .ival = "poisson",
			    .oval = RATE_PROCESS_POISSON,
			    .help = "Rate follows Poisson process",
			  },
		},
		.parent = "rate",
	},
	{
		.name	= "rate_cycle",
		.alias	= "ratecycle",
		.lname	= "I/O rate cycle",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, ratecycle),
		.help	= "Window average for rate limits (msec)",
		.def	= "1000",
		.parent = "rate",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RATE,
	},
	{
		.name	= "rate_ignore_thinktime",
		.lname	= "Rate ignore thinktime",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, rate_ign_think),
		.help	= "Rated IO ignores thinktime settings",
		.parent = "rate",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_RATE,
	},
	{
		.name	= "max_latency",
		.lname	= "Max Latency (usec)",
		.type	= FIO_OPT_ULL,
		.off1	= offsetof(struct thread_options, max_latency[DDIR_READ]),
		.off2	= offsetof(struct thread_options, max_latency[DDIR_WRITE]),
		.off3	= offsetof(struct thread_options, max_latency[DDIR_TRIM]),
		.help	= "Maximum tolerated IO latency (usec)",
		.is_time = 1,
		.category = FIO_OPT_C_IO,
		.group = FIO_OPT_G_LATPROF,
	},
	/* ===== [한국어] 레이턴시 목표 옵션 ===== */
	{
		.name	= "latency_target",  /* 목표 레이턴시 (us): 이를 만족하는 최대 큐 깊이를 자동 탐색 */
		.lname	= "Latency Target (usec)",
		.type	= FIO_OPT_STR_VAL_TIME,
		.off1	= offsetof(struct thread_options, latency_target),
		.help	= "Ramp to max queue depth supporting this latency",
		.is_time = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_LATPROF,
	},
	{
		.name	= "latency_window",
		.lname	= "Latency Window (usec)",
		.type	= FIO_OPT_STR_VAL_TIME,
		.off1	= offsetof(struct thread_options, latency_window),
		.help	= "Time to sustain latency_target",
		.is_time = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_LATPROF,
	},
	{
		.name	= "latency_percentile",
		.lname	= "Latency Percentile",
		.type	= FIO_OPT_FLOAT_LIST,
		.off1	= offsetof(struct thread_options, latency_percentile),
		.help	= "Percentile of IOs must be below latency_target",
		.def	= "100",
		.maxlen	= 1,
		.minfp	= 0.0,
		.maxfp	= 100.0,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_LATPROF,
	},
	{
		.name	= "latency_run",
		.lname	= "Latency Run",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, latency_run),
		.help	= "Keep adjusting queue depth to match latency_target",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_LATPROF,
	},
	{
		.name	= "invalidate",
		.lname	= "Cache invalidate",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, invalidate_cache),
		.help	= "Invalidate buffer/page cache prior to running job",
		.def	= "1",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_TYPE,
	},
	{
		.name	= "sync",
		.lname	= "Synchronous I/O",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, sync_io),
		.help	= "Use synchronous write IO",
		.def	= "none",
		.hide	= 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_TYPE,
		.posval = {
			  { .ival = "none",
			    .oval = 0,
			  },
			  { .ival = "0",
			    .oval = 0,
			  },
			  { .ival = "sync",
			    .oval = O_SYNC,
			  },
			  { .ival = "1",
			    .oval = O_SYNC,
			  },
#ifdef O_DSYNC
			  { .ival = "dsync",
			    .oval = O_DSYNC,
			  },
#endif
		},
	},
#ifdef FIO_HAVE_WRITE_HINT
	{
		.name	= "write_hint",
		.lname	= "Write hint",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, write_hint),
		.help	= "Set expected write life time",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
		.posval = {
			  { .ival = "none",
			    .oval = RWH_WRITE_LIFE_NONE,
			  },
			  { .ival = "short",
			    .oval = RWH_WRITE_LIFE_SHORT,
			  },
			  { .ival = "medium",
			    .oval = RWH_WRITE_LIFE_MEDIUM,
			  },
			  { .ival = "long",
			    .oval = RWH_WRITE_LIFE_LONG,
			  },
			  { .ival = "extreme",
			    .oval = RWH_WRITE_LIFE_EXTREME,
			  },
		},
	},
#endif
	{
		.name	= "create_serialize",
		.lname	= "Create serialize",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, create_serialize),
		.help	= "Serialize creation of job files",
		.def	= "1",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "create_fsync",
		.lname	= "Create fsync",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, create_fsync),
		.help	= "fsync file after creation",
		.def	= "1",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "create_on_open",
		.lname	= "Create on open",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, create_on_open),
		.help	= "Create files when they are opened for IO",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "create_only",
		.lname	= "Create Only",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, create_only),
		.help	= "Only perform file creation phase",
		.category = FIO_OPT_C_FILE,
		.def	= "0",
	},
	{
		.name	= "allow_file_create",
		.lname	= "Allow file create",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, allow_create),
		.help	= "Permit fio to create files, if they don't exist",
		.def	= "1",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_FILENAME,
	},
	{
		.name	= "allow_mounted_write",
		.lname	= "Allow mounted write",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, allow_mounted_write),
		.help	= "Allow writes to a mounted partition",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_FILENAME,
	},
	{
		.name	= "pre_read",
		.lname	= "Pre-read files",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, pre_read),
		.help	= "Pre-read files before starting official testing",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	/* ===== [한국어] CPU 친화성 및 NUMA 옵션 ===== */
#ifdef FIO_HAVE_CPU_AFFINITY
	{
		.name	= "cpumask",   /* CPU 친화성 비트마스크 */
		.lname	= "CPU mask",
		.type	= FIO_OPT_INT,
		.cb	= str_cpumask_cb,
		.off1	= offsetof(struct thread_options, cpumask),
		.help	= "CPU affinity mask",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CRED,
	},
	{
		.name	= "cpus_allowed",
		.lname	= "CPUs allowed",
		.type	= FIO_OPT_STR,
		.cb	= str_cpus_allowed_cb,
		.off1	= offsetof(struct thread_options, cpumask),
		.help	= "Set CPUs allowed",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CRED,
	},
	{
		.name	= "cpus_allowed_policy",
		.lname	= "CPUs allowed distribution policy",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, cpus_allowed_policy),
		.help	= "Distribution policy for cpus_allowed",
		.parent = "cpus_allowed",
		.prio	= 1,
		.posval = {
			  { .ival = "shared",
			    .oval = FIO_CPUS_SHARED,
			    .help = "Mask shared between threads",
			  },
			  { .ival = "split",
			    .oval = FIO_CPUS_SPLIT,
			    .help = "Mask split between threads",
			  },
		},
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CRED,
	},
#else
	{
		.name	= "cpumask",
		.lname	= "CPU mask",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support CPU affinities",
	},
	{
		.name	= "cpus_allowed",
		.lname	= "CPUs allowed",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support CPU affinities",
	},
	{
		.name	= "cpus_allowed_policy",
		.lname	= "CPUs allowed distribution policy",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support CPU affinities",
	},
#endif
#ifdef CONFIG_LIBNUMA
	{
		.name	= "numa_cpu_nodes",
		.lname	= "NUMA CPU Nodes",
		.type	= FIO_OPT_STR,
		.cb	= str_numa_cpunodes_cb,
		.off1	= offsetof(struct thread_options, numa_cpunodes),
		.help	= "NUMA CPU nodes bind",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "numa_mem_policy",
		.lname	= "NUMA Memory Policy",
		.type	= FIO_OPT_STR,
		.cb	= str_numa_mpol_cb,
		.off1	= offsetof(struct thread_options, numa_memnodes),
		.help	= "NUMA memory policy setup",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
	},
#else
	{
		.name	= "numa_cpu_nodes",
		.lname	= "NUMA CPU Nodes",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Build fio with libnuma-dev(el) to enable this option",
	},
	{
		.name	= "numa_mem_policy",
		.lname	= "NUMA Memory Policy",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Build fio with libnuma-dev(el) to enable this option",
	},
#endif
#ifdef CONFIG_CUDA
	{
		.name	= "gpu_dev_id",
		.lname	= "GPU device ID",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, gpu_dev_id),
		.help	= "Set GPU device ID for GPUDirect RDMA",
		.def    = "0",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
	},
#endif
	{
		.name	= "end_fsync",
		.lname	= "End fsync",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, end_fsync),
		.help	= "Include fsync at the end of job",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "fsync_on_close",
		.lname	= "Fsync on close",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, fsync_on_close),
		.help	= "fsync files on close",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
#ifdef CONFIG_SYNCFS
	{
		.name	= "end_syncfs",
		.lname	= "End sync FS",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, end_syncfs),
		.help	= "Include sync of FS at the end of job",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
#else
	{
		.name	= "end_syncfs",
		.lname	= "End sync FS",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support syncfs",
	},
#endif
	{
		.name	= "unlink",
		.lname	= "Unlink file",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, unlink),
		.help	= "Unlink created files after job has completed",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "unlink_each_loop",
		.lname	= "Unlink file after each loop of a job",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, unlink_each_loop),
		.help	= "Unlink created files after each loop in a job has completed",
		.def	= "0",
		.category = FIO_OPT_C_FILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "exitall",
		.lname	= "Exit-all on terminate",
		.type	= FIO_OPT_STR_SET,
		.cb	= str_exitall_cb,
		.help	= "Terminate all jobs when one exits",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_PROCESS,
	},
	{
		.name	= "exit_what",
		.lname	= "What jobs to quit on terminate",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, exit_what),
		.help	= "Fine-grained control for exitall",
		.def	= "group",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_PROCESS,
		.posval	= {
			  { .ival = "group",
			    .oval = TERMINATE_GROUP,
			    .help = "exit_all=1 default behaviour",
			  },
			  { .ival = "stonewall",
			    .oval = TERMINATE_STONEWALL,
			    .help = "quit all currently running jobs; continue with next stonewall",
			  },
			  { .ival = "all",
			    .oval = TERMINATE_ALL,
			    .help = "Quit everything",
			  },
		},
	},
	{
		.name	= "exitall_on_error",
		.lname	= "Exit-all on terminate in error",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, exitall_error),
		.help	= "Terminate all jobs when one exits in error",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_PROCESS,
	},
	{
		.name	= "stonewall",
		.lname	= "Wait for previous",
		.alias	= "wait_for_previous",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, stonewall),
		.help	= "Insert a hard barrier between this job and previous",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_PROCESS,
	},
	{
		.name	= "new_group",
		.lname	= "New group",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, new_group),
		.help	= "Mark the start of a new group (for reporting)",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_PROCESS,
	},
	{
		.name	= "thread",
		.lname	= "Thread",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, use_thread),
		.help	= "Use threads instead of processes",
#ifdef CONFIG_NO_SHM
		.def	= "1",
		.no_warn_def = 1,
#endif
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_PROCESS,
	},
	{
		.name	= "per_job_logs",
		.lname	= "Per Job Logs",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, per_job_logs),
		.help	= "Include job number in generated log files or not",
		.def	= "1",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	/* ===== [한국어] 로깅 및 통계 옵션 ===== */
	{
		.name	= "write_bw_log",  /* 대역폭 로그 파일 기록 */
		.lname	= "Write bandwidth log",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, bw_log_file),
		.cb	= str_write_bw_log_cb,
		.help	= "Write log of bandwidth during run",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "write_lat_log",
		.lname	= "Write latency log",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, lat_log_file),
		.cb	= str_write_lat_log_cb,
		.help	= "Write log of latency during run",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "write_iops_log",
		.lname	= "Write IOPS log",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, iops_log_file),
		.cb	= str_write_iops_log_cb,
		.help	= "Write log of IOPS during run",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "log_entries",
		.lname	= "Log entries",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, log_entries),
		.help	= "Initial number of entries in a job IO log",
		.def	= __fio_stringify(DEF_LOG_ENTRIES),
		.minval	= DEF_LOG_ENTRIES,
		.maxval	= MAX_LOG_ENTRIES,
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "log_avg_msec",
		.lname	= "Log averaging (msec)",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, log_avg_msec),
		.help	= "Average bw/iops/lat logs over this period of time",
		.def	= "0",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "log_hist_msec",
		.lname	= "Log histograms (msec)",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, log_hist_msec),
		.help	= "Dump completion latency histograms at frequency of this time value",
		.def	= "0",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "log_hist_coarseness",
		.lname	= "Histogram logs coarseness",
		.type	= FIO_OPT_INT,
		.maxval = 6,
		.minval = 0,
		.off1	= offsetof(struct thread_options, log_hist_coarseness),
		.help	= "Integer in range [0,6]. Higher coarseness outputs"
			" fewer histogram bins per sample. The number of bins for"
			" these are [1216, 608, 304, 152, 76, 38, 19] respectively.",
		.def	= "0",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "write_hist_log",
		.lname	= "Write latency histogram logs",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, hist_log_file),
		.cb	= str_write_hist_log_cb,
		.help	= "Write log of latency histograms during run",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "log_window_value",
		.alias  = "log_max_value",
		.lname	= "Log maximum, average or both values",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, log_max),
		.help	= "Log max, average or both sample in a window",
		.def	= "avg",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
		.posval	= {
			  { .ival = "avg",
			    .oval = IO_LOG_SAMPLE_AVG,
			    .help = "Log average value over the window",
			  },
			  { .ival = "max",
			    .oval = IO_LOG_SAMPLE_MAX,
			    .help = "Log maximum value in the window",
			  },
			  { .ival = "both",
			    .oval = IO_LOG_SAMPLE_BOTH,
			    .help = "Log both average and maximum values over the window"
			  },
			  /* Compatibility with former boolean values */
			  { .ival = "0",
			    .oval = IO_LOG_SAMPLE_AVG,
			    .help = "Alias for 'avg'",
			  },
			  { .ival = "1",
			    .oval = IO_LOG_SAMPLE_MAX,
			    .help = "Alias for 'max'",
			  },
		},
	},
	{
		.name	= "log_offset",
		.lname	= "Log offset of IO",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, log_offset),
		.help	= "Include offset of IO for each log entry",
		.def	= "0",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "log_prio",
		.lname	= "Log priority of IO",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, log_prio),
		.help	= "Include priority value of IO for each log entry",
		.def	= "0",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "log_issue_time",
		.lname	= "Log IO issue time",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, log_issue_time),
		.help	= "Include IO issue time for each log entry",
		.def	= "0",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
#ifdef CONFIG_ZLIB
	{
		.name	= "log_compression",
		.lname	= "Log compression",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, log_gz),
		.help	= "Log in compressed chunks of this size",
		.minval	= 1024ULL,
		.maxval	= 512 * 1024 * 1024ULL,
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
#ifdef FIO_HAVE_CPU_AFFINITY
	{
		.name	= "log_compression_cpus",
		.lname	= "Log Compression CPUs",
		.type	= FIO_OPT_STR,
		.cb	= str_log_cpus_allowed_cb,
		.off1	= offsetof(struct thread_options, log_gz_cpumask),
		.parent = "log_compression",
		.help	= "Limit log compression to these CPUs",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
#else
	{
		.name	= "log_compression_cpus",
		.lname	= "Log Compression CPUs",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support CPU affinities",
	},
#endif
	{
		.name	= "log_store_compressed",
		.lname	= "Log store compressed",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, log_gz_store),
		.help	= "Store logs in a compressed format",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
#else
	{
		.name	= "log_compression",
		.lname	= "Log compression",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Install libz-dev(el) to get compression support",
	},
	{
		.name	= "log_store_compressed",
		.lname	= "Log store compressed",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Install libz-dev(el) to get compression support",
	},
#endif
	{
		.name = "log_alternate_epoch",
		.alias = "log_unix_epoch",
		.lname = "Log epoch alternate",
		.type = FIO_OPT_BOOL,
		.off1 = offsetof(struct thread_options, log_alternate_epoch),
		.help = "Use alternate epoch time in log files. Uses the same epoch as that is used by clock_gettime with specified log_alternate_epoch_clock_id.",
		.category = FIO_OPT_C_LOG,
		.group = FIO_OPT_G_INVALID,
	},
	{
		.name = "log_alternate_epoch_clock_id",
		.lname = "Log alternate epoch clock_id",
		.type = FIO_OPT_INT,
		.off1 = offsetof(struct thread_options, log_alternate_epoch_clock_id),
		.help = "If log_alternate_epoch is true, this option specifies the clock_id from clock_gettime whose epoch should be used. If log_alternate_epoch is false, this option has no effect. Default value is 0, or CLOCK_REALTIME",
		.category = FIO_OPT_C_LOG,
		.group = FIO_OPT_G_INVALID,
	},
	{
		.name	= "block_error_percentiles",
		.lname	= "Block error percentiles",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, block_error_hist),
		.help	= "Record trim block errors and make a histogram",
		.def	= "0",
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "bwavgtime",
		.lname	= "Bandwidth average time",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, bw_avg_time),
		.help	= "Time window over which to calculate bandwidth"
			  " (msec)",
		.def	= "500",
		.parent	= "write_bw_log",
		.hide	= 1,
		.interval = 100,
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "iopsavgtime",
		.lname	= "IOPS average time",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, iops_avg_time),
		.help	= "Time window over which to calculate IOPS (msec)",
		.def	= "500",
		.parent	= "write_iops_log",
		.hide	= 1,
		.interval = 100,
		.category = FIO_OPT_C_LOG,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "group_reporting",
		.lname	= "Group reporting",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, group_reporting),
		.help	= "Do reporting on a per-group basis",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "stats",
		.lname	= "Stats",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, stats),
		.help	= "Enable collection of stats",
		.def	= "1",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "zero_buffers",
		.lname	= "Zero I/O buffers",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, zero_buffers),
		.help	= "Init IO buffers to all zeroes",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "refill_buffers",
		.lname	= "Refill I/O buffers",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct thread_options, refill_buffers),
		.help	= "Refill IO buffers on every IO submit",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "scramble_buffers",
		.lname	= "Scramble I/O buffers",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, scramble_buffers),
		.help	= "Slightly scramble buffers on every IO submit",
		.def	= "1",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "buffer_pattern",
		.lname	= "Buffer pattern",
		.type	= FIO_OPT_STR,
		.cb	= str_buffer_pattern_cb,
		.off1	= offsetof(struct thread_options, buffer_pattern),
		.help	= "Fill pattern for IO buffers",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "buffer_compress_percentage",
		.lname	= "Buffer compression percentage",
		.type	= FIO_OPT_INT,
		.cb	= str_buffer_compress_cb,
		.off1	= offsetof(struct thread_options, compress_percentage),
		.maxval	= 100,
		.minval	= 0,
		.help	= "How compressible the buffer is (approximately)",
		.interval = 5,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "buffer_compress_chunk",
		.lname	= "Buffer compression chunk size",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, compress_chunk),
		.parent	= "buffer_compress_percentage",
		.hide	= 1,
		.help	= "Size of compressible region in buffer",
		.def	= "512",
		.interval = 256,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "dedupe_percentage",
		.lname	= "Dedupe percentage",
		.type	= FIO_OPT_INT,
		.cb	= str_dedupe_cb,
		.off1	= offsetof(struct thread_options, dedupe_percentage),
		.maxval	= 100,
		.minval	= 0,
		.help	= "Percentage of buffers that are dedupable",
		.interval = 1,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "dedupe_global",
		.lname	= "Global deduplication",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, dedupe_global),
		.help	= "Share deduplication buffers across jobs",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "dedupe_mode",
		.lname	= "Dedupe mode",
		.help	= "Mode for the deduplication buffer generation",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, dedupe_mode),
		.parent	= "dedupe_percentage",
		.def	= "repeat",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
		.posval	= {
			   { .ival = "repeat",
			     .oval = DEDUPE_MODE_REPEAT,
			     .help = "repeat previous page",
			   },
			   { .ival = "working_set",
			     .oval = DEDUPE_MODE_WORKING_SET,
			     .help = "choose a page randomly from limited working set defined in dedupe_working_set_percentage",
			   },
		},
	},
	{
		.name	= "dedupe_working_set_percentage",
		.lname	= "Dedupe working set percentage",
		.help	= "Dedupe working set size in percentages from file or device size used to generate dedupe patterns from",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, dedupe_working_set_percentage),
		.parent	= "dedupe_percentage",
		.def	= "5",
		.maxval	= 100,
		.minval	= 0,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "clat_percentiles",
		.lname	= "Completion latency percentiles",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, clat_percentiles),
		.help	= "Enable the reporting of completion latency percentiles",
		.def	= "1",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "lat_percentiles",
		.lname	= "IO latency percentiles",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, lat_percentiles),
		.help	= "Enable the reporting of IO latency percentiles",
		.def	= "0",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "slat_percentiles",
		.lname	= "Submission latency percentiles",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, slat_percentiles),
		.help	= "Enable the reporting of submission latency percentiles",
		.def	= "0",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "percentile_list",
		.lname	= "Percentile list",
		.type	= FIO_OPT_FLOAT_LIST,
		.off1	= offsetof(struct thread_options, percentile_list),
		.off2	= offsetof(struct thread_options, percentile_precision),
		.help	= "Specify a custom list of percentiles to report for "
			  "completion latency and block errors",
		.def    = "1:5:10:20:30:40:50:60:70:80:90:95:99:99.5:99.9:99.95:99.99",
		.maxlen	= FIO_IO_U_LIST_MAX_LEN,
		.minfp	= 0.0,
		.maxfp	= 100.0,
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "significant_figures",
		.lname	= "Significant figures",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, sig_figs),
		.maxval	= 10,
		.minval	= 1,
		.help	= "Significant figures for output-format set to normal",
		.def	= "4",
		.interval = 1,
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},

#ifdef FIO_HAVE_DISK_UTIL
	{
		.name	= "disk_util",
		.lname	= "Disk utilization",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, do_disk_util),
		.help	= "Log disk utilization statistics",
		.def	= "1",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
#else
	{
		.name	= "disk_util",
		.lname	= "Disk utilization",
		.type	= FIO_OPT_UNSUPPORTED,
		.help	= "Your platform does not support disk utilization",
	},
#endif
	{
		.name	= "gtod_reduce",
		.lname	= "Reduce gettimeofday() calls",
		.type	= FIO_OPT_BOOL,
		.help	= "Greatly reduce number of gettimeofday() calls",
		.cb	= str_gtod_reduce_cb,
		.def	= "0",
		.hide_on_set = 1,
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "disable_lat",
		.lname	= "Disable all latency stats",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, disable_lat),
		.help	= "Disable latency numbers",
		.parent	= "gtod_reduce",
		.hide	= 1,
		.def	= "0",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "disable_clat",
		.lname	= "Disable completion latency stats",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, disable_clat),
		.help	= "Disable completion latency numbers",
		.parent	= "gtod_reduce",
		.hide	= 1,
		.def	= "0",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "disable_slat",
		.lname	= "Disable submission latency stats",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, disable_slat),
		.help	= "Disable submission latency numbers",
		.parent	= "gtod_reduce",
		.hide	= 1,
		.def	= "0",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "disable_bw_measurement",
		.alias	= "disable_bw",
		.lname	= "Disable bandwidth stats",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, disable_bw),
		.help	= "Disable bandwidth logging",
		.parent	= "gtod_reduce",
		.hide	= 1,
		.def	= "0",
		.category = FIO_OPT_C_STAT,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "gtod_cpu",
		.lname	= "Dedicated gettimeofday() CPU",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, gtod_cpu),
		.help	= "Set up dedicated gettimeofday() thread on this CPU",
		.verify	= gtod_cpu_verify,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CLOCK,
	},
	{
		.name	= "job_start_clock_id",
		.lname	= "Job start clock_id",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, job_start_clock_id),
		.help	= "The clock_id passed to the call to clock_gettime used to record job_start in the json output format. Default is 0, or CLOCK_REALTIME",
		.verify	= gtod_cpu_verify,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CLOCK,
	},
	{
		.name	= "unified_rw_reporting",
		.lname	= "Unified RW Reporting",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, unified_rw_rep),
		.help	= "Unify reporting across data direction",
		.def	= "none",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
		.posval	= {
			  { .ival = "none",
			    .oval = UNIFIED_SPLIT,
			    .help = "Normal statistics reporting",
			  },
			  { .ival = "mixed",
			    .oval = UNIFIED_MIXED,
			    .help = "Statistics are summed per data direction and reported together",
			  },
			  { .ival = "both",
			    .oval = UNIFIED_BOTH,
			    .help = "Statistics are reported normally, followed by the mixed statistics"
			  },
			  /* Compatibility with former boolean values */
			  { .ival = "0",
			    .oval = UNIFIED_SPLIT,
			    .help = "Alias for 'none'",
			  },
			  { .ival = "1",
			    .oval = UNIFIED_MIXED,
			    .help = "Alias for 'mixed'",
			  },
			  { .ival = "2",
			    .oval = UNIFIED_BOTH,
			    .help = "Alias for 'both'",
			  },
		},
	},
	{
		.name	= "continue_on_error",
		.lname	= "Continue on error",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, continue_on_error),
		.help	= "Continue on non-fatal errors during IO",
		.def	= "none",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_ERR,
		.posval = {
			  { .ival = "none",
			    .oval = ERROR_TYPE_NONE,
			    .help = "Exit when an error is encountered",
			  },
			  { .ival = "read",
			    .oval = ERROR_TYPE_READ,
			    .help = "Continue on read errors only",
			  },
			  { .ival = "write",
			    .oval = ERROR_TYPE_WRITE,
			    .help = "Continue on write errors only",
			  },
			  { .ival = "io",
			    .oval = ERROR_TYPE_READ | ERROR_TYPE_WRITE,
			    .help = "Continue on any IO errors",
			  },
			  { .ival = "verify",
			    .oval = ERROR_TYPE_VERIFY,
			    .help = "Continue on verify errors only",
			  },
			  { .ival = "all",
			    .oval = ERROR_TYPE_ANY,
			    .help = "Continue on all io and verify errors",
			  },
			  { .ival = "0",
			    .oval = ERROR_TYPE_NONE,
			    .help = "Alias for 'none'",
			  },
			  { .ival = "1",
			    .oval = ERROR_TYPE_ANY,
			    .help = "Alias for 'all'",
			  },
		},
	},
	{
		.name	= "ignore_error",
		.lname	= "Ignore Error",
		.type	= FIO_OPT_STR,
		.cb	= str_ignore_error_cb,
		.off1	= offsetof(struct thread_options, ignore_error_nr),
		.help	= "Set a specific list of errors to ignore",
		.parent	= "rw",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_ERR,
	},
	{
		.name	= "error_dump",
		.lname	= "Error Dump",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, error_dump),
		.def	= "0",
		.help	= "Dump info on each error",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_ERR,
	},
	{
		.name	= "profile",
		.lname	= "Profile",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, profile),
		.help	= "Select a specific builtin performance test",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "cgroup",
		.lname	= "Cgroup",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct thread_options, cgroup),
		.help	= "Add job to cgroup of this name",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CGROUP,
	},
	{
		.name	= "cgroup_nodelete",
		.lname	= "Cgroup no-delete",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct thread_options, cgroup_nodelete),
		.help	= "Do not delete cgroups after job completion",
		.def	= "0",
		.parent	= "cgroup",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CGROUP,
	},
	{
		.name	= "cgroup_weight",
		.lname	= "Cgroup weight",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, cgroup_weight),
		.help	= "Use given weight for cgroup",
		.minval = 100,
		.maxval	= 1000,
		.parent	= "cgroup",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CGROUP,
	},
	{
		.name	= "uid",
		.lname	= "User ID",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, uid),
		.help	= "Run job with this user ID",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CRED,
	},
	{
		.name	= "gid",
		.lname	= "Group ID",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, gid),
		.help	= "Run job with this group ID",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_CRED,
	},
	{
		.name	= "kb_base",
		.lname	= "KB Base",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, kb_base),
		.prio	= 1,
		.def	= "1024",
		.posval = {
			  { .ival = "1024",
			    .oval = 1024,
			    .help = "Inputs invert IEC and SI prefixes (for compatibility); outputs prefer binary",
			  },
			  { .ival = "1000",
			    .oval = 1000,
			    .help = "Inputs use IEC and SI prefixes; outputs prefer SI",
			  },
		},
		.help	= "Unit prefix interpretation for quantities of data (IEC and SI)",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "unit_base",
		.lname	= "Unit for quantities of data (Bits or Bytes)",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct thread_options, unit_base),
		.prio	= 1,
		.posval = {
			  { .ival = "0",
			    .oval = N2S_NONE,
			    .help = "Auto-detect",
			  },
			  { .ival = "8",
			    .oval = N2S_BYTEPERSEC,
			    .help = "Normal (byte based)",
			  },
			  { .ival = "1",
			    .oval = N2S_BITPERSEC,
			    .help = "Bit based",
			  },
		},
		.help	= "Bit multiple of result summary data (8 for byte, 1 for bit)",
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "hugepage-size",
		.lname	= "Hugepage size",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, hugepage_size),
		.help	= "When using hugepages, specify size of each page",
		.def	= __fio_stringify(FIO_HUGE_PAGE),
		.interval = 1024 * 1024,
		.category = FIO_OPT_C_GENERAL,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "flow_id",
		.lname	= "I/O flow ID",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, flow_id),
		.help	= "The flow index ID to use",
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_FLOW,
	},
	{
		.name	= "flow",
		.lname	= "I/O flow weight",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, flow),
		.help	= "Weight for flow control of this job",
		.parent	= "flow_id",
		.hide	= 1,
		.def	= "0",
		.maxval	= FLOW_MAX_WEIGHT,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_FLOW,
	},
	{
		.name	= "flow_watermark",
		.lname	= "I/O flow watermark",
		.type	= FIO_OPT_SOFT_DEPRECATED,
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_FLOW,
	},
	{
		.name	= "flow_sleep",
		.lname	= "I/O flow sleep",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct thread_options, flow_sleep),
		.help	= "How many microseconds to sleep after being held"
			" back by the flow control mechanism",
		.parent	= "flow_id",
		.hide	= 1,
		.def	= "0",
		.category = FIO_OPT_C_IO,
		.group	= FIO_OPT_G_IO_FLOW,
	},
	{
		.name   = "steadystate",
		.lname  = "Steady state threshold",
		.alias  = "ss",
		.type   = FIO_OPT_STR,
		.off1   = offsetof(struct thread_options, ss_state),
		.cb	= str_steadystate_cb,
		.help   = "Define the criterion and limit to judge when a job has reached steady state",
		.def	= "iops_slope:0.01%",
		.posval	= {
			  { .ival = "iops",
			    .oval = FIO_SS_IOPS,
			    .help = "maximum mean deviation of IOPS measurements",
			  },
			  { .ival = "iops_slope",
			    .oval = FIO_SS_IOPS_SLOPE,
			    .help = "slope calculated from IOPS measurements",
			  },
			  { .ival = "bw",
			    .oval = FIO_SS_BW,
			    .help = "maximum mean deviation of bandwidth measurements",
			  },
			  {
			    .ival = "bw_slope",
			    .oval = FIO_SS_BW_SLOPE,
			    .help = "slope calculated from bandwidth measurements",
			  },
                          { .ival = "lat",
                            .oval = FIO_SS_LAT,
                            .help = "maximum mean deviation of latency measurements",
                          },
                          { .ival = "lat_slope",
                            .oval = FIO_SS_LAT_SLOPE,
                            .help = "slope calculated from latency measurements",
                          },
		},
		.category = FIO_OPT_C_GENERAL,
		.group  = FIO_OPT_G_RUNTIME,
	},
        {
		.name   = "steadystate_duration",
		.lname  = "Steady state duration",
		.alias  = "ss_dur",
		.parent	= "steadystate",
		.type   = FIO_OPT_STR_VAL_TIME,
		.off1   = offsetof(struct thread_options, ss_dur),
		.help   = "Stop workload upon attaining steady state for specified duration",
		.def    = "0",
		.is_seconds = 1,
		.is_time = 1,
		.category = FIO_OPT_C_GENERAL,
		.group  = FIO_OPT_G_RUNTIME,
	},
        {
		.name   = "steadystate_ramp_time",
		.lname  = "Steady state ramp time",
		.alias  = "ss_ramp",
		.parent	= "steadystate",
		.type   = FIO_OPT_STR_VAL_TIME,
		.off1   = offsetof(struct thread_options, ss_ramp_time),
		.help   = "Delay before initiation of data collection for steady state job termination testing",
		.def    = "0",
		.is_seconds = 1,
		.is_time = 1,
		.category = FIO_OPT_C_GENERAL,
		.group  = FIO_OPT_G_RUNTIME,
	},
        {
		.name   = "steadystate_check_interval",
		.lname  = "Steady state check interval",
		.alias  = "ss_interval",
		.parent	= "steadystate",
		.type   = FIO_OPT_STR_VAL_TIME,
		.off1   = offsetof(struct thread_options, ss_check_interval),
		.help   = "Polling interval for the steady state check (too low means steadystate will not converge)",
		.def    = "1",
		.is_seconds = 1,
		.is_time = 1,
		.category = FIO_OPT_C_GENERAL,
		.group  = FIO_OPT_G_RUNTIME,
	},
	{
		.name = NULL,
	},
};

/*
 * [한국어] ===== Part 3: API 함수들 =====
 */

/*
 * [한국어]
 * add_to_lopt() — 단일 fio_option 을 getopt_long 의 struct option 으로 매핑.
 *
 * @lopt: [out] glibc struct option 슬롯 (name/has_arg/flag/val 필드).
 * @o:    원본 fio_option. type 을 보고 has_arg 결정.
 * @name: 옵션 이름 (주 name 또는 alias).
 * @val:  getopt_long 이 매칭 시 반환할 정수 — FIO_GETOPT_JOB 또는 FIO_GETOPT_IOENGINE.
 *
 * 왜 필요한가: fio 는 --옵션=값 과 잡 파일 양쪽을 지원. 전자는 glibc getopt_long(3) 이 파싱하며
 * struct option 배열을 요구한다. 이 헬퍼가 fio_option 의 타입 정보를 getopt_long 의 has_arg
 * (required_argument / optional_argument) 로 변환.
 *
 * 동작: STR_SET 타입만 optional_argument (값 없이 플래그만도 가능 — 예: --readonly).
 *       나머지는 required_argument (값 필수 — 예: --rw=read).
 */
static void add_to_lopt(struct option *lopt, struct fio_option *o,
			const char *name, int val)
{
	lopt->name = (char *) name;
	lopt->val = val;
	if (o->type == FIO_OPT_STR_SET)
		lopt->has_arg = optional_argument;
	else
		lopt->has_arg = required_argument;
}

/*
 * [한국어]
 * options_to_lopts() — fio_option 배열 전체를 getopt_long 포맷으로 일괄 변환.
 *
 * @opts:         원본 fio_option[] (fio_options 또는 엔진 전용 options).
 * @long_options: [out] glibc struct option 배열. 기존 엔트리 뒤에 append.
 * @i:            현재 append 시작 인덱스 (append 위치).
 * @option_type:  FIO_GETOPT_JOB 또는 FIO_GETOPT_IOENGINE — getopt_long 반환값으로 구분.
 *
 * 왜 필요한가: fio_options[] 는 수백 개 — 각각을 getopt_long 엔트리로 확장. alias도 별도 엔트리로.
 *
 * 동작: name 이 NULL (sentinel) 까지 순회하며 add_to_lopt. alias 있으면 추가 슬롯 사용.
 *       FIO_NR_OPTIONS 상한 assert.
 *
 * 호출 체인: fio_options_dup_and_init / fio_options_set_ioengine_opts → [options_to_lopts] → add_to_lopt.
 */
static void options_to_lopts(struct fio_option *opts,
			      struct option *long_options,
			      int i, int option_type)
{
	struct fio_option *o = &opts[0];
	while (o->name) {
		add_to_lopt(&long_options[i], o, o->name, option_type);
		if (o->alias) {
			i++;
			add_to_lopt(&long_options[i], o, o->alias, option_type);
		}

		i++;
		o++;
		assert(i < FIO_NR_OPTIONS);
	}
}

/*
 * [한국어]
 * fio_options_set_ioengine_opts() — 엔진별 CLI 옵션을 getopt_long 배열에 교체 등록.
 *
 * @long_options: 기존 getopt_long 배열. FIO_GETOPT_IOENGINE 슬롯부터 덮어씀.
 * @td:           현재 잡 (td->io_ops->options = 엔진 전용 fio_option[]).
 *
 * 왜 필요한가: ioengine=libaio vs ioengine=io_uring 등 엔진별로 CLI 노출 옵션이 다르다
 * (예: libaio의 userspace_reap, io_uring의 sqthread_poll). 엔진이 선택되면 해당 엔진의
 * 옵션 테이블을 getopt_long 엔트리로 노출하여 --<engine-opt>=val 로 CLI 지정 가능.
 *
 * 동작 단계:
 * 1) long_options 에서 FIO_GETOPT_IOENGINE 표시된 첫 슬롯 찾기 (이전 엔진 옵션 시작점).
 * 2) 그 자리에 NUL memset — 이전 엔진 옵션 제거.
 * 3) td->eo 있으면 options_to_lopts 로 새 엔진 옵션 append.
 *
 * 호출 체인: init.c::ioengine_load → [fio_options_set_ioengine_opts] → options_to_lopts.
 */
void fio_options_set_ioengine_opts(struct option *long_options,
				   struct thread_data *td)
{
	unsigned int i;

	i = 0;
	while (long_options[i].name) {
		if (long_options[i].val == FIO_GETOPT_IOENGINE) {
			memset(&long_options[i], 0, sizeof(*long_options));
			break;
		}
		i++;
	}

	/*
	 * Just clear out the prior ioengine options.
	 */
	if (!td || !td->eo)
		return;

	options_to_lopts(td->io_ops->options, long_options, i,
			 FIO_GETOPT_IOENGINE);
}

/*
 * [한국어]
 * fio_options_dup_and_init() — fio_options[] 전역 옵션 테이블을 getopt_long 배열에 등록.
 *
 * @long_options: [in,out] 글로벌 CLI 옵션 배열 (main에서 선언). 기존 엔트리(--help/--version 등)
 *                뒤에 fio_options[] 엔트리 append.
 *
 * 왜 필요한가: main()의 parse_cmd_line()이 getopt_long 을 호출하려면 먼저 전체 옵션 배열을 준비.
 * 이 함수가 호출되면 fio_options[] 의 모든 옵션이 CLI --name=val 형식으로 사용 가능해진다.
 *
 * 동작:
 * 1) options_init(fio_options) — parse.c 의 내부 초기화 (def 값 파싱 검증 등).
 * 2) long_options 배열의 기존 마지막 엔트리 위치 i 탐색.
 * 3) options_to_lopts 로 fio_options 전부를 i 뒤에 FIO_GETOPT_JOB 타입으로 추가.
 *
 * 호출 체인: init.c::parse_cmd_line → [fio_options_dup_and_init] → options_to_lopts.
 * 실행 컨텍스트: main() 초기 단계. 1회 호출.
 */
void fio_options_dup_and_init(struct option *long_options)
{
	unsigned int i;

	options_init(fio_options);

	i = 0;
	while (long_options[i].name)
		i++;

	options_to_lopts(fio_options, long_options, i, FIO_GETOPT_JOB);
}

/*
 * [한국어]
 * struct fio_keyword — 잡 파일 예약 키워드 치환 테이블 엔트리.
 *
 * 왜 필요한가: 잡 파일(INI/CLI)에서 시스템 종속 값을 하드코딩하지 않고 "$pagesize" 같은
 * 심볼로 표기하면 런타임에 현재 시스템의 실제 값으로 치환. 이식성 높은 잡 파일 작성.
 *
 * 사용 예:
 *   size=$mb_memory    → size=16384 (16GB 시스템에서, MB 단위)
 *   blocksize=$pagesize → blocksize=4096 (x86_64에서)
 *   numjobs=$ncpus     → numjobs=16 (16코어에서)
 *
 * 치환 흐름: fio_options_parse → dup_and_sub_options → fio_keyword_replace(opt)
 *            → 각 키워드 word를 replace로 문자열 치환 → 산술 연산 있으면 bc_calc.
 */
struct fio_keyword {
	const char *word;
	/* [한국어] 치환 대상 키워드 문자열. 항상 '$'로 시작. 순회 종료는 NULL sentinel.
	 * 설정자: 정적 테이블 fio_keywords[] 초기화.
	 * 읽는 자: fio_keyword_replace() 가 strstr(opt, kw->word) 로 매칭.
	 * 값 범위: "$pagesize" / "$mb_memory" / "$ncpus" / NULL.
	 * 동기화: 읽기 전용 정적 데이터. */

	const char *desc;
	/* [한국어] 사용자용 설명 (도움말/디버그 출력용). 현재 코드에서는 직접 사용 안 함.
	 * 값 범위: 인간 읽기용 영문 설명. */

	char *replace;
	/* [한국어] 런타임에 fio_keywords_init() 가 계산해 sprintf한 실제 값의 strdup 복사본.
	 * 설정자: fio_keywords_init() 가 1회 세팅 (프로세스 시작 시).
	 * 읽는 자: fio_keyword_replace() 가 이 문자열로 치환.
	 * 해제: fio_keywords_exit() 가 free 후 NULL.
	 * 동기화: 초기화 완료 후 읽기 전용 — 단일 메인 스레드가 파싱하므로 락 불요. */
};

/*
 * [한국어] 정적 키워드 테이블. NULL sentinel 로 순회 종료.
 * 새 키워드 추가 시 fio_keywords_init() 에도 해당 인덱스의 .replace 계산 추가 필요.
 */
static struct fio_keyword fio_keywords[] = {
	{
		.word	= "$pagesize",
		.desc	= "Page size in the system",
	},
	{
		.word	= "$mb_memory",
		.desc	= "Megabytes of memory online",
	},
	{
		.word	= "$ncpus",
		.desc	= "Number of CPUs online in the system",
	},
	{
		.word	= NULL,
	},
};

/*
 * [한국어]
 * fio_keywords_exit() — fio_keywords_init() 가 strdup한 replace 문자열 해제.
 *
 * 호출 시점: fio 프로세스 종료 단계 (deinitialize_fio).
 * 왜 필요한가: Valgrind/leak sanitizer 청결. fio_keywords_init() 은 세 엔트리에 strdup 함.
 */
void fio_keywords_exit(void)
{
	struct fio_keyword *kw;

	kw = &fio_keywords[0];
	while (kw->word) {
		free(kw->replace);
		kw->replace = NULL;
		kw++;
	}
}

/*
 * [한국어]
 * fio_keywords_init() — $pagesize / $mb_memory / $ncpus 실제 값 계산 & 문자열화.
 *
 * 왜 필요한가: 런타임에 시스템 정보를 조회해 치환 문자열 준비. 잡 파일 파싱 전에 호출되어야 함.
 *
 * 동작 단계:
 * 1) page_size (os/os.h 가 제공하는 sysconf(_SC_PAGESIZE) 결과) → "4096" 등 → strdup.
 * 2) os_phys_mem() / (1024*1024) = 전체 물리 메모리 MB → "16384" 등 → strdup.
 * 3) cpus_configured() = sysconf(_SC_NPROCESSORS_CONF) = 구성된 CPU 수 → "16" → strdup.
 *
 * 호출 체인: init.c::main → initialize_fio → [fio_keywords_init] → sysconf.
 * 주의: fio_keywords[] 인덱스와 대응 필드 순서가 고정 — 배열 재배치 시 함수도 수정 필수.
 */
void fio_keywords_init(void)
{
	unsigned long long mb_memory;
	char buf[128];
	long l;

	sprintf(buf, "%lu", (unsigned long) page_size);
	fio_keywords[0].replace = strdup(buf);

	mb_memory = os_phys_mem() / (1024 * 1024);
	sprintf(buf, "%llu", mb_memory);
	fio_keywords[1].replace = strdup(buf);

	l = cpus_configured();
	sprintf(buf, "%lu", l);
	fio_keywords[2].replace = strdup(buf);
}

#define BC_APP		"bc"   /* [한국어] POSIX "bc(1)" 임의정밀도 계산기 실행파일 이름. PATH 상에서 탐색. */

/*
 * [한국어]
 * bc_calc() — 옵션 값 문자열에 산술 연산자 있으면 bc(1) 서브프로세스로 계산.
 *
 * @str: "옵션=수식" 전체 문자열 (수정 가능 — 계산 성공 시 free 후 새 strdup 반환).
 * @return: "옵션=결과값" 새 문자열 (실패 시 원본 str 또는 NULL).
 *
 * 왜 필요한가: 사용자가 "size=1024*1024" 처럼 리터럴 수식을 쓰면 parse.c 의 str_to_decimal 은
 * 연산자를 모른다. 이 함수가 수식을 외부 bc(1) 프로세스에 위임해 실제 숫자로 교체.
 *
 * 동작 단계:
 * 1) '+', '-', '*', '/' 중 하나도 없거나 작은따옴표(이미 인용됨)가 있으면 원본 그대로 반환.
 * 2) '=' 위치 찾아 옵션명/값 분리.
 * 3) 버퍼 오버플로 방어 — 128/100 바이트 상한.
 * 4) `which bc` 실행하여 bc 설치 확인. 없으면 에러 로그 + NULL.
 * 5) popen("echo '값' | bc", "r") 로 서브프로세스 출력 읽기.
 * 6) "옵션=" 프리픽스 복구 후 strdup. 원본 free.
 *
 * 보안 주의: popen 은 쉘 인터프리트 — 값에 '; rm -rf /' 넣으면 실행됨. 그래서 `strchr(str, '\'')`
 * 로 작은따옴표 있는 경우를 거부 (이미 인용된 경우는 그대로 통과 의미지만 추가 이스케이프 방어).
 * fio 는 일반적으로 신뢰된 잡 파일을 가정하므로 이정도가 허용.
 *
 * 호출 체인: fio_keyword_replace → [bc_calc] → popen/fread/pclose.
 */
static char *bc_calc(char *str)
{
	char buf[128], *tmp;
	FILE *f;
	int ret;

	/*
	 * No math, just return string
	 */
	if ((!strchr(str, '+') && !strchr(str, '-') && !strchr(str, '*') &&
	     !strchr(str, '/')) || strchr(str, '\''))
		return str;

	/*
	 * Split option from value, we only need to calculate the value
	 */
	tmp = strchr(str, '=');
	if (!tmp)
		return str;

	tmp++;

	/*
	 * Prevent buffer overflows; such a case isn't reasonable anyway
	 */
	if (strlen(str) >= 128 || strlen(tmp) > 100)
		return str;

	sprintf(buf, "which %s > /dev/null", BC_APP);
	if (system(buf)) {
		log_err("fio: bc is needed for performing math\n");
		return NULL;
	}

	sprintf(buf, "echo '%s' | %s", tmp, BC_APP);
	f = popen(buf, "r");
	if (!f)
		return NULL;

	ret = fread(&buf[tmp - str], 1, 128 - (tmp - str), f);
	if (ret <= 0) {
		pclose(f);
		return NULL;
	}

	pclose(f);
	buf[(tmp - str) + ret - 1] = '\0';
	memcpy(buf, str, tmp - str);
	free(str);
	return strdup(buf);
}

/*
 * [한국어]
 * fio_option_dup_subs() — 환경 변수 ${VAR} 치환 및 옵션 복제.
 *
 * @opt: 원본 옵션 문자열 (수정 안 됨).
 * @return: 치환된 새 문자열 strdup. 호출자가 free 책임. 실패 시 NULL.
 *
 * 왜 필요한가: 잡 파일에서 환경 변수 참조를 허용해 외부 설정과 결합. 예:
 *   filename=${FIO_DEVICE}    → /etc/fio/mytest.fio 를 여러 환경에서 재사용 가능.
 *   directory=${HOME}/fiodata → 사용자별 데이터 위치.
 * 미정의 환경 변수는 빈 문자열로 치환 (에러 아님).
 *
 * 동작 단계:
 * 1) 입력 길이 OPT_LEN_MAX 초과 검사.
 * 2) "in" 버퍼에 opt 복사. "out" 버퍼에 결과 축적.
 * 3) 순회하며 "${" 패턴 감지 → "}" 위치 찾기 → 그 사이 이름을 getenv(). 값을 out에 복사.
 * 4) 패턴이 아닌 일반 문자는 out에 1:1 복사.
 * 5) 최종 out을 strdup.
 *
 * 호출 체인: dup_and_sub_options → [fio_option_dup_subs] → getenv(3).
 */
char *fio_option_dup_subs(const char *opt)
{
	char out[OPT_LEN_MAX+1];
	char in[OPT_LEN_MAX+1];
	char *outptr = out;
	char *inptr = in;
	char *ch1, *ch2, *env;
	ssize_t nchr = OPT_LEN_MAX;
	size_t envlen;

	if (strlen(opt) + 1 > OPT_LEN_MAX) {
		log_err("OPT_LEN_MAX (%d) is too small\n", OPT_LEN_MAX);
		return NULL;
	}

	snprintf(in, sizeof(in), "%s", opt);

	while (*inptr && nchr > 0) {
		if (inptr[0] == '$' && inptr[1] == '{') {
			ch2 = strchr(inptr, '}');
			if (ch2 && inptr+1 < ch2) {
				ch1 = inptr+2;
				inptr = ch2+1;
				*ch2 = '\0';

				env = getenv(ch1);
				if (env) {
					envlen = strlen(env);
					if (envlen <= nchr) {
						memcpy(outptr, env, envlen);
						outptr += envlen;
						nchr -= envlen;
					}
				}

				continue;
			}
		}

		*outptr++ = *inptr++;
		--nchr;
	}

	*outptr = '\0';
	return strdup(out);
}

/*
 * [한국어]
 * fio_keyword_replace() — 예약 키워드 치환 + 산술 연산 처리.
 *
 * @opt: 입력 문자열 (소유권 이동 — 이 함수가 free 하고 새 문자열 반환).
 * @return: 치환 후 새 문자열. 실패 시 NULL.
 *
 * 왜 필요한가: fio_option_dup_subs 가 환경 변수를 처리했다면, 이 함수는 fio 예약 키워드
 * ($pagesize/$mb_memory/$ncpus) 를 처리. 치환 후 계산식이 나타나면 bc_calc 로 평가.
 *
 * 동작 단계:
 * 1) fio_keywords[] 순회 — 각 word가 opt에 있으면:
 *    a) word 앞부분 복사, b) replace 삽입, c) word 뒤 나머지 복사 → 새 버퍼 완성.
 *    d) 원본 free, opt = 새 버퍼. docalc = 1.
 * 2) 치환이 한 번이라도 있었으면 bc_calc(opt) 호출 — "size=1024*1024" 같은 연산 평가.
 *
 * 호출 체인: dup_and_sub_options → [fio_keyword_replace] → bc_calc.
 */
static char *fio_keyword_replace(char *opt)
{
	char *s;
	int i;
	int docalc = 0;

	for (i = 0; fio_keywords[i].word != NULL; i++) {
		struct fio_keyword *kw = &fio_keywords[i];

		while ((s = strstr(opt, kw->word)) != NULL) {
			char *new = calloc(strlen(opt) + 1, 1);
			char *o_org = opt;
			int olen = s - opt;
			int len;

			/*
			 * Copy part of the string before the keyword and
			 * sprintf() the replacement after it.
			 */
			memcpy(new, opt, olen);
			len = sprintf(new + olen, "%s", kw->replace);

			/*
			 * If there's more in the original string, copy that
			 * in too
			 */
			opt += olen + strlen(kw->word);
			/* keeps final zero thanks to calloc */
			if (strlen(opt))
				memcpy(new + olen + len, opt, strlen(opt));

			/*
			 * replace opt and free the old opt
			 */
			opt = new;
			free(o_org);

			docalc = 1;
		}
	}

	/*
	 * Check for potential math and invoke bc, if possible
	 */
	if (docalc)
		opt = bc_calc(opt);

	return opt;
}

/*
 * [한국어]
 * dup_and_sub_options() — opts[] 배열 각 엔트리에 환경변수 + 키워드 치환 적용.
 *
 * @opts:     원본 옵션 문자열 배열.
 * @num_opts: 엔트리 개수.
 * @return:   새로 할당된 char** 배열. 각 엔트리는 치환 완료된 strdup.
 *
 * 왜 필요한가: fio_options_parse 진입 시 모든 옵션에 일괄 치환 수행 — 이후 실제 파싱 단계는
 * 치환된 최종 문자열만 다루면 됨. 단일 경로로 단순화.
 *
 * 동작: opts 각 엔트리에 대해 fio_option_dup_subs(환경변수) → fio_keyword_replace(키워드+연산).
 *
 * 호출 체인: fio_options_parse → [dup_and_sub_options] → fio_option_dup_subs → fio_keyword_replace.
 */
static char **dup_and_sub_options(char **opts, int num_opts)
{
	int i;
	char **opts_copy = malloc(num_opts * sizeof(*opts));
	for (i = 0; i < num_opts; i++) {
		opts_copy[i] = fio_option_dup_subs(opts[i]);
		if (!opts_copy[i])
			continue;
		opts_copy[i] = fio_keyword_replace(opts_copy[i]);
	}
	return opts_copy;
}

/*
 * [한국어]
 * show_closest_option() — 오타 추천: 가장 가까운 옵션 이름 안내.
 *
 * @opt: 인식 실패한 옵션 문자열 (예: "randomgenerator=tausworth", "blocksize=4k" 의 앞부분).
 *
 * 왜 필요한가: UX 개선 — 사용자가 옵션명 오타("threads" vs "thread")를 쳤을 때
 * 단순히 "Bad option" 만 출력하지 않고 "Did you mean ...?" 로 가장 유사한 것을 제안.
 *
 * 동작 단계:
 * 1) '=' 앞 부분만 이름으로 추출 (strdup 후 '='에 NUL 삽입).
 * 2) fio_options[] 전부 순회하며 string_distance() (parse.c, Levenshtein) 계산.
 * 3) 최소 거리 엔트리 선택.
 * 4) string_distance_ok (거리 임계값 이내) 이고 UNSUPPORTED 아니면 "Did you mean X?" 출력.
 *
 * 호출 체인: fio_options_parse (엔진 옵션 재시도 후에도 못 찾음) → [show_closest_option].
 */
static void show_closest_option(const char *opt)
{
	int best_option, best_distance;
	int i, distance;
	char *name;

	if (!strlen(opt))
		return;

	name = strdup(opt);
	i = 0;
	while (name[i] != '\0' && name[i] != '=')
		i++;
	name[i] = '\0';

	best_option = -1;
	best_distance = INT_MAX;
	i = 0;
	while (fio_options[i].name) {
		distance = string_distance(name, fio_options[i].name);
		if (distance < best_distance) {
			best_distance = distance;
			best_option = i;
		}
		i++;
	}

	if (best_option != -1 && string_distance_ok(name, best_distance) &&
	    fio_options[best_option].type != FIO_OPT_UNSUPPORTED)
		log_err("Did you mean %s?\n", fio_options[best_option].name);

	free(name);
}

/*
 * [한국어]
 * fio_options_parse() — 잡의 모든 옵션 파싱 메인 드라이버 (init.c::add_job의 핵심 호출).
 *
 * @td:       대상 thread_data (파싱 결과가 td->o 에 채워짐).
 * @opts:     옵션 문자열 배열 ("name=value" 형식).
 * @num_opts: 개수.
 * @return:   0=모두 성공, 비트 OR된 오류 코드 (하나라도 실패하면 0이 아님).
 *
 * 왜 필요한가: fio의 옵션 소스(CLI/INI)를 통일된 인터페이스로 파싱. add_job 이 잡 별로 1회 호출.
 *
 * 동작 단계 (5단계):
 *   1) sort_options(): .prio 필드 기준 정렬 — 예: directory(.prio=1) 가 filename(.prio=-1) 보다 먼저.
 *   2) dup_and_sub_options(): 각 문자열에 ${ENV}/$keyword 치환 + bc 계산 적용된 복사본 생성.
 *   3) 메인 루프: 각 opts_copy[i] 를 parse_option(parse.c)에 넘김. 매칭된 fio_option 은 o에,
 *      파싱 성공시 fio_option_mark_set 으로 설정 비트맵 갱신. 인식 실패 시 unknown++.
 *   4) unknown > 0 이면 ioengine_load(td) — ioengine 옵션이 먼저 파싱돼야 엔진 ops 로드 가능.
 *      이후 td->io_ops->options 로 재시도 — 엔진 전용 옵션을 매칭.
 *   5) 그래도 인식 실패한 건들에 대해 show_closest_option() 으로 오타 추천.
 *
 * 실행 컨텍스트: 메인 프로세스 init 단계, 잡 1개씩 순차. 잡 스레드 생성 이전.
 * 호출 체인: init.c::add_job → [fio_options_parse] → sort_options/parse_option/ioengine_load.
 */
int fio_options_parse(struct thread_data *td, char **opts, int num_opts)
{
	int i, ret, unknown;
	char **opts_copy;

	sort_options(opts, fio_options, num_opts);
	opts_copy = dup_and_sub_options(opts, num_opts);

	for (ret = 0, i = 0, unknown = 0; i < num_opts; i++) {
		const struct fio_option *o;
		int newret = parse_option(opts_copy[i], opts[i], fio_options,
						&o, &td->o, &td->opt_list);

		if (!newret && o)
			fio_option_mark_set(&td->o, o);

		if (opts_copy[i]) {
			if (newret && !o) {
				unknown++;
				continue;
			}
			free(opts_copy[i]);
			opts_copy[i] = NULL;
		}

		ret |= newret;
	}

	if (unknown) {
		ret |= ioengine_load(td);
		if (td->eo) {
			sort_options(opts_copy, td->io_ops->options, num_opts);
			opts = opts_copy;
		}
		for (i = 0; i < num_opts; i++) {
			const struct fio_option *o = NULL;
			int newret = 1;

			if (!opts_copy[i])
				continue;

			if (td->eo)
				newret = parse_option(opts_copy[i], opts[i],
						      td->io_ops->options, &o,
						      td->eo, &td->opt_list);

			ret |= newret;
			if (!o) {
				log_err("Bad option <%s>\n", opts[i]);
				show_closest_option(opts[i]);
			}
			free(opts_copy[i]);
			opts_copy[i] = NULL;
		}
	}

	free(opts_copy);
	return ret;
}

/*
 * [한국어]
 * fio_cmd_option_parse() — CLI 단일 옵션 파싱 (--name=val 형식 1개).
 *
 * @td:  대상 잡. td->o에 저장.
 * @opt: 옵션 이름 (예: "rw").
 * @val: 값 (예: "randread").
 * @return: 0=성공, 음수=실패.
 *
 * 왜 필요한가: init.c::parse_cmd_line 이 getopt_long 루프에서 매 옵션마다 호출.
 * fio_options_parse 는 배열 일괄 처리용이고, 이 함수는 개별 CLI 옵션용.
 *
 * 호출 체인: init.c::parse_cmd_line → [fio_cmd_option_parse] → parse.c::parse_cmd_option.
 * 파싱 성공 시 fio_option_mark_set 으로 설정 비트맵 갱신.
 */
int fio_cmd_option_parse(struct thread_data *td, const char *opt, char *val)
{
	int ret;

	ret = parse_cmd_option(opt, val, fio_options, &td->o, &td->opt_list);
	if (!ret) {
		const struct fio_option *o;

		o = find_option_c(fio_options, opt);
		if (o)
			fio_option_mark_set(&td->o, o);
	}

	return ret;
}

/*
 * [한국어]
 * fio_cmd_ioengine_option_parse() — 엔진 전용 CLI 옵션 파싱.
 *
 * @td:  잡 (td->io_ops->options 에 엔진 옵션 배열, td->eo 에 저장 대상 구조체).
 * @opt: 엔진 옵션 이름 (예: libaio의 "userspace_reap", io_uring의 "sqthread_poll").
 * @val: 값.
 * @return: 0=성공.
 *
 * 왜 필요한가: CLI에서 --userspace_reap 같은 엔진별 옵션은 fio_options[]에 없다. 이 함수가
 * 엔진의 고유 옵션 테이블을 참조해 td->eo 에 직접 저장.
 *
 * 호출 체인: init.c → getopt_long → val=FIO_GETOPT_IOENGINE → [fio_cmd_ioengine_option_parse].
 */
int fio_cmd_ioengine_option_parse(struct thread_data *td, const char *opt,
				char *val)
{
	return parse_cmd_option(opt, val, td->io_ops->options, td->eo,
					&td->opt_list);
}

/*
 * [한국어]
 * fio_fill_default_options() — 잡의 모든 옵션에 .def 기본값 적용.
 *
 * @td: 초기화 대상. td->o.magic 을 OPT_MAGIC 으로 세팅 (cb_data_to_td/kb_base 검증용).
 *
 * 왜 필요한가: fio_options_parse 전에 기본값을 먼저 채우고, 사용자 지정값이 덮어쓰는 방식.
 * 이렇게 해야 사용자가 일부만 지정해도 나머지는 자동으로 합리적 기본값을 가진다.
 *
 * 호출 체인: init.c::add_job 시작 시 → [fio_fill_default_options] → parse.c::fill_default_options.
 */
void fio_fill_default_options(struct thread_data *td)
{
	td->o.magic = OPT_MAGIC;
	fill_default_options(&td->o, fio_options);
}

/*
 * [한국어]
 * fio_show_option_help() — `fio --cmdhelp=<opt>` 출력.
 *
 * @opt: 도움말을 볼 옵션 이름. NULL/빈 문자열이면 전체 목록.
 * @return: 0=성공.
 *
 * 왜 필요한가: 사용자가 특정 옵션 의미를 빠르게 조회. fio --cmdhelp=bssplit → bssplit 옵션의
 * .help/.type/.posval 등을 표시.
 *
 * 호출 체인: init.c::parse_cmd_line --cmdhelp 처리 → [fio_show_option_help] → parse.c::show_cmd_help.
 */
int fio_show_option_help(const char *opt)
{
	return show_cmd_help(fio_options, opt);
}

/*
 * [한국어]
 * fio_options_mem_dupe() — 잡의 모든 문자열 옵션을 deep-copy (fork 전 안전성 보장).
 *
 * @td: 대상 잡.
 *
 * 왜 필요한가: 기본 잡(def_thread)에서 각 잡(new thread_data)으로 옵션을 memcpy 하면 문자열
 * 포인터는 공유 상태. 잡별로 독립적인 strdup 사본이 필요 — 한 잡이 free 하면 다른 잡이
 * 댕글링. 특히 use_thread=0 (fork) 경우 프로세스 간에도 공유 금지.
 *
 * 동작 단계:
 * 1) options_mem_dupe(fio_options, &td->o) — parse.c 헬퍼가 FIO_OPT_STR_STORE 타입
 *    필드들을 순회하며 strdup으로 대체.
 * 2) ioengine_so_path 가 있으면 별도 strdup (parse.c 미관리).
 * 3) td->eo (엔진 옵션 구조체) 전체를 새로 malloc + memcpy + 그 안의 문자열들 dupe.
 *
 * 호출 체인: init.c::add_job 마지막 → [fio_options_mem_dupe].
 */
void fio_options_mem_dupe(struct thread_data *td)
{
	options_mem_dupe(fio_options, &td->o);

	if (td->o.ioengine_so_path)
		td->o.ioengine_so_path = strdup(td->o.ioengine_so_path);

	if (td->eo && td->io_ops) {
		void *oldeo = td->eo;

		td->eo = malloc(td->io_ops->option_struct_size);
		memcpy(td->eo, oldeo, td->io_ops->option_struct_size);
		options_mem_dupe(td->io_ops->options, td->eo);
	}
}

/*
 * [한국어]
 * fio_get_kb_base() — k/m/g 접미사 해석 시 배수 조회 (1024 vs 1000).
 *
 * @data: parse.c 가 전달한 값. 일반적으로 &td->o 이지만 엔진 전용 옵션에서는 td->eo일 수 있음.
 * @return: 1024 (이진, 기본) 또는 1000 (십진, kb_base=1000 지정 시).
 *
 * 왜 필요한가: "size=1k" 가 1024 인지 1000 인지 옵션에 따라 달라진다 (kb_base 옵션). parse.c 의
 * str_to_decimal 이 이 함수를 콜백으로 호출해 결정.
 *
 * 특이점 (HACK): 엔진 전용 옵션은 data 가 thread_options 가 아니라 엔진 내부 구조체를 가리킨다.
 * 안전하게 역참조 불가. 그래서 o->magic == OPT_MAGIC 검사 — thread_options 는 첫 필드가 magic
 * 이므로 메모리상 첫 4바이트로 구분 가능. 매치 안되면 전역 기본 1024 반환 (엔진 옵션은 잡 kb_base
 * 지정 무시 — 드물고 사용자도 거의 영향 없음).
 *
 * 호출 체인: parse.c::str_to_decimal (kb_base가 콜백 필드로 전달됨) → [fio_get_kb_base].
 */
unsigned int fio_get_kb_base(void *data)
{
	struct thread_data *td = cb_data_to_td(data);
	struct thread_options *o = &td->o;
	unsigned int kb_base = 0;

	/*
	 * This is a hack... For private options, *data is not holding
	 * a pointer to the thread_options, but to private data. This means
	 * we can't safely dereference it, but magic is first so mem wise
	 * it is valid. But this also means that if the job first sets
	 * kb_base and expects that to be honored by private options,
	 * it will be disappointed. We will return the global default
	 * for this.
	 */
	if (o && o->magic == OPT_MAGIC)
		kb_base = o->kb_base;
	if (!kb_base)
		kb_base = 1024;

	return kb_base;
}

/*
 * [한국어]
 * add_option() — fio_options[] 전역 테이블에 엔트리 동적 추가.
 *
 * @o: 추가할 fio_option 템플릿 (memcpy 로 복제됨).
 * @return: 0=성공, 1=FIO_MAX_OPTS 도달 (용량 초과).
 *
 * 왜 필요한가: 프로파일(tio-pmem 같은)이나 동적 모듈이 fio_options 에 자체 옵션을 추가.
 * 정적 배열이지만 FIO_MAX_OPTS 상한까지 남은 슬롯에 append. sentinel(.name=NULL) 를 뒤로 이동.
 *
 * 호출 체인: profile.c::register_profile → [add_option] 여러 번.
 * 읽는 자: parse.c 가 fio_options 를 순회할 때 동적 추가된 옵션도 매칭 대상.
 */
int add_option(const struct fio_option *o)
{
	struct fio_option *__o;
	int opt_index = 0;

	__o = fio_options;
	while (__o->name) {
		opt_index++;
		__o++;
	}

	if (opt_index + 1 == FIO_MAX_OPTS) {
		log_err("fio: FIO_MAX_OPTS is too small\n");
		return 1;
	}

	memcpy(&fio_options[opt_index], o, sizeof(*o));
	fio_options[opt_index + 1].name = NULL;
	return 0;
}

/*
 * [한국어]
 * invalidate_profile_options() — 프로파일 unload 시 해당 프로파일의 옵션을 무효화.
 *
 * @prof_name: 프로파일 이름 (예: "tiobench").
 *
 * 왜 필요한가: 한 fio 실행 내에서 여러 프로파일을 순차 사용 가능. 이전 프로파일의 옵션이
 * 남아있으면 다음 잡에서 의도치 않게 매칭될 수 있어 FIO_OPT_INVALID 로 타입 바꿔 사용 금지 처리.
 *
 * 동작: fio_options 순회 → prof_name 일치하면 type=INVALID, prof_name=NULL 로 무력화.
 *
 * 호출 체인: profile.c::unregister_profile → [invalidate_profile_options].
 */
void invalidate_profile_options(const char *prof_name)
{
	struct fio_option *o;

	o = fio_options;
	while (o->name) {
		if (o->prof_name && !strcmp(o->prof_name, prof_name)) {
			o->type = FIO_OPT_INVALID;
			o->prof_name = NULL;
		}
		o++;
	}
}

/*
 * [한국어]
 * add_opt_posval() — 기존 옵션의 posval 목록에 새 허용 값 동적 추가.
 *
 * @optname: 대상 옵션 이름 (예: "ioengine").
 * @ival:    추가할 허용 문자열 (예: "libaio"). oval=0 (정수값 미사용 — ioengine 경우 이름만 매칭).
 * @help:    도움말.
 *
 * 왜 필요한가: ioengine 옵션의 posval 은 정적으로 모든 엔진 이름을 포함할 수 없다 (동적 .so
 * 엔진 등). 각 엔진이 자신의 constructor(__attribute__((constructor))) 에서 register_ioengine
 * 을 호출할 때, 간접적으로 add_opt_posval("ioengine", 엔진이름, help) 을 호출해 자기를 등록.
 *
 * 동작: find_option 으로 옵션 찾고, PARSE_MAX_VP=32 슬롯 중 빈 자리에 삽입.
 *
 * 호출 체인: engines/*.c constructor → register_ioengine → ioengines.c::add_ioengine_ops →
 *            [add_opt_posval("ioengine", ops->name, help)].
 */
void add_opt_posval(const char *optname, const char *ival, const char *help)
{
	struct fio_option *o;
	unsigned int i;

	o = find_option(fio_options, optname);
	if (!o)
		return;

	for (i = 0; i < PARSE_MAX_VP; i++) {
		if (o->posval[i].ival)
			continue;

		o->posval[i].ival = ival;
		o->posval[i].help = help;
		break;
	}
}

/*
 * [한국어]
 * del_opt_posval() — 옵션의 posval 에서 특정 허용 값 제거.
 *
 * @optname: 옵션 이름.
 * @ival:    제거할 허용 문자열.
 *
 * 왜 필요한가: 엔진 dlclose 또는 동적 엔진 언로드 시 posval 에서 해당 엔진 제거. 메모리 해제가
 * 아니라 슬롯을 NULL 로 비우는 방식 (다른 엔트리는 배열 상 고정 위치 유지).
 *
 * 호출 체인: engines/*.c destructor → unregister_ioengine → ioengines.c → [del_opt_posval].
 */
void del_opt_posval(const char *optname, const char *ival)
{
	struct fio_option *o;
	unsigned int i;

	o = find_option(fio_options, optname);
	if (!o)
		return;

	for (i = 0; i < PARSE_MAX_VP; i++) {
		if (!o->posval[i].ival)
			continue;
		if (strcmp(o->posval[i].ival, ival))
			continue;

		o->posval[i].ival = NULL;
		o->posval[i].help = NULL;
	}
}

/*
 * [한국어]
 * fio_options_free() — 잡의 옵션 관련 동적 메모리 전체 해제.
 *
 * @td: 대상 잡 (잡 종료 시 reap_threads 단계에서 호출).
 *
 * 왜 필요한가: fio_options_mem_dupe 로 strdup/malloc 한 모든 리소스를 해제. 누수 방지.
 *
 * 해제 대상:
 * - fio_options 기반 문자열 필드들 (options_free 헬퍼).
 * - td->o.ioengine_so_path (별도 strdup).
 * - td->eo 본체 및 그 안의 문자열들 (엔진 옵션).
 *
 * 호출 체인: backend.c::reap_threads → [fio_options_free] → parse.c::options_free.
 */
void fio_options_free(struct thread_data *td)
{
	options_free(fio_options, &td->o);
	if (td->o.ioengine_so_path) {
		free(td->o.ioengine_so_path);
		td->o.ioengine_so_path = NULL;
	}

	if (td->eo && td->io_ops && td->io_ops->options) {
		options_free(td->io_ops->options, td->eo);
		free(td->eo);
		td->eo = NULL;
	}
}

/*
 * [한국어]
 * fio_dump_options_free() — td->opt_list (파싱된 옵션의 원본 텍스트 리스트) 해제.
 *
 * @td: 대상 잡.
 *
 * 왜 필요한가: parse_option 이 성공 시 추가한 print_option 엔트리(name/value strdup)들을 free.
 * 이 리스트는 --showcmd / dump 등 사용자 출력에 사용된다.
 *
 * 동작: flist_empty 될 때까지 flist_first_entry 로 하나씩 떼어내 각 필드 free.
 */
void fio_dump_options_free(struct thread_data *td)
{
	while (!flist_empty(&td->opt_list)) {
		struct print_option *p;

		p = flist_first_entry(&td->opt_list, struct print_option, list);
		flist_del_init(&p->list);
		free(p->name);
		free(p->value);
		free(p);
	}
}

/*
 * [한국어]
 * fio_option_find() — fio_options[] 에서 이름으로 fio_option 찾기.
 *
 * @name: 옵션 이름.
 * @return: 매칭된 엔트리 포인터 또는 NULL.
 *
 * 왜 필요한가: 외부 모듈(프로파일, server.c 등)이 특정 옵션 메타데이터에 접근할 때.
 * 예: server.c 가 세션 옵션 직렬화 시 fio_option 구조에서 type/off1 조회.
 */
struct fio_option *fio_option_find(const char *name)
{
	return find_option(fio_options, name);
}

/*
 * [한국어]
 * find_next_opt() — 특정 off1 offset 을 공유하는 다음 옵션 탐색 (alias 지원용).
 *
 * @from: 검색 시작점 (NULL이면 처음부터).
 * @off1: 찾을 offsetof 값.
 * @return: 매칭 옵션 또는 NULL.
 *
 * 왜 필요한가: 한 thread_options 필드에 여러 옵션이 저장 가능 (예: bs와 blocksize 둘 다 bs 필드 가리킴).
 * __fio_option_is_set 이 "같은 필드를 쓰는 옵션들 중 하나라도 명시 설정되었는가" 확인 때 루프.
 */
static struct fio_option *find_next_opt(struct fio_option *from,
					unsigned int off1)
{
	struct fio_option *opt;

	if (!from)
		from = &fio_options[0];
	else
		from++;

	opt = NULL;
	do {
		if (off1 == from->off1) {
			opt = from;
			break;
		}
		from++;
	} while (from->name);

	return opt;
}

/*
 * [한국어]
 * opt_is_set() — set_options 비트맵에서 해당 옵션의 설정 비트 조회.
 *
 * @o:   thread_options.
 * @opt: fio_option 엔트리.
 * @return: 0=미설정(기본값), !0=사용자가 명시 설정.
 *
 * 왜 필요한가: 잡 파일/CLI에서 명시 지정된 옵션과 기본값을 구분. 예: verify가 기본 VERIFY_NONE
 * 인지 사용자가 의도적으로 verify=none 으로 지정한 것인지 구분 필요한 케이스 (str_verify_pattern_cb).
 *
 * 동작: opt - fio_options[0] = 인덱스 오프셋. 64비트 워드 배열에서 해당 비트 추출.
 */
static int opt_is_set(struct thread_options *o, struct fio_option *opt)
{
	unsigned int opt_off, index, offset;

	opt_off = opt - &fio_options[0];
	index = opt_off / (8 * sizeof(uint64_t));
	offset = opt_off & ((8 * sizeof(uint64_t)) - 1);
	return (o->set_options[index] & ((uint64_t)1 << offset)) != 0;
}

/*
 * [한국어]
 * __fio_option_is_set() — 특정 thread_options 필드를 명시 설정한 옵션이 하나라도 있는지 확인.
 *
 * @o:    thread_options.
 * @off1: thread_options 내 필드 오프셋 (offsetof).
 * @return: true=어떤 옵션이든 이 필드를 명시 설정, false=모두 기본값.
 *
 * 왜 필요한가: fio_option_is_set(&td->o, verify) 매크로 뒤편 구현. 예를 들어 "verify" 와
 * "verify_hdr_only" 가 같은 필드를 공유할 수 있으므로 한 이름만 체크하면 안되고 같은 off1 을
 * 공유하는 모든 옵션을 순회해 하나라도 세팅되었으면 true.
 *
 * 동작: find_next_opt 루프 돌며 opt_is_set 확인. 발견 즉시 true.
 */
bool __fio_option_is_set(struct thread_options *o, unsigned int off1)
{
	struct fio_option *opt, *next;

	next = NULL;
	while ((opt = find_next_opt(next, off1)) != NULL) {
		if (opt_is_set(o, opt))
			return true;

		next = opt;
	}

	return false;
}

/*
 * [한국어]
 * fio_option_mark_set() — 옵션 파싱 성공 시 set_options 비트맵에 설정 비트 기록.
 *
 * @o:   thread_options.
 * @opt: 방금 파싱 성공한 fio_option.
 *
 * 왜 필요한가: 나중에 __fio_option_is_set 이 질의할 수 있도록 마킹. 이 비트맵은 fio_options[]
 * 인덱스 기반 (opt - &fio_options[0] 로 계산) — 옵션 수가 FIO_MAX_OPTS 이내라 uint64_t 배열 몇 개로 표현.
 *
 * 호출 체인: fio_options_parse 의 파싱 성공 브랜치 → [fio_option_mark_set].
 */
void fio_option_mark_set(struct thread_options *o, const struct fio_option *opt)
{
	unsigned int opt_off, index, offset;

	opt_off = opt - &fio_options[0];
	index = opt_off / (8 * sizeof(uint64_t));
	offset = opt_off & ((8 * sizeof(uint64_t)) - 1);
	o->set_options[index] |= (uint64_t)1 << offset;
}
