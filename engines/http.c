/*
 * [한국어 설명] HTTP/HTTPS 오브젝트 스토리지 I/O 엔진 (http.c)
 *
 * === 파일의 역할 ===
 * libcurl easy interface로 HTTP(S) GET/PUT/DELETE/HEAD 요청을 보내 S3/Swift/WebDAV
 * 오브젝트 스토리지에 I/O를 수행하는 fio 엔진 "http"를 구현한다. io_u의 DDIR_WRITE는
 * HTTP PUT(= CURLOPT_UPLOAD)에, DDIR_READ는 HTTP GET(= CURLOPT_HTTPGET)에, DDIR_TRIM은
 * HTTP DELETE(= CURLOPT_CUSTOMREQUEST="DELETE")에 매핑된다. 요청 본문/응답 본문은
 * io_u->xfer_buf와 _http_read/_http_write 콜백을 통해 직접 복사되며, S3 모드에서는
 * AWS Signature V4(HMAC-SHA256 체인 + 페이로드 SHA256)로 헤더 서명을 붙이고, Swift
 * 모드에서는 x-auth-token과 MD5 etag를, WebDAV 모드에서는 CURLOPT_USERPWD 기반의
 * HTTP Basic/Digest 인증(CURLAUTH_ANY)을 사용한다. 객체 매핑 방식으로는 블록마다
 * 별도 객체를 만드는 object-per-block 모드(FIO_HTTP_OBJECT_BLOCK)와 파일 1개를 객체로
 * 두고 HTTP Range 헤더로 부분 접근하는 모드(FIO_HTTP_OBJECT_RANGE)를 지원한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * --ioengine=http로 선택되는 동적 엔진 플러그인이다. ioengine_ops에 FIO_SYNCIO 플래그가
 * 세팅되어 있어 backend가 queue() 호출 하나에서 I/O가 완료된 것으로 간주하고
 * FIO_Q_COMPLETED를 반환받으며(동기 엔진 계약), 따라서 getevents/event는 항상 0/NULL을
 * 돌려주는 no-op이다. 또한 FIO_DISKLESSIO 플래그로 로컬 파일 실체가 없어도(remote object)
 * 잡 파일 크기를 사용자 지정만으로 주행할 수 있다. libcurl easy 핸들(blocking)을 스레드당
 * 한 개 생성하므로 td->o.use_thread=1로 강제해 잡 단위 fork를 금지한다. 실행 컨텍스트는
 * 모두 fio 잡 스레드(유저스페이스)이며, curl_easy_perform() 내부에서 TCP 소켓 I/O가
 * blocking으로 수행된다.
 *
 * === 타 모듈과의 연결 ===
 * 상단: fio 코어(backend.c의 do_io() → td_io_queue() → td->io_ops->queue()) 경로로
 *        fio_http_queue()가 호출되고, init 경로에서는 ioengines.c가 setup/open_file을,
 *        종료 경로에서는 cleanup을 호출한다. verify 경로에서 _gen_base64_md5 계열은
 *        사용되지 않으며, 실제로는 S3 canonical request 내 페이로드 해시 용도로만 쓰인다.
 * 하단: libcurl (curl_easy_init/setopt/perform/getinfo/cleanup, curl_slist_*), OpenSSL
 *        (SHA256/MD5/HMAC_* APIs). HMAC 컨텍스트는 OpenSSL 버전에 따라 불투명(HMAC_CTX_new)
 *        /투명(HMAC_CTX 스택 + HMAC_CTX_init) 두 경로로 분기한다(CONFIG_HAVE_OPAQUE_HMAC_CTX).
 *        OpenSSL 3.0 deprecation 경고는 `#pragma GCC diagnostic ignored
 *        "-Wdeprecated-declarations"`로 억제(신 EVP_MAC API로 옮기지 않음).
 * 데이터 흐름: io_u->xfer_buf ↔ http_curl_stream(buf/pos/max) ↔ _http_read/_http_write
 *        콜백 ↔ libcurl 내부 TLS/TCP 버퍼 ↔ 원격 S3/Swift/WebDAV. S3 서명 체인은
 *        secret key → kDate → kRegion → kService → kSigning → signature를 순차 HMAC한다.
 * 공유 상태: http_options(td->eo)는 파싱 직후 잡 단위 읽기전용. http_data(td->io_ops_data)는
 *        스레드 1개가 소유하는 CURL* 래퍼이므로 락 불필요.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_http_setup(): CURL* 생성·옵션 적용(HTTPS 검증, Basic/Digest 인증, verbose 디버그,
 *   READ/WRITE/SEEK 콜백 등록)과 use_thread=1 강제.
 * - fio_http_queue(): io_u.ddir(READ/WRITE/TRIM)을 GET/PUT/DELETE로 매핑하고, S3/Swift
 *   서명 헤더를 붙인 뒤 curl_easy_perform()로 요청을 blocking 수행. 항상 FIO_Q_COMPLETED 반환.
 * - _add_aws_auth_header(): AWS SigV4 canonical request 구성, 서명 키 HMAC-SHA256 체인,
 *   Authorization 헤더 삽입. SSE-C와 security token 헤더도 여기서 처리.
 * - _add_swift_header(): OpenStack Swift의 x-auth-token과 etag(MD5) 헤더 부착.
 * - _http_read()/_http_write()/_http_seek(): libcurl이 업로드/다운로드 중 호출하는 버퍼
 *   콜백. io_u->xfer_buf를 http_curl_stream으로 감싸 pos 커서를 전진시킨다.
 * - _hmac(): OpenSSL HMAC-SHA256 one-shot 래퍼. SigV4 체인의 각 단계에서 반복 호출된다.
 * - struct http_options: 엔진 옵션(모드, HTTPS, host, Basic 인증 user/pass, S3 키·리전·
 *   SSE-C·security token·storage class, Swift 토큰, object_mode, verbose).
 * - struct http_data: 스레드당 CURL 핸들 1개.
 * - struct http_curl_stream: queue() 로컬 스택 버퍼 상태(buf/pos/max)로 콜백에 전달.
 */

/*
 * HTTP GET/PUT IO engine
 *
 * IO engine to perform HTTP(S) GET/PUT requests via libcurl-easy.
 *
 * Copyright (C) 2018 SUSE LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2 as published by the Free Software Foundation..
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <pthread.h>        /* [한국어] fio가 POSIX thread API를 통해 잡을 스레드로 돌릴 때 사용하는 헤더. 이 파일 자체는 pthread 호출을 직접 쓰진 않지만 fio.h/libcurl 내부에서 필요로 해서 포함된다. */
#include <time.h>           /* [한국어] time()/gmtime()/strftime()을 통해 AWS SigV4 서명에 필요한 ISO8601(x-amz-date) 타임스탬프를 생성하기 위해 필요. */
#include <curl/curl.h>      /* [한국어] libcurl easy interface의 CURL*, curl_easy_*, curl_slist_*, CURLOPT_* 옵션 상수 선언. HTTP 요청 송수신의 실제 엔진. */
#include <openssl/hmac.h>   /* [한국어] HMAC-SHA256 서명 체인(kDate→kRegion→kService→kSigning→signature)을 만들기 위한 HMAC_CTX/HMAC_Init_ex/Update/Final API. */
#include <openssl/sha.h>    /* [한국어] 페이로드 및 canonical request의 SHA-256 해시(x-amz-content-sha256, signed canonical request) 계산용. */
#include <openssl/md5.h>    /* [한국어] Swift의 etag 헤더와 S3 SSE-C 고객 키 MD5(base64) 계산용. */
#include "fio.h"            /* [한국어] thread_data/io_u/ioengine_ops/register_ioengine 등 fio 엔진 계약에 필요한 핵심 선언. */
#include "../optgroup.h"    /* [한국어] FIO_OPT_G_HTTP 등 엔진 옵션 그룹 상수 정의. options[] 테이블의 .group에 사용된다. */

/*
 * Silence OpenSSL 3.0 deprecated function warnings
 */
/* [한국어] OpenSSL 3.0 이후 HMAC_*/MD5/SHA256 one-shot API는 deprecated로 마킹됨. 이 엔진은
 * 신 EVP_MAC 기반 API로 포팅하지 않고 기존 호출을 그대로 두므로, 컴파일 경고를 전역적으로
 * 억제한다. 향후 OpenSSL 4에서 제거되면 EVP_MAC_init/update/final과 EVP_Q_digest로 전환 필요. */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

/* [한국어] HTTP 엔진이 사용하는 모드 상수들을 한 enum에 모아둔 정의.
 * 의미상 세 그룹(mode/https/object_mode)이 공존하지만 C enum의 타입만 공유되고 각각
 * 독립된 옵션 필드(http_options.mode, .https, .object_mode)에 저장된다. */
enum {
	FIO_HTTP_WEBDAV		= 0,	/* [한국어] http_mode=webdav: HTTP Basic/Digest 인증으로 PUT/GET/DELETE 수행. */
	FIO_HTTP_S3		= 1,	/* [한국어] http_mode=s3: AWS Signature V4 HMAC-SHA256 서명 헤더를 요청마다 계산. */
	FIO_HTTP_SWIFT		= 2,	/* [한국어] http_mode=swift: OpenStack Swift의 x-auth-token 기반 인증과 MD5 etag. */

	FIO_HTTPS_OFF		= 0,	/* [한국어] https=off: 평문 HTTP 사용 (http://...). */
	FIO_HTTPS_ON		= 1,	/* [한국어] https=on: TLS 사용 + 서버 인증서 검증(기본 libcurl 동작). */
	FIO_HTTPS_INSECURE	= 2,	/* [한국어] https=insecure: TLS는 쓰되 SSL_VERIFYPEER/HOST를 0으로 낮춰 자체 서명 인증서 허용(테스트용). */

	FIO_HTTP_OBJECT_BLOCK	= 0,	/* [한국어] object_mode=block: 블록 1개 = 객체 1개. 객체 이름에 offset/length가 박힌다(<file>_<off>_<len>). */
	FIO_HTTP_OBJECT_RANGE	= 1,	/* [한국어] object_mode=range: 객체 1개에 대해 HTTP Range 헤더로 부분 읽기(GET)만 수행. */
};

