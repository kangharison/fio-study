/*
 * [한국어 설명] CRC/해시 벤치마크 테스트 헤더 (test.h)
 *
 * === 파일의 역할 ===
 * fio의 `--crctest[=type1,type2,...]` 옵션으로 실행하는 해시/체크섬 알고리즘
 * 벤치마크 진입점(fio_crctest)을 외부에 노출한다. 본 헤더는 단 하나의 함수
 * 선언만 포함하며, 실제 구현은 crc/test.c 에 있다. test.c 는 CHUNK=128KB를
 * NR_CHUNKS=2048 개(총 256MB) 미리 할당해서 각 알고리즘을 반복 실행하고,
 * 처리량(MiB/sec)을 출력한다. 이 헤더는 fio 의 main() 경로(fio.c)에서
 * `--crctest` 명령줄 옵션이 파싱되었을 때만 간접 호출된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   main() [fio.c]
 *     → parse_options() [init.c]           // `--crctest` 옵션 검출
 *     → fio_crctest(type) [crc/test.c]     // 본 헤더가 노출하는 진입점
 *       → md5/sha/crc32/crc32c/crc64/... 각 알고리즘 구현 호출
 *       → fio_gettime / utime_since_now 로 구간 시간 측정
 *       → 결과를 stdout 에 "crc32c: ..., avg: XX MB/sec" 형식으로 출력
 *   실행 후 프로세스 종료(실제 I/O 실행 경로로 넘어가지 않음).
 *
 * === 타 모듈과의 연결 ===
 * - crc/test.c: 벤치마크 실제 구현(struct test_type 배열, 각 알고리즘 호출).
 * - fio.c / init.c: `--crctest` CLI 옵션 처리.
 * - crc/{md5,sha1,sha256,sha3,sha512,crc7,crc16,crc32,crc32c,crc64,xxhash,
 *        fnv,murmur3,crct10dif_common}.[ch]: 각 알고리즘의 제공자.
 * - lib/fio_time.h: fio_gettime/utime_since_now 시간 측정.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_crctest(const char *type): 주어진 type 문자열이 가리키는 알고리즘
 *   (또는 NULL이면 전체)의 벤치마크를 실행하고 결과를 stdout 에 출력한다.
 *   구현 세부는 crc/test.c 의 struct test_type 배열과 do_test() 루프가 담당.
 */
#ifndef FIO_CRC_TEST_H
/* [한국어] 헤더 중복 포함 방지용 가드 — crc/test.c 와 fio.c 가 동시에 포함할 수 있으므로 필수. */
#define FIO_CRC_TEST_H

/*
 * [한국어]
 * fio_crctest - 해시/체크섬 알고리즘 벤치마크 실행
 *
 * @type: 실행할 알고리즘 이름(예: "crc32c", "sha256"). NULL이면 전체 실행.
 *        crc/test.c 의 struct test_type 배열에 등록된 이름과 일치해야 한다.
 * @return: 알고리즘을 찾지 못했거나 출력에 실패한 경우 음수, 성공 시 0.
 *          호출자는 반환값을 프로세스 종료 코드로 사용할 수 있다.
 *
 * 벤치마크는 256MB(CHUNK=128KB × NR_CHUNKS=2048) 버퍼를 반복 해시하여 평균
 * 처리량(MiB/sec)을 계산한다. 실제 I/O 경로에 개입하지 않는 진단 전용 함수.
 *
 * 호출 체인:
 *   main() → parse_options() → fio_crctest() → 각 알고리즘 init/update/final
 */
int fio_crctest(const char *type);

#endif
