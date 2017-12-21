/*----------------------------------------------------------------------------
  elm.c: errors, logging and malloc.

  (See README for background and elm.h for real documentation).

  Copyright (C) 2012, Adrian Ratnapala, under the ISC license. See file LICENSE.
*/


#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

#include <sys/resource.h>

#ifdef TEST
#include "0unit.h"
#endif
#include "elm.h"

/* Set this to one to test some emergency-fail code. */
#ifndef FAKE_FAIL
#define FAKE_FAIL 0
#endif

const char *elm_version()
{
        return ELM_VERSION;
}


// Errors ---------------------------------------------------------------------

static int no_rescue()
{
        return -1;
}

static void* panic_nomem(const char* file, int line, const char *func)
/* Report an out-of-memory condition with panic(). */
{
        panic(error_nomem(file, line, func));
        return NULL;
}

Error *elm_mkerr(const ErrorType *etype, const char *file, int line, const char *func)
/* malloc()s an error & fills out the metadata. */
{
        Error* e = malloc(sizeof(Error));
        if(!e)
                panic_nomem(file, line, func);

        *e = (Error){
                .type = etype,
                .meta = (LogMeta) {
                        .file = file,
                        .line = line,
                        .func = func,
                },
        };
        return e;
}

void destroy_error(Error *e)
/* calls an error's cleanup method, and then free()s the error. */
{
        if(!e)
                return;

        assert(e->type);
        if(e->type->cleanup)
                e->type->cleanup(e->data);
        else
                free(e->data);

        if(e->type != nomem_error_type)
                free(e);
}

Error *keep_first_error(Error *one, Error *two)
{
        if(!one)
                return two;
        destroy_error(two);
        return one;
}



// -- Message Error - Just wraps a message string in an error object.

int error_fwrite(Error *e, FILE *out)
/* Write error to stdio in human readable form. */
{
        const ErrorType *etype = e->type;
        int (*cb)(Error *, FILE*) = etype->fwrite;
        if(cb)
                return cb(e, out);
        return fwrite(e->data, 1, strlen(e->data), out);
}

static const ErrorType _error_type = {0};

const ErrorType *const error_type = &_error_type;

extern Error *init_error_v(Error *e, const char *zfmt, va_list va)
{
        e->data = (char*)zfmt; // in case of panic
        if( vasprintf((char**)&e->data, zfmt, va) < 0 )
                panic(e);

        return e;
}

Error *init_error(Error *e, const char *zfmt, ...)
{
        va_list va;
        va_start(va, zfmt);

        PanicReturn ret;
        Error *pe = TRY(ret);
        if(!pe) {
                init_error_v(e, zfmt, va);
                NO_WORRIES(ret);
        }
        va_end(va);

        if(pe)
                panic(pe);
        return e;
}

// -- Sysstem Error - FIX: what are they? ---------------------------

typedef struct
{
        char *zname;
        int   errnum;
        char  zmsg[];
} Sys_Error;

int sys_error_fwrite(Error *e, FILE *out)
/* Write error to stdio in human readable form. */
{
        Sys_Error  *se = e->data;
        const char *es = strerror(se->errnum);

        if(!se->zname)
                return fprintf(out, "%s: %s", se->zmsg, es);
        else
                return fprintf(out, "%s (%s): %s", se->zmsg, se->zname, es);

}

static const ErrorType _sys_error_type = {
        fwrite    : sys_error_fwrite,
        cleanup   : free
};

const ErrorType *const sys_error_type = &_sys_error_type;

Error *init_sys_error(Error *e, const char* zname, int errnum,
                      const char *zfmt, ...)
{
        char *zmsg = NULL;
        va_list va;

        va_start(va, zfmt);
        int n = vasprintf(&zmsg, zfmt, va);
        va_end(va);
        if(n < 0) //do the next best thing.
                PANIC("SYS_ERROR %s (errno = %d)", zfmt, errnum);

        int nmsg  = strlen(zmsg) + 1;
        int nname = (zname ? strlen(zname) + 1 : 0);
        Sys_Error *se = malloc(sizeof(Sys_Error) + nmsg + nname );
        if(!se) {
                free(zmsg);
                panic_nomem(e->meta.file, e->meta.line, e->meta.func);
        }

        se->errnum = errnum;
        memcpy(se->zmsg,  zmsg,  nmsg);
        if(!zname)
                se->zname = 0;
        else {
                se->zname = se->zmsg + nmsg;
                memcpy(se->zname, zname, nname);
        }

        e->data = se;

        free(zmsg);
        errno = 0;
        return e;
}


int sys_error(Error *e, char **zname, char **zmsg)
{
        if(zname)
                *zname = NULL;
        if(zmsg)
                *zmsg = NULL;

        if(!e)
                return 0;
        if(e->type != sys_error_type)
                return -1;

        Sys_Error *se = e->data;

        const char *n = se->zname;
        if(zname && n && !(*zname = strdup(n)))
                PANIC_NOMEM();
        if((zmsg) && !(*zmsg = strdup(se->zmsg)))
                PANIC_NOMEM();

        return se->errnum;
}