/* [한국어] HTTP 엔진의 스레드 단위 런타임 상태.
 * 설정자: fio_http_setup()이 calloc으로 할당해 td->io_ops_data에 저장.
 * 해제자: fio_http_cleanup()이 td 종료 시 curl_easy_cleanup 후 free.
 * 소유자: 잡 스레드 1개 (td->o.use_thread=1로 강제되어 프로세스 분기 없음). */
struct http_data {
	CURL *curl;
	/* [한국어] libcurl easy 핸들. 모든 요청에 재사용되어 TCP/TLS 연결 풀링과 DNS 캐시
	 * 혜택을 받는다.
	 * 설정자: fio_http_setup()에서 curl_easy_init()으로 생성 후 전역 CURLOPT_* 세팅.
	 * 읽는 자: fio_http_queue()가 CURLOPT_URL/READDATA/WRITEDATA/UPLOAD/HTTPGET/
	 *          CUSTOMREQUEST 등을 교체하며 요청마다 호출.
	 * 값 범위: NULL 아님(setup 실패 시 cleanup 경로로 진입). 종료 시 curl_easy_cleanup().
	 * 동기화: 잡 스레드 1개만 접근 — libcurl easy 핸들의 스레드 안전성은 핸들 단위로만
	 *         보장되므로, 다른 스레드와 공유 금지. */
};

/* [한국어] HTTP 엔진 전용 옵션 구조체. fio CLI/잡파일 파서가 options[] 테이블을 따라
 * 각 필드를 채운다. td->eo == (struct http_options*)로 접근.
 * 라이프사이클: 잡 파싱 시 생성 → 잡 종료까지 읽기 전용. */
struct http_options {
	void *pad;
	/* [한국어] fio 옵션 파서가 구조체 시작부에 삽입하는 포인터 크기 패딩.
	 * 설정자: fio 코어(옵션 덤프/복사 로직). 읽는 자: 엔진 코드에서는 사용 안 함.
	 * 값 범위: 예약됨. 동기화: 읽기 전용. */
	unsigned int https;
	/* [한국어] HTTPS 모드 선택 값(FIO_HTTPS_OFF/ON/INSECURE).
	 * 설정자: "https" 옵션 파서. 읽는 자: fio_http_setup()이 SSL_VERIFY*를 조정할 때,
	 *         fio_http_queue()가 URL 프리픽스(http://|https://)를 고를 때.
	 * 값 범위: 0/1/2. 동기화: 잡 파싱 후 불변. */
	char *host;
	/* [한국어] 요청 대상 호스트 문자열. "bucket.s3.amazonaws.com" 또는 로컬 테스트의
	 * "localhost"처럼 scheme 없이 저장된다. URI 경로는 io_u->file->file_name이 붙는다.
	 * 설정자: "http_host" 옵션. 읽는 자: fio_http_queue()에서 URL 조립, SigV4 canonical
	 *         request의 host 헤더.
	 * 값 범위: non-null 문자열. 기본값 "localhost". 동기화: 파싱 후 불변. */
	char *user;
	/* [한국어] HTTP Basic/Digest 인증 사용자명(WebDAV).
	 * 설정자: "http_user" 옵션. 읽는 자: fio_http_setup()이 CURLOPT_USERNAME으로 설정.
	 * 값 범위: NULL 가능(익명). 동기화: 파싱 후 불변. */
	char *pass;
	/* [한국어] HTTP Basic/Digest 인증 비밀번호.
	 * 설정자: "http_pass" 옵션. 읽는 자: fio_http_setup()이 CURLOPT_PASSWORD로 설정.
	 * 값 범위: NULL 가능. user와 pass 둘 다 세팅돼야 CURLAUTH_ANY 활성화. */
	char *s3_key;
	/* [한국어] AWS Secret Access Key. SigV4 체인의 초기 키 "AWS4"+s3_key로 사용.
	 * 설정자: "http_s3_key". 읽는 자: _add_aws_auth_header()에서 kDate HMAC 초기 키.
	 * 값 범위: 기본 "". 동기화: 파싱 후 불변(민감 정보). */
	char *s3_keyid;
	/* [한국어] AWS Access Key ID. Authorization 헤더의 Credential 부분에 들어간다.
	 * 설정자: "http_s3_keyid". 읽는 자: _add_aws_auth_header(). 값 범위: 기본 "". */
	char *s3_security_token;
	/* [한국어] AWS STS 임시 보안 토큰. 설정 시 x-amz-security-token 헤더와
	 * SignedHeaders/canonical 목록에 "x-amz-security-token"이 추가된다.
	 * 설정자: "http_s3_security_token". 값 범위: 기본 "". NULL 비교로 유무 판정. */
	char *s3_region;
	/* [한국어] AWS 리전(kRegion 스코프). SigV4 Credential과 string-to-sign에 포함.
	 * 설정자: "http_s3_region". 기본 "us-east-1". 값 범위: "us-west-2" 등. */
	char *s3_sse_customer_key;
	/* [한국어] S3 SSE-C 고객 제공 암호화 키(32바이트 원본). base64/MD5(base64)로 변환되어
	 * x-amz-server-side-encryption-customer-{key,key-md5} 헤더에 삽입되고 SigV4
	 * canonical 헤더에도 포함된다. 설정자: "http_s3_sse_customer_key". 기본 "". */
	char *s3_sse_customer_algorithm;
	/* [한국어] SSE-C 알고리즘 이름. 기본 "AES256".
	 * 설정자: "http_s3_sse_customer_algorithm". x-amz-server-side-encryption-customer-
	 * algorithm 헤더 값. */
	char *s3_storage_class;
	/* [한국어] S3 스토리지 클래스(STANDARD/REDUCED_REDUNDANCY/GLACIER 등).
	 * 설정자: "http_s3_storage_class". 기본 "STANDARD". x-amz-storage-class 헤더. */
	char *swift_auth_token;
	/* [한국어] OpenStack Swift 인증 토큰. x-auth-token 헤더로 송신.
	 * 설정자: "http_swift_auth_token". 값 범위: 기본 "". */
	int verbose;
	/* [한국어] libcurl 디버깅 상세도. 0=조용, 1=CURLOPT_VERBOSE, >1은 추가로 trace 콜백.
	 * 설정자: "http_verbose". 읽는 자: fio_http_setup(). */
	unsigned int mode;
	/* [한국어] 프로토콜 모드(FIO_HTTP_WEBDAV/S3/SWIFT). fio_http_queue()가 이 값으로
	 * 서명/헤더 함수를 분기한다. 설정자: "http_mode". 기본 webdav. */
	unsigned int object_mode;
	/* [한국어] 객체 매핑 방식(FIO_HTTP_OBJECT_BLOCK/RANGE).
	 * 설정자: "http_object_mode". 읽는 자: queue()가 object_path 조립과 Range 헤더
	 * 부착 여부를 결정. */
};

/* [한국어] libcurl read/write/seek 콜백에 userdata로 전달되는 버퍼 상태.
 * fio_http_queue() 스택에 자동 변수로 잡히고, 요청 1회가 끝나면 수명이 종료된다. */
struct http_curl_stream {
	char *buf;
	/* [한국어] io_u->xfer_buf를 그대로 가리키는 포인터(복사 없음). PUT(업로드)에서는
	 * libcurl이 여기서 읽고, GET(다운로드)에서는 libcurl이 여기에 쓴다.
	 * 설정자: fio_http_queue(). 읽는 자: _http_read/_http_write/_http_seek.
	 * 값 범위: 유효한 fio I/O 버퍼. 동기화: 단일 스레드. */
	size_t pos;
	/* [한국어] 현재 전송/수신 오프셋(바이트). 콜백이 처리할 때마다 전진한다.
	 * 설정자: memset으로 0 초기화 → 콜백들이 증가. 읽는 자: 같은 콜백들.
	 * 값 범위: [0, max]. 동기화: 단일 스레드. */
	size_t max;
	/* [한국어] 전송/수신 가능한 최대 바이트 수(= io_u->xfer_buflen).
	 * 설정자: fio_http_queue(). 읽는 자: 경계 체크용. 동기화: 단일 스레드. */
};

/* [한국어] fio 옵션 파서 테이블. 각 엔트리는 (CLI/잡파일 옵션 이름, 저장 위치, 타입,
 * 기본값, 허용 값)을 기술한다. fio 코어가 이 테이블을 읽어 http_options 필드를 자동으로
 * 채워준다. .group=FIO_OPT_G_HTTP로 --enghelp에서 그룹화된다. */
