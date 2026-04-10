/*
 * [한국어 설명] 모의 테스트용 TAP 프로토콜 헤더 (tap.h)
 *
 * === 파일의 역할 ===
 * TAP(Test Anything Protocol) 형식의 테스트 출력을 지원하는 모의 테스트용 헤더 파일이다.
 * 테스트 결과를 표준화된 TAP 형식으로 출력하여 자동화된 테스트 도구와 연동할 수 있게 한다.
 * 테스트 계획 선언, 성공/실패 보고, 진단 메시지 출력 등의 매크로와 함수를 제공한다.
 */
/*
 * TAP (Test Anything Protocol) output support for FIO mock tests
 *
 * This provides a simple TAP output format for automated testing.
 * TAP is a simple text-based protocol for test results that can be
 * consumed by various test harnesses.
 *
 * Format:
 *   TAP version 13
 *   1..N
 *   ok 1 - test description
 *   not ok 2 - test description
 *   # diagnostic message
 */

#ifndef FIO_MOCK_TAP_H
#define FIO_MOCK_TAP_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

static int tap_test_count = 0;
static int tap_failures = 0;
static bool tap_planned = false;

/* Initialize TAP output */
static inline void tap_init(void) {
    printf("TAP version 13\n");
    tap_test_count = 0;
    tap_failures = 0;
    tap_planned = false;
}

/* Plan the number of tests */
static inline void tap_plan(int n) {
    printf("1..%d\n", n);
    tap_planned = true;
}

/* Report a test result */
static inline void tap_ok(bool condition, const char *fmt, ...) {
    va_list args;
    tap_test_count++;

    if (condition) {
        printf("ok %d - ", tap_test_count);
    } else {
        printf("not ok %d - ", tap_test_count);
        tap_failures++;
    }

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

/* Skip a test */
static inline void tap_skip(const char *reason, ...) {
    va_list args;
    tap_test_count++;

    printf("ok %d # SKIP ", tap_test_count);
    va_start(args, reason);
    vprintf(reason, args);
    va_end(args);
    printf("\n");
}

/* Output a diagnostic message */
static inline void tap_diag(const char *fmt, ...) {
    va_list args;
    printf("# ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

/* Check if a value is within tolerance */
static inline bool tap_within_tolerance(double actual, double expected, double tolerance) {
    double diff = actual - expected;
    if (diff < 0) diff = -diff;
    return diff <= tolerance;
}

/* Finish TAP output and return exit code */
static inline int tap_done(void) {
    if (!tap_planned) {
        printf("1..%d\n", tap_test_count);
    }

    if (tap_failures > 0) {
        tap_diag("Failed %d/%d tests", tap_failures, tap_test_count);
        return 1;
    }

    tap_diag("All tests passed");
    return 0;
}

#endif /* FIO_MOCK_TAP_H */
