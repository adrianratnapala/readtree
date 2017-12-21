/*----------------------------------------------------------------------------
  elm: errors, logging and malloc.

  The elm module provies three commonly used utilities which are conceptually
  quite different, but can entangle at the impelmentation level.  These
  are:

  errors  - describes error events and helps handle them (for now by exiting
            the program, but we might do exceptions in future).

  logging - writes (log) events to a stdio stream.

  malloc  - wrapper for malloc() to allocate memory or die trying.  We never
            return anything but success (in future it will be possible to trap
            failures).


  Copyright (C) 2012, Adrian Ratnapala, under the ISC license. See file LICENSE.
*/

#ifndef ELM_H
#define ELM_H

#ifndef CHECK_FMT
# ifdef __GNUC__
#  define CHECK_FMT(N) __attribute__((format(printf,(N),(N)+1)))
# else
#  define CHECK_FMT(N)
# endif
#endif

#include <stdio.h>
#include <setjmp.h>

// ---------------------------------------------------------------------------------

/*
  Elm version IDs are utf-8 strings comprised of "elm0-" followed by a sequence
  of one or more numbers:
        * Each number is in the range [0, 1000).
        * Each number is space padded to three characters.
        * Each number (including the last!) is followed a "." or "-".

  IDs can be converted reversibly into a conventional looking version strings
  by stripping out the spaces and any trailing '.'; unlike conventional
  strings, IDs can be compared using strcmp().

  Here are some examples:
        0.5 release         "elm0-  0.  5."
        0.42 pre            "elm0-  0. 42-"
        0.42 pre 2          "elm0-  0. 42-  2."   // try not to do this.
        0.42 release        "elm0-  0. 42."
        0.42 post           "elm0-  0. 42.   ."
        0.42.3 release      "elm0-  0. 42.  3."

  Here the unnumbered "pre" and "post" describe everyday builds done during
  debugging.

  You can obtain the version of elm that you compiled against using the macro:
*/

#ifndef ELM_VERSION
//#define ELM_VERSION "elm0-  0.  7    ."  // post-release
#define ELM_VERSION "elm0-  0.  7.  1."     // exact-release
//#define ELM_VERSION "elm0-  0.  7-"  // pre-release
#endif

/* At run time you can get the version ID of the linked library using:*/
extern const char *elm_version();

/*
  Many parts of elm use the LogMeta struct to hold metadata about various
  events that happen in the program.  For now these metadata are only the
  source code location (filename, line number, function) where the event
  occurred.  In future we might include things like timestamps.
*/
typedef struct LogMeta LogMeta;
struct LogMeta {
        const char *func;
        const char *file;
        int line;
};


/*-- Errors -------------------------------------------------------------------
  Errors are our poor man's exception objects.  They are created when some bad
  event happens, and contain data to describe that event.
*/
typedef struct Error Error;

/* You can write an error to a stream using: */
int error_fwrite(Error *e, FILE *out);

/* To discard an error object, call destroy_error. */
extern void destroy_error(Error *e);

/*
  Sometimes you want to push through a sequence of operations that might fail,
  and only report one of the errors (usually the first).  A helper for this is:
*/
extern Error *keep_first_error(Error *one, Error *two);
/*
  This function returns `one` unless it is NULL and two is not-NULL.  Any
  unused error is silently destroyed.
*/

/*
  The compile-time C type of all errors is `Error *`; but at run-time each
  error has a field `error->type` which can be compared to known constants.
  These constants are actually pointers to C structs of type:
*/
typedef const struct ErrorType ErrorType;

/*
  Some error types are pre-defined by ELM0, the simplest is plain-old
  `error_type`.  You can generate plain errors created by passing a printf-like
  message string to the ERROR macro.  For example:

        return ERROR("There were %d %s sitting on the wall.", -3, "bottles");

  The formated message will be printed every time `error_fwrite` is called.
*/
ErrorType *const error_type;
#define ERROR(...) ERROR_WITH(error, __VA_ARGS__)

