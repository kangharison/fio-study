/*
 * fio - the flexible io tester
 *
 * Copyright (C) 2005 Jens Axboe <axboe@suse.de>
 * Copyright (C) 2006-2012 Jens Axboe <axboe@kernel.dk>
 *
 * The license below covers all files distributed with fio unless otherwise
 * noted in the file itself.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

/*
 * [한국어 설명] fio 프로그램 최상위 진입점 (fio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio(Flexible I/O Tester) 실행 파일의 `main()` 진입점 하나만을 포함하는
 * 의도적으로 매우 얇은(shim) 파일이다. 실제 초기화/파싱/실행/집계 로직은 각각
 * init.c, backend.c, client.c, server.c 등 별도 모듈에 분산되어 있으며,
 * fio.c는 이 모듈들을 "프로그램의 기동 순서"에 맞게 호출하는 지휘봉 역할만 한다.
 * 얇게 유지되는 이유는 fio가 단일 바이너리로 (1) CLI 로컬 실행, (2) `--server`
 * 서버 데몬, (3) `--client=host` 클라이언트 모드 세 가지를 동시에 지원해야 하고,
 * 이 분기 지점을 최상단에 단 하나만 두기 위함이다.
 * 프로그램의 종료 코드(0=성공/비0=실패) 계약도 이 파일에서 결정된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 바이너리 실행의 가장 바깥 껍질이다. ELF 엔트리(_start → libc __libc_start_main)
 * 다음에 가장 먼저 도달하는 사용자 코드가 바로 이 `main()`이다.
 * 호출 체인(로컬 모드):
 *   _start → __libc_start_main → main()
 *     → initialize_fio()         [init.c]   : envp 저장, 디버그/로그/메모리/뮤텍스 초기화
 *     → fio_server_create_sk_key() [server.c]: 서버-클라이언트 통신용 TLS 키
 *     → parse_options()          [init.c]   : 잡 파일·CLI 파싱, ioengine 로드, td 생성
 *     → setvbuf(stdout,_IOLBF)              : 스레드 출력 라인 원자성 확보
 *     → fio_time_init()          [gettime.c]: 시간 소스(CPU clock/CLOCK_MONOTONIC) 결정
 *     → fio_backend(NULL)        [backend.c]: 잡별 스레드/프로세스 fork, I/O 엔진 구동,
 *                                             get_io_u/td_io_queue/td_io_getevents 루프,
 *                                             show_run_stats()로 결과 집계·출력
 * 호출 체인(클라이언트 모드):
 *     → set_genesis_time()                  : 분산 시각 기준점 기록
 *     → fio_start_all_clients() [client.c]  : 원격 서버들에 잡 송신·실행 지시
 *     → fio_handle_clients(&fio_client_ops) : 각 서버의 status/ETA/결과 이벤트 수신·표시
 * 실행 컨텍스트: 호스트 유저스페이스, 단일 스레드(메인). 잡 스레드/프로세스는
 * fio_backend 내부에서 pthread_create 또는 fork(2)로 생성된다. 시그널 처리
 * (SIGTERM/SIGINT/SIGHUP/SIGUSR1/SIGUSR2)와 terminate_threads() 경로는 backend.c가
 * 담당하므로 이 파일에서는 등장하지 않는다. 아키텍처/컴파일러 sanity check도
 * init.c/arch/ 레이어에서 수행된다.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h        : 이 파일이 사용하는 모든 함수 선언(initialize_fio, parse_options,
 *                  fio_backend, nr_clients 전역 등)을 묶은 통합 헤더.
 * - init.c       : initialize_fio / deinitialize_fio / parse_options 정의.
 *                  parse_options()가 전역 `thread_data` 배열을 구성해 backend.c가
 *                  그대로 사용한다 — 즉 이 파일은 데이터를 직접 주고받지 않고
 *                  "전역 상태"를 매개로 간접 연결된다.
 * - backend.c    : fio_backend() 정의. 실질적인 I/O 루프·통계 수집.
 * - client.c     : fio_start_all_clients / fio_handle_clients / fio_client_ops 정의.
 *                  nr_clients 전역이 0보다 크면 이 경로로 분기.
 * - server.c     : fio_server_create_sk_key / fio_server_destroy_sk_key 정의.
 *                  서버/클라이언트 양쪽 모두에서 TLS 소켓 키가 필요하므로 main에서
 *                  선초기화·후해제한다.
 * - gettime.c    : fio_time_init. clock_gettime(CLOCK_MONOTONIC) 해상도 측정과
 *                  선택적 TSC(Time Stamp Counter) 보정.
 * 공유하는 핵심 자료구조:
 *   - `nr_clients` (전역 정수): 클라이언트 모드 판별 플래그. init.c/parse_options가 설정,
 *     이 파일이 읽음.
 *   - `fio_client_ops` (전역 struct): 클라이언트 이벤트 콜백 집합. client.c에 정의되고
 *     이 파일이 주소를 fio_handle_clients에 전달.
 *   - `thread_data[]` (전역 배열): parse_options가 채우고 fio_backend가 소비 —
 *     이 파일에서는 직접 건드리지 않지만, main의 호출 순서가 곧 이 배열의
 *     생명주기(생성→사용→해제)를 결정한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - main(argc, argv, envp): 프로그램 진입점. 초기화 → 소켓 키 → 옵션 파싱 →
 *   stdout 라인 버퍼링 → 시간 초기화 → (nr_clients ? 클라이언트 처리 : fio_backend) →
 *   소켓 키 해제 → deinitialize_fio → ret 반환. goto 레이블 `done_key`/`done`으로
 *   에러 단계별 부분 정리(partial cleanup) 경로를 구성한다.
 * 이 파일에는 구조체 정의가 없다(모두 외부 참조). 전역 변수도 선언하지 않는다 —
 * fio.h를 통해 타 모듈의 심볼을 참조만 한다.
 */

