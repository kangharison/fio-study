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
 * [한국어 설명] fio 프로젝트 메인 진입점 파일 (fio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio(Flexible I/O Tester)의 최상위 진입점(main 함수)을 포함한다.
 * fio는 리눅스/유닉스 시스템에서 디스크 I/O 성능을 벤치마킹하기 위한 도구로,
 * 다양한 I/O 패턴(순차, 랜덤, 혼합 등)과 I/O 엔진(libaio, io_uring, sync 등)을
 * 지원한다.
 *
 * === 전체 I/O 흐름에서의 위치 ===
 * 1. [fio.c - main()] ← 현재 파일. 프로그램 시작점.
 *    - 환경 초기화 (initialize_fio)
 *    - 서버 소켓 키 생성 (fio_server_create_sk_key)
 *    - 명령줄 옵션 파싱 (parse_options)
 *    - 시간 서브시스템 초기화 (fio_time_init)
 * 2. 클라이언트-서버 모드 분기:
 *    a. 클라이언트 모드 (nr_clients > 0):
 *       - 원격 fio 서버에 접속하여 작업을 전송하고 결과를 수집한다.
 *       - fio_start_all_clients() → fio_handle_clients()
 *    b. 로컬(백엔드) 모드 (nr_clients == 0):
 *       - 로컬에서 직접 I/O 워크로드를 실행한다.
 *       - fio_backend() → 스레드/프로세스 생성 → I/O 엔진 호출 → 결과 집계
 * 3. 종료 정리 (deinitialize_fio)
 *
 * === 주요 의존 헤더 ===
 * - fio.h: fio의 핵심 자료구조(thread_data, io_u 등)와 함수 선언을 포함하는
 *          통합 헤더 파일. 이 헤더를 통해 거의 모든 fio 서브시스템에 접근한다.
 */

/* fio의 통합 헤더 파일 포함 - 모든 핵심 자료구조와 함수 선언이 여기에 있다 */
#include "fio.h"

/*
 * [한국어 설명] main - fio 프로그램의 메인 진입점 함수
 *
 * @param argc: 명령줄 인자의 개수 (예: "fio test.fio" → argc=2)
 * @param argv: 명령줄 인자 문자열 배열 (argv[0]="fio", argv[1]="test.fio")
 * @param envp: 환경 변수 문자열 배열 (예: "PATH=/usr/bin", "HOME=/root" 등)
 * @return: 프로그램 종료 코드 (0=성공, 1=실패)
 *
 * 이 함수는 다음 순서로 fio를 실행한다:
 *   1) 전역 환경 초기화
 *   2) 서버 통신용 소켓 키 생성
 *   3) 명령줄 옵션 파싱 (잡 파일, --name, --ioengine 등)
 *   4) stdout 라인 버퍼링 설정
 *   5) 시간 서브시스템 초기화
 *   6) 클라이언트 모드 또는 로컬 백엔드 모드로 I/O 테스트 실행
 *   7) 자원 정리 후 종료
 */
int main(int argc, char *argv[], char *envp[])
{
	/*
	 * ret: 프로그램 종료 코드. 기본값 1(실패)로 초기화한다.
	 * 정상적으로 I/O 테스트가 완료되면 0으로 갱신된다.
	 */
	int ret = 1;

	/*
	 * initialize_fio(): fio의 전역 환경을 초기화한다.
	 * - 환경 변수(envp)를 내부에 저장
	 * - 디버그 레벨, 로그 시스템 설정
	 * - 메모리 할당기 초기화
	 * - 각종 전역 뮤텍스/락 초기화
	 * 실패 시 0이 아닌 값을 반환하며, 즉시 프로그램을 종료한다.
	 */
	if (initialize_fio(envp))
		return 1;

	/*
	 * fio_server_create_sk_key(): 서버 모드에서 사용하는
	 * 스레드 로컬 저장소(TLS) 소켓 키를 생성한다.
	 * fio는 클라이언트-서버 구조를 지원하며, 이 키는
	 * 각 스레드가 자신만의 서버 소켓 연결을 유지하는 데 사용된다.
	 * 실패 시 done 레이블로 점프하여 정리 후 종료한다.
	 */
	if (fio_server_create_sk_key())
		goto done;

	/*
	 * parse_options(): 명령줄 인자(argc, argv)를 파싱한다.
	 * - 잡(job) 파일 파싱 (예: test.fio)
	 * - 인라인 잡 옵션 처리 (--name, --rw, --bs, --ioengine 등)
	 * - 전역 옵션 처리 (--output, --debug, --server 등)
	 * - 클라이언트 모드인 경우 nr_clients 변수를 설정
	 * 파싱 실패 시 done_key 레이블로 점프하여 소켓 키 해제 후 종료한다.
	 */
	if (parse_options(argc, argv))
		goto done_key;

	/*
	 * line buffer stdout to avoid output lines from multiple
	 * threads getting mixed
	 */
	/*
	 * [한국어] stdout을 라인 버퍼링(_IOLBF) 모드로 설정한다.
	 * fio는 멀티스레드로 동작하므로, 여러 스레드가 동시에 stdout에
	 * 출력하면 줄이 뒤섞일 수 있다. 라인 버퍼링을 적용하면
	 * 개행 문자(\n)가 나올 때마다 버퍼를 플러시하여
	 * 한 줄 단위로 원자적(atomic) 출력을 보장한다.
	 * 인자: stdout=대상 스트림, NULL=시스템 자동 버퍼, _IOLBF=라인 버퍼링, 0=기본 크기
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/*
	 * fio_time_init(): 시간 측정 서브시스템을 초기화한다.
	 * - clock_gettime()에 사용할 클록 소스를 결정 (CLOCK_MONOTONIC 등)
	 * - 타이머 해상도를 확인하고 보정값을 계산
	 * I/O 레이턴시와 IOPS 측정의 정확도에 직접적으로 영향을 미친다.
	 */
	fio_time_init();

	/*
	 * nr_clients: 원격 fio 서버에 연결할 클라이언트 수.
	 * parse_options()에서 --client 옵션이 지정된 경우 이 값이 설정된다.
	 * 0보다 크면 클라이언트-서버 모드로 동작하고,
	 * 0이면 로컬에서 직접 I/O 테스트를 실행한다.
	 */
	if (nr_clients) {
		/*
		 * === 클라이언트 모드 ===
		 * 원격 fio 서버들에 접속하여 잡을 전송하고 결과를 수집한다.
		 * 분산 I/O 테스트 시나리오에서 사용된다.
		 */

		/*
		 * set_genesis_time(): 모든 클라이언트의 시작 시각을 동기화하기 위해
		 * "기준 시각(genesis time)"을 기록한다. 분산 환경에서
		 * 여러 서버의 결과를 시간축 기준으로 정렬/비교하는 데 사용된다.
		 */
		set_genesis_time();

		/*
		 * fio_start_all_clients(): 등록된 모든 원격 클라이언트에
		 * 접속하여 잡 설정을 전송하고 테스트 시작을 요청한다.
		 * 실패 시 done_key 레이블로 점프하여 정리 후 종료한다.
		 */
		if (fio_start_all_clients())
			goto done_key;

		/*
		 * fio_handle_clients(): 모든 클라이언트로부터 결과를 수신하고 처리한다.
		 * fio_client_ops는 클라이언트 이벤트 핸들러(콜백 함수 집합)로,
		 * 각 클라이언트의 상태 변화, 결과 데이터, ETA 정보 등을 처리한다.
		 * 반환값은 전체 테스트의 성공(0) 또는 실패(0이 아닌 값) 여부이다.
		 */
		ret = fio_handle_clients(&fio_client_ops);
	} else
		/*
		 * === 로컬(백엔드) 모드 ===
		 * fio_backend(): fio의 핵심 실행 함수. 로컬에서 직접 I/O 테스트를 수행한다.
		 * - 잡(job)별로 스레드 또는 프로세스를 생성
		 * - 각 스레드에서 지정된 I/O 엔진을 통해 읽기/쓰기 수행
		 * - I/O 완료 후 레이턴시, IOPS, 대역폭 등 통계를 집계
		 * - 결과를 포맷에 맞춰 출력 (일반 텍스트, JSON 등)
		 * 인자 NULL은 서버 연결 정보가 없음(로컬 실행)을 의미한다.
		 * 반환값은 테스트의 성공(0) 또는 실패(0이 아닌 값) 여부이다.
		 */
		ret = fio_backend(NULL);

/*
 * done_key 레이블: 소켓 키 해제가 필요한 경우 여기로 점프한다.
 * parse_options() 또는 fio_start_all_clients() 실패 시 도달한다.
 */
done_key:
	/*
	 * fio_server_destroy_sk_key(): 앞서 생성한 TLS 소켓 키를 해제한다.
	 * 메모리 누수를 방지하기 위해 반드시 호출해야 한다.
	 */
	fio_server_destroy_sk_key();

/*
 * done 레이블: 최종 정리를 수행하는 지점.
 * fio_server_create_sk_key() 실패 시 여기로 직접 점프한다.
 */
done:
	/*
	 * deinitialize_fio(): fio의 모든 전역 자원을 해제한다.
	 * - 할당된 메모리 해제
	 * - 뮤텍스/락 파괴
	 * - 로그 시스템 종료
	 * - I/O 엔진 플러그인 언로드
	 * 프로그램 종료 전 반드시 호출하여 깨끗하게 정리한다.
	 */
	deinitialize_fio();

	/*
	 * ret 값을 반환하여 프로그램을 종료한다.
	 * 0: 모든 I/O 테스트가 성공적으로 완료됨
	 * 1: 초기화 실패, 옵션 파싱 실패, 또는 테스트 실행 중 오류 발생
	 */
	return ret;
}