/*
  You can define your own error types.  The easiest case is when you want
  errors that behave just like plain errors, but which have some non-standard
  type-constant.  For this, just create zero-filled ErrorType struct:

        ErrorType _my_error_type = {0},
                  *my_error_type = &_my_error_type;

  Generate errors of this type by calling:

        ERROR_WITH(my_error, "Only %d bottles left", 2)


  I recommend for each new type you define, you wrap ERROR_WITH in two macros:

       #define MY_ERROR(msg, ...) ERROR_WITH(my_error, msg, __VA_ARGS__)
       #define MY_PANIC(msg, ...) panic(MY_ERROR(msg, __VA_ARGS__))

  (see below for a discussion of `panic`).
*/

Error *elm_mkerr(const ErrorType *etype, const char *file, int line, const char *func);
Error *init_error(Error *e, const char *zfmt, ...) CHECK_FMT(2);
#define ERROR_ALLOC(T) elm_mkerr(T##_type, __FILE__,__LINE__,__func__)
#define ERROR_WITH(T, ...) init_error(ERROR_ALLOC(T), __VA_ARGS__)

/*
  Notice the error is actually created with ERROR_ALLOC, but initialised by
  `init_error`.  You must always use ERROR_ALLOC, but you can define your own
  initialiser functions (constructors).

  To see how, lets look at the Error struct itself:
*/

struct Error {
        const ErrorType *type;  // method table
        void      *data;  // different error types define meanings for this
        LogMeta    meta;  // type invariant meta data (where & when)
};

/*
  `.type` and `.meta` are initialised for you by ERROR_ALLOC.  `data` is
  initialised to NULL; but the default behaviour (which happens when your
  ErrorType is zero-filled) is that `error_fwrite` and `destroy_error` treat it
  as a human-readable, null-terminated string that can destroyed using
  `free()`.

  So you might define your type as

        Error *my_error_init(Error* e, const char *arg_s, int arg_n)
        {
                e->data = ... some free()able string  ....
                return e;
        }

        #define MY_ERROR(S, N) my_error_init(ERROR_ALLOC(my_error), S, N)
        #define MY_PANIC(S, N) panic(MY_ERROR(S, N))

  Note that the intialiser must always return the error it was given.


  You can customise further by implementing non-default method for ErrorType.
  These are defined as:
*/

struct ErrorType {
        /* fwrite sends a human readable representation to a stdio stream. */
        int  (*fwrite)(Error *e, FILE *out);
        /* cleanup error->data. */
        void (*cleanup)(void *data);
};

/*
  Now you `data` pointer can be anything and it is your responsibility to make
  sure the constructor, `.fwrite` and `.cleanup` work together to handle it.

  * If `.fwrite` is non-null, then `error_fwrite` simply wraps your method.
  * If `.cleanup` is non-null it is responsible for deallocating your `data`
    only, `destroy_error` will take care of the Error object itself.



  For an example of this mechanism, look at the el predefined type `sys_error`
  which wraps up `errno` like error codes.  If you have an errno, you can do:

        SYS_ERROR(errno, msg_prefix[, ...])

  msg_prefix is an explanation of what you were doing when the error happened;
  it can also take printf-like varargs.  If your error is linked to a specific
  file which you know the name of, then you should use:

        IO_ERROR(filename, errno, msg_prefix[, ...])

  But in spite of the name, IO_ERROR actually produces a SYS_ERROR object.
*/
extern Error *init_sys_error(Error *e, const char* zname, int errnum,
                                       const char *zmsg, ...);
extern const ErrorType *const sys_error_type;
#define IO_ERROR(F,N,...) \
        init_sys_error(ERROR_ALLOC(sys_error), F, (N), __VA_ARGS__)
#define SYS_ERROR(N,...)  IO_ERROR(0, N, __VA_ARGS__)

#define SYS_PANIC(N,...) panic(SYS_ERROR(N, __VA_ARGS__))
#define IO_PANIC(F,N,...) panic(IO_ERROR(F,N, __VA_ARGS__))

/* You can unpack a SYS_ERROR (or IO_ERROR) with: */
extern int sys_error(Error *e, char **zname, char **zmsg);
/*
   If `e` is NULL, sys_error returns zero, if `e` points to an error OTHER than
   SYS_ERROR, it returns -1; in both these cases it ignores zname and zmasg.
   Otherwise returns errno; if zname != NULL, *zname is the filename (or null
   if none exists), if zmsg != NULL *zmsg is the same as strerror(returned
   errno), or null if there is no SYS_ERROR.  Both strings are returned in
   free()'able buffers.
*/


