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
 * [한국어] 특정 방향(읽기/쓰기/트림)에 대한 bssplit 파싱
 * split_parse_ddir()로 파싱 후 퍼센트 합계를 검증하고,
 * 미지정 항목에 나머지 퍼센트를 균등 분배한다.
 * 최종적으로 퍼센트 기준으로 정렬하여 런타임 조회를 최적화한다.
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
 * [한국어] 읽기/쓰기/트림 방향별 분할 파싱 드라이버
 *
 * "읽기값,쓰기값,트림값" 형식의 문자열을 분리하여
 * 각 방향(DDIR_READ, DDIR_WRITE, DDIR_TRIM)에 대해 fn 콜백을 호출한다.
 * 콤마가 없으면 동일한 값을 세 방향 모두에 적용한다.
 *
 * 예: bssplit=4k/50:8k/50,16k/100
 *     → READ: 4k(50%)+8k(50%), WRITE: 16k(100%), TRIM: 16k(100%)
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

/* [한국어] FDP(Flexible Data Placement) ID 정렬 비교 함수 */
static int fio_fdp_cmp(const void *p1, const void *p2)
{
	const uint16_t *t1 = p1;
	const uint16_t *t2 = p2;

	return *t1 - *t2;
}

/*
 * [한국어] FDP placement ID 리스트 파싱 콜백
 * "1,2,3-5" 형식으로 단일 ID와 범위를 모두 지원한다.
 * 최대 FIO_MAX_DP_IDS개까지 허용, 파싱 후 오름차순 정렬한다.
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
 * [한국어] FDP scheme 파일 검증 콜백
 * dp_scheme_file 옵션으로 지정된 파일이 실제로 존재하는 일반 파일인지 확인한다.
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
 * [한국어] bssplit 옵션 파싱 콜백
 * "4k/50:8k/50,16k/100" 같은 블록 크기 분포를 파싱한다.
 * str_split_parse()를 통해 읽기/쓰기/트림 방향별로 분리 처리한다.
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
 * [한국어] cmdprio_bssplit 개별 항목 파싱
 * "bs/perc/class/level/hint" 형식을 파싱한다.
 * 예: "4k/50/1/2/0" → 4K 블록의 50%에 대해 class=1, level=2, hint=0 우선순위 적용
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
 * [한국어] cmdprio_bssplit 방향별 파싱 — 콜론 구분 항목들을 순회하며
 * parse_cmdprio_bssplit_entry()로 각 항목을 파싱하고 정렬한다.
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
 * [한국어] errno 이름 문자열을 숫자로 변환
 * 예: "EINVAL" → 22, "ENOENT" → 2
 * ignore_error, continue_on_error 옵션에서 사용된다.
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

/* [한국어] replay_skip 옵션 콜백: blktrace 재생 시 건너뛸 I/O 방향 지정 */
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
 * [한국어] ignore_error 옵션 콜백: 무시할 errno 목록 파싱
 * 방향별(읽기/쓰기/트림)로 콤마 구분된 errno 이름/번호를 파싱한다.
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
 * [한국어] rw(readwrite) 옵션 콜백
 * "read", "write", "randread", "randrw" 등의 I/O 패턴 설정.
 * 콤마 뒤에 시퀀셜 I/O의 시작 오프셋을 지정할 수 있다.
 * 예: "rw=write,4k" → 순차 쓰기를 4K 오프셋부터 시작
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
 * [한국어] mem(iomem) 옵션 콜백: 메모리 할당 방법 설정
 * "mmap:/path" 형식에서 ":"뒤의 파일 경로를 추출한다.
 */
static int str_mem_cb(void *data, const char *mem)
{
	struct thread_data *td = cb_data_to_td(data);

	if (td->o.mem_type == MEM_MMAPHUGE || td->o.mem_type == MEM_MMAP ||
	    td->o.mem_type == MEM_MMAPSHARED)
		td->o.mmapfile = get_opt_postfix(mem);

	return 0;
}

/* [한국어] clocksource 옵션 콜백: 시간 소스 설정 후 시간 보정 수행 */
static int fio_clock_source_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	fio_clock_source = td->o.clocksource;
	fio_clock_source_set = 1;
	fio_clock_init();
	return 0;
}

/* [한국어] rwmixread 콜백: 읽기 비율 설정 시 쓰기 비율 자동 계산 (100-val) */
static int str_rwmix_read_cb(void *data, long long *val)
{
	struct thread_data *td = cb_data_to_td(data);

	td->o.rwmix[DDIR_READ] = *val;
	td->o.rwmix[DDIR_WRITE] = 100 - *val;
	return 0;
}