/* [한국어] fio의 통합 헤더. 이 파일이 호출하는 모든 외부 함수(initialize_fio,
 * fio_server_create_sk_key, parse_options, setvbuf 이외, fio_time_init,
 * set_genesis_time, fio_start_all_clients, fio_handle_clients, fio_backend,
 * fio_server_destroy_sk_key, deinitialize_fio)와 전역 심볼(nr_clients,
 * fio_client_ops)의 선언이 여기 한 줄을 통해 가시화된다.
 * 직접 <stdio.h>를 포함하지 않아도 되는 이유는 fio.h가 전역 환경 헤더를 재포함하기
 * 때문이다 — setvbuf/_IOLBF/stdout은 fio.h 경유의 <stdio.h>로 공급된다. */
#include "fio.h"

/*
 * [한국어]
 * main - fio 바이너리의 최상위 진입점
 *
 * @argc: 명령줄 인자 개수. 예) `fio --name=t --rw=read test.fio` → argc=4.
 *        libc 런타임이 커널의 execve(2) 스택으로부터 복원해 넘겨준다.
 * @argv: 명령줄 인자 문자열 배열. argv[0]은 실행 파일 경로, argv[argc]는 NULL.
 *        parse_options()에 그대로 전달되어 잡 파일·CLI 옵션으로 해석된다.
 * @envp: 환경 변수 배열(`KEY=VALUE\0` 문자열들, NULL 종단). POSIX 표준은 아니지만
 *        리눅스/BSD libc에서 지원되는 main의 3번째 인자. initialize_fio()가 내부에
 *        저장하여 fio의 일부 서브시스템(로그 경로, 기본 CPU 마스크 등)이 참조한다.
 * @return: 프로세스 종료 코드. 0=모든 잡 정상 종료/모든 클라이언트 성공,
 *          비0(여기서는 기본 1)=초기화/파싱/실행 중 오류. shell의 `$?`로 노출된다.
 *
 * 이 함수는 "프로그램의 기동·종료 뼈대"이며, 실제 I/O 로직은 전혀 포함하지 않는다.
 * 각 단계별로 실패 시 goto 레이블을 이용해 "이미 획득한 자원만" 해제하도록 설계되어
 * 있다(부분 정리, partial cleanup). 시그널 핸들러 설치, 아키텍처 sanity,
 * 잡 스레드/프로세스 스폰(fork/pthread_create), terminate_threads() 경로, 통계
 * 집계(show_run_stats)는 하위 호출(fio_backend)이 담당한다.
 *
 * 실행 컨텍스트:
 *   - 메인 프로세스, 단일 스레드. 이 시점에 아직 잡 스레드는 존재하지 않는다.
 *   - 진입 직후 libc에 의해 전역 생성자(__attribute__((constructor)))와 ioengine
 *     자동 등록(static register/unregister)이 이미 실행된 상태다.
 *
 * 호출 체인:
 *   _start → __libc_start_main → [main]
 *     → initialize_fio → fio_server_create_sk_key → parse_options → setvbuf
 *     → fio_time_init → (fio_start_all_clients → fio_handle_clients | fio_backend)
 *     → fio_server_destroy_sk_key → deinitialize_fio → return
 */
