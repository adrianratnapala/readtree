/*----------------------------------------------------------------------------
  test_elm.c: unit tests for ELM

  These unit tests use 0unit to keep ELM on the straight-and-narrow.  The twist
  is that we also have to handle various error and panic situations.  Also it's
  nice to test 0unit itself.  Therefore this test program can be made to
  produce errors.  And the whole system is then run inside n0run.py which makes
  sure we fail when expected.

  Copyright (C) 2012, Adrian Ratnapala, under the ISC license. See file LICENSE.
*/


#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/resource.h>

#include "0unit.h"
#include "elm.h"

/* Set this to one to test some emergency-fail code. */
#ifndef FAKE_FAIL
#define FAKE_FAIL 0
#endif


// ----------------------------------------------------------------------------

static int test_versions()
/* This test is fairly thin.  It
  (a) Checks that the runtime version is the same as the compile-time
      version (since test_elm is always built with elm).
  (b) It does some syntax validation on that string.

  By hand I have shown that the following invalid version strings
  fail the syntax validation:

        ver = "lemo-  0.";
        ver =  "  0.  4.";
        ver = "elm0- x0.  4.";
        ver = "elm0- /0.  4.";
        ver = "elm0-  0. 4.";
        ver = "elm0-  0.   4.";
        ver = "elm0- 0.   4.";
        ver = "elm0-   0. 4.";
*/
{
        const char *ver = elm_version();
        const char *nums = ver + 5;

        CHK(0 == strcmp(ver, ELM_VERSION));
        CHK(0 <  strcmp(ver, "elm0-  0."));
        CHK(0 >  strcmp(ver, "elm0-  1."));

        CHK(0 == strncmp(ver, "elm0-", 5));
        CHK(0 == strlen(nums) % 4);

        for(const char* nu = nums; *nu; nu += 4) {
                CHK(nu[3] == '.' || nu[3] == '-');

                CHK(nu[2] <= '9'); if(nu[2] < '0') goto s3;
                CHK(nu[1] <= '9'); if(nu[1] < '0') goto s2;
                CHK(nu[0] <= '9'); if(nu[0] < '0') goto s1;

                continue;
        s3:     CHK(nu[2] == ' ');
        s2:     CHK(nu[1] == ' ');
        s1:     CHK(nu[0] == ' ');
        }

        PASS();
}

// ----------------------------------------------------------------------------
static int chk_error( Error *err, const ErrorType *type,
                                  const char *zvalue )
{
        size_t size;
        char *buf;
        FILE *mstream;

        CHK( err );
        CHK( err->type == type );
        CHK( mstream = open_memstream(&buf, &size) );
        CHK( error_fwrite(err, mstream) == strlen(zvalue) );
        fclose(mstream);

        CHK( size == strlen(zvalue) );
        CHK( !memcmp(zvalue, buf, size) );
        free(buf);

        PASS_QUIETLY();
}

static int test_errors()
{
        int pre_line = __LINE__;
        Error *e = ERROR("goodbye world!");

        CHK(chk_error(e, error_type, "goodbye world!"));
        CHK(!strcmp(e->meta.file, __FILE__));
        CHK(!strcmp(e->meta.func, __func__));
        CHK(e->meta.line == pre_line + 1);

        destroy_error(e);
        PASS();
}

static int test_error_format()
{
        int pre_line = __LINE__;
        Error *e[] = {
                ERROR("Happy unbirthday!"),
                ERROR("%04d every year.", 364),
                ERROR("%04d every %xth year.", 365, 4),
        };

        CHK(chk_error(e[0], error_type, "Happy unbirthday!"));
        CHK(chk_error(e[1], error_type, "0364 every year."));
        CHK(chk_error(e[2], error_type, "0365 every 4th year."));

        for(int k = 0; k < 3; k++) {
                CHK(!strcmp(e[k]->meta.file, __FILE__));
                CHK(!strcmp(e[k]->meta.func, __func__));
                CHK(e[k]->meta.line == pre_line + 2 + k);
                destroy_error(e[k]);
        }

        PASS();
}