/* [한국어] rwmixwrite 콜백: 쓰기 비율 설정 시 읽기 비율 자동 계산 (100-val) */
static int str_rwmix_write_cb(void *data, long long *val)
{
	struct thread_data *td = cb_data_to_td(data);

	td->o.rwmix[DDIR_WRITE] = *val;
	td->o.rwmix[DDIR_READ] = 100 - *val;
	return 0;
}

/* [한국어] exitall 콜백: 하나의 잡이 끝나면 모든 잡을 종료하는 플래그 설정 */
static int str_exitall_cb(void)
{
	exitall_on_terminate = true;
	return 0;
}

#ifdef FIO_HAVE_CPU_AFFINITY
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

/* [한국어] cpumask 콜백: CPU 친화성 마스크를 비트맵으로 설정 */
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

/* [한국어] cpus_allowed 콜백: "0,2-4,6" 형식의 CPU 목록을 CPU 마스크로 변환 */
static int str_cpus_allowed_cb(void *data, const char *input)
{
	struct thread_data *td = cb_data_to_td(data);

	if (parse_dryrun())
		return 0;
	if (!input)
		return 1;

	return set_cpus_allowed(td, &td->o.cpumask, input);
}

/* [한국어] verify_cpus_allowed 콜백: 검증 스레드의 CPU 친화성 설정 */
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
/* [한국어] log_cpus_allowed 콜백: 로그 스레드의 CPU 친화성 설정 */
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
/* [한국어] numa_cpu_nodes 콜백: NUMA CPU 노드 설정 (libnuma 사용) */
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
 * [한국어] numa_mem_policy 콜백: NUMA 메모리 정책 설정
 * "interleave:0-1", "bind:2", "prefer:1" 등의 형식을 파싱하여
 * 메모리 할당 시 NUMA 정책을 적용한다.
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
 * [한국어] file_service_type 콜백: 파일 서비스 순서 설정
 * "random", "roundrobin", "sequential", "gauss", "zipf" 등
 * 콜론 뒤에 분포 파라미터를 지정할 수 있다.
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
 * [한국어] server_file_remove 콜백: 서버 모드에서 파일 삭제 정책 설정
 * 대기, 완료 후 삭제 등의 정책을 파싱한다.
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
 * [한국어] random_distribution 콜백: 랜덤 I/O 오프셋의 분포 설정
 *
 * 지원 분포: random(균등), zipf, pareto, gauss(정규), zoned, zoned_abs
 * 각 분포별 파라미터를 콜론 뒤에 지정한다:
 *   - zipf:theta (예: zipf:1.2) — 지프 분포, theta가 클수록 편향
 *   - pareto:input (예: pareto:0.5) — 파레토 분포
 *   - gauss:dev (예: gauss:4.0) — 정규 분포, dev는 표준편차
 *   - zoned:ratio/size (예: zoned:60/10:30/20:10/70)
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

static bool is_valid_steadystate(unsigned int state)
{
	return (state == FIO_SS_IOPS || state == FIO_SS_IOPS_SLOPE ||
		state == FIO_SS_BW || state == FIO_SS_BW_SLOPE ||
		state == FIO_SS_LAT || state == FIO_SS_LAT_SLOPE);
}

/*
 * [한국어] steadystate 콜백: Steady State 감지 기준 파싱
 * "iops:10%" — IOPS 변동이 10% 이내면 안정 상태로 판단
 * "bw:5%" — 대역폭 변동이 5% 이내면 안정 상태
 * "iops_slope:0.1%" — IOPS 기울기 기준
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
 * [한국어] filename 옵션 콜백: 대상 파일/디바이스 경로 설정
 * 콜론으로 여러 파일을 지정할 수 있다: filename=/dev/sda:/dev/sdb
 * nrfiles를 자동 설정한다.
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

/* [한국어] directory 콜백: 작업 디렉토리 검증 (존재 여부 확인) */
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

/* [한국어] opendir 콜백: 디렉토리 내 모든 파일을 재귀적으로 대상에 추가 */
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
 * [한국어] buffer_pattern 콜백: I/O 버퍼에 채울 패턴 설정
 * 문자열, 16진수, "%o"(오프셋 삽입) 등 다양한 형식을 지원한다.
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

