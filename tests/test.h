/* Minimal unit test framework */

#ifndef TEST_H_
#define TEST_H_

#include <stdio.h>

/* Test counters - defined in driver.c */
extern int test_pass_count;
extern int test_fail_count;

#define TEST_INIT()          \
    do {                     \
        test_pass_count = 0; \
        test_fail_count = 0; \
    } while (0)

#define TEST_ASSERT(cond, msg)                                      \
    do {                                                            \
        if (cond) {                                                 \
            test_pass_count++;                                      \
        } else {                                                    \
            test_fail_count++;                                      \
            printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        }                                                           \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg)                                         \
    do {                                                                  \
        long _a = (long) (a);                                             \
        long _b = (long) (b);                                             \
        if (_a == _b) {                                                   \
            test_pass_count++;                                            \
        } else {                                                          \
            test_fail_count++;                                            \
            printf("FAIL: %s:%d: %s (expected %ld, got %ld)\n", __FILE__, \
                   __LINE__, (msg), _b, _a);                              \
        }                                                                 \
    } while (0)

#define TEST_ASSERT_RANGE(val, lo, hi, msg)                           \
    do {                                                              \
        long _v = (long) (val);                                       \
        long _lo = (long) (lo);                                       \
        long _hi = (long) (hi);                                       \
        if (_v >= _lo && _v <= _hi) {                                 \
            test_pass_count++;                                        \
        } else {                                                      \
            test_fail_count++;                                        \
            printf("FAIL: %s:%d: %s (value %ld not in [%ld, %ld])\n", \
                   __FILE__, __LINE__, (msg), _v, _lo, _hi);          \
        }                                                             \
    } while (0)

#define TEST_RUN(fn) \
    do {             \
        fn();        \
    } while (0)

#define TEST_SUMMARY()                                             \
    do {                                                           \
        printf("\n=== Test Summary ===\n");                        \
        printf("Passed: %d\n", test_pass_count);                   \
        printf("Failed: %d\n", test_fail_count);                   \
        printf("Total:  %d\n", test_pass_count + test_fail_count); \
    } while (0)

#define TEST_RESULT() (test_fail_count > 0 ? 1 : 0)

#endif /* TEST_H_ */
