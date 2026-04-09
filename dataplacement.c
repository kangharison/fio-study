/*
 * Note: This is similar to a very basic setup
 * of ZBD devices
 *
 * Specify fdp=1 (With char devices /dev/ng0n1)
 */
/*
 * [한국어] dataplacement.c - FDP(Flexible Data Placement) 데이터 배치 구현
 *
 * NVMe FDP 또는 스트림(Streams) 프로토콜을 사용하여 쓰기 I/O에
 * 데이터 배치 힌트(dtype/dspec)를 부여하는 기능을 구현한다.
 *
 * 주요 기능:
 *   1) dp_init()           - 각 파일에 대해 RUH 정보 조회 및 스킴 로드
 *   2) dp_fill_dspec_data() - 쓰기 I/O의 dtype/dspec 값을 결정
 *   3) fdp_free_ruhs_info() - RUH 정보 메모리 해제
 *
 * 배치 ID 선택 방식:
 *   - FIO_DP_RR     : 라운드 로빈 (순차 순환)
 *   - FIO_DP_RANDOM : 균등 랜덤
 *   - FIO_DP_SCHEME : 오프셋 범위 기반 매핑 (스킴 파일에서 로드)
 */

#include <errno.h>      /* 에러 코드 */
#include <string.h>     /* 문자열 처리 */
#include <stdlib.h>     /* 메모리 할당 */
#include <unistd.h>     /* POSIX API */
#include "fio.h"        /* fio 핵심 구조체 */
#include "file.h"       /* 파일 관련 구조체 */

#include "pshared.h"        /* 프로세스 간 공유 뮤텍스/세마포어 */
#include "dataplacement.h"  /* 데이터 배치 헤더 */

/* [한국어] FDP RUH 정보 조회 - I/O 엔진의 fdp_fetch_ruhs 콜백을 호출하여
 * 디바이스에서 사용 가능한 RUH(Reclaim Unit Handle) 목록을 가져온다. */
static int fdp_ruh_info(struct thread_data *td, struct fio_file *f,
			struct fio_ruhs_info *ruhs)
{
	int ret = -EINVAL;

	if (!td->io_ops) {
		log_err("fio: no ops set in fdp init?!\n");
		return ret;
	}

	if (td->io_ops->fdp_fetch_ruhs) {
		ret = td->io_ops->fdp_fetch_ruhs(td, f, ruhs);
		if (ret < 0) {
			td_verror(td, errno, "fdp fetch ruhs failed");
			log_err("%s: fdp fetch ruhs failed (%d)\n",
				f->file_name, errno);
		}
	} else {
		log_err("%s: engine (%s) lacks fetch ruhs\n",
			f->file_name, td->io_ops->name);
	}

	return ret;
}

/* [한국어] RUH 정보 초기화 - 디바이스에서 RUH를 조회하거나 사용자 지정 스트림 ID를 설정
 *
 * 스트림(Streams) 모드:
 *   사용자가 지정한 dp_ids를 직접 사용하여 fio_ruhs_info를 구성한다.
 *
 * FDP 모드:
 *   1단계) 헤더만 조회하여 RUH 개수를 파악
 *   2단계) 전체 RUH 목록을 재조회
 *   3단계) 사용자 지정 dp_ids가 있으면 해당 인덱스의 RUH만 선택
 *          없으면 디바이스가 보고한 전체 RUH 사용 (최대 FIO_MAX_DP_IDS) */