/*-- Panic --------------------------------------------------------------------
  Extreme errors can be handled using panic(), which either:
        -> Logs a standard error and then calls exit(), or
        -> Unwinds the stack much like exception handling.

  Be warned, this is C, there is no garbage collection or automatic destructor
  calling, which can make stack unwinding less useful than in other languages.
*/

/* You can panic using an valid (and non-NULL) error object by calling. */
extern void panic(Error *e);

/* panic_if is like panic() except it is a no-op if the argument is NULL. */
static inline void panic_if(Error *e)
{
        if(e)
                panic(e);
}

/*
   Or you can create a new error in analogy to ERROR_WITH and ERROR, and then
   immediately panic with it.
*/
#define PANIC_WITH(T, ...) panic(ERROR_WITH(T, __VA_ARGS__))
#define PANIC(...) panic(ERROR(__VA_ARGS__)) // a new message error


/*
    By default, panic() will result in a call to exit().  If you don't want
    this to happen, you must first allocate a PanicReturn object R and then call
    the macro TRY(R)
*/
typedef struct PanicReturn PanicReturn;
struct PanicReturn {
        jmp_buf jmp_buf; /*MUST be first*/
        PanicReturn *prev;
        Error       *error;
};

/*
    TRY returns either NULL or an error raised by a panic.  If it returns NULL,
    code exectues as normal until a panic, at which point execution returns to
    the TRY call which will now return the corresponding error.  In addition
    the ".error" member of the PanicReturn object will also be set to the same
    error.  Once you no longer want to protect your errors this way, call
    NO_WORRIES.  Every TRY must have a corresponding NO_WORRIES; any number of
    TRY / NO_WORRIES pairs can be nested.

    One good way to use this is

        PanicReturn ret;

        if(ret.error = TRY(ret)) { // the assignment he is redundant, but nice.
                // handle the error
                destroy_error(ret.error);     // remember to do this!
                return -1;
        }

        ... do something which might panic() ...

        NO_WORRIES(ret)

    If you get an error that you can't handle, you can always panic again.
    This is because code which executes when "TRY(...) != NULL" behaves as if
    NO_WORRIES has already been called.  The above example avoids a double call
    to NO_WORRIES because of the "return"; if you want to carry on inside your
    function after trapping an error, you need

            PanicReturn ret;
            if( TRY(ret) ) {
                ... clean up after error ...
            } else {
                ... something dangerous
                NO_WORRIES(ret);
            }

            ... rest of function ...
*/
#define TRY(R) (_PANIC_SET(R) ? _panic_pop(&(R)) :  0)
#define NO_WORRIES(R) _panic_pop(&(R))

/*
   If you ever want to know whether or not you are inside a TRY/NO_WORRIES
   pair, you can call
 */
int panic_is_caught();

#define _PANIC_SET(R) (_panic_set_return(&(R))||setjmp(*(jmp_buf*)(&R)))
extern Error *_panic_pop(PanicReturn *check);
extern int _panic_set_return(PanicReturn *ret);

/* One common reason to catch serious errors is in unit tests - to see that
   code is throwing them when it should.  Assuming you use 0unit, you can make
   these tests cleaner using:
*/
#define CHK_PANIC(T, R)\
        {if( (R).error = TRY((R)) ){          \
                CHK((R).error->type ==(T));   \
                destroy_error((R).error);     \
        } else {

#define CHK_PANIC_END(R)                               \
                NO_WORRIES((R));                       \
                CHK(!"Expected panic never happened!");\
        }}

/* The idea is you do:

        PanicReturn ret;
        CHK_PANIC(expected_error_type, ret);
                ... some code here that MUST panic() with an error of
                    expected_error_type ...
        END_CHK_PANIC(ret);
*/



/*-- Logging ------------------------------------------------------------------
Logger objects take human-readable messages about events in your program,
decorate them with metadata and then (optionally) write them some stdio stream
(FILE*).  Different loggers can decorate messages differently, and write them
to different streams.  Loggers might also just swallow the messages; this makes
it possible to log verbosely, but then suppress annoying messages without
changing much code.
*/