static int test_simple_custom_error()
{
        ErrorType _sc_error_type = {0},
                  *sc_error_type = &_sc_error_type;

        Error *e = ERROR_WITH(sc_error, "Custom error %d", 42);
        CHK(chk_error(e, sc_error_type, "Custom error 42"));
        destroy_error(e);

        PASS();
}

static int test_keep_first_error()
{
        Error *e1, *e2;
        CHK(NULL == keep_first_error(NULL, NULL));

        e1 = ERROR("one");
        CHK(e1 == keep_first_error(e1, NULL));
        CHK(e1 == keep_first_error(NULL, e1));

        e2 = ERROR("two");
        CHK(e1 == keep_first_error(e1, e2));

        destroy_error(e1);
        PASS();
}

static int test_system_error()
{
        char *xerror;
        PanicReturn ret;

        Error *eno = SYS_ERROR(EEXIST, "pretending");
        Error *enf = IO_ERROR("hello", ENOENT, "gone");

        asprintf(&xerror, "pretending: %s", strerror(EEXIST));
        CHK( chk_error(eno, sys_error_type, xerror) );
        destroy_error(eno);

        ret.error = NULL;;
        if ( TRY(ret) ) {
                CHK( chk_error( ret.error, sys_error_type, xerror ) );
                destroy_error(ret.error);
        } else {
                SYS_PANIC(EEXIST, "pretending");
                NO_WORRIES(ret);
        }
        CHK(ret.error);
        free(xerror);

        asprintf(&xerror, "gone (hello): %s", strerror(ENOENT));
        CHK( chk_error(enf, sys_error_type, xerror) );
        destroy_error(enf);


        ret.error = NULL;;
        if ( TRY(ret) ) {
                CHK( chk_error( ret.error, sys_error_type, xerror ) );
                destroy_error(ret.error);
        } else {
                IO_PANIC("hello", ENOENT, "gone");
                NO_WORRIES(ret);
        }
        CHK(ret.error);
        free(xerror);

        PASS();
}

static int test_variadic_system_error()
{
        char *xerror;

        Error *eno = SYS_ERROR(ENOTTY, "tty %d, %x", 12, 15);
        asprintf(&xerror, "tty 12, f: %s", strerror(ENOTTY));
        CHK( chk_error(eno, sys_error_type, xerror) );
        destroy_error(eno);
        free(xerror);

        Error *enf = IO_ERROR("every thing", ENOENT, "%d", 42);
        asprintf(&xerror, "42 (every thing): %s", strerror(ENOENT));
        CHK( chk_error(enf, sys_error_type, xerror) );
        destroy_error(enf);
        free(xerror);


        PASS();
}

static int test_unpack_system_error()
{
        void *UNTOUCHED_PTR = (void*)0xfafafaf;
        Error *e = NULL;
        char *zname = NULL, *zmsg = NULL;

        //If `e` is NULL, sys_error returns zero,... it ignores zname and zmsg
        zmsg = zname = UNTOUCHED_PTR;
        CHK(0 == sys_error(NULL, &zname, &zmsg));
        CHK(zname == NULL && zmsg == NULL);

        //If `e` points to an error OTHER than SYS_ERROR, it returns -1...
        e = ERROR("I am not a sys error.");
        zmsg = zname = UNTOUCHED_PTR;
        CHK(-1 == sys_error(e, &zname, &zmsg));
        CHK(zname == NULL && zmsg == NULL);
        destroy_error(e);

        //If `e` is a SYS_ERROR, but there is no file, zname is cleared NULL ...
        e = SYS_ERROR(42, "I am not a sys error.");
        zmsg = zname = UNTOUCHED_PTR;
        CHK(42 == sys_error(e, &zname, NULL));
        CHK(zmsg == UNTOUCHED_PTR && zname == NULL);
        destroy_error(e);

        //Otherwise returns errno
        e = IO_ERROR("in a cake", ENOENT, "format(%d)", 33);

        //if zname != NULL, *zname is the filename
        zmsg = zname = UNTOUCHED_PTR;
        CHK(ENOENT == sys_error(e, NULL, NULL));
        CHK(zname == UNTOUCHED_PTR && zmsg == UNTOUCHED_PTR);

        zmsg = zname = UNTOUCHED_PTR;
        CHK(ENOENT == sys_error(e, &zname, NULL));
        CHK(zmsg == UNTOUCHED_PTR && zname && !strcmp(zname, "in a cake"));
        free(zname);
        zname = NULL;

        //*zmsg is the msg_prefix after substituting format varargs.
        zmsg = zname = UNTOUCHED_PTR;
        CHK(ENOENT == sys_error(e, NULL, &zmsg));
        CHK(zname == UNTOUCHED_PTR && zmsg && !strcmp(zmsg, "format(33)"));
        free(zmsg);
        zmsg = NULL;

        zmsg = zname = UNTOUCHED_PTR;
        CHK(ENOENT == sys_error(e, &zname, &zmsg));
        CHK(zname && !strcmp(zname, "in a cake"));
        CHK(zmsg && !strcmp(zmsg, "format(33)"));

        free(zname);
        free(zmsg);

        destroy_error(e);

        PASS();
}