int main(int argc, char *argv[], char *envp[])
{
	/* [한국어] 프로세스 종료 코드. 명시적으로 성공을 기록하지 않는 한 "실패"가
	 * 기본값이 되도록 1로 초기화한다 — 에러 goto 경로(done_key/done)가 ret을
	 * 갱신하지 않고 빠져나가도 shell이 실패를 인지할 수 있다. 정상 경로에서는
	 * fio_handle_clients() 또는 fio_backend() 반환값으로 덮어쓰여진다. */
	int ret = 1;

	/* [한국어] initialize_fio(envp) [init.c]: fio 전역 환경을 가장 먼저 세팅한다.
	 * 내부에서 환경 변수 포인터 저장, log_info/log_err 스트림 결정, smalloc 풀
	 * 초기화, 전역 뮤텍스(stat_mutex 등) 생성, 디버그 플래그 파싱을 수행한다.
	 * 0 반환 = 성공. 비0 반환 시 아직 아무 자원도 획득되지 않았으므로 바로 return 1. */
	if (initialize_fio(envp))
		return 1;

	/* [한국어] fio_server_create_sk_key() [server.c]: 서버-클라이언트 통신에 쓰이는
	 * pthread_key_t(TLS 슬롯)를 생성한다. 서버 모드는 물론 클라이언트 모드에서도
	 * 연결 컨텍스트를 스레드 로컬에 보관해야 하므로 두 경로 공히 필요하다.
	 * 실패 시 initialize_fio만 되돌리면 되므로 `done` 레이블로 점프(소켓 키 해제는 스킵). */
	if (fio_server_create_sk_key())
		goto done;

	/* [한국어] parse_options(argc, argv) [init.c]: CLI 옵션과 잡 파일을 해석하여
	 * 전역 `thread_data` 배열(각 잡 1개 = 1 thread_data)을 구성한다.
	 * 부수효과: (1) ioengine 플러그인 로드(load_ioengine), (2) 옵션 검증,
	 * (3) 클라이언트 모드면 전역 `nr_clients` 증가, (4) --server 지정 시
	 * 내부에서 fio_start_server()를 호출하고 그대로 프로세스를 종료한다(서버 모드는
	 * 여기서 리턴하지 않음). 실패 시 소켓 키는 이미 생성되었으므로 `done_key`로 점프. */
	if (parse_options(argc, argv))
		goto done_key;

	/*
	 * line buffer stdout to avoid output lines from multiple
	 * threads getting mixed
	 */
	/* [한국어] setvbuf(stdout, NULL, _IOLBF, 0): stdout을 라인 버퍼링으로 전환.
	 *   - stdout    : 대상 스트림(FILE *).
	 *   - NULL      : libc가 내부 버퍼를 자동 할당하도록 위임.
	 *   - _IOLBF    : <stdio.h>의 라인 버퍼링 모드 매크로. '\n' 또는 버퍼 가득 참 시 flush.
	 *   - 0         : 크기 0 → 기본값 사용(BUFSIZ).
	 * 이유: fio는 잡 수만큼 스레드/프로세스를 만들고 모두 stdout에 진행률·ETA·
	 * 결과를 기록한다. 블록 버퍼링(파이프·리디렉션 시 기본)이면 여러 스레드의
	 * write 조각이 한 줄 안에서 섞일 수 있다. 라인 단위로 flush하면 단일
	 * write(2) 호출이 한 줄을 이루어 커널이 원자적으로 내보낸다. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* [한국어] fio_time_init() [gettime.c]: 시간 측정 서브시스템 초기화.
	 * 수행 내용: (1) clock_gettime(CLOCK_MONOTONIC) 해상도 측정,
	 * (2) CONFIG_TLS_THREAD·아키텍처별 TSC(rdtsc) 사용 가능 여부 판정,
	 * (3) 필요 시 TSC→ns 환산 계수 보정, (4) 에폭(epoch) 기준 시각 기록.
	 * 이후 모든 잡의 레이턴시/IOPS/대역폭 계산이 이 초기화에 의존한다.
	 * 참고: "set_epoch_time"류 동작은 내부에서 수행되며 별도 호출은 backend가 담당. */
	fio_time_init();

	/* [한국어] nr_clients (전역, client.c 정의): 원격 서버에 붙어 돌릴 클라이언트 수.
	 * parse_options에서 --client=host 옵션 1개당 1씩 증가한다. 0보다 크면
	 * 로컬 백엔드를 돌리지 않고 순수히 분산 드라이버 역할만 한다. */
	if (nr_clients) {
		/* [한국어] === 클라이언트(분산) 모드 진입 === */

		/* [한국어] set_genesis_time() [time.c]: 모든 클라이언트가 공유할 "창세 시각"을
		 * 지금 이 순간으로 기록. 각 원격 서버의 결과 타임스탬프를 이 기준에
		 * 상대화하여 다수 서버의 I/O 구간을 시각 축에서 정렬·비교할 수 있게 한다. */
		set_genesis_time();

		/* [한국어] fio_start_all_clients() [client.c]: 등록된 모든 원격 클라이언트에
		 * TCP/Unix 소켓으로 접속하고 잡 설정(인코딩된 옵션 패킷)을 송신하여
		 * 실행을 시작하게 한다. 하나라도 실패하면 전체 실행을 포기하고 `done_key`로
		 * 점프하여 이미 만든 소켓 키를 해제한 뒤 종료한다. */
		if (fio_start_all_clients())
			goto done_key;

		/* [한국어] fio_handle_clients(&fio_client_ops) [client.c]: 각 클라이언트로부터
		 * (a) 상태 전이(CONNECT/START/STOP), (b) ETA 프레임, (c) per-job 결과,
		 * (d) 최종 집계를 이벤트 루프(poll/epoll)로 수신하고 fio_client_ops의
		 * 콜백으로 디스패치한다. 모든 클라이언트가 정상 종료하면 0을 반환하며,
		 * 이 값이 바로 프로세스 종료 코드가 된다. */
		ret = fio_handle_clients(&fio_client_ops);
	} else
		/* [한국어] === 로컬 백엔드 모드 === fio_backend(NULL) [backend.c]:
		 * 서버 컨텍스트 없이(NULL) 로컬에서 직접 워크로드를 돌린다. 내부에서
		 * (1) 시그널 핸들러 설치(SIGTERM/SIGINT/SIGHUP/SIGUSR1/SIGUSR2)로
		 *     terminate_threads() 유도 경로 구성,
		 * (2) 잡별로 pthread_create 또는 fork(2) 호출(use_thread 옵션에 따름),
		 * (3) get_io_u → td_io_prep → td_io_queue → td_io_commit →
		 *     td_io_getevents → put_io_u의 I/O 유닛 생명주기 루프 구동,
		 * (4) 모든 잡 합류 후 show_run_stats() 호출로 텍스트/JSON 결과 출력,
		 * (5) 실패 잡 수 또는 0을 반환.
		 * 이 반환값이 main의 최종 종료 코드로 전파된다. */
		ret = fio_backend(NULL);

/* [한국어] done_key: parse_options·fio_start_all_clients 실패 시 진입.
 * 이미 fio_server_create_sk_key로 TLS 키를 만든 상태이므로 해제 경로가 필요하다. */
done_key:
	/* [한국어] fio_server_destroy_sk_key() [server.c]: pthread_key_delete로
	 * TLS 슬롯을 해제. 해당 키에 저장된 스레드별 값이 있다면 프로세스 종료 시
	 * OS가 회수하지만, 정상 종료 경로에서 pthread 리소스 누수 경고를 피하기 위해
	 * 명시적으로 해제한다. */
	fio_server_destroy_sk_key();

/* [한국어] done: fio_server_create_sk_key 실패 시 소켓 키가 없으므로
 * done_key를 건너뛰고 여기로 곧장 점프한다. */
done:
	/* [한국어] deinitialize_fio() [init.c]: initialize_fio의 정반대 동작.
	 * smalloc 풀 해제, 전역 뮤텍스 파괴, 로그 버퍼 flush·닫기, 로드된 ioengine
	 * 플러그인 dlclose 등을 수행한다. 이후 어떤 fio API도 호출하면 안 된다. */
	deinitialize_fio();

	/* [한국어] ret을 반환하며 프로세스 종료. libc가 atexit 훅 실행 후 커널에
	 * exit_group(2)로 프로세스를 회수한다. 0=성공, 1(기본)=초기화 실패 또는
	 * fio_handle_clients/fio_backend가 보고한 잡 실패. shell에서 `$?`로 관측 가능. */
	return ret;
}
