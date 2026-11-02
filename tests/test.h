// Minimal test harness: CHECK records a failure and continues; a test binary returns non-zero
// if any check failed.
#pragma once

#include <math.h>
#include <stdio.h>

static int test_failures;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);           \
            test_failures++;                                                                   \
        }                                                                                      \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                     \
    do {                                                                                       \
        long long a_ = (long long)(a), b_ = (long long)(b);                                    \
        if (a_ != b_) {                                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s == %s (%lld vs %lld)\n", __FILE__,        \
                    __LINE__, #a, #b, a_, b_);                                                 \
            test_failures++;                                                                   \
        }                                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                  \
    do {                                                                                       \
        double a_ = (double)(a), b_ = (double)(b);                                             \
        if (!(fabs(a_ - b_) <= (tol))) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s ~= %s (%g vs %g, tol %g)\n", __FILE__,    \
                    __LINE__, #a, #b, a_, b_, (double)(tol));                                  \
            test_failures++;                                                                   \
        }                                                                                      \
    } while (0)

#define RUN(test)                                                                              \
    do {                                                                                       \
        int before_ = test_failures;                                                           \
        test();                                                                                \
        fprintf(stderr, "%s %s\n", test_failures == before_ ? "pass" : "FAIL", #test);         \
    } while (0)