// ----------------------------------------------------------------------------

static int test_logging()
{
        static const char *expected_text =
                "TEST: Hello Logs!\n"
                "TEST: Hello Logs #2!\n"
                "TEST: -1+4 == 8\n"
                "TEST: goodbye world!\n"
                ;

        size_t size;
        char *buf;

        FILE *mstream = open_memstream(&buf, &size);
        CHK( mstream != NULL );

        Logger *lg = new_logger("TEST", mstream, NULL);
        CHK(lg);

        Logger *nlg = new_logger("NULL_TEST", 0, NULL);
        CHK(nlg);

        CHK( LOG_F(nlg, "Hello Logs!") == 0 );
        CHK( LOG_F(lg, "Hello Logs!") == 18);
        CHK( size == 18 );
        CHK( !memcmp(buf, expected_text, size) );

        LOG_F(nlg, "Hello Logs #%d!", 2);
        LOG_F(lg, "Hello Logs #%d!", 2);
        CHK( size == 18 + 21 );
        CHK( !memcmp(buf, expected_text, size) );

        LOG_UNLESS(lg, 4+4 == 8);
        CHK( size == 18 + 21 );
        LOG_UNLESS(lg, -1+4 == 8);
        CHK( size == 18 + 21 + 16 );

        Error *e = ERROR("goodbye world!");
        CHK( log_error(nlg, e) == 0 );
        CHK( log_error(lg, e) == 21 );
        CHK( size == 18 + 21 + 16 + 21 );
        CHK( !memcmp(buf, expected_text, size) );

        destroy_logger(lg);
        destroy_logger(nlg);
        fclose(mstream);
        free(buf);
        destroy_error(e);

        PASS();
}


static int test_logger_refcounts()
{
        static const char *expected_text =
                "TEST: Logging with two refs.\n"
                "TEST: Logging with oen ref!\n"
                ;

        size_t size;
        char *buf;

        FILE *mstream = open_memstream(&buf, &size);
        CHK(mstream != NULL);

        Logger *lg  = new_logger("TEST", mstream, NULL);
        CHK(lg);

        CHK(lg == ref_logger(lg));

        if(!FAKE_FAIL) {
                CHK(LOG_F(lg, "Logging with two refs.") == 29);
                CHK(!memcmp(buf, expected_text, size));

                CHK(NULL == destroy_logger(lg));
                CHK(LOG_F(lg, "Logging with oen ref!") == 28);
                CHK(!memcmp(buf, expected_text, size));
        }

        destroy_logger(lg);
        fclose(mstream);
        free(buf);

        PASS();
}

static int test_static_logger_refcounts()
{
        CHK(NULL == destroy_logger(null_log));
        CHK(NULL == destroy_logger(dbg_log));
        CHK(NULL == destroy_logger(std_log));
        CHK(NULL == destroy_logger(err_log));

        // if it works, valgrind won't complain
        CHK(NULL == destroy_logger(null_log));
        CHK(NULL == destroy_logger(dbg_log));
        CHK(NULL == destroy_logger(std_log));
        CHK(NULL == destroy_logger(err_log));

        // null_log is the only one we can cleanly write to
        CHK(0 == LOG_F(null_log, "I'm still alive!"));

        // if it works, valgrind won't complain
        CHK(null_log == ref_logger(null_log));
        CHK(dbg_log  == ref_logger(dbg_log));
        CHK(std_log  == ref_logger(std_log));
        CHK(err_log  == ref_logger(err_log));

        PASS();
}

