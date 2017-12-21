/*----------------------------------------------------------------------------
  0example.c -- an example of how to use 0unit

  0unit is tiny, unit testing helper.  All of its code resides in the single
  header file "0unit.h".  This file is an example of how to test something.

  Copyright (C) 2012, Adrian Ratnapala, under the ISC license. See file LICENSE.
*/

#include <string.h>

#include "0unit.h"
#include "elm.h"

// Tests are functions you write, they return 0 on failure and 1 on success
static int test_something_good()
{
        int x = 3;

        /* The CHK macro is an assertion.  If its argument is false, the test
           fails immediately.  Otherwise execution carries on.  All the
           assertions here will pass.
        */

        CHK(42 == 6 * 7);
        CHK(strlen("answer") < strlen("question"));
        CHK(4 == ++x);
        CHK(5 == ++x);

        // end every test with PASS(), this prints a success message to stdout.
        PASS();
}

// This is another test, but it is designed to fail.
static int test_something_bad()
{
        int x = 3;

        // The first assertion will pass
        CHK(42 == 6 * 7);
        // the second will fail
        CHK(strlen("wrong answer") < strlen("question"));
        // the fourth and fifth will never be executed
        CHK(5 == ++x);
        CHK(5 == ++x);

        PASS();
}

// 0unit cannot run tests automatically, you must call them yourself.
int main(int argc, const char **argv)
{
        // this test will fail
        test_something_bad();
        // but execution will continue so this one can pass.
        test_something_good();

        return 0;
}

/*
The output should be something like
        FAILED: 0example.c:39:test_something_bad <strlen("wrong answer") < strlen("question")>
        passed: test_something_good
*/