/* [한국어] buffer_compress_percentage 콜백: 압축 가능한 데이터 비율 설정 */
static int str_buffer_compress_cb(void *data, unsigned long long *il)
{
	struct thread_data *td = cb_data_to_td(data);

	td->flags |= TD_F_COMPRESS;
	td->o.compress_percentage = *il;
	return 0;
}

/* [한국어] dedupe_percentage 콜백: 중복 제거 가능 데이터 비율 설정 */
static int str_dedupe_cb(void *data, unsigned long long *il)
{
	struct thread_data *td = cb_data_to_td(data);

	td->flags |= TD_F_COMPRESS;
	td->o.dedupe_percentage = *il;
	td->o.refill_buffers = 1;
	return 0;
}

/* [한국어] verify_pattern 콜백: 데이터 검증에 사용할 패턴 설정 */
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
 * [한국어] gtod_reduce 콜백: gettimeofday 호출 최소화 모드
 * 활성화하면 상세 레이턴시 통계를 비활성화하여 오버헤드를 줄인다.
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

/* [한국어] offset 콜백: I/O 시작 오프셋을 읽기/쓰기/트림 방향별로 설정 */
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

/* [한국어] offset_increment 콜백: 다중 스레드 시 각 스레드의 오프셋 증분 */
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

/* [한국어] size 콜백: 각 스레드의 총 I/O 크기 설정 (퍼센트 지원) */
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

/* [한국어] io_size 콜백: 실제 수행할 I/O 양 설정 (size와 독립) */
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

/* [한국어] zoneskip 콜백: 존(zone) 경계에서 건너뛸 바이트 수 설정 */
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

/* [한국어] write_bw_log 콜백: 대역폭 로그 파일명 설정 */
static int str_write_bw_log_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	if (str)
		td->o.bw_log_file = strdup(str);

	td->o.write_bw_log = 1;
	return 0;
}

/* [한국어] write_lat_log 콜백: 레이턴시 로그 파일명 설정 */
static int str_write_lat_log_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	if (str)
		td->o.lat_log_file = strdup(str);

	td->o.write_lat_log = 1;
	return 0;
}

/* [한국어] write_iops_log 콜백: IOPS 로그 파일명 설정 */
static int str_write_iops_log_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	if (str)
		td->o.iops_log_file = strdup(str);

	td->o.write_iops_log = 1;
	return 0;
}

/* [한국어] write_hist_log 콜백: 히스토그램 로그 파일명 설정 */
static int str_write_hist_log_cb(void *data, const char *str)
{
	struct thread_data *td = cb_data_to_td(data);

	if (str)
		td->o.hist_log_file = strdup(str);

	td->o.write_hist_log = 1;
	return 0;
}

/*
 * [한국어] 외부 I/O 엔진 콜백: "external:/path/to/engine.so" 파싱
 *
 * ioengine=external:/path/to/so 형식에서 공유 라이브러리 경로를 추출하여
 * ioengine_so_path에 저장한다. dlopen()으로 런타임에 로드된다.
 *
 * 메모리 레이아웃:
 *   "external:/path/to/so\0" ← strdup된 원본
 *   "external\0"             ← parse 후 ->ioengine
 *            "/path/to/so\0" ← str 인자 = ->ioengine_so_path
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

/* [한국어] fio 옵션을 getopt_long의 option 구조체로 변환 */
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

/* [한국어] fio_option 배열 전체를 getopt_long 옵션 배열로 변환 */
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
 * [한국어] I/O 엔진 전용 옵션을 getopt_long 배열에 설정
 * 엔진이 변경되면 이전 엔진 옵션을 지우고 새 엔진 옵션을 추가한다.
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

/* [한국어] fio_options 초기화 후 getopt_long 배열에 추가 */
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
 * [한국어] 키워드 치환 시스템
 * 잡 파일에서 $pagesize, $mb_memory, $ncpus 등의 변수를
 * 실제 시스템 값으로 치환한다.
 * 예: size=$mb_memory → size=16384 (16GB 시스템에서)
 */
struct fio_keyword {
	const char *word;      /* 키워드 (예: "$pagesize") */
	const char *desc;      /* 설명 */
	char *replace;         /* 치환될 실제 값 문자열 */
};

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

/* [한국어] 키워드 치환 문자열 메모리 해제 */
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

/* [한국어] 키워드 치환 값 초기화: 페이지 크기, 메모리 용량, CPU 수 */
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

#define BC_APP		"bc"

