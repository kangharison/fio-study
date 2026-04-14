/*
 * [한국어 설명] MurmurHash3 해시 헤더 (murmur3.h)
 *
 * === 파일의 역할 ===
 * MurmurHash3 32비트 해시 함수의 인터페이스를 정의한다.
 * MurmurHash3는 Austin Appleby가 설계한 비암호학적 해시로,
 * 뛰어��� 분산성과 빠른 속도로 해시 ��이블, 데이터 검증 등에 널리 사용된다.
 * SMHasher 테스트 스위트를 완벽히 통과하는 고품질 해시이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → murmurhash3()
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=murmur3 옵션 시 호출
 * - murmur3.c: 구현
 * - crc/test.c: 벤치마크 테스트
 */
#ifndef FIO_MURMUR3_H
#define FIO_MURMUR3_H

#include <inttypes.h>

uint32_t murmurhash3(const void *key, uint32_t len, uint32_t seed);

#endif