// Raw Stderr -----------------------------------------------------------------

static ssize_t emergency_write(const char *str)
{
        return write(2, str, strlen(str));
}

static int emergency_message(const char *pre, LogMeta *meta, const char *post)
/* Log to stderr without using stdio. meta is ignored if it equals NULL. */
{
        int n = 0;
        n += emergency_write(pre);

        if(meta) {
                n += emergency_write(" (in ");
                n += emergency_write(meta->file);
                n += emergency_write(":");
                n += emergency_write(meta->func);
                n += emergency_write(")");
        }

        n += emergency_write(": ");
        n += emergency_write(post);
        n += emergency_write("\n");
        fsync(2);
        return n;
}

// Logs -----------------------------------------------------------------------

typedef int (*VPrintf)(Logger *lg, LogMeta *meta, const char *msg, va_list va);
typedef int (*FWritePrefix)(Logger *lg, LogMeta *meta);

struct Logger {
        /*Loggers decorate messages, and send them to a stream. Or drop them.*/
        int  nrefs;
        FILE *stream;         // the output stream
        const char *zname;    // prefix text emitted before each message


        /* User redefinable functions (methods) */
        VPrintf vprintf;
        FWritePrefix fwrite_prefix;
};

static void init_static_logger(Logger *lg)
/* Idempotently ensures initialisation of builtin loggers before each use. */
{
        if(lg->nrefs)
                return;

        switch( (uintptr_t)lg->stream ) {
        case 0: break; /* leave NULL logs alone */
        case 1: lg->stream = stdout; break;
        case 2: lg->stream = stderr; break;
        default :
                assert( !"init_static_logger: invalid stream constant!" );
                break;
        }

        lg->nrefs = -1;
}

static int log_prefix(Logger *lg, LogMeta *meta)
{
        return fprintf(lg->stream, "%s: ", lg->zname);
}

static int dbg_prefix(Logger *lg, LogMeta *meta)
{
        return fprintf(lg->stream, "%s (%s:%d in %s): ",
                lg->zname, meta->file, meta->line, meta->func);
}

static int log_vprintf(Logger *lg, LogMeta *meta, const char *msg, va_list va)
/* method: format a message vprintf style, then log it. */
{
        init_static_logger(lg);

        int nprefix = lg->fwrite_prefix(lg, meta);
        if ( nprefix <= 0 )
                goto no_write;

        int nbody = vfprintf(lg->stream, msg, va);
        if ( nbody <= 0 )
                goto no_write;

        if ( fputc('\n', lg->stream) == EOF)
                goto no_write;

        if( fflush(lg->stream) == EOF )
                goto no_write;

        if(!FAKE_FAIL)
                return nbody + nprefix + 1;

no_write:
        emergency_message("LOGFAILED", meta, msg);
        return -1;
}

int log_error(Logger *lg, Error *err)
/* Convert an error to a string, then log it. Metadata come from the error. */
{
        init_static_logger(lg);
        if(!lg->stream) // is this a null log?
                return 0;


        // this has to be a special case, else LibC might do its own malloc
        ErrorType *etype = err->type;
        if(etype == nomem_error_type)
                return etype->fwrite(err, lg->stream /* probably ignored */);


        if(FAKE_FAIL)
                goto no_write;

        int nprefix = lg->fwrite_prefix(lg, &err->meta);
        if ( nprefix <= 0 )
                goto no_write;

        // ask the error to write its own text
        int nbody = error_fwrite(err, lg->stream);
        if(nbody < 0)
                goto no_write;


        if ( fputc('\n', lg->stream) == EOF)
                goto no_write;

        if( fflush(lg->stream) == EOF )
                goto no_write;

        return nbody + nprefix + 1;

no_write:
        if(errno == ENOMEM)
                panic_nomem(err->meta.file,
                            err->meta.line,
                            err->meta.func
                           );
        emergency_message("LOGFAILED", &err->meta, "Error logging error.");
        return -1;
}

// A handful of builtin loggers are statically allocated.
Logger _elm_std_log = {
        stream  : (FILE*)1,
        zname   : "LOG",
        vprintf : log_vprintf,
        fwrite_prefix : log_prefix,
};

Logger _elm_err_log = {
        stream  : (FILE*)2,
        zname   : "ERROR",
        vprintf : log_vprintf,
        fwrite_prefix : log_prefix,
};

Logger _elm_dbg_log = {
        stream  : (FILE*)2,
        zname : "DBG",
        vprintf : log_vprintf,
        fwrite_prefix : dbg_prefix,
};

Logger _elm_null_log = {
        stream  : (FILE*)0,
        zname : "NULL",
        vprintf : log_vprintf,
        fwrite_prefix : dbg_prefix,
};


// User created loggers ------.

