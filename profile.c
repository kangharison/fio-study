/*
 * [한국어] profile.c - fio 프로파일 시스템
 *
 * 이 파일은 사전 정의된 워크로드 프로파일을 등록, 검색, 로드하는 기능을 구현한다.
 * 프로파일은 미리 설정된 fio 명령줄 옵션 조합으로, 특정 워크로드 패턴을
 * 간편하게 재현할 수 있게 해준다.
 *
 * 주요 기능:
 *   1) register_profile()   - 프로파일을 전역 리스트에 등록하고 옵션을 추가
 *   2) unregister_profile() - 프로파일을 전역 리스트에서 제거
 *   3) load_profile()       - 이름으로 프로파일을 찾아 명령줄 옵션을 적용
 *   4) profile_add_hooks()  - 프로파일의 I/O 후크를 thread_data에 연결
 */
#include "fio.h"
#include "profile.h"
#include "debug.h"
#include "flist.h"
#include "options.h"

/* [한국어] 등록된 프로파일들의 전역 연결 리스트 헤드 */
static FLIST_HEAD(profile_list);

/* [한국어] 이름으로 프로파일을 검색하는 함수
 * profile_list를 순회하며 이름이 일치하는 프로파일을 반환한다. */
struct profile_ops *find_profile(const char *profile)
{
	struct profile_ops *ops = NULL;
	struct flist_head *n;

	flist_for_each(n, &profile_list) {
		ops = flist_entry(n, struct profile_ops, list);
		if (!strcmp(profile, ops->name))
			break;

		ops = NULL;
	}

	return ops;
}

/* [한국어] 프로파일을 로드하여 해당 명령줄 옵션을 적용하는 함수
 * prep_cmd()로 사전 준비 후 cmdline 옵션을 추가한다. */
int load_profile(const char *profile)
{
	struct profile_ops *ops;

	dprint(FD_PROFILE, "loading profile '%s'\n", profile);

	ops = find_profile(profile);
	if (ops) {
		if (ops->prep_cmd()) {
			log_err("fio: profile %s prep failed\n", profile);
			return 1;
		}
		add_job_opts(ops->cmdline, FIO_CLIENT_TYPE_CLI);
		return 0;
	}

	log_err("fio: profile '%s' not found\n", profile);
	return 1;
}

/* [한국어] 프로파일 고유 옵션들을 fio 옵션 시스템에 등록하는 내부 함수 */
static int add_profile_options(struct profile_ops *ops)
{
	struct fio_option *o;

	if (!ops->options)
		return 0;

	o = ops->options;
	while (o->name) {
		o->prof_name = ops->name;
		o->prof_opts = ops->opt_data;
		if (add_option(o))
			return 1;
		o++;
	}

	return 0;
}

/* [한국어] 프로파일을 전역 리스트에 등록하는 함수
 * 프로파일 옵션을 추가하고, profile_list에 연결하며,
 * --profile 옵션의 가능한 값으로 등록한다. */
int register_profile(struct profile_ops *ops)
{
	int ret;

	dprint(FD_PROFILE, "register profile '%s'\n", ops->name);

	ret = add_profile_options(ops);
	if (!ret) {
		flist_add_tail(&ops->list, &profile_list);
		add_opt_posval("profile", ops->name, ops->desc);
		return 0;
	}

	invalidate_profile_options(ops->name);
	return ret;
}

/* [한국어] 프로파일을 전역 리스트에서 제거하는 함수
 * 연결 리스트에서 분리하고, 관련 옵션을 무효화한다. */
void unregister_profile(struct profile_ops *ops)
{
	dprint(FD_PROFILE, "unregister profile '%s'\n", ops->name);
	flist_del(&ops->list);
	invalidate_profile_options(ops->name);
	del_opt_posval("profile", ops->name);
}

/* [한국어] 프로파일의 I/O 후크를 스레드 데이터에 연결하는 함수
 * 실행 중인 프로파일이 있으면 해당 io_ops를 td에 복사한다. */
void profile_add_hooks(struct thread_data *td)
{
	struct profile_ops *ops;

	if (!exec_profile)
		return;

	ops = find_profile(exec_profile);
	if (!ops)
		return;

	if (ops->io_ops) {
		td->prof_io_ops = *ops->io_ops;
		td->flags |= TD_F_PROFILE_OPS;
	}
}

/* [한국어] 프로파일의 td_init 콜백을 호출하는 함수
 * 스레드 초기화 시 프로파일별 초기화 로직을 실행한다. */
int profile_td_init(struct thread_data *td)
{
	struct prof_io_ops *ops = &td->prof_io_ops;

	if (ops->td_init)
		return ops->td_init(td);

	return 0;
}

/* [한국어] 프로파일의 td_exit 콜백을 호출하는 함수
 * 스레드 종료 시 프로파일별 정리 로직을 실행한다. */
void profile_td_exit(struct thread_data *td)
{
	struct prof_io_ops *ops = &td->prof_io_ops;

	if (ops->td_exit)
		ops->td_exit(td);
}