static int init_ruh_info(struct thread_data *td, struct fio_file *f)
{
	struct fio_ruhs_info *ruhs, *tmp;
	uint32_t nr_ruhs;
	int i, ret;

	/* set up the data structure used for FDP to work with the supplied stream IDs */
	if (td->o.dp_type == FIO_DP_STREAMS) {
		if (!td->o.dp_nr_ids) {
			log_err("fio: stream IDs must be provided for dataplacement=streams\n");
			return -EINVAL;
		}
		ruhs = scalloc(1, sizeof(*ruhs) + td->o.dp_nr_ids * sizeof(*ruhs->plis));
		if (!ruhs)
			return -ENOMEM;

		ruhs->nr_ruhs = td->o.dp_nr_ids;
		for (int i = 0; i < ruhs->nr_ruhs; i++)
			ruhs->plis[i] = td->o.dp_ids[i];

		f->ruhs_info = ruhs;
		return 0;
	}

	/*
	 * Since we don't know the actual number of ruhs. Only fetch the header.
	 * We will reallocate this buffer and then fetch all the ruhs again.
	 */
	/* 1단계: 헤더만 조회하여 RUH 개수 확인 */
	ruhs = calloc(1, sizeof(*ruhs));
	ret = fdp_ruh_info(td, f, ruhs);
	if (ret) {
		log_err("fio: ruh info failed for %s (%d)\n",
			f->file_name, -ret);
		goto out;
	}

	/* 2단계: 전체 RUH 목록을 저장할 버퍼를 재할당하고 다시 조회 */
	nr_ruhs = ruhs->nr_ruhs;
	ruhs = realloc(ruhs, sizeof(*ruhs) + nr_ruhs * sizeof(*ruhs->plis));
	if (!ruhs) {
		log_err("fio: ruhs buffer realloc failed for %s\n",
			f->file_name);
		ret = -ENOMEM;
		goto out;
	}

	ruhs->nr_ruhs = nr_ruhs;
	ret = fdp_ruh_info(td, f, ruhs);
	if (ret) {
		log_err("fio: ruh info failed for %s (%d)\n",
			f->file_name, -ret);
		goto out;
	}

	/* 3단계: 사용자 지정 dp_ids 유무에 따라 RUH 선택 */
	if (td->o.dp_nr_ids == 0) {
		/* 사용자 지정 없음: 디바이스의 전체 RUH 사용 (최대 FIO_MAX_DP_IDS) */
		if (ruhs->nr_ruhs > FIO_MAX_DP_IDS)
			ruhs->nr_ruhs = FIO_MAX_DP_IDS;
	} else {
		/* 사용자 지정 ID 유효성 검사: 디바이스 RUH 범위 내인지 확인 */
		for (i = 0; i < td->o.dp_nr_ids; i++) {
			if (td->o.dp_ids[i] >= ruhs->nr_ruhs) {
				log_err("fio: for %s PID index %d must be smaller than %d\n",
					f->file_name, td->o.dp_ids[i],
					ruhs->nr_ruhs);
				ret = -EINVAL;
				goto out;
			}
		}
		ruhs->nr_ruhs = td->o.dp_nr_ids;
	}

	/* 공유 메모리에 최종 RUH 정보를 복사 */
	tmp = scalloc(1, sizeof(*tmp) + ruhs->nr_ruhs * sizeof(*tmp->plis));
	if (!tmp) {
		ret = -ENOMEM;
		goto out;
	}

	if (td->o.dp_nr_ids == 0) {
		/* 디바이스 보고 RUH 전체를 그대로 복사 */
		for (i = 0; i < ruhs->nr_ruhs; i++)
			tmp->plis[i] = ruhs->plis[i];

		tmp->nr_ruhs = ruhs->nr_ruhs;
		f->ruhs_info = tmp;
		free(ruhs);

		return 0;
	}

	/* 사용자 지정 인덱스에 해당하는 RUH의 placement ID만 선택 */
	tmp->nr_ruhs = td->o.dp_nr_ids;
	for (i = 0; i < td->o.dp_nr_ids; i++)
		tmp->plis[i] = ruhs->plis[td->o.dp_ids[i]];
	f->ruhs_info = tmp;
out:
	free(ruhs);
	return ret;
}

/* [한국어] RUH 스킴(scheme) 초기화 - dp_id_select=scheme일 때
 * 외부 스킴 파일에서 오프셋 범위 <-> 배치 ID 매핑 테이블을 로드한다.
 * 파일 형식: "시작오프셋,끝오프셋,placement_id" (줄 단위, CSV) */
static int init_ruh_scheme(struct thread_data *td, struct fio_file *f)
{
	struct fio_ruhs_scheme *ruh_scheme;
	FILE *scheme_fp;
	unsigned long long start, end;
	uint16_t pli;
	int ret = 0;

	if (td->o.dp_id_select != FIO_DP_SCHEME)
		return 0;

	/* Get the scheme from the file */
	scheme_fp = fopen(td->o.dp_scheme_file, "r");

	if (!scheme_fp) {
		log_err("fio: ruh scheme failed to open scheme file %s\n",
			td->o.dp_scheme_file);
		ret = -errno;
		goto out;
	}

	ruh_scheme = scalloc(1, sizeof(*ruh_scheme));
	if (!ruh_scheme) {
		ret = -ENOMEM;
		goto out_with_close_fp;
	}

	/* 스킴 파일에서 최대 DP_MAX_SCHEME_ENTRIES개의 항목을 파싱 */
	for (int i = 0;
		i < DP_MAX_SCHEME_ENTRIES && fscanf(scheme_fp, "%llu,%llu,%hu\n", &start, &end, &pli) == 3;
		i++) {

		ruh_scheme->scheme_entries[i].start_offset = start;
		ruh_scheme->scheme_entries[i].end_offset = end;
		ruh_scheme->scheme_entries[i].pli = pli;
		ruh_scheme->nr_schemes++;
	}

	/* 최대 항목 수를 초과하는 경우 경고 출력 */
	if (fscanf(scheme_fp, "%llu,%llu,%hu\n", &start, &end, &pli) == 3)
		log_info("fio: too many scheme entries in %s. Only the first %d scheme entries are applied\n",
			 td->o.dp_scheme_file,
			 DP_MAX_SCHEME_ENTRIES);

	f->ruhs_scheme = ruh_scheme;

out_with_close_fp:
	fclose(scheme_fp);
out:
	return ret;
}