Logger *new_logger(const char *zname, FILE *stream, const char *opts)
/* Create a standard logger that writes to "stream". */
{
        FWritePrefix fwp = log_prefix;

        if(opts) {
                int ch;
                for(const char *o=opts; ch=*o; o++) switch(ch) {
                case 'd': fwp = dbg_prefix; continue;
                }
        }

        Logger *lg = malloc( sizeof(Logger) );
        if( !lg )
                PANIC_NOMEM();

        assert(zname);

        lg->stream  = stream;
        lg->vprintf = log_vprintf;
        lg->fwrite_prefix = fwp;
        lg->zname = strdup(zname);

        lg->nrefs = 1;
        return lg;
}

Logger *ref_logger(Logger *lg)
{
        int nrefs = lg->nrefs;
        if(nrefs > 0)
                lg->nrefs = ++nrefs;
        return lg;
}

Error *destroy_logger(Logger *lg)
/* Frees a non-builtin logger. */
{
        if(!lg)
                return NULL;
        int nrefs = lg->nrefs;
        if(nrefs <= 0) // static logger, do not touch.
                return NULL;
        if(--nrefs) {
                lg->nrefs = nrefs;
                return NULL;
        }

        free((char*)lg->zname);
        free(lg);
        return NULL;
}


int log_f(Logger *lg,
           const char *file,
           int         line,
           const char *func,
           const char *msg, ...)
/* Format a message vprintf style, then log it. */
{
        va_list va;
        int n;
        init_static_logger(lg);
        if(!lg->stream) // is this a null log?
                return 0;

        assert(lg);
        va_start(va, msg);
                LogMeta m = {
                file : file,
                line : line,
                func : func,
        };
        n = lg->vprintf(lg, &m, msg, va);
        va_end(va);
        return n;
}



// Malloc ---------------------------------------------------------------------
/*
        A malloc() wrapper that checks the results and exits on failure, after
        printing an error message to stderr.  These are macros, not functions,
        because they gather source-location metadata.

        MALLOC(N)     - Allocate N bytes (or die trying).
        ZALLOC(N)     - Allocate N zeroed bytes.
        PANIC_NOMEM() - Called when malloc fails.

        You can call PANIC_NOMEM yourself, if detect an out of memory
        condition.  Or if you want to supply your own metadata, you can call

        void panic_nomem(const char* file, int line, const char *func)

        In future, the exit might not be unconditional.  The panic might
        conditionally do a longjmp to a place where you can try to carry on.
*/

static int nomem_fwrite(Error *e, FILE *out);

static const ErrorType _nomem_error_type = {
        fwrite    : nomem_fwrite,
        cleanup   : NULL,
};

const ErrorType *const nomem_error_type = &_nomem_error_type;

static Error nomem_error =  {
        .type = &_nomem_error_type,
        .meta = {
                .line = -1,
                .file = "",
                .func = "",
        }
};

static int nomem_fwrite(Error *e, FILE *out)
{
        return emergency_message("NOMEM", &nomem_error.meta, "Out of virtual memory");
}

Error *error_nomem(const char* file, int line, const char *func)
{
        nomem_error.meta = (LogMeta){
                file : file,
                line : line,
                func : func,
        };
        return &nomem_error;
}

static PanicRescue nomem_rescue = no_rescue;

PanicRescue panic_rescue_nomem(PanicRescue new_rescue)
{
        PanicRescue old = nomem_rescue;
        if(new_rescue)
                nomem_rescue = new_rescue;
        return old;
}

void *malloc_or_die(const char* file, int line, const char *func, size_t n)
{
        void *ret = malloc(n);
        if(ret)
                return ret;
        if(!nomem_rescue() && (ret = malloc(n)))
                return ret;

        panic_nomem(file, line, func);
        return 0;
}

// -- Panic ----------------------------

static PanicReturn *_panic_return;

Error *_panic_pop(PanicReturn *check)
{
        assert(_panic_return);
        assert(_panic_return == check);
        _panic_return = _panic_return->prev;
        return check->error;
}

static void throw_panic(Error *err)
{
        assert(_panic_return);
        _panic_return->error = err;
        longjmp(_panic_return->jmp_buf, -1);
}

int _panic_set_return(PanicReturn *ret)
{
        ret->prev  = _panic_return;
        ret->error = 0;
        _panic_return = ret;
        return 0;
}

static void death_panic(Error *e)
{
        /*A glorious and righteous hack to hijack the most appropriate logger.*/
        init_static_logger(&_elm_dbg_log);
        Logger panic_log = _elm_dbg_log;
        panic_log.zname = "PANIC!";

        log_error( &panic_log, e);

        exit(e->type == nomem_error_type ? ENOMEM : sys_error(e, NULL, NULL));
}

void panic(Error *e)
{
        assert(e && e->type);
        if( _panic_return )
                throw_panic(e);
        else
                death_panic(e);
}

int panic_is_caught()
{
        return !!_panic_return;
}