static int test_debug_logger()
{
        size_t size;
        char *buf, *expect;

        FILE *mstream = open_memstream(&buf, &size);
        CHK( mstream != NULL );

        Logger *lg = new_logger("DTEST", mstream, "d");
        CHK(lg);

        char *text = "Eeek, a (pretend) software bug!";
        int line_p = __LINE__;
        LOG_F(lg, "Eeek, a (pretend) software bug!" );
        int n = asprintf(&expect, "DTEST (%s:%d in %s): %s\n",
                __FILE__, line_p + 1, __func__, text );
        CHK( n > 0 );

        CHK( strlen(expect) == size );
        CHK( !memcmp(expect, buf, size) );

        free(expect);
        destroy_logger(lg);
        fclose(mstream);
        free(buf);

        PASS();
}

// ----------------------------------------------------------------------------

static int test_malloc(int n)
{
        // ------------------
        char *ttk = MALLOC(n);
        CHK(ttk != NULL);
        memset(ttk, 0, n);
        ttk[10] = '5';

        CHK( n > 2048);
        CHK( ttk[0] == 0 );
        CHK( ttk[10] == '5' );

        for(int k = n - 1024; k < n; k++ )
                CHK( ttk[k] == 0 );

        free(ttk);


        // ------------------
        const char *test = "test";
        char *mlc = MALLOC(strlen(test) + 1);
        CHK( mlc );
        CHK( strcpy(mlc, test) == mlc );
        CHK( !strcmp(mlc, test) );

        free(mlc);

        PASS();
}

static const struct rlimit *setup_rlimit(size_t lim, struct rlimit *_new_lim)
{
        static struct rlimit old_lim;
        getrlimit(RLIMIT_AS, &old_lim);

        struct rlimit new_lim = {
                .rlim_cur = lim,
                .rlim_max = old_lim.rlim_max,
        };
        if(setrlimit(RLIMIT_AS, &new_lim))
                return NULL;

        if(_new_lim)
                *_new_lim = new_lim;
        return &old_lim;
}

static int teardown_rlimit(const struct rlimit *old_lim)
{
        return setrlimit(RLIMIT_AS, old_lim);
}

static int test_bad_malloc(void)
{
        const struct rlimit *old_lim;
        struct rlimit new_lim;
        CHK(old_lim = setup_rlimit(128*1024*1024, &new_lim));

        PanicReturn ret;
        Error *expected_nomem = TRY(ret);
        if(!expected_nomem) {
                CHK(!malloc(2 * new_lim.rlim_cur));
                MALLOC(new_lim.rlim_cur);
                CHK(!"Unreachable code reached.");
                NO_WORRIES(ret);
        }

        CHK(expected_nomem->type == nomem_error_type);
        destroy_error(expected_nomem);

        CHK(!teardown_rlimit(old_lim));
        PASS();
}

const struct rlimit *fix_rlimit;

static int nomem_rescue()
// Callback set by test_rescued_alloc, lifts the artificial memory limit.
{
        if(teardown_rlimit(fix_rlimit))
                return -1;
        fix_rlimit = NULL;
        return 0;
}

static int test_rescued_malloc(void)
{
        struct rlimit new_lim;
        CHK(fix_rlimit = setup_rlimit(72*1024*1024, &new_lim));

        PanicRescue old_rescue = panic_rescue_nomem(nomem_rescue);

        void *good_memory = NULL;
        CHK(good_memory = MALLOC(128*1024*1024));
        CHK(!fix_rlimit); // can pass only if nomem_rescue was called.
        free(good_memory);

        CHK(panic_rescue_nomem(old_rescue) == nomem_rescue);
        CHK(old_rescue == panic_rescue_nomem(NULL));
        CHK(old_rescue == panic_rescue_nomem(NULL));

        PASS();
}

