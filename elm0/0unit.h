/*----------------------------------------------------------------------------
  zunit.h: unit testing without a real framework.

  These are minute definitoins to help you write unit tests.  See 0example.c.

  Copyright (C) 2012, Adrian Ratnapala, under the ISC license. See file LICENSE.
*/


#ifndef _0UNIT_H
#define _0UNIT_H

#include <stdio.h>
#include <stdarg.h>

#ifndef CHECK_FMT
# ifdef __GNUC__
#  define CHECK_FMT(N) __attribute__((format(printf,(N),(N)+1)))
# else
#  define CHECK_FMT(N)
# endif
#endif

#define CHKV(T, ...) do {\
                        if(!chk(!!(T), __FILE__, __LINE__, __func__, __VA_ARGS__))\
                                goto fail; \
               } while(0)

#define CHK(T) CHKV(T, "%s", #T)
#define FAIL(...) CHKV(0, __VA_ARGS__)
#define WRN(...) wrn( 0, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define PASSV(...)                \
        return pass(__VA_ARGS__); \
        goto fail;                \
        fail:                     \
        return 0

#define PASS() PASSV(__func__)

#define PASS_ONLY()              \
        return pass( __func__ ); \
        goto fail;               \

#define PASS_QUIETLY()           \
        return 1;                \
        goto fail;               \
        fail:                    \
        return 0


#define ZUNIT_ANSI_COLOUR(d, s) "\033[" #d "m\033[1m" s "\033[0m"

static int zunit_npass = 0;
static int zunit_nfail = 0;

inline static int zunit_report()
{
        if(!zunit_nfail) {
                printf(ZUNIT_ANSI_COLOUR(32, "All %d tests passed\n"), zunit_npass);
                return 0;
        }

        printf(ZUNIT_ANSI_COLOUR(31, "%d of %d tests FAILED.\n"), zunit_nfail, zunit_nfail + zunit_npass);
        return !!zunit_nfail;
}

inline static int fail(const char *prefix,
                       const char *file, int line, const char *test,
                       const char *fmt, va_list va)
{
        printf("%s %s:%d:%s <", prefix, file, line, test);
        zunit_nfail++;
        vprintf(fmt, va);
        printf(">\n");
        fflush(stdout);
        va_end(va);
        return 0;
}

CHECK_FMT(5)
inline static int chk(int pass, const char *file, int line, const char *test,
                                const char *fmt, ...)
{
        if( pass )
                return 1;
        va_list va;
        va_start(va, fmt);
        return fail(ZUNIT_ANSI_COLOUR(31, "FAILED:"), file, line, test, fmt, va);
}

CHECK_FMT(5)
inline static int wrn(int pass, const char *file, int line, const char *test,
                                const char *fmt, ...)
{
        if( pass )
                return 1;
        va_list va;
        va_start(va, fmt);
        return fail(ZUNIT_ANSI_COLOUR(31, "WARNING:"), file, line, test, fmt, va);
}

CHECK_FMT(1)
inline static int pass(const char *test, ...) {
        fputs(ZUNIT_ANSI_COLOUR(32, "passed:")" ", stdout);
        va_list va;
        va_start(va, test);
        vprintf(test, va);
        va_end(va);
        putchar('\n');
        zunit_npass++;
        return 1;
}

#endif /* _ZUNIT_H */