/* [한국어] 데이터 배치 초기화 진입점 - 모든 파일에 대해 RUH 정보와 스킴을 초기화 */
int dp_init(struct thread_data *td)
{
	struct fio_file *f;
	int i, ret = 0;

	for_each_file(td, f, i) {
		ret = init_ruh_info(td, f);
		if (ret)
			break;

		ret = init_ruh_scheme(td, f);
		if (ret)
			break;
	}
	return ret;
}

/* [한국어] RUH 정보 및 스킴 메모리 해제 - 파일 종료 시 호출 */
void fdp_free_ruhs_info(struct fio_file *f)
{
	if (!f->ruhs_info)
		return;
	sfree(f->ruhs_info);
	f->ruhs_info = NULL;

	if (!f->ruhs_scheme)
		return;
	sfree(f->ruhs_scheme);
	f->ruhs_scheme = NULL;
}

/* [한국어] I/O 제출 시 데이터 배치 힌트(dtype/dspec) 설정
 * 쓰기 I/O에 대해 선택 정책에 따라 placement ID를 결정한다.
 *
 * 선택 정책:
 *   - FIO_DP_RR     : 라운드 로빈으로 plis 배열을 순환
 *   - FIO_DP_SCHEME : io_u의 오프셋이 해당하는 스킴 항목의 pli 사용
 *   - FIO_DP_RANDOM : 랜덤으로 plis 배열에서 선택 */
void dp_fill_dspec_data(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	struct fio_ruhs_info *ruhs = f->ruhs_info;
	int dspec;

	/* 쓰기가 아니거나 RUH 정보가 없으면 배치 힌트를 설정하지 않음 */
	if (!ruhs || io_u->ddir != DDIR_WRITE) {
		io_u->dtype = 0;
		io_u->dspec = 0;
		return;
	}

	if (td->o.dp_id_select == FIO_DP_RR) {
		/* 라운드 로빈: pli_loc을 순환하며 선택 */
		if (ruhs->pli_loc >= ruhs->nr_ruhs)
			ruhs->pli_loc = 0;

		dspec = ruhs->plis[ruhs->pli_loc++];
	} else if (td->o.dp_id_select == FIO_DP_SCHEME) {
		/* 스킴: io_u의 오프셋이 속하는 범위의 pli를 사용 */
		struct fio_ruhs_scheme *ruhs_scheme = f->ruhs_scheme;
		unsigned long long offset = io_u->offset;
		int i;

		for (i = 0; i < ruhs_scheme->nr_schemes; i++) {
			if (offset >= ruhs_scheme->scheme_entries[i].start_offset &&
			    offset < ruhs_scheme->scheme_entries[i].end_offset) {
				dspec = ruhs_scheme->scheme_entries[i].pli;
				break;
			}
		}

		/*
		 * If the write offset is not affected by any scheme entry,
		 * 0(default RUH) will be assigned to dspec
		 */
		/* 어떤 스킴 항목에도 해당하지 않으면 기본 RUH(0) 사용 */
		if (i == ruhs_scheme->nr_schemes)
			dspec = 0;
	} else {
		/* 랜덤: plis 배열에서 무작위 인덱스를 선택 */
		ruhs->pli_loc = rand_between(&td->fdp_state, 0, ruhs->nr_ruhs - 1);
		dspec = ruhs->plis[ruhs->pli_loc];
	}

	/* FDP이면 FDP_DIR_DTYPE, 스트림이면 STREAMS_DIR_DTYPE 설정 */
	io_u->dtype = td->o.dp_type == FIO_DP_FDP ? FDP_DIR_DTYPE : STREAMS_DIR_DTYPE;
	io_u->dspec = dspec;
	dprint(FD_IO, "dtype set to 0x%x, dspec set to 0x%x\n", io_u->dtype, io_u->dspec);
}
