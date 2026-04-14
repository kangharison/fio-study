/*
 * [한국어 설명] CRC/해시 벤치마크 테스트 헤더 (test.h)
 *
 * === 파일의 역할 ===
 * fio의 --crctest 옵션으로 실행하는 해시/체크섬 알고리즘 성능 벤치마크의
 * 인터페이스를 정의한다. 각 알고리즘의 처리 속도(MiB/sec)를 측정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: main() [fio.c] → fio_crctest() [test.c]
 *
 * === 타 모듈과의 연결 ===
 * - test.c: 벤치마크 구현 (md5, crc32/32c/64, sha1/256/512, sha3, xxhash 등)
 * - fio.c: --crctest 명령줄 옵션 처리
 */
#ifndef FIO_CRC_TEST_H
#define FIO_CRC_TEST_H

int fio_crctest(const char *type);

#endif
