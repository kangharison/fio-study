/*
 * Copyright (C) 2020 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 */
/*
 * [한국어] zbd_types.h - ZBD(Zoned Block Device) 타입 정의
 *
 * 존(zone) 기반 블록 디바이스(ZBD)와 관련된 열거형과 구조체를 정의한다.
 * ZBD는 SSD/HDD에서 존 단위로 데이터를 관리하는 인터페이스이다.
 *
 * 주요 정의:
 *   - zbd_zoned_model : 디바이스의 존 모델 (에뮬레이션/호스트 인식/호스트 관리)
 *   - zbd_zone_type   : 존의 유형 (일반/순차 쓰기 필수/순차 쓰기 선호)
 *   - zbd_zone_cond   : 존의 상태 (빈/열린/닫힌/읽기전용/가득찬/오프라인)
 *   - zbd_zone        : 개별 존의 정보 (시작 주소, 쓰기 포인터, 길이, 용량 등)
 */
#ifndef FIO_ZBD_TYPES_H
#define FIO_ZBD_TYPES_H

#include <inttypes.h>

/* [한국어] 동시에 열 수 있는 최대 쓰기 존 수 */
#define ZBD_MAX_WRITE_ZONES	4096

/*
 * Zoned block device models.
 */
/* [한국어] 존 블록 디바이스 모델 - 디바이스가 존을 지원하는 방식 */
enum zbd_zoned_model {
	ZBD_NONE		= 0x1,	/* No zone support. Emulate zones. */ /* 존 미지원. 존을 에뮬레이션 */
	ZBD_HOST_AWARE		= 0x2,	/* Host-aware zoned block device */  /* 호스트 인식 존 디바이스 */
	ZBD_HOST_MANAGED	= 0x3,	/* Host-managed zoned block device */ /* 호스트 관리 존 디바이스 */
};

/*
 * Zone types.
 */
/* [한국어] 존의 유형 - 존이 데이터를 받아들이는 방식 */
enum zbd_zone_type {
	ZBD_ZONE_TYPE_CNV	= 0x1,	/* Conventional */                  /* 일반(conventional) 존: 랜덤 쓰기 가능 */
	ZBD_ZONE_TYPE_SWR	= 0x2,	/* Sequential write required */     /* 순차 쓰기 필수 존 */
	ZBD_ZONE_TYPE_SWP	= 0x3,	/* Sequential write preferred */    /* 순차 쓰기 선호 존 */
};

/*
 * Zone conditions.
 */
/* [한국어] 존의 현재 상태(condition) */
enum zbd_zone_cond {
        ZBD_ZONE_COND_NOT_WP    = 0x0,  /* 쓰기 포인터 없음 (일반 존) */
        ZBD_ZONE_COND_EMPTY     = 0x1,  /* 빈 존 (쓰기 포인터 = 시작 위치) */
        ZBD_ZONE_COND_IMP_OPEN  = 0x2,  /* 암시적으로 열린 존 (I/O에 의해 자동 열림) */
        ZBD_ZONE_COND_EXP_OPEN  = 0x3,  /* 명시적으로 열린 존 (Open Zone 명령) */
        ZBD_ZONE_COND_CLOSED    = 0x4,  /* 닫힌 존 (쓰기 포인터 유지, 리소스 해제) */
        ZBD_ZONE_COND_READONLY  = 0xD,  /* 읽기 전용 존 */
        ZBD_ZONE_COND_FULL      = 0xE,  /* 가득 찬 존 (쓰기 포인터 = 끝 위치) */
        ZBD_ZONE_COND_OFFLINE   = 0xF,  /* 오프라인 존 (접근 불가) */
};

/*
 * Zone descriptor.
 */
/* [한국어] 존 디스크립터 - 개별 존의 전체 정보 */
struct zbd_zone {
	uint64_t		start;    /* 존의 시작 오프셋 (바이트) */
	uint64_t		wp;       /* 쓰기 포인터(Write Pointer) 위치 */
	uint64_t		len;      /* 존의 전체 길이 (바이트) */
	uint64_t		capacity; /* 존의 실제 사용 가능 용량 (len 이하) */
	enum zbd_zone_type	type;     /* 존 유형 */
	enum zbd_zone_cond	cond;     /* 존 상태 */
};

#endif /* FIO_ZBD_TYPES_H */