typedef struct Logger Logger;

/*
  Elm defines three loggers, they are always available, without initialisation.
*/
extern Logger
       _elm_null_log,// Log to nowhere, swallows all messages,
       _elm_std_log, // Log to standard output
       _elm_err_log, // Log to standard error
       _elm_dbg_log; // Log to standard error, but include medatadata
                 //     (FILENAME:LINENUM in FUNCNAME)


#define null_log (&_elm_null_log)
#define std_log (&_elm_std_log)
#define err_log (&_elm_err_log)
#define dbg_log (&_elm_dbg_log)

/*
  One common use for loggers (over printf), is to allow you to write code that
  always sends log messages which can be switched off en masse without changing
  the bulk of the code.  You can do this by referring to these four logs by
  aliases, such a "info", "verbose" or just "log".  Then you can assign these
  aliases to "null_log" when you don't want to see the corresponding output.

  For more flexibility, you might want to create you own loggers by calling
*/
extern Logger *new_logger(const char *zname, FILE *stream, const char *opts);
/*
  which creates a logger that writes to "stream". Its name "zname", is
  prepended before all output messages (along with some punctuation).  If
  "stream" is NULL, you will get a null logger; it will silently ignore all
  messages.

  You can modify the style of logging by setting "opts" to be non-NULL, this
  string is just a list of option charactors, the only one defined so far is
  'd' which causes the logger to print out the source location metadata (like
  the debug logger).  All other option characters are ignored, in this version
  of elm.  opts==NULL is equivalent to opts="".

  Loggers are reference counted, you can increment and decrement references
  using:
*/
Logger *ref_logger(Logger *lg);
Error *destroy_logger(Logger *lg);
/*
  (In spite of its name, `destroy_logger` only destroys the logger when
  the reference count drops to zero).  These function do nothing at all
  to the standard (statically allocated) loggers.
*/

/*
  To log a message, call
        LOG_F(logger, fmt, ...)
  Where "logger" is the logger,
        "fmt"    is a printf-style format string,
        "..."    are zero or more arguments to munge into the string.

  This macro returns the number of bytes written to the output stream, or -1 on
  error, in which case errno is set appropriately.
*/

#define LOG_F(L,...) log_f(L, __FILE__, __LINE__, __func__,  __VA_ARGS__)
extern int log_f(Logger *lg,
           const char *file,
           int         line,
           const char *func,
           const char *fmt,
           ...) CHECK_FMT(5);

/*
   You can also log an error using log_error.  The metadata (such as the line
   number) will come from the error, not from the location of the logging call.
   The human readable text produced by the err->fwrite() method will also be
   logged.
 */
extern int log_error(Logger *lg, Error *err);

#define LOG_UNLESS(L, T) do {\
                        if(!(T)) log_f(L, __FILE__, __LINE__, __func__, #T); \
               } while(0)

#define DBG_UNLESS(T) LOG_UNLESS(dbg_log, T)



/*-- Malloc ------------------------------------------------------------------

  Code is much simpler when there are no possible ways to fail.  The following
  macros help you in the case where the only possible error is failed memory
  allocation (running out of virtual address space).

  MALLOC() just wraps malloc(), except that it never returns NULL.  If malloc()
  fails, the program panics with the special nomem error type.  Like any
  other panic, this error is catchable.

  If you do detect an out of memory condition yourself, but you want to treat
  it in the same way as a failed MALLOC(), you can call PANIC_NOMEM().
  ERROR_NOMEM() returns an instance of the nomem error without panicing.

  ZALLOC() is the same as MALLOC() except it zeros the allocated memory.
*/


extern const ErrorType *const nomem_error_type;

typedef int(*PanicRescue)();

PanicRescue panic_rescue_nomem(PanicRescue new_rescue);

extern Error *error_nomem(const char* file, int line, const char *func);
extern void *malloc_or_die(const char* file, int line, const char *func, size_t n);
#define PANIC_NOMEM() panic(ERROR_NOMEM())
#define ERROR_NOMEM() error_nomem(__FILE__, __LINE__, __func__)

#define MALLOC(N) malloc_or_die(__FILE__, __LINE__, __func__, N)




#endif /*ELM_H*/