static int runtests_malloc_fail(void)
{
        struct rlimit mem_lim;

        mem_lim.rlim_cur = 128*1024*1024;
        mem_lim.rlim_max = mem_lim.rlim_cur;

        int err = setrlimit(RLIMIT_AS, &mem_lim);
        assert(!err);
        test_malloc(128 * 1024);
        test_malloc(mem_lim.rlim_cur);
        return 0;
}



// ----------------------------------------------------------------------------

static int chk_recursive_panic(int depth)
{
        PanicReturn ret;
        Error *err;
        static int catch_count = 0;

        assert( depth >= 0 && depth <= 10 );
        if(depth == 10)
                PANIC("You've gone too far this time!");

        if(err = TRY(ret)) {
                catch_count++;
                CHK(chk_error(err, error_type,
                        "You've gone too far this time!"));
                CHK(depth);
                if(depth > 1)
                        panic(err);
                else {
                        destroy_error(err);
                        return -depth;
                }
                CHK(!"never");
        }

        CHK( chk_recursive_panic(depth+1) == -1 );
        NO_WORRIES(ret);

        CHK( depth == 0 );
        CHK( catch_count == 9);
        catch_count = 0;
        PASS_QUIETLY();
}

static int test_recursive_panic()
{
        // do it twice to check the static catch_counted is handled right.
        CHK( chk_recursive_panic(0) );
        CHK( chk_recursive_panic(0) );
        PASS();
}

static int test_try_panic()
{
        PanicReturn ret;
        Error *err;
        int failed = 0, succeeded = 0;

        // throw an error and catch it.
        if ( err = TRY(ret) ) {
                CHK( !panic_is_caught() );
                CHK( err->type == error_type );
                CHK( chk_error( err, error_type,
                        "not in 07 years!" ) );
                destroy_error(err);
                failed = 1;
        } else {
                CHK( panic_is_caught() );
                PANIC("not in %02d %s!", 7, "years");
                assert(!"never");
                NO_WORRIES(ret);
        }
        CHK( !panic_is_caught() );

        // don't throw an error and don't catch it.
        if ( err = TRY(ret) ) {
                panic(err);
        } else {
                succeeded = 1;
                NO_WORRIES(ret);
        }
        CHK( !panic_is_caught() );

        CHK( failed && succeeded );
        PASS();
}

static int test_panic_if()
{
        PanicReturn ret;
        Error *err;

        if(err = TRY(ret)) {
                log_error(dbg_log, err);
                destroy_error(err);
                CHK(!"Caught unexpected panic.");
        } else {
                panic_if(NULL);
                NO_WORRIES(ret);
        }
        CHK(!err);

        Error *const e_worry = ERROR("This is a worry");

        if(err = TRY(ret)) {
                // We got the expected error.
                CHK(err == e_worry);
        } else {
                panic_if(e_worry);
                NO_WORRIES(ret);
        }
        if(!err)
                FAIL("Expected to catch an error, but did not.");
        destroy_error(err);

        PASS();
}


// -- Main -----------------------------

int main(int argc, const char **argv)
{
        test_versions();

        test_errors();
        test_error_format();
        test_keep_first_error();
        test_simple_custom_error();

        test_panic_if();

        test_system_error();
        test_variadic_system_error();
        test_unpack_system_error();

        test_bad_malloc();
        test_rescued_malloc();

        test_logging();
        test_debug_logger();
        LOG_F(null_log, "EEEK!  I'm invisible!  Don't look!");
        test_logger_refcounts();
        test_static_logger_refcounts();

        test_try_panic();
        test_recursive_panic();
        if( argc > 1 && !strcmp(argv[1], "--panic") )
                PANIC("The slithy toves!"); //FIX
        if( argc > 1 && !strncmp(argv[1], "--panic=", 8) ) {
                const char *start = argv[1] + 8;
                char *end = NULL;
                long ern = strtol(start, &end, 0);
                assert(end != start && end != NULL);
                SYS_PANIC(ern, "Panic");

                PANIC("The slithy toves!"); //FIX
        }
        if(FAKE_FAIL)
                runtests_malloc_fail();
        else
                test_malloc(128 * 1024);

        return zunit_report();
}

