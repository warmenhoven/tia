/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIA_TEST_FRAMEWORK_H
#define TIA_TEST_FRAMEWORK_H

#include <stdio.h>

#define ASSERT_EQ(a, b) do { \
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "%s:%d: %s != %s (%ld vs %ld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
        return 1; \
    } \
} while (0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        fprintf(stderr, "%s:%d: %s was false\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    ++_total; \
    if (fn()) { fprintf(stderr, "FAIL %s\n", #fn); ++_fails; } \
    else { printf("ok   %s\n", #fn); } \
} while (0)

#define TEST_MAIN_BEGIN int main(void) { int _fails = 0, _total = 0;
#define TEST_MAIN_END \
    fprintf(stderr, "%d/%d passed\n", _total - _fails, _total); \
    return _fails ? 1 : 0; }

#endif
