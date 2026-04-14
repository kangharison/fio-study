/*
 * [한국어 설명] tiobench 호환 워크로드 프로파일 (tiobench.c)
 *
 * === 파일의 역할 ===
 * 고전적인 tiobench 벤치마크를 에뮬레이션하는 워크로드 프로파일이다.
 * --profile=tiobench 옵션으로 사용하며, 기존 tiobench 결과와 비교할 수 있도록 동일한 I/O 패턴을 재현한다.
 * 순차 읽기/쓰기 및 랜덤 읽기/쓰기 테스트를 포함하여 디스크 성능을 측정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * profile.c의 register_profile()로 등록되어 init.c에서 --profile=tiobench 옵션 파싱 시 활성화된다.
 * act.c와 달리 런타임 콜백(io_ops)이 없고, 커맨드라인 옵션만 동적으로 생성한다.
 *
 * === 타 모듈과의 연결 ===
 * - profile.h: profile_ops 구조체를 통해 프로파일 시스템에 등록
 * - parse.h: fio_option 배열로 tiobench 전용 옵션(size, block, numruns 등) 정의
 *
 * === 주요 콜백 함수 ===
 * - tb_prep_cmdline(): 사용자 옵션을 fio 커맨드라인 문자열로 변환
 */
#include "../fio.h"
#include "../profile.h"
#include "../parse.h"
#include "../optgroup.h"

static unsigned long long size;
static unsigned int loops = 1;
static unsigned int bs = 4096;
static unsigned int nthreads = 1;
static char *dir;

static char sz_idx[80], bs_idx[80], loop_idx[80], dir_idx[80], t_idx[80];

/* [한국어] tiobench 기본 옵션 배열 - 순차쓰기/랜덤쓰기/순차읽기/랜덤읽기 순서로 실행 */
static const char *tb_opts[] = {
	"buffered=0", sz_idx, bs_idx, loop_idx, dir_idx, t_idx,
	"timeout=600", "group_reporting", "thread", "overwrite=1",
	"filename=.fio.tio.1:.fio.tio.2:.fio.tio.3:.fio.tio.4",
	"ioengine=sync",
	"name=seqwrite", "rw=write", "end_fsync=1",
	"name=randwrite", "stonewall", "rw=randwrite", "end_fsync=1",
	"name=seqread", "stonewall", "rw=read",
	"name=randread", "stonewall", "rw=randread", NULL,
};

struct tiobench_options {
	unsigned int pad;
	unsigned long long size;
	unsigned int loops;
	unsigned int bs;
	unsigned int nthreads;
	char *dir;
};

static struct tiobench_options tiobench_options;

static struct fio_option options[] = {
	{
		.name	= "size",
		.lname	= "Tiobench size",
		.type	= FIO_OPT_STR_VAL,
		.off1	= offsetof(struct tiobench_options, size),
		.help	= "Size in MiB",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_TIOBENCH,
	},
	{
		.name	= "block",
		.lname	= "Tiobench block",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct tiobench_options, bs),
		.help	= "Block size in bytes",
		.def	= "4096",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_TIOBENCH,
	},
	{
		.name	= "numruns",
		.lname	= "Tiobench numruns",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct tiobench_options, loops),
		.help	= "Number of runs",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_TIOBENCH,
	},
	{
		.name	= "dir",
		.lname	= "Tiobench directory",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct tiobench_options, dir),
		.help	= "Test directory",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_TIOBENCH,
		.no_free = true,
	},
	{
		.name	= "threads",
		.lname	= "Tiobench threads",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct tiobench_options, nthreads),
		.help	= "Number of Threads",
		.category = FIO_OPT_C_PROFILE,
		.group	= FIO_OPT_G_TIOBENCH,
	},
	{
		.name	= NULL,
	},
};

/*
 * Fill our private options into the command line
 */
/*
 * [한국어] tb_prep_cmdline - profile_ops.prep_cmd 콜백
 * tiobench 옵션(size, block, loops, dir, threads)을 fio 커맨드라인 형식으로 변환한다.
 * size가 지정되지 않으면 전체 메모리의 4배를 기본값으로 사용한다.
 */
static int tb_prep_cmdline(void)
{
	/*
	 * tiobench uses size as MiB, so multiply up
	 */
	size *= 1024 * 1024ULL;
	if (size)
		sprintf(sz_idx, "size=%llu", size);
	else
		strcpy(sz_idx, "size=4*1024*$mb_memory");

	sprintf(bs_idx, "bs=%u", bs);
	sprintf(loop_idx, "loops=%u", loops);

	if (dir)
		sprintf(dir_idx, "directory=%s", dir);
	else
		sprintf(dir_idx, "directory=./");

	sprintf(t_idx, "numjobs=%u", nthreads);
	return 0;
}

/* [한국어] tiobench 프로파일 등록 구조체 - --profile=tiobench로 선택 시 사용 */
static struct profile_ops tiobench_profile = {
	.name		= "tiobench",
	.desc		= "tiotest/tiobench benchmark",
	.prep_cmd	= tb_prep_cmdline,
	.cmdline	= tb_opts,
	.options	= options,
	.opt_data	= &tiobench_options,
};

/* [한국어] tiobench_register - fio 시작 시 자동 호출, tiobench 프로파일을 등록 */
static void fio_init tiobench_register(void)
{
	if (register_profile(&tiobench_profile))
		log_err("fio: failed to register profile 'tiobench'\n");
}

/* [한국어] tiobench_unregister - fio 종료 시 자동 호출, 프로파일 등록 해제 */
static void fio_exit tiobench_unregister(void)
{
	unregister_profile(&tiobench_profile);
}