static struct fio_option options[] = {
	{
		.name     = "https",
		.lname    = "https",
		.type     = FIO_OPT_STR,
		.help     = "Enable https",
		.off1     = offsetof(struct http_options, https),
		.def      = "off",
		.posval = {
			  { .ival = "off",
			    .oval = FIO_HTTPS_OFF,
			    .help = "No HTTPS",
			  },
			  { .ival = "on",
			    .oval = FIO_HTTPS_ON,
			    .help = "Enable HTTPS",
			  },
			  { .ival = "insecure",
			    .oval = FIO_HTTPS_INSECURE,
			    .help = "Enable HTTPS, disable peer verification",
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_host",
		.lname    = "http_host",
		.type     = FIO_OPT_STR_STORE,
		.help     = "Hostname (S3 bucket)",
		.off1     = offsetof(struct http_options, host),
		.def	  = "localhost",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_user",
		.lname    = "http_user",
		.type     = FIO_OPT_STR_STORE,
		.help     = "HTTP user name",
		.off1     = offsetof(struct http_options, user),
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_pass",
		.lname    = "http_pass",
		.type     = FIO_OPT_STR_STORE,
		.help     = "HTTP password",
		.off1     = offsetof(struct http_options, pass),
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_s3_key",
		.lname    = "S3 secret key",
		.type     = FIO_OPT_STR_STORE,
		.help     = "S3 secret key",
		.off1     = offsetof(struct http_options, s3_key),
		.def	  = "",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_s3_keyid",
		.lname    = "S3 key id",
		.type     = FIO_OPT_STR_STORE,
		.help     = "S3 key id",
		.off1     = offsetof(struct http_options, s3_keyid),
		.def	  = "",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_s3_security_token",
		.lname    = "S3 security token",
		.type     = FIO_OPT_STR_STORE,
		.help     = "S3 security token",
		.off1     = offsetof(struct http_options, s3_security_token),
		.def	  = "",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_swift_auth_token",
		.lname    = "Swift auth token",
		.type     = FIO_OPT_STR_STORE,
		.help     = "OpenStack Swift auth token",
		.off1     = offsetof(struct http_options, swift_auth_token),
		.def	  = "",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_s3_region",
		.lname    = "S3 region",
		.type     = FIO_OPT_STR_STORE,
		.help     = "S3 region",
		.off1     = offsetof(struct http_options, s3_region),
		.def	  = "us-east-1",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_s3_sse_customer_key",
		.lname    = "SSE Customer Key",
		.type     = FIO_OPT_STR_STORE,
		.help     = "S3 SSE Customer Key",
		.off1     = offsetof(struct http_options, s3_sse_customer_key),
		.def	  = "",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_s3_sse_customer_algorithm",
		.lname    = "SSE Customer Algorithm",
		.type     = FIO_OPT_STR_STORE,
		.help     = "S3 SSE Customer Algorithm",
		.off1     = offsetof(struct http_options, s3_sse_customer_algorithm),
		.def	  = "AES256",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_s3_storage_class",
		.lname    = "S3 Storage class",
		.type     = FIO_OPT_STR_STORE,
		.help     = "S3 Storage Class",
		.off1     = offsetof(struct http_options, s3_storage_class),
		.def	  = "STANDARD",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_mode",
		.lname    = "Request mode to use",
		.type     = FIO_OPT_STR,
		.help     = "Whether to use WebDAV, Swift, or S3",
		.off1     = offsetof(struct http_options, mode),
		.def	  = "webdav",
		.posval = {
			  { .ival = "webdav",
			    .oval = FIO_HTTP_WEBDAV,
			    .help = "WebDAV server",
			  },
			  { .ival = "s3",
			    .oval = FIO_HTTP_S3,
			    .help = "S3 storage backend",
			  },
			  { .ival = "swift",
			    .oval = FIO_HTTP_SWIFT,
			    .help = "OpenStack Swift storage",
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_verbose",
		.lname    = "HTTP verbosity level",
		.type     = FIO_OPT_INT,
		.help     = "increase http engine verbosity",
		.off1     = offsetof(struct http_options, verbose),
		.def	  = "0",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = "http_object_mode",
		.lname    = "Object mode to use",
		.type     = FIO_OPT_STR,
		.help     = "How to structure objects when issuing HTTP requests",
		.off1     = offsetof(struct http_options, object_mode),
		.def	  = "block",
		.posval = {
			  { .ival = "block",
			    .oval = FIO_HTTP_OBJECT_BLOCK,
			    .help = "One object per block",
			  },
			  { .ival = "range",
			    .oval = FIO_HTTP_OBJECT_RANGE,
			    .help = "One object per file, range reads per block",
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_HTTP,
	},
	{
		.name     = NULL,
	},
};

/*
 * [한국어]
 * _aws_uriencode - AWS SigV4 canonical URI 인코딩 수행.
 *
 * @uri: 원본 경로 문자열 (예: "/my-bucket/obj_0_4096").
 * @return: RFC3986 unreserved를 제외한 문자는 %HH로 퍼센트 인코딩한 새 문자열(malloc).
 *          호출자(_add_aws_auth_header)가 free() 책임. 실패 시 NULL.
 *
 * SigV4는 canonical request의 URI 구간이 AWS 규칙(언리저브드[A-Za-z0-9_.~-]와 '/'를 제외한
 * 모든 바이트를 퍼센트 인코딩)에 정확히 맞아야 서명 불일치가 나지 않는다. libcurl 기본
 * 인코딩과 규칙이 다를 수 있어 자체 구현.
 * 실행 컨텍스트: 잡 스레드, queue() 경로에서만 호출.
 * 호출 체인: fio_http_queue() → _add_aws_auth_header() → [이 함수]
 */
static char *_aws_uriencode(const char *uri)
{
	size_t bufsize = 1024;                                    /* [한국어] 고정 버퍼 크기. URI가 이 이상이면 실패로 처리(경로가 매우 길 때만). */
	char *r = malloc(bufsize);                                /* [한국어] 결과 문자열용 힙 버퍼 할당. 호출자가 free. */
	char c;                                                   /* [한국어] 현재 바이트. uri[i]==0이면 루프 종료 신호. */
	int i, n;                                                 /* [한국어] i=입력 인덱스, n=출력 인덱스. */
	const char *hex = "0123456789ABCDEF";                     /* [한국어] 퍼센트 인코딩용 대문자 16진표. SigV4는 대문자 hex 요구. */

	if (!r) {                                                 /* [한국어] malloc 실패 시 상위에 NULL 전파. */
		log_err("malloc failed\n");                       /* [한국어] fio 공용 에러 로거. */
		return NULL;                                      /* [한국어] 호출자는 NULL 체크 없이 dereference하므로 상위에서 크래시 가능 — 방어적 코딩 필요 구간. */
	}

	n = 0;                                                    /* [한국어] 출력 커서 초기화. */
	for (i = 0; (c = uri[i]); i++) {                          /* [한국어] NUL 종단까지 한 바이트씩 스캔. */
		if (n > bufsize-5) {                              /* [한국어] %HH(3바이트) + 안전 마진을 위한 경계 체크. */
			log_err("encoding the URL failed\n");     /* [한국어] 버퍼 오버플로 방지: 실패를 상위에 보고. */
			free(r);                                  /* [한국어] 누수 방지. */
			return NULL;                              /* [한국어] 호출자(SigV4 빌더)는 NULL 처리 부재 — 운영상 발생 가능성 낮지만 리스크 존재. */
		}

		if ( (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
		|| (c >= '0' && c <= '9') || c == '_' || c == '-'
		|| c == '~' || c == '.' || c == '/')              /* [한국어] SigV4 unreserved + '/' 는 그대로 통과. RFC3986 + AWS 예외('/'). */
			r[n++] = c;                               /* [한국어] 있는 그대로 복사. */
		else {
			r[n++] = '%';                             /* [한국어] 퍼센트 접두. */
			r[n++] = hex[(c >> 4 ) & 0xF];            /* [한국어] 상위 니블. */
			r[n++] = hex[c & 0xF];                    /* [한국어] 하위 니블. */
		}
	}
	r[n++] = 0;                                               /* [한국어] C 문자열 종단. */
	return r;                                                 /* [한국어] 호출자에게 소유권 이전. */
}

/*
 * [한국어]
 * _conv_hex - 바이너리 버퍼를 소문자 16진 문자열로 변환(힙 할당).
 *
 * @p: 원본 바이트 버퍼.
 * @len: 바이트 길이.
 * @return: `len*2+1` 바이트의 NUL-종단 문자열. 호출자가 free 책임.
 *
 * SigV4 canonical request의 x-amz-content-sha256과 Authorization Signature 값은
 * 소문자 hex이어야 하므로 대문자 _aws_uriencode()용 표와 별도의 소문자 테이블을 쓴다.
 * 호출 체인: _gen_hex_sha256 / _gen_hex_md5 / _add_aws_auth_header → [이 함수]
 */
static char *_conv_hex(const unsigned char *p, size_t len)
{
	char *r;                                                  /* [한국어] 출력 문자열 포인터. */
	int i,n;                                                  /* [한국어] 입력/출력 인덱스. */
	const char *hex = "0123456789abcdef";                     /* [한국어] 소문자 hex 표(대소문자 규약 준수). */
	r = malloc(len * 2 + 1);                                  /* [한국어] 각 바이트당 2자 + NUL. 실패 체크 없음(운영상 치명적이지만 원본 유지). */
	n = 0;                                                    /* [한국어] 출력 커서. */
	for (i = 0; i < len; i++) {                               /* [한국어] 전체 바이트 순회. */
		r[n++] = hex[(p[i] >> 4 ) & 0xF];                 /* [한국어] 상위 니블. */
		r[n++] = hex[p[i] & 0xF];                         /* [한국어] 하위 니블. */
	}
	r[n] = 0;                                                 /* [한국어] NUL 종단. */

	return r;                                                 /* [한국어] 호출자에게 소유권 이전. */
}

/*
 * [한국어]
 * _gen_hex_sha256 - 메시지의 SHA-256 해시를 소문자 hex 문자열로 반환.
 *
 * @p: 메시지 바이트(NUL 종단 아니어도 됨).
 * @len: 메시지 길이.
 * @return: 64자 hex 문자열(malloc). 호출자가 free.
 *
 * SigV4: x-amz-content-sha256 헤더 값과 canonical request의 hashed payload, 그리고
 * 상위 canonical request 자체의 hash(= csha)에 사용된다. 빈 메시지("", 0)도 규정된
 * "e3b0c44..." 해시를 반환하도록 호출된다(GET/DELETE 경로).
 */
static char *_gen_hex_sha256(const char *p, size_t len)
{
	unsigned char hash[SHA256_DIGEST_LENGTH];                 /* [한국어] 32바이트 raw 해시 저장 공간(스택). */

	SHA256((unsigned char*)p, len, hash);                     /* [한국어] OpenSSL one-shot SHA-256. */
	return _conv_hex(hash, SHA256_DIGEST_LENGTH);             /* [한국어] 소문자 hex 문자열로 변환해 반환. */
}

/*
 * [한국어]
 * _gen_hex_md5 - 메시지의 MD5 해시를 소문자 hex 문자열로 반환.
 *
 * @p, @len: 해시 대상.
 * @return: 32자 hex 문자열(malloc). 호출자 free.
 *
 * Swift 모드의 etag 헤더(무결성 체크 용도)에만 쓰인다. MD5는 암호학적 용도로 부적합하나
 * Swift etag는 프로토콜 규정이므로 유지.
 */
static char *_gen_hex_md5(const char *p, size_t len)
{
	unsigned char hash[MD5_DIGEST_LENGTH];                    /* [한국어] 16바이트 MD5 raw 저장. */

	MD5((unsigned char*)p, len, hash);                        /* [한국어] OpenSSL one-shot MD5. */
	return _conv_hex(hash, MD5_DIGEST_LENGTH);                /* [한국어] hex 변환. */
}

/*
 * [한국어]
 * _conv_base64_encode - 바이너리 버퍼를 표준 Base64로 인코딩(malloc).
 *
 * @p, @len: 원본 바이너리. @return: NUL-종단 Base64 문자열. 호출자 free.
 *
 * S3 SSE-C 헤더는 고객 키(32바이트 원본)와 그 MD5를 Base64로 보내야 한다. OpenSSL의
 * EVP_EncodeBlock을 쓰지 않고 자체 테이블로 직접 인코딩(의존성 최소화).
 * 호출 체인: _gen_base64_md5 / _add_aws_auth_header → [이 함수]
 */
static char *_conv_base64_encode(const unsigned char *p, size_t len)
{
	char *r, *ret;                                            /* [한국어] r=전진 커서, ret=버퍼 시작(반환용). */
	int i;                                                    /* [한국어] 입력 인덱스. */
	static const char sEncodingTable[] = {                    /* [한국어] 표준 Base64 알파벳(A-Z a-z 0-9 + /). URL-safe 아님. */
		'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
		'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
		'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
		'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
		'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
		'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
		'w', 'x', 'y', 'z', '0', '1', '2', '3',
		'4', '5', '6', '7', '8', '9', '+', '/'
	};

	size_t out_len = 4 * ((len + 2) / 3);                     /* [한국어] 3바이트→4문자 블록 변환 후 패딩 포함 길이. */
	ret = r = malloc(out_len + 1);                            /* [한국어] +1은 NUL. 실패 체크 없음(원본 유지). */

	for (i = 0; i < len - 2; i += 3) {                        /* [한국어] 전체 3바이트 묶음을 한 번에 처리. i+2<len인 동안 루프. */
		*r++ = sEncodingTable[(p[i] >> 2) & 0x3F];        /* [한국어] 첫 6비트. */
		*r++ = sEncodingTable[((p[i] & 0x3) << 4) | ((int) (p[i + 1] & 0xF0) >> 4)]; /* [한국어] 2+4비트 결합. */
		*r++ = sEncodingTable[((p[i + 1] & 0xF) << 2) | ((int) (p[i + 2] & 0xC0) >> 6)]; /* [한국어] 4+2비트 결합. */
		*r++ = sEncodingTable[p[i + 2] & 0x3F];           /* [한국어] 마지막 6비트. */
	}

	if (i < len) {                                            /* [한국어] 1 또는 2 바이트가 남은 패딩 처리. */
		*r++ = sEncodingTable[(p[i] >> 2) & 0x3F];        /* [한국어] 첫 6비트는 공통. */
		if (i == (len - 1)) {                             /* [한국어] 남은 바이트가 정확히 1개. */
			*r++ = sEncodingTable[((p[i] & 0x3) << 4)];/* [한국어] 하위 2비트 + 0으로 채움. */
			*r++ = '=';                               /* [한국어] 패딩 '=' 1개. */
		} else {                                          /* [한국어] 남은 바이트 2개. */
			*r++ = sEncodingTable[((p[i] & 0x3) << 4) | ((int) (p[i + 1] & 0xF0) >> 4)]; /* [한국어] 2+4비트. */
			*r++ = sEncodingTable[((p[i + 1] & 0xF) << 2)];/* [한국어] 4비트 + 0 패드. */
		}
		*r++ = '=';                                       /* [한국어] 두 경우 모두 마지막 '=' 하나 더. */
	}

	ret[out_len]=0;                                           /* [한국어] NUL 종단. */
	return ret;                                               /* [한국어] 소유권 이전. */
}

/*
 * [한국어]
 * _gen_base64_md5 - 입력 버퍼의 MD5 해시를 Base64로 반환.
 *
 * @p, @len: 해시 대상. @return: 24자 Base64 문자열(malloc).
 *
 * S3 SSE-C의 x-amz-server-side-encryption-customer-key-md5 헤더 값 생성.
 */
static char *_gen_base64_md5(const unsigned char *p, size_t len)
{
	unsigned char hash[MD5_DIGEST_LENGTH];                    /* [한국어] 16바이트 MD5 원본. */
	MD5((unsigned char*)p, len, hash);                        /* [한국어] OpenSSL MD5 one-shot. */
	return _conv_base64_encode(hash, MD5_DIGEST_LENGTH);      /* [한국어] Base64 인코딩. */
}

/*
 * [한국어]
 * _hmac - HMAC-SHA256(key, data) 한 번 계산. SigV4 서명 키 파생 체인에서 반복 호출.
 *
 * @md: 출력 버퍼(SHA256_DIGEST_LENGTH=32 바이트 이상). 결과 MAC이 여기에 덮어쓰인다.
 * @key: HMAC 키(바이너리 허용).
 * @key_len: 키 바이트 수.
 * @data: HMAC 메시지(NUL-종단 문자열; 길이는 strlen으로 측정).
 *
 * OpenSSL 1.1+ 에서는 HMAC_CTX가 opaque이므로 HMAC_CTX_new/free를 써야 하고, 이전 버전에서는
 * 스택에 HMAC_CTX를 잡고 HMAC_CTX_init/cleanup으로 관리한다. configure가 정의하는
 * CONFIG_HAVE_OPAQUE_HMAC_CTX로 분기. (OpenSSL 3.0에서는 EVP_MAC이 권장되지만 여기서는
 * deprecated 경고를 억제하고 구 API를 그대로 사용.)
 * 호출 체인: _add_aws_auth_header() → [이 함수] ×5 (kDate/kRegion/kService/kSigning/sig)
 */
static void _hmac(unsigned char *md, void *key, int key_len, char *data) {
#ifndef CONFIG_HAVE_OPAQUE_HMAC_CTX
	HMAC_CTX _ctx;                                            /* [한국어] 구 OpenSSL: 스택에 ctx 할당(ABI 노출됨). */
#endif
	HMAC_CTX *ctx;                                            /* [한국어] 실제 API에 넘길 포인터. 두 분기에서 각각 다른 소스. */
	unsigned int hmac_len;                                    /* [한국어] HMAC_Final이 실제 MAC 길이를 여기로 돌려준다(SHA-256이면 32). */

#ifdef CONFIG_HAVE_OPAQUE_HMAC_CTX
	ctx = HMAC_CTX_new();                                     /* [한국어] OpenSSL 1.1+: 힙 할당 및 초기화. */
#else
	ctx = &_ctx;                                              /* [한국어] 스택 ctx 주소. */
	/* work-around crash in certain versions of libssl */
	HMAC_CTX_init(ctx);                                       /* [한국어] 일부 libssl 버전에서 미초기화 사용 시 크래시 회피. */
#endif
	HMAC_Init_ex(ctx, key, key_len, EVP_sha256(), NULL);      /* [한국어] HMAC-SHA256으로 키 설정. engine=NULL(기본). */
	HMAC_Update(ctx, (unsigned char*)data, strlen(data));     /* [한국어] 메시지 누적. SigV4 단계 문자열(날짜/리전/서비스/"aws4_request"/string-to-sign). */
	HMAC_Final(ctx, md, &hmac_len);                           /* [한국어] MAC을 md에 출력. */
#ifdef CONFIG_HAVE_OPAQUE_HMAC_CTX
	HMAC_CTX_free(ctx);                                       /* [한국어] 힙 해제. */
#else
	HMAC_CTX_cleanup(ctx);                                    /* [한국어] 내부 리소스 해제(스택 ctx는 자동 소멸). */
#endif
}

/*
 * [한국어]
 * _curl_trace - libcurl CURLOPT_DEBUGFUNCTION 콜백. http_verbose>1일 때 등록된다.
 *
 * @handle: libcurl easy 핸들(사용 안 함).
 * @type: 이벤트 종류(TEXT/HEADER/DATA/SSL_DATA in/out 등).
 * @data, @size: 로그 메시지. HEADER/DATA는 raw 바이트일 수 있음.
 * @userp: 사용자 포인터(사용 안 함).
 * @return: 0 고정(libcurl 규약).
 *
 * SSL_DATA_*는 무시하고 텍스트/헤더/데이터만 stderr/fio 로그에 찍어 디버깅을 돕는다.
 * 실행 컨텍스트: curl_easy_perform() 내부(잡 스레드).
 */
static int _curl_trace(CURL *handle, curl_infotype type,
	     char *data, size_t size,
	     void *userp)
{
	const char *text;                                         /* [한국어] 방향/종류 레이블 문자열. */
	(void)handle; /* prevent compiler warning */              /* [한국어] 미사용 인자 경고 억제. */
	(void)userp;                                              /* [한국어] 위와 동일. */

	switch (type) {
	case CURLINFO_TEXT:                                       /* [한국어] libcurl 내부 디버그 텍스트. */
		fprintf(stderr, "== Info: %s", data);             /* [한국어] stderr로 즉시 출력. */
		fio_fallthrough;                                  /* [한국어] 컴파일러의 fallthrough 허용 마크(-Wimplicit-fallthrough 무음). */
	default:
	case CURLINFO_SSL_DATA_OUT:                               /* [한국어] TLS raw 바이트(암호문) — 무시. */
	case CURLINFO_SSL_DATA_IN:                                /* [한국어] 위와 동일. */
		return 0;                                         /* [한국어] 처리 종료. */

	case CURLINFO_HEADER_OUT:                                 /* [한국어] 우리가 서버로 보낸 HTTP 헤더. */
		text = "=> Send header";
		break;
	case CURLINFO_DATA_OUT:                                   /* [한국어] 보낸 본문(PUT 페이로드 등). */
		text = "=> Send data";
		break;
	case CURLINFO_HEADER_IN:                                  /* [한국어] 수신한 응답 헤더. */
		text = "<= Recv header";
		break;
	case CURLINFO_DATA_IN:                                    /* [한국어] 수신한 응답 본문. */
		text = "<= Recv data";
		break;
	}

	log_info("%s: %s", text, data);                           /* [한국어] fio 표준 로그로 출력. */
	return 0;                                                 /* [한국어] libcurl은 0만 정상으로 취급. */
}

/* https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html
 * https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-authenticating-requests.html#signing-request-intro
 */
/*
 * [한국어]
 * _add_aws_auth_header - AWS Signature V4(서명 버전 4)를 계산해 libcurl 헤더 리스트에
 *                        Authorization을 포함한 필수 x-amz-* 헤더를 모두 붙인다.
 *
 * @curl:  요청을 수행할 easy 핸들. 최종적으로 CURLOPT_HTTPHEADER에 slist를 세팅.
 * @slist: 추가 헤더가 차곡차곡 accumulate되는 리스트(함수가 append만 하고 호출자에게
 *         최종 포인터 갱신은 하지 않음 — 호출자 fio_http_queue()가 동일 지역 변수를
 *         새로 관리하며 free_all 책임).
 * @o:     http_options (S3 키/리전/SSE-C/스토리지 클래스/보안 토큰).
 * @op:    I/O 방향(DDIR_READ→GET, DDIR_WRITE→PUT, DDIR_TRIM→DELETE).
 * @uri:   원본 URI 경로. 내부에서 _aws_uriencode로 정규화됨.
 * @buf, @len: PUT 페이로드(DDIR_WRITE 때만 해시 대상). GET/DELETE는 빈 페이로드 해시.
 *
 * SigV4 절차:
 *  1) canonical request 구성: METHOD, URI, canonical headers, signed headers, payload hash.
 *  2) string-to-sign: "AWS4-HMAC-SHA256\n<date_iso>\n<date_short>/<region>/s3/aws4_request\n<SHA256(canonical)>".
 *  3) 서명 키 파생: kSecret="AWS4"+s3_key → kDate=HMAC(kSecret,date_short)
 *                   → kRegion=HMAC(kDate,region) → kService=HMAC(kRegion,"s3")
 *                   → kSigning=HMAC(kService,"aws4_request").
 *  4) signature = HMAC(kSigning, string-to-sign), hex.
 *  5) Authorization 헤더 조립.
 * 실행 컨텍스트: 잡 스레드 queue() 경로. 요청마다 새로 계산되어 시계 오차로 서명이
 *                15분 이상 벗어나면 서버가 RequestTimeTooSkewed를 반환할 수 있음.
 */
static void _add_aws_auth_header(CURL *curl, struct curl_slist *slist, struct http_options *o,
		int op, const char *uri, char *buf, size_t len)
{
	char date_short[16];                                      /* [한국어] YYYYMMDD — Credential 스코프와 kDate 파생에 사용. */
	char date_iso[32];                                        /* [한국어] YYYYMMDDTHHMMSSZ (UTC) — x-amz-date 헤더와 string-to-sign에 사용. */
	char method[8];                                           /* [한국어] "GET"/"PUT"/"DELETE" 문자열 버퍼. */
	char dkey[128];                                           /* [한국어] 초기 서명 키 "AWS4"+s3_key 조립용. */
	char creq[4096];                                          /* [한국어] canonical request 전체 문자열. */
	char sts[512];                                            /* [한국어] string-to-sign 문자열. */
	char s[2048];                                             /* [한국어] 각 HTTP 헤더 라인 임시 조립 버퍼. */
	char *uri_encoded = NULL;                                 /* [한국어] _aws_uriencode가 반환한 인코딩 URI. 함수 말미에서 free. */
	char *dsha = NULL;                                        /* [한국어] 페이로드 hex SHA-256. x-amz-content-sha256 헤더 값. */
	char *csha = NULL;                                        /* [한국어] canonical request의 hex SHA-256(string-to-sign 포함). */
	char *signature = NULL;                                   /* [한국어] 최종 HMAC 결과의 hex(서명 값). */
	const char *service = "s3";                               /* [한국어] SigV4 서비스 스코프 고정. */
	const char *aws = "aws4_request";                         /* [한국어] SigV4 terminator 문자열. */
	unsigned char md[SHA256_DIGEST_LENGTH];                   /* [한국어] HMAC 단계별 중간 결과 저장(32바이트). */
	unsigned char sse_key[33] = {0};                          /* [한국어] SSE-C 고객 키(최대 32바이트 + NUL). 0으로 초기화해 "미설정" 판별에 활용. */
	char *sse_key_base64 = NULL;                              /* [한국어] SSE-C 키 Base64. */
	char *sse_key_md5_base64 = NULL;                          /* [한국어] SSE-C 키의 MD5 Base64(무결성 체크). */
	char security_token_header[2048] = {0};                   /* [한국어] 토큰이 있으면 canonical headers 블록에 포함할 전체 행. */
	char security_token_list_item[24] = {0};                  /* [한국어] signed headers 목록에 삽입할 "x-amz-security-token;" 토큰. */

	time_t t = time(NULL);                                    /* [한국어] UTC epoch 초 획득. 서명은 로컬 시계 기반. */
	struct tm *gtm = gmtime(&t);                              /* [한국어] UTC로 분해(주의: 정적 버퍼 반환 — 멀티스레드에서는 race 가능성. fio 엔진은 잡당 스레드 1개이므로 실질적 문제 없음). */

	strftime (date_short, sizeof(date_short), "%Y%m%d", gtm); /* [한국어] 날짜 단축형. */
	strftime (date_iso, sizeof(date_iso), "%Y%m%dT%H%M%SZ", gtm); /* [한국어] ISO 기본형, Z=UTC. */
	uri_encoded = _aws_uriencode(uri);                        /* [한국어] canonical URI 인코딩. */

	if (o->s3_security_token != NULL) {                       /* [한국어] STS 임시 토큰 사용 경로. */
		snprintf(security_token_header, sizeof(security_token_header),
				"x-amz-security-token:%s\n", o->s3_security_token); /* [한국어] canonical headers 행(끝에 \n). */
		sprintf(security_token_list_item, "x-amz-security-token;"); /* [한국어] SignedHeaders 목록에 삽입될 토큰(뒤 ';'로 다음 헤더와 연결). */
	}

	if (o->s3_sse_customer_key != NULL)                       /* [한국어] SSE-C 키가 주어진 경우. */
		strncpy((char*)sse_key, o->s3_sse_customer_key, sizeof(sse_key) - 1); /* [한국어] 최대 32바이트만 복사(33번째는 NUL 보장). */

	if (op == DDIR_WRITE) {                                   /* [한국어] PUT: 페이로드 해시 계산. */
		dsha = _gen_hex_sha256(buf, len);                 /* [한국어] 실제 본문 SHA-256. */
		sprintf(method, "PUT");                           /* [한국어] HTTP 메서드. */
	} else {
		/* DDIR_READ && DDIR_TRIM supply an empty body */
		if (op == DDIR_READ)                              /* [한국어] GET. */
			sprintf(method, "GET");
		else                                              /* [한국어] TRIM=DELETE. */
			sprintf(method, "DELETE");
		dsha = _gen_hex_sha256("", 0);                    /* [한국어] 빈 페이로드의 고정 SHA-256("e3b0c44..."). */
	}

	/* Create the canonical request first */
	/* [한국어] canonical request 조립: <METHOD>\n<URI>\n<query=빈줄>\n<headers...>\n\n<signedHeaders>\n<payloadHash>
	 * SSE-C 사용 여부에 따라 canonical headers/SignedHeaders 목록이 다르므로 분기. */
	if (sse_key[0] != '\0') {
		sse_key_base64 = _conv_base64_encode(sse_key, sizeof(sse_key) - 1); /* [한국어] 32바이트 SSE-C 키를 Base64로. */
		sse_key_md5_base64 = _gen_base64_md5(sse_key, sizeof(sse_key) - 1); /* [한국어] 키 MD5의 Base64(무결성 검증용). */
		snprintf(creq, sizeof(creq),
			"%s\n"
			"%s\n"
			"\n"
			"host:%s\n"
			"x-amz-content-sha256:%s\n"
			"x-amz-date:%s\n"
			"x-amz-server-side-encryption-customer-algorithm:%s\n"
			"x-amz-server-side-encryption-customer-key:%s\n"
			"x-amz-server-side-encryption-customer-key-md5:%s\n"
			"%s" /* security token if provided */
			"x-amz-storage-class:%s\n"
			"\n"
			"host;x-amz-content-sha256;x-amz-date;"
			"x-amz-server-side-encryption-customer-algorithm;"
			"x-amz-server-side-encryption-customer-key;"
			"x-amz-server-side-encryption-customer-key-md5;"
			"%s"
			"x-amz-storage-class\n"
			"%s"
			, method
			, uri_encoded, o->host, dsha, date_iso
			, o->s3_sse_customer_algorithm, sse_key_base64
			, sse_key_md5_base64, security_token_header
			, o->s3_storage_class, security_token_list_item, dsha);
	} else {
		snprintf(creq, sizeof(creq),
			"%s\n"
			"%s\n"
			"\n"
			"host:%s\n"
			"x-amz-content-sha256:%s\n"
			"x-amz-date:%s\n"
			"%s" /* security token if provided */
			"x-amz-storage-class:%s\n"
			"\n"
			"host;x-amz-content-sha256;x-amz-date;%sx-amz-storage-class\n"
			"%s"
			, method
			, uri_encoded, o->host, dsha, date_iso
			, security_token_header, o->s3_storage_class
			, security_token_list_item, dsha);
	}

	csha = _gen_hex_sha256(creq, strlen(creq));               /* [한국어] canonical request 해시(string-to-sign에 끼움). */
	snprintf(sts, sizeof(sts), "AWS4-HMAC-SHA256\n%s\n%s/%s/%s/%s\n%s",
			date_iso, date_short, o->s3_region, service, aws, csha); /* [한국어] string-to-sign: 알고리즘/시간/스코프/csha. */

	snprintf((char *)dkey, sizeof(dkey), "AWS4%s", o->s3_key);/* [한국어] 초기 시크릿 키 kSecret = "AWS4"+s3_key. */
	_hmac(md, dkey, strlen(dkey), date_short);                /* [한국어] kDate = HMAC(kSecret, date_short). */
	_hmac(md, md, SHA256_DIGEST_LENGTH, o->s3_region);        /* [한국어] kRegion = HMAC(kDate, region). */
	_hmac(md, md, SHA256_DIGEST_LENGTH, (char*) service);     /* [한국어] kService = HMAC(kRegion, "s3"). */
	_hmac(md, md, SHA256_DIGEST_LENGTH, (char*) aws);         /* [한국어] kSigning = HMAC(kService, "aws4_request"). */
	_hmac(md, md, SHA256_DIGEST_LENGTH, sts);                 /* [한국어] 최종 signature raw = HMAC(kSigning, string-to-sign). */

	signature = _conv_hex(md, SHA256_DIGEST_LENGTH);          /* [한국어] Authorization 헤더용 hex. */

	/* Suppress automatic Accept: header */
	slist = curl_slist_append(slist, "Accept:");              /* [한국어] libcurl이 자동으로 Accept: */ /* 를 넣는 걸 빈 값으로 덮어 SigV4 canonical headers와 불일치 방지. */

	snprintf(s, sizeof(s), "x-amz-content-sha256: %s", dsha); /* [한국어] 페이로드 SHA-256 선언 헤더(필수). */
	slist = curl_slist_append(slist, s);                      /* [한국어] 위 x-amz-content-sha256 추가. */

	snprintf(s, sizeof(s), "x-amz-date: %s", date_iso);       /* [한국어] 요청 시각. Date 헤더 대신 이 값을 사용. */
	slist = curl_slist_append(slist, s);                      /* [한국어] 목록에 추가. */

	if (sse_key[0] != '\0') {                                 /* [한국어] SSE-C 세 헤더 추가. */
		snprintf(s, sizeof(s), "x-amz-server-side-encryption-customer-algorithm: %s", o->s3_sse_customer_algorithm); /* [한국어] 알고리즘(AES256). */
		slist = curl_slist_append(slist, s);
		snprintf(s, sizeof(s), "x-amz-server-side-encryption-customer-key: %s", sse_key_base64); /* [한국어] 키(Base64). */
		slist = curl_slist_append(slist, s);
		snprintf(s, sizeof(s), "x-amz-server-side-encryption-customer-key-md5: %s", sse_key_md5_base64); /* [한국어] 키 MD5(Base64) — 전송 중 키 오염 탐지. */
		slist = curl_slist_append(slist, s);
	}

	if (o->s3_security_token != NULL) {                       /* [한국어] STS 토큰 헤더 부착. */
		snprintf(s, sizeof(s), "x-amz-security-token: %s", o->s3_security_token);
		slist = curl_slist_append(slist, s);
	}

	snprintf(s, sizeof(s), "x-amz-storage-class: %s", o->s3_storage_class); /* [한국어] 스토리지 클래스 고지. */
	slist = curl_slist_append(slist, s);

	if (sse_key[0] != '\0') {                                 /* [한국어] Authorization 헤더. SSE-C 사용 시 SignedHeaders 목록이 더 길다. */
		snprintf(s, sizeof(s), "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s/%s/s3/aws4_request,"
			"SignedHeaders=host;x-amz-content-sha256;"
			"x-amz-date;x-amz-server-side-encryption-customer-algorithm;"
			"x-amz-server-side-encryption-customer-key;"
			"x-amz-server-side-encryption-customer-key-md5;"
			"%s"
			"x-amz-storage-class,"
			"Signature=%s",
		o->s3_keyid, date_short, o->s3_region, security_token_list_item, signature); /* [한국어] Credential/서명 조립. */
	} else {
		snprintf(s, sizeof(s), "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s/%s/s3/aws4_request,"
			"SignedHeaders=host;x-amz-content-sha256;x-amz-date;%sx-amz-storage-class,Signature=%s",
			o->s3_keyid, date_short, o->s3_region, security_token_list_item, signature); /* [한국어] SSE-C 미사용 경로. */
	}
	slist = curl_slist_append(slist, s);                      /* [한국어] Authorization 라인 등록. */

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);        /* [한국어] libcurl이 사용할 헤더 리스트 지정(요청 전송 시 본문 앞에 주입). */

	free(uri_encoded);                                        /* [한국어] 누수 방지. */
	free(csha);
	free(dsha);
	free(signature);
	if (sse_key_base64 != NULL) {                             /* [한국어] SSE-C 경로에서만 할당됨. */
		free(sse_key_base64);
		free(sse_key_md5_base64);
	}
}

/*
 * [한국어]
 * _add_swift_header - OpenStack Swift 인증/etag 헤더를 slist에 추가.
 *
 * @curl, @slist, @o: 앞과 동일.
 * @op: DDIR_WRITE일 때만 본문 MD5 etag를 계산(업로드 무결성).
 * @uri, @buf, @len: PUT일 때 etag 계산용 입력.
 *
 * Swift는 SigV4 대신 사전에 발급된 토큰(x-auth-token)을 헤더로 실어 인증한다. PUT 시
 * 서버가 etag(=MD5)로 수신 데이터 검증.
 */
static void _add_swift_header(CURL *curl, struct curl_slist *slist, struct http_options *o,
		int op, const char *uri, char *buf, size_t len)
{
	char *dsha = NULL;                                        /* [한국어] 변수명은 dsha지만 실제로는 MD5 hex(etag). */
	char s[512];                                              /* [한국어] 헤더 라인 버퍼. */

	if (op == DDIR_WRITE) {
		dsha = _gen_hex_md5(buf, len);                    /* [한국어] 업로드 본문 MD5. */
	}
	/* Suppress automatic Accept: header */
	slist = curl_slist_append(slist, "Accept:");              /* [한국어] 자동 Accept 비활성(불필요 헤더 제거). */

	snprintf(s, sizeof(s), "etag: %s", dsha);                 /* [한국어] PUT이 아닐 때는 dsha=NULL이어도 snprintf는 "(null)" 등을 출력할 수 있음(원본 유지). */
	slist = curl_slist_append(slist, s);

	snprintf(s, sizeof(s), "x-auth-token: %s", o->swift_auth_token); /* [한국어] Swift 인증 토큰. */
	slist = curl_slist_append(slist, s);

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);        /* [한국어] libcurl에 헤더 주입. */

	free(dsha);                                               /* [한국어] free(NULL)은 안전. */
}

/*
 * [한국어]
 * _append_range_header - HTTP Range 헤더("Range: bytes=start-end")를 slist에 붙인다.
 *
 * @slist: 기존 헤더 리스트(가변, 반환값으로 새 노드 포함). @offset/@length: io_u 기반.
 * @file_size: 객체 크기(바이트). 요청 범위를 파일 경계로 클램핑.
 * @return: 수정된 slist. offset이 파일 끝을 넘으면 원본 반환(헤더 미추가).
 *
 * object_mode=range + DDIR_READ 경로에서만 사용되어 하나의 객체에 대해 partial read를
 * 수행하게 한다. HTTP/1.1 Range는 end-inclusive이므로 end=offset+length-1.
 */
static struct curl_slist* _append_range_header(struct curl_slist *slist, unsigned long long offset, unsigned long long length, unsigned long long file_size)
{
	char s[256];                                              /* [한국어] 헤더 문자열 버퍼. */
	unsigned long long end_byte;                              /* [한국어] inclusive end 바이트 인덱스. */

	/* Don't request beyond end of file */
	if (offset >= file_size) {
		return slist;                                     /* [한국어] 시작이 이미 파일 밖이면 Range 추가 안 함. */
	}

	/* Calculate end byte, but cap it at file size - 1 because end range is inclusive */
	end_byte = offset + length - 1;                           /* [한국어] 요청된 끝 바이트. */
	if (end_byte >= file_size) {
		end_byte = file_size - 1;                         /* [한국어] 파일 경계로 클램프. */
	}

	snprintf(s, sizeof(s), "Range: bytes=%llu-%llu", offset, end_byte); /* [한국어] RFC7233 bytes= 단위. */
	return curl_slist_append(slist, s);                       /* [한국어] 리스트에 추가해 반환. */
}

/*
 * [한국어]
 * fio_http_cleanup - ioengine_ops.cleanup 콜백. 잡 종료 시 호출돼 CURL 핸들과 state 해제.
 *
 * @td: 종료 중인 잡. td->io_ops_data가 http_data*이다.
 *
 * setup이 실패해 io_ops_data가 NULL이면 no-op. 같은 경로가 setup의 cleanup goto에서도
 * 호출된다. 실행 컨텍스트: 잡 스레드 종료 단계.
 */
static void fio_http_cleanup(struct thread_data *td)
{
	struct http_data *http = td->io_ops_data;                 /* [한국어] 스레드별 상태 포인터. */

	if (http) {                                               /* [한국어] 할당됐을 때만 해제. */
		curl_easy_cleanup(http->curl);                    /* [한국어] libcurl 핸들 파괴(연결/TLS 세션 정리). */
		free(http);                                       /* [한국어] 래퍼 구조체 해제. */
	}
}

/*
 * [한국어]
 * _http_read - libcurl CURLOPT_READFUNCTION 콜백(PUT 업로드 시 호출).
 *
 * @ptr: libcurl이 네트워크로 보낼 데이터를 여기에 쓰길 원한다.
 * @size, @nmemb: 요청 크기(바이트=size*nmemb).
 * @stream: CURLOPT_READDATA로 넘긴 http_curl_stream*.
 * @return: 실제 채운 바이트. 0=EOF(업로드 종료).
 *
 * io_u->xfer_buf에서 state->pos 커서를 전진시켜가며 libcurl에 공급. stream==NULL은
 * 다운로드 경로(GET)에서 unused로 설정된 경우이며 0을 반환해 libcurl에 EOF를 알린다.
 */
static size_t _http_read(void *ptr, size_t size, size_t nmemb, void *stream)
{
	struct http_curl_stream *state = stream;                  /* [한국어] 사용자 state 복원. */
	size_t len = size * nmemb;                                /* [한국어] 요청 총 바이트. */
	/* We're retrieving; nothing is supposed to be read locally */
	if (!stream)
		return 0;                                         /* [한국어] GET 경로에서 READDATA=NULL이면 EOF 즉시 보고. */
	if (len+state->pos > state->max)
		len = state->max - state->pos;                    /* [한국어] 버퍼 끝 넘지 않게 자름. */
	memcpy(ptr, &state->buf[state->pos], len);                /* [한국어] xfer_buf → libcurl 송신 버퍼. */
	state->pos += len;                                        /* [한국어] 다음 호출에 전진된 위치 사용. */
	return len;                                               /* [한국어] libcurl은 이 바이트만큼 전송 시도. */
}

/*
 * [한국어]
 * _http_write - libcurl CURLOPT_WRITEFUNCTION 콜백(GET 다운로드 시 호출).
 *
 * @ptr: 네트워크에서 도착한 바이트. @size/@nmemb: 받은 크기.
 * @stream: CURLOPT_WRITEDATA. @return: 처리한 바이트. 불일치 시 CURLE_WRITE_ERROR.
 *
 * stream==NULL은 PUT 경로에서 응답 본문을 버릴 때. GET 시 state->buf(=xfer_buf)에 누적.
 * libcurl 규약상 size=1로 호출되며 nmemb가 실제 길이.
 */
static size_t _http_write(void *ptr, size_t size, size_t nmemb, void *stream)
{
	struct http_curl_stream *state = stream;                  /* [한국어] state 복원. */
	/* We're just discarding the returned body after a PUT */
	if (!stream)
		return nmemb;                                     /* [한국어] PUT 응답은 버림: 받은 만큼 처리한 것처럼 반환. */
	if (size != 1)
		return CURLE_WRITE_ERROR;                         /* [한국어] libcurl 규약 위반 — 방어적 실패. */
	if (nmemb + state->pos > state->max)
		return CURLE_WRITE_ERROR;                         /* [한국어] 버퍼 오버플로 방지. */
	memcpy(&state->buf[state->pos], ptr, nmemb);              /* [한국어] 수신 데이터를 xfer_buf에 누적. */
	state->pos += nmemb;                                      /* [한국어] 커서 전진. */
	return nmemb;                                             /* [한국어] 성공 처리한 바이트 수. */
}

/*
 * [한국어]
 * _http_seek - libcurl CURLOPT_SEEKFUNCTION 콜백. 업로드 중 재전송 필요 시 호출.
 *
 * @stream: state. @offset: 절대 오프셋. @origin: SEEK_SET만 지원.
 * @return: CURL_SEEKFUNC_OK/FAIL.
 *
 * TCP 재전송/redirect 등에서 libcurl이 버퍼 커서를 되감아야 할 때 쓰인다.
 */
static int _http_seek(void *stream, curl_off_t offset, int origin)
{
	struct http_curl_stream *state = stream;                  /* [한국어] state 포인터 복원. */
	if (offset < state->max && origin == SEEK_SET) {          /* [한국어] 절대 seek이고 범위 내일 때만 허용. */
		state->pos = offset;                              /* [한국어] 커서를 요청된 위치로. */
		return CURL_SEEKFUNC_OK;
	} else
		return CURL_SEEKFUNC_FAIL;                        /* [한국어] 그 외는 실패 — libcurl이 요청을 포기한다. */
}

/*
 * [한국어]
 * fio_http_queue - ioengine_ops.queue 콜백. 하나의 io_u를 HTTP 요청으로 변환해 동기 실행.
 *
 * @td: 현재 잡. @io_u: 제출할 I/O 유닛.
 * @return: 항상 FIO_Q_COMPLETED(FIO_SYNCIO 엔진 계약). 에러 시 io_u->error=EIO 설정 후에도
 *          completed로 반환(backend가 에러 경로로 처리).
 *
 * 절차:
 *  1) object_mode에 따라 객체 경로 구성(block=파일명_offset_length, range=파일명).
 *  2) https 모드에 따라 URL 스킴 선택.
 *  3) CURLOPT_URL/SEEKDATA/INFILESIZE_LARGE 갱신.
 *  4) object_mode=range + READ이면 Range 헤더 추가.
 *  5) S3/Swift 모드면 인증 헤더 서명/부착.
 *  6) ddir별로 PUT(UPLOAD)/GET(HTTPGET)/DELETE(CUSTOMREQUEST) 옵션 설정 후
 *     curl_easy_perform() 블로킹 실행, HTTP 응답 코드 확인.
 *  7) 404는 GET에서 "zero read"로, DELETE에서 성공으로 관대하게 처리.
 *  8) slist 해제.
 *
 * 실행 컨텍스트: 잡 스레드. 호출 체인: backend.c → td_io_queue() → [이 함수]
 */
static enum fio_q_status fio_http_queue(struct thread_data *td,
					 struct io_u *io_u)
{
	struct http_data *http = td->io_ops_data;                 /* [한국어] 스레드 CURL 핸들 래퍼. */
	struct http_options *o = td->eo;                          /* [한국어] 잡 옵션. */
	struct http_curl_stream _curl_stream;                     /* [한국어] 스택 state. libcurl 콜백 userdata. */
	struct curl_slist *slist = NULL;                          /* [한국어] 요청 헤더 리스트(누적). */
	char object_path_buf[512];                                /* [한국어] block 모드 객체 경로 버퍼. */
	char *object_path;                                        /* [한국어] 선택된 경로 포인터(buf 또는 file_name). */
	char url[1024];                                           /* [한국어] 최종 URL. */
	long status;                                              /* [한국어] HTTP 응답 코드. */
	CURLcode res;                                             /* [한국어] libcurl 수행 결과. */

	fio_ro_check(td, io_u);                                   /* [한국어] read-only 잡에 write가 들어오면 abort(매크로). */
	memset(&_curl_stream, 0, sizeof(_curl_stream));           /* [한국어] pos/max/buf 전부 0 초기화. */
	if (o->object_mode == FIO_HTTP_OBJECT_BLOCK) {            /* [한국어] 블록당 객체 1개 모드. */
		snprintf(object_path_buf, sizeof(object_path_buf), "%s_%llu_%llu", io_u->file->file_name,
			io_u->offset, io_u->xfer_buflen);         /* [한국어] 이름에 offset/length 인코딩. */
		object_path = object_path_buf;
	} else
		object_path = io_u->file->file_name;              /* [한국어] range 모드: 객체 1개 공유, 범위만 변경. */
	if (o->https == FIO_HTTPS_OFF)
		snprintf(url, sizeof(url), "http://%s%s", o->host, object_path); /* [한국어] 평문. */
	else
		snprintf(url, sizeof(url), "https://%s%s", o->host, object_path); /* [한국어] TLS. */

	curl_easy_setopt(http->curl, CURLOPT_URL, url);           /* [한국어] 요청 대상 URL 갱신. */
	_curl_stream.buf = io_u->xfer_buf;                        /* [한국어] I/O 버퍼를 state에 연결(복사 없음). */
	_curl_stream.max = io_u->xfer_buflen;                     /* [한국어] 최대 크기. */
	curl_easy_setopt(http->curl, CURLOPT_SEEKDATA, &_curl_stream); /* [한국어] seek 콜백 userdata. */
	curl_easy_setopt(http->curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)io_u->xfer_buflen); /* [한국어] PUT Content-Length 힌트. */

	if (io_u->ddir == DDIR_READ && o->object_mode == FIO_HTTP_OBJECT_RANGE)
		slist = _append_range_header(slist, io_u->offset, io_u->xfer_buflen, io_u->file->real_file_size); /* [한국어] 부분 읽기 Range 헤더. */

	if (o->mode == FIO_HTTP_S3)
		_add_aws_auth_header(http->curl, slist, o, io_u->ddir, object_path,
			io_u->xfer_buf, io_u->xfer_buflen);       /* [한국어] SigV4 서명 헤더 부착. */
	else if (o->mode == FIO_HTTP_SWIFT)
		_add_swift_header(http->curl, slist, o, io_u->ddir, object_path,
			io_u->xfer_buf, io_u->xfer_buflen);       /* [한국어] Swift 토큰/etag 헤더 부착. */

	if (io_u->ddir == DDIR_WRITE) {                           /* [한국어] ▶ PUT 경로. */
		curl_easy_setopt(http->curl, CURLOPT_READDATA, &_curl_stream); /* [한국어] 업로드 소스를 _http_read 콜백이 볼 수 있게. */
		curl_easy_setopt(http->curl, CURLOPT_WRITEDATA, NULL); /* [한국어] 응답 본문은 버림(NULL 시 _http_write가 조용히 소비). */
		curl_easy_setopt(http->curl, CURLOPT_UPLOAD, 1L); /* [한국어] PUT 모드. Expect: 100-continue는 libcurl이 관리. */
		res = curl_easy_perform(http->curl);              /* [한국어] 동기 실행(blocking): 전송+수신 완료까지 대기. */
		if (res == CURLE_OK) {
			curl_easy_getinfo(http->curl, CURLINFO_RESPONSE_CODE, &status); /* [한국어] HTTP 상태 코드 조회. */
			if (status == 100 || (status >= 200 && status <= 204))
				goto out;                         /* [한국어] 100 Continue 또는 2xx(성공). */
			log_err("DDIR_WRITE failed with HTTP status code %ld\n", status);
		}
		goto err;                                         /* [한국어] 네트워크 에러 또는 HTTP 에러. */
	} else if (io_u->ddir == DDIR_READ) {                     /* [한국어] ▶ GET 경로. */
		curl_easy_setopt(http->curl, CURLOPT_READDATA, NULL); /* [한국어] 업로드 소스 없음. */
		curl_easy_setopt(http->curl, CURLOPT_WRITEDATA, &_curl_stream); /* [한국어] 수신 본문을 xfer_buf에 적재. */
		curl_easy_setopt(http->curl, CURLOPT_HTTPGET, 1L); /* [한국어] UPLOAD 모드에서 GET으로 전환. */
		res = curl_easy_perform(http->curl);              /* [한국어] 동기 수행. */
		if (res == CURLE_OK) {
			curl_easy_getinfo(http->curl, CURLINFO_RESPONSE_CODE, &status);
			/* 206 "Partial Content" means success when using the
			 * Range header */
			if (status == 200 || (o->object_mode == FIO_HTTP_OBJECT_RANGE && status == 206))
				goto out;                         /* [한국어] 200 또는 range 모드의 206. */
			else if (status == 404) {
				/* Object doesn't exist. Pretend we read
				 * zeroes */
				memset(io_u->xfer_buf, 0, io_u->xfer_buflen); /* [한국어] 미존재 객체 = 0으로 채워 성공 처리. 벤치마크 흐름을 끊지 않기 위함. */
				goto out;
			}
			log_err("DDIR_READ failed with HTTP status code %ld\n", status);
		}
		goto err;
	} else if (io_u->ddir == DDIR_TRIM) {                     /* [한국어] ▶ DELETE 경로. */
		curl_easy_setopt(http->curl, CURLOPT_HTTPGET, 1L); /* [한국어] UPLOAD 플래그 해제용(GET으로 되돌린 뒤 CUSTOMREQUEST로 오버라이드). */
		curl_easy_setopt(http->curl, CURLOPT_CUSTOMREQUEST, "DELETE"); /* [한국어] 메서드를 DELETE로 강제. */
		curl_easy_setopt(http->curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)0); /* [한국어] 본문 없음. */
		curl_easy_setopt(http->curl, CURLOPT_READDATA, NULL);
		curl_easy_setopt(http->curl, CURLOPT_WRITEDATA, NULL); /* [한국어] 응답 본문도 무시. */
		res = curl_easy_perform(http->curl);
		if (res == CURLE_OK) {
			curl_easy_getinfo(http->curl, CURLINFO_RESPONSE_CODE, &status);
			if (status == 200 || status == 202 || status == 204 || status == 404)
				goto out;                         /* [한국어] 200/202/204는 성공, 404는 이미 없음(성공으로 취급). */
			log_err("DDIR_TRIM failed with HTTP status code %ld\n", status);
		}
		goto err;
	}

	log_err("WARNING: Only DDIR_READ/DDIR_WRITE/DDIR_TRIM are supported!\n"); /* [한국어] SYNC/그 외 ddir은 지원 안 함. */

err:
	io_u->error = EIO;                                        /* [한국어] fio 표준 I/O 에러. */
	td_verror(td, io_u->error, "transfer");                   /* [한국어] 잡 단위 verror 기록(최초 에러만). */
out:
	curl_slist_free_all(slist);                               /* [한국어] 요청별로 헤더 리스트 전체 해제(누수 방지). */
	return FIO_Q_COMPLETED;                                   /* [한국어] 동기 엔진: 즉시 완료로 보고 → backend가 event() 거치지 않고 통계 반영. */
}

/*
 * [한국어]
 * fio_http_event - ioengine_ops.event 콜백. FIO_SYNCIO 엔진은 getevents가 0을 반환하므로
 * 실제로는 호출되지 않는다. 계약상 필드를 채우기 위한 더미.
 */
static struct io_u *fio_http_event(struct thread_data *td, int event)
{
	/* sync IO engine - never any outstanding events */
	return NULL;                                              /* [한국어] 미사용 경로: NULL 반환. */
}

/*
 * [한국어]
 * fio_http_getevents - ioengine_ops.getevents 콜백. 동기 엔진이므로 대기 이벤트 없음.
 *
 * @td, @min, @max, @t: 무시. @return: 0.
 */
static int fio_http_getevents(struct thread_data *td, unsigned int min,
	unsigned int max, const struct timespec *t)
{
	/* sync IO engine - never any outstanding events */
	return 0;                                                 /* [한국어] 미결 이벤트 0개. */
}

/*
 * [한국어]
 * fio_http_setup - ioengine_ops.setup 콜백. 잡 스레드가 처음 엔진을 초기화할 때 호출.
 *
 * @td: 잡. @return: 0=성공, 1=실패.
 *
 * CURL* 생성, verbose/SSL 검증/인증/콜백 등록, use_thread=1 강제.
 * 실패 시 cleanup 경로로 goto 해서 부분 할당 해제.
 * 실행 컨텍스트: 잡 스레드 초기화 단계.
 * 호출 체인: backend → td_io_init() → ioops->setup
 */
static int fio_http_setup(struct thread_data *td)
{
	struct http_data *http = NULL;                            /* [한국어] 할당 중 실패 시 cleanup에서 안전 비교하기 위해 NULL 초기화. */
	struct http_options *o = td->eo;                          /* [한국어] 엔진 옵션 포인터. */

	/* allocate engine specific structure to deal with libhttp. */
	http = calloc(1, sizeof(*http));                          /* [한국어] http_data 0-초기화 할당. */
	if (!http) {
		log_err("calloc failed.\n");
		goto cleanup;                                     /* [한국어] 실패 경로: 부분 할당이 없어도 cleanup은 안전. */
	}

	http->curl = curl_easy_init();                            /* [한국어] CURL 핸들 생성(TLS/DNS 캐시 소유). */
	if (o->verbose)
		curl_easy_setopt(http->curl, CURLOPT_VERBOSE, 1L); /* [한국어] 내부 상세 로그 활성. */
	if (o->verbose > 1)
		curl_easy_setopt(http->curl, CURLOPT_DEBUGFUNCTION, &_curl_trace); /* [한국어] 상세도 2 이상이면 trace 콜백으로 상세 캡처. */
	curl_easy_setopt(http->curl, CURLOPT_NOPROGRESS, 1L);     /* [한국어] 진행률 표시 비활성(벤치마크이므로 불필요). */
	curl_easy_setopt(http->curl, CURLOPT_FOLLOWLOCATION, 1L); /* [한국어] 3xx 리다이렉트 자동 추종. */
	curl_easy_setopt(http->curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP|CURLPROTO_HTTPS); /* [한국어] 리다이렉트 허용 프로토콜 제한(file:// 등 차단). */
	if (o->https == FIO_HTTPS_INSECURE) {
		curl_easy_setopt(http->curl, CURLOPT_SSL_VERIFYPEER, 0L); /* [한국어] 인증서 체인 검증 끔. */
		curl_easy_setopt(http->curl, CURLOPT_SSL_VERIFYHOST, 0L); /* [한국어] SAN/CN 호스트 검증 끔(개발/테스트용). */
	}
	curl_easy_setopt(http->curl, CURLOPT_READFUNCTION, _http_read);   /* [한국어] 업로드 소스 콜백. */
	curl_easy_setopt(http->curl, CURLOPT_WRITEFUNCTION, _http_write); /* [한국어] 수신 싱크 콜백. */
	curl_easy_setopt(http->curl, CURLOPT_SEEKFUNCTION, &_http_seek);  /* [한국어] 재전송 시 되감기 콜백. */
	if (o->user && o->pass) {
		curl_easy_setopt(http->curl, CURLOPT_USERNAME, o->user); /* [한국어] Basic/Digest 사용자. */
		curl_easy_setopt(http->curl, CURLOPT_PASSWORD, o->pass); /* [한국어] 비밀번호. */
		curl_easy_setopt(http->curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY); /* [한국어] 서버가 받는 방식(Basic/Digest/NTLM 등) 자동 선택. */
	}

	td->io_ops_data = http;                                   /* [한국어] 스레드 상태 등록 — queue/cleanup에서 이 포인터로 접근. */

	/* Force single process mode. */
	td->o.use_thread = 1;                                     /* [한국어] 프로세스 fork 금지: CURL 핸들과 TLS 상태는 fork로 복제 시 불안정. */

	return 0;                                                 /* [한국어] 성공. */
cleanup:
	fio_http_cleanup(td);                                     /* [한국어] 이미 할당된 부분을 자동 해제. */
	return 1;                                                 /* [한국어] fio backend에 실패 통지. */
}

/*
 * [한국어]
 * fio_http_open - ioengine_ops.open_file 콜백. 로컬 파일이 없으므로(원격 객체) no-op.
 *
 * @td, @f: 사용 안 함. @return: 0(성공).
 *
 * FIO_DISKLESSIO 플래그와 함께 설정돼 backend가 로컬 fd open을 시도하지 않게 한다.
 */
static int fio_http_open(struct thread_data *td, struct fio_file *f)
{
	return 0;                                                 /* [한국어] 열어야 할 파일 없음. */
}
/*
 * [한국어]
 * fio_http_invalidate - ioengine_ops.invalidate 콜백. 로컬 페이지캐시 무효화 요청 처리.
 *                        원격 객체에는 의미 없음 → no-op.
 */
static int fio_http_invalidate(struct thread_data *td, struct fio_file *f)
{
	return 0;                                                 /* [한국어] 로컬 캐시 없음 — 할 일 없음. */
}

/* [한국어] ioengine_ops 테이블 — fio backend가 "http" 엔진을 선택했을 때 이 구조체를
 * 통해 모든 콜백을 호출한다.
 * 필드 요약:
 *  - name: --ioengine=<name> 에 쓰이는 식별자.
 *  - version: ABI 버전(FIO_IOOPS_VERSION). 커널-사용자처럼 서로 다른 fio 버전의 외부
 *             엔진이 로드되는 걸 방지.
 *  - flags:
 *      FIO_DISKLESSIO: 로컬 블록 디바이스/파일이 없는 원격 엔진. backend가 로컬 open/size
 *                      검증을 건너뛴다.
 *      FIO_SYNCIO: queue()가 FIO_Q_COMPLETED를 돌려주는 동기 엔진. getevents/event는 no-op.
 *  - setup/queue/getevents/event/cleanup/open_file/invalidate: 콜백 포인터.
 *  - options/option_struct_size: 엔진별 옵션 테이블과 구조체 크기(파서가 calloc). */
FIO_STATIC struct ioengine_ops ioengine = {
	.name = "http",                                           /* [한국어] fio --ioengine=http. */
	.version		= FIO_IOOPS_VERSION,              /* [한국어] 엔진 ABI 버전 체크 값. */
	.flags			= FIO_DISKLESSIO | FIO_SYNCIO,    /* [한국어] 원격+동기 엔진 속성. */
	.setup			= fio_http_setup,                 /* [한국어] 초기화 콜백. */
	.queue			= fio_http_queue,                 /* [한국어] I/O 제출/실행 콜백(핵심). */
	.getevents		= fio_http_getevents,             /* [한국어] 동기 엔진: 0 반환. */
	.event			= fio_http_event,                 /* [한국어] 동기 엔진: NULL 반환. */
	.cleanup		= fio_http_cleanup,               /* [한국어] 종료 콜백. */
	.open_file		= fio_http_open,                  /* [한국어] 원격: no-op. */
	.invalidate		= fio_http_invalidate,            /* [한국어] 로컬 캐시 없음: no-op. */
	.options		= options,                        /* [한국어] 엔진 옵션 테이블. */
	.option_struct_size	= sizeof(struct http_options),    /* [한국어] 옵션 구조체 크기(파서가 할당). */
};

/*
 * [한국어]
 * fio_http_register - 공유 라이브러리 로드 시 자동 호출(__attribute__((constructor))).
 *                     fio 코어의 ioengine 레지스트리에 본 엔진을 등록한다.
 */
static void fio_init fio_http_register(void)
{
	register_ioengine(&ioengine);                             /* [한국어] 런타임에 "http" 엔진 테이블 등록. */
}

/*
 * [한국어]
 * fio_http_unregister - 라이브러리 언로드 시 호출(destructor). 레지스트리에서 제거.
 */
static void fio_exit fio_http_unregister(void)
{
	unregister_ioengine(&ioengine);                           /* [한국어] 등록 해제 — 댕글링 포인터 예방. */
}
