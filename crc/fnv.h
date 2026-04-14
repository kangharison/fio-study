/*
 * [한국어 설명] FNV (Fowler-Noll-Vo) 해시 헤더 (fnv.h)
 *
 * === 파일의 역할 ===
 * FNV-1a 64비트 해시 함수의 인터페이스를 정의한다.
 * FNV는 비암호학적 해시 함수로, 구현이 단순하고 빠르며
 * 해시 테이블이나 체크섬 등에 널리 사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → fnv()
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=fnv 옵션 시 데이터 무결성 검증
 * - fnv.c: 구현
 * - crc/test.c: 벤치마크 테스트
 */
#ifndef FIO_FNV_H
#define FIO_FNV_H

#include <inttypes.h>

uint64_t fnv(const void *, uint32_t, uint64_t);

#endif