/*
 * [한국어] bc(1) 계산기를 사용한 산술 표현식 계산
 * 옵션 값에 +, -, *, / 연산자가 포함되면 bc로 계산한다.
 * 예: size=1024*1024 → bc가 1048576으로 계산
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
 * [한국어] 환경 변수 치환: ${VARNAME} → 환경 변수 값으로 대체
 * 예: filename=${FIO_DEVICE} → filename=/dev/nvme0n1
 * VARNAME이 미정의면 빈 문자열로 치환된다.
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
 * [한국어] 예약 키워드 치환: $pagesize, $mb_memory, $ncpus → 실제 값
 * 치환 후 산술 연산이 포함되었으면 bc로 계산한다.
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

/* [한국어] 옵션 배열을 복사하면서 환경 변수 및 키워드 치환 적용 */
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

/* [한국어] 알 수 없는 옵션에 대해 가장 유사한 옵션명을 추천 (레벤슈타인 거리) */
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
 * [한국어] 잡 파일의 옵션 목록 전체 파싱 — 메인 파싱 드라이버
 *
 * 1) 옵션을 우선순위(prio)로 정렬
 * 2) 환경 변수/키워드 치환 적용
 * 3) 각 옵션을 parse_option()으로 파싱
 * 4) 인식 못한 옵션은 I/O 엔진 옵션으로 재시도
 * 5) 여전히 인식 못하면 유사 옵션 추천 후 에러
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

/* [한국어] 커맨드라인 옵션 파싱 (--name val) */
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

/* [한국어] I/O 엔진 전용 커맨드라인 옵션 파싱 */
int fio_cmd_ioengine_option_parse(struct thread_data *td, const char *opt,
				char *val)
{
	return parse_cmd_option(opt, val, td->io_ops->options, td->eo,
					&td->opt_list);
}

/* [한국어] thread_data에 모든 옵션의 기본값 적용 */
void fio_fill_default_options(struct thread_data *td)
{
	td->o.magic = OPT_MAGIC;
	fill_default_options(&td->o, fio_options);
}

/* [한국어] 옵션 도움말 표시 (fio --cmdhelp=옵션명) */
int fio_show_option_help(const char *opt)
{
	return show_cmd_help(fio_options, opt);
}

/*
 * [한국어] 문자열 옵션 메모리 복제
 * fork/clone 시 자식이 독립적인 문자열 복사본을 가지도록 한다.
 * I/O 엔진 옵션(eo)도 함께 복제한다.
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
 * [한국어] kb_base 값 조회: 1024(이진) 또는 1000(십진)
 * 옵션 파싱 시 k/m/g 접미사의 배수를 결정한다.
 * data가 유효한 thread_options가 아닐 수 있어 magic 검사를 한다.
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
 * [한국어] fio_options[] 배열에 옵션을 동적으로 추가
 * I/O 엔진이 자체 옵션을 등록할 때 사용한다.
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

/* [한국어] 지정된 프로파일의 옵션을 INVALID로 무효화 */
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

/* [한국어] 옵션의 posval에 새로운 허용 값을 동적 추가 (엔진 등록용) */
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

/* [한국어] 옵션의 posval에서 특정 허용 값을 삭제 */
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

/* [한국어] 스레드의 모든 옵션 문자열 및 엔진 옵션 메모리 해제 */
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

/* [한국어] 옵션 덤프 리스트(print_option) 메모리 해제 */
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

/* [한국어] 이름으로 fio_options[]에서 옵션 검색 */
struct fio_option *fio_option_find(const char *name)
{
	return find_option(fio_options, name);
}

/* [한국어] 동일한 off1을 가진 다음 옵션을 찾는다 (여러 옵션이 같은 변수를 가리킬 수 있음) */
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
 * [한국어] 특정 옵션이 설정되었는지 비트맵에서 확인
 * set_options[]는 uint64_t 배열로, 각 비트가 fio_options[] 인덱스에 대응한다.
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
 * [한국어] 특정 오프셋에 해당하는 옵션이 사용자에 의해 명시적으로 설정되었는지 확인
 * 같은 off1을 가진 모든 옵션을 순회하여 하나라도 설정되었으면 true 반환.
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

/* [한국어] 옵션이 설정되었음을 비트맵에 마킹 — 파싱 성공 후 호출 */
void fio_option_mark_set(struct thread_options *o, const struct fio_option *opt)
{
	unsigned int opt_off, index, offset;

	opt_off = opt - &fio_options[0];
	index = opt_off / (8 * sizeof(uint64_t));
	offset = opt_off & ((8 * sizeof(uint64_t)) - 1);
	o->set_options[index] |= (uint64_t)1 << offset;
}
