#!/usr/bin/python3
"""
  elm-fail-run.py -- extension of n0run.py for when elm should fail.

  n0run.py is a framework for running unit tests and should be independent of
  the system-under-test.  Used as a program, it just assumes that all tests
  pass.  But used as a module, it provides classes that can be extended to do
  more complicated things.  This program is written specifically for doctored
  versions of the elm unit tests, which we hope will fail or die in specific
  ways.

  Copyright (C) 2012, Adrian Ratnapala, under the ISC license. See file LICENSE.
"""




from n0run import *

class Elm_Fail_Runner(Fail_Runner) :
        err_matchers = compile_matchers ([
                ('NOMEM', br'^NOMEM \(in test_elm.c:(?P<n>test_malloc+)'),
                ('LOGFAILED', br'^LOGFAILED \(in test_elm.c:(?P<n>test_logging)\)'),
                ('LOGFAILED', br'^LOGFAILED \(in test_elm.c:(?P<n>test_debug_logger)\)'),
        ])

        def __init__(s, command, source) :
                import errno
                Fail_Runner.__init__(s, [command], source, xerrno=errno.ENOMEM ) ;

        def pheck_output(s) :
                for err in Fail_Runner.check_output(s) :
                        yield err

                import os
                if s.data.errno != os.errno.ENOMEM :
                        yield Fail("test program failed but not with ENOMEM",
                                s.data.errno, s.data.command)

class Panic_Runner(Fail_Runner) :
        def __init__(s, command, source, xerrno=None) :
                if xerrno is None:
                        xerrno = 255
                        panic_arg = '--panic'
                else:
                        panic_arg = ('--panic=%d' % xerrno)

                Fail_Runner.__init__(s, [command, panic_arg], source, xerrno=xerrno ) ;

        def peck_output(s) :
                for err in Fail_Runner.check_output(s) :
                        yield err

                if s.data.errno != s.xerrno :
                        yield Fail("test program failed but not with %d" % s.xerrno,
                                s.data.errno, s.data.command)


class Elm_Panic_Runner(Panic_Runner) :
        # FIX: the different errors do not have very consistent formats
        err_matchers = Elm_Fail_Runner.err_matchers + compile_matchers ([
                ('PANIC', br'^PANIC! \(test_elm.c:[0-9]+ in (?P<n>main)\):'+
                          br' .*'),
                ])



class Elm_Fail_Panic_Runner(Panic_Runner) :
        err_matchers = Elm_Fail_Runner.err_matchers + compile_matchers ([
                ('LOGFAILED', br'^LOGFAILED \(in test_elm.c:(?P<n>main)\):'+
                              br' Error logging error.'),
               ])




if __name__ == "__main__":
        from sys import stdout, stderr

        print('elm-test with panic ...')
        trunner  = Elm_Panic_Runner('./elm-test', 'test_elm.c')
        tresults = run_main(trunner)
        results = tresults

        results.check_found( results.run - {'main'} )
        results.check_run( results.src - {'test_malloc'} )
        results.check_matched('passed', results.run - {'main'} )
        results.check_matched('NOMEM', set() )
        results.check_matched('PANIC', {'main',});
        stderr.flush()
        stdout.flush()

        print('elm-test with SYS_PANIC ...')
        trunner  = Elm_Panic_Runner('./elm-test', 'test_elm.c', xerrno=13)
        tresults = run_main(trunner)
        results = tresults

        results.check_found( results.run - {'main'} )
        results.check_run( results.src - {'test_malloc'} )
        results.check_matched('passed', results.run - {'main'} )
        results.check_matched('NOMEM', set() )
        results.check_matched('PANIC', {'main',});
        stderr.flush()
        stdout.flush()



        print('elm-fail with panic ...')
        prunner  = Elm_Fail_Panic_Runner('./elm-fail', 'test_elm.c')
        presults = run_main(prunner)
        results = presults

        results.check_found( results.run - {'main'} )
        results.check_run( results.src - {'test_malloc'} )
        results.check_matched('passed', results.run - {'test_logging', 'main'} )
        results.check_matched('NOMEM', set() )
        results.check_matched('LOGFAILED', {'test_logging',
                                            'test_debug_logger',
                                            'main'})
        stderr.flush()
        stdout.flush()

        print('elm-fail with out panic ...')
        runner  = Elm_Fail_Runner('./elm-fail', 'test_elm.c')
        results = run_main(runner)

        results.check_found( results.run )
        results.check_run( results.src )
        results.check_matched('passed', results.run - {'test_logging'} )
        results.check_matched('NOMEM', {'test_malloc'} )
        results.check_matched('LOGFAILED', {'test_logging',
                                            'test_debug_logger'})

        stderr.flush()
        stdout.flush()

        sys.exit(results.errno or presults.errno or tresults.errno)
