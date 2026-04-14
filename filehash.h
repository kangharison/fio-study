/*
 * [한국어] filehash.h - 파일 해시 테이블 헤더
 *
 * 동일한 파일을 여러 fio job이 공유할 때 사용되는 해시 테이블 API를 선언한다.
 * 파일명을 키로 사용하여 fio_file 구조체를 빠르게 검색할 수 있다.
 
 * === 파일의 역할 ===
 * 파일 해시 테이블 API를 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * filehash.c와 짝을 이루는 헤더.
 *
 * === 타 모듈과의 연결 ===
 * - filehash.c: 이 헤더의 함수 구현
 *
 * === 주요 함수/구조체 요약 ===
 * - file_hash_init/exit(): 초기화/정리
 * - add_file_hash()/lookup_file_hash(): 추가/검색
 */
#ifndef FIO_FILE_HASH_H
#define FIO_FILE_HASH_H

#include "lib/types.h"

/* [한국어] 해시 테이블 초기화 (smalloc으로 버킷 배열 할당 + bloom 필터 생성) */
extern void file_hash_init(void);
/* [한국어] 해시 테이블 종료 및 자원 해제 */
extern void file_hash_exit(void);
/* [한국어] 파일명으로 해시 테이블에서 fio_file 검색 (세마포어 보호) */
extern struct fio_file *lookup_file_hash(const char *);
/* [한국어] fio_file을 해시 테이블에 추가 (이미 존재하면 기존 alias 반환) */
extern struct fio_file *add_file_hash(struct fio_file *);
/* [한국어] 해시 테이블에서 fio_file 제거 */
extern void remove_file_hash(struct fio_file *);
/* [한국어] 해시 테이블 전역 잠금 획득 */
extern void fio_file_hash_lock(void);
/* [한국어] 해시 테이블 전역 잠금 해제 */
extern void fio_file_hash_unlock(void);
/* [한국어] bloom 필터로 파일 존재 여부 빠른 확인 (set=true면 필터에 추가도 수행) */
extern bool file_bloom_exists(const char *, bool);

#endif
