/*
 * [한국어 설명] 바이트 스왑 (Byte Swap) 엔디언 변환 유틸리티 (bswap.h)
 *
 * === 파일의 역할 ===
 * 빅엔디언(big-endian)과 리틀엔디언(little-endian) 간의 바이트 순서 변환 함수를 제공한다.
 * __be32_to_cpu()와 __be64_to_cpu()를 통해 32비트 및 64비트 값의 엔디언 변환을 수행한다.
 * 리틀엔디언 시스템에서는 바이트를 재배치하고, 빅엔디언 시스템에서는 값을 그대로 반환한다.
 *
 * === fio에서의 사용 ===
 * 네트워크 프로토콜이나 디스크 포맷 등에서 빅엔디언으로 저장된 데이터를 CPU의 네이티브 바이트 순서로
 * 변환할 때 사용된다. 특히 크로스 플랫폼 호환성을 위해 I/O 로그나 검증 데이터 처리에 활용된다.
 */
#ifndef FIO_BSWAP_H
#define FIO_BSWAP_H

#include <inttypes.h>

#ifdef CONFIG_LITTLE_ENDIAN
static inline uint32_t __be32_to_cpu(uint32_t val)
{
	uint32_t c1, c2, c3, c4;

	c1 = (val >> 24) & 0xff;
	c2 = (val >> 16) & 0xff;
	c3 = (val >> 8) & 0xff;
	c4 = val & 0xff;

	return c1 | c2 << 8 | c3 << 16 | c4 << 24;
}

static inline uint64_t __be64_to_cpu(uint64_t val)
{
	uint64_t c1, c2, c3, c4, c5, c6, c7, c8;

	c1 = (val >> 56) & 0xff;
	c2 = (val >> 48) & 0xff;
	c3 = (val >> 40) & 0xff;
	c4 = (val >> 32) & 0xff;
	c5 = (val >> 24) & 0xff;
	c6 = (val >> 16) & 0xff;
	c7 = (val >> 8) & 0xff;
	c8 = val & 0xff;

	return c1 | c2 << 8 | c3 << 16 | c4 << 24 | c5 << 32 | c6 << 40 | c7 << 48 | c8 << 56;
}
#else
static inline uint64_t __be64_to_cpu(uint64_t val)
{
	return val;
}

static inline uint32_t __be32_to_cpu(uint32_t val)
{
	return val;
}
#endif

#endif
