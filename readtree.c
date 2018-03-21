#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/limits.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "readtree.h"

#define MAX_IN_DIR 1000000

#define MIN_READ 16184
#define MIN_READ_DIR 128


// Internal representation of a directory entry which have not read yet.
typedef struct
{
        // full path (i.e. regardless of tree root) to the object.
        char *full_path;
        // The final component of `path`
        const char *name;
        // The file-type as in a struct dirent (see readdir(1) or <dirent.h>),
        // except that it never contains DT_UNKNOWN or DT_LINK.  If the real
        // dirent contained one of those values, we will have used stat() to
        // find out the truth.
        int de_type;
} Stub_;

bool read_tree_accept_suffix_(const void *arg, const char *path, const char *fname)
{
        const char *suff = arg;
        assert(suff);
        assert(fname);
        size_t n_suff = strlen(suff), n = strlen(fname);
        if(n_suff > n)
                return false;
        return !memcmp(suff, fname + n - n_suff, n_suff);
}

bool read_tree_accept_all_(const void *arg, const char *path, const char *name)
{
        return true;
}

static Tree *read_tree_(
        const ReadTreeConf *conf,
        const char *root,
        unsigned *pnsub,
        Error **perr);

static char *read_file_(const char *path, unsigned *psize, Error **perr)
{
        errno = 0;
        size_t used = 0, block_size = MIN_READ + 1;
        char *block = NULL;
        int fd = open(path, O_RDONLY);
        if(fd < 0) {
                *perr = IO_ERROR(path, errno, "Opening file");
                return NULL;
        }

        block = malloc(block_size);
        for(;;) {
                if(!block) {
                        close(fd);
                        PANIC_NOMEM();
                }

                assert(block_size - used > 1);
                ssize_t n = read(fd, block + used, block_size - used - 1);
                if(n < 0) {
                        *perr = IO_ERROR(path, errno, "Reading file");
                        goto error;
                }
                //LOG_F(dbg_log, "Read %ld bytes from %s", n, path);
                if(n == 0) {
                        goto eof;
                }

                used += n;
                if(block_size - used >= (1 + MIN_READ)) {
                        continue;
                }

                if((block_size *= 2) > UINT_MAX) {
                        *perr = IO_ERROR(path, EINVAL, "Reading too big a file");
                        goto error;
                }

                block = realloc(block, block_size *= 2);
        };

eof:
        block = realloc(block, used + 1);
        block[used] = 0;
        do close(fd); while(errno == EINTR);
        if(errno) {
                *perr = IO_ERROR(path, errno, "Closing file");
                free(block);
                return NULL;
        }
        *psize = used;
        //LOG_F(dbg_log, "Successfully read file %s (%u bytes).", path, *psize);
        return block;

error:
        assert(fd >= 0);
        do close(fd); while(errno == EINTR);
        free(block);
        return NULL;
}

static int de_type_(const char *path, const struct dirent *de)
{
        unsigned char de_type = de->d_type;
        if(de_type != DT_UNKNOWN && de_type != DT_LNK)
                return de_type;

        struct stat st;
        if(0 >  stat(path, &st)) {
                int ern = errno;
                //LOG_F(err_log, "stat(%s) failed: %s", path, strerror(ern));
                errno = ern;
                return -1;
        }
        //LOG_F(dbg_log, "stat(%s) returns mode %0x", path, S_IFBLK);
        switch(st.st_mode  & S_IFMT) {
        case S_IFBLK: return DT_BLK;
        case S_IFCHR: return DT_CHR;
        case S_IFIFO: return DT_FIFO;
        case S_IFDIR: return DT_DIR;
        case S_IFREG: return DT_REG;
        case S_IFSOCK: return DT_SOCK;
        case S_IFLNK:
                PANIC("stat of %s returned S_IFLINK!", path);
        default:
                PANIC("Unknown filetype %x from stat() of %s!",
                        (unsigned)(st.st_mode  & S_IFMT), path);
                return -1;
        }
}

static Error *from_stub_(
        const ReadTreeConf *conf,
        unsigned root_len,
        Tree *pr,
        const Stub_ stub)
{
        assert(pr);
        const char *name = stub.name;
        if(!name)
                PANIC("NULL name from scandir of %s!", stub.full_path);

        assert(stub.full_path[root_len] == '/');
        Tree r = {
                .full_path = stub.full_path,
                .path = stub.full_path + root_len + 1,
        };
        Error *err = NULL;

        switch(stub.de_type) {
        case DT_DIR:
                r.sub = read_tree_(conf, r.full_path, &r.nsub, &err);
                break;
        case DT_LNK:
                r.content = read_file_(r.full_path, &r.size, &err);
                break;
        case DT_REG:
                r.content = read_file_(r.full_path, &r.size, &err);
                break;
        default:
                return IO_ERROR(r.full_path, EINVAL,
                "Reading something that is neither a file nor directory.");
        }

        if(err)
                return err;
        *pr = r;
        return NULL;
}

static void destroy_tree_(Tree t)
{
        free(t.full_path);
        free(t.content);
        for(unsigned k = 0; k < t.nsub; k++) {
                destroy_tree_(t.sub[k]);
        }
        free(t.sub);
}


static bool reject_early(const ReadTreeConf *conf, const char *name)
{
        assert(name);
        return *name == '.';
}

static bool accept(const ReadTreeConf *conf, Stub_ stub, Error **perr)
{
        AcceptClosure closure;
        switch(stub.de_type) {
        case DT_DIR: closure = conf->accept_dir; break;
        case DT_REG: closure = conf->accept_file; break;
        default:
                *perr = IO_ERROR(stub.full_path, EINVAL, "Unknown filetype");
                return false;
        }

        return closure.fun_(closure.arg_, stub.full_path, stub.name);
}


static int qsort_fun_(const void *va, const void *vb, void *arg)
{
        const Stub_ *a = va, *b = vb;
        const char *name_a = a->name;
        const char *name_b = b->name;
        return strcmp(name_a, name_b);
}

static Error *next_stub_(
        const ReadTreeConf *conf,
        Stub_ *pstub,
        const char *full_dir_name,
        DIR *dir)
{
        struct dirent *de;
        if(!(de = readdir(dir))) {
                if(!errno) {
                        *pstub = (Stub_){0};
                        return NULL;
                }
                IO_PANIC(full_dir_name, errno,
                        "readdir() failed after opendir()");
        }

        if(reject_early(conf, de->d_name)) {
                return next_stub_(conf, pstub, full_dir_name, dir);
        }

        Stub_ tde;
        Error *err = NULL;

        const char *fname = de->d_name;
        size_t nf = strlen(fname);
        size_t nd = strlen(full_dir_name);
        if(full_dir_name[nd-1] == '/')
                // FIX:
                PANIC("ReadTtee allowed an untrimmed root directory");
        tde.full_path = MALLOC(nf + nd + 2);
        memcpy(tde.full_path, full_dir_name, nd);
        tde.full_path[nd] = '/';
        memcpy(tde.full_path + nd + 1, fname, nf + 1);

        int de_type = de_type_(tde.full_path, de);
        if(de_type < 0) {
                err = IO_ERROR(tde.full_path, errno,
                        "While getting file-type of directory entry");
                free(tde.full_path);
                goto done;
        }
        tde.de_type = de_type;

        int name_len = strnlen(de->d_name, NAME_MAX + 1);
        if(name_len > NAME_MAX)
                PANIC("filename under %s longer than %d bytes",
                        full_dir_name, NAME_MAX);
        tde.name = tde.full_path + strlen(tde.full_path) - name_len;

        if(accept(conf, tde, &err)) {
                assert(!err);
                *pstub = tde;
                return NULL;
        }
        free(tde.full_path);

done:
        if(err) {
                *pstub = (Stub_){0};
                return err;
        }
        return next_stub_(conf, pstub, full_dir_name, dir);
}


static Error *load_stubv_(
        const ReadTreeConf *conf,
        const char *full_dir_path,
        unsigned *pnstub,
        Stub_ **pstubv)
{
        assert(full_dir_path);
        errno = 0;
        DIR *dir = opendir(full_dir_path);
        if(!dir) {
                return IO_ERROR(full_dir_path, errno, "Readtree opening dir");
        }

        Stub_ *stubv = NULL;
        Error *err = NULL;
        int used = 0, alloced = 0;

        for(;;) {
                if(used == alloced) {
                        if(!(alloced *= 2))
                                alloced = 1;
                        //LOG_F(dbg_log, "allocating %d dirent ptrs", alloced);
                        stubv = realloc(stubv, alloced * sizeof stubv[0]);
                        if(!stubv) {
                                PANIC_NOMEM();
                        }
                }

                Stub_ stub;
                err = next_stub_(conf, &stub, full_dir_path, dir);
                if(!stub.full_path)
                        break;

                assert(used < alloced);
                stubv[used++] = stub;
                if(used >= MAX_IN_DIR) {
                        err = ERROR("Directory %s has > %d entries!",
                                full_dir_path, MAX_IN_DIR);
                        used = 0;
                }
        }


        // Clean-up stage, on the good and bad paths both.  Bad (err != NULL)
        // implies used == 0, but sed == 0 can also happen on the good path.
        //LOG_F(dbg_log, "trimming alloc to %d dirent ptrs", used);
        stubv = realloc(stubv, used * sizeof stubv[0]);
        if(!stubv && used) {
                PANIC_NOMEM();
        }

        qsort_r(stubv, used, sizeof stubv[0], qsort_fun_, NULL);
        *pstubv = stubv;
        *pnstub = used;
        closedir(dir);
        return err;
}

static Tree *read_tree_(
        const ReadTreeConf *conf,
        const char *full_dir_path,
        unsigned *pnsub,
        Error **perr)
{
        Stub_ *stub;
        unsigned n;
        Error * err = load_stubv_(conf, full_dir_path, &n, &stub);
        if(err) {
                *perr = err;
                return NULL;
        }
        assert(stub || !n);
        struct Tree *subv = MALLOC(sizeof(Tree)*n);

        int nconverted;
        unsigned root_len = strlen(conf->root);
        for(nconverted = 0; nconverted < n; nconverted++) {
                err = from_stub_(
                        conf,
                        root_len,
                        subv + nconverted,
                        stub[nconverted]);
                if(err)
                        break;
        }

        assert(n >= 0);
        if(!err) {
                // Happy path: just return the subv.
                *pnsub = (unsigned)n;
        } else {
                // Sad path: clean up stubs and already-converted trees.
                for(int k = 0; k < n; k++) {
                        free(stub[k].full_path);
                }
                for(int k = 0; k < nconverted; k++) {
                        destroy_tree_(subv[k]);
                }
                free(subv);
                subv = NULL;
        }

        free(stub);
        *perr = err;
        return subv;
}

void destroy_tree(Tree *tree)
{
        if(!tree)
                return;
        destroy_tree_(*tree);
        free(tree);
}

Error *fill_out_config_(ReadTreeConf *conf)
{
        if(!conf->accept_dir.fun_)
                conf->accept_dir = (AcceptClosure)READ_TREE_ACCEPT_ALL();
        if(!conf->accept_file.fun_)
                conf->accept_file = (AcceptClosure)READ_TREE_ACCEPT_ALL();
        return NULL;
}

Error *read_tree(const ReadTreeConf *pconf, Tree **ptree)
{
        if(!pconf)
                return ERROR("ReadTreeConf is NULL");
        ReadTreeConf conf = *pconf;
        fill_out_config_(&conf);
        if(!conf.root)
                return ERROR("Configured ReadTree 'root' is null");
        size_t root_len = strnlen(conf.root, PATH_MAX + 1);
        if(root_len > PATH_MAX)
                return ERROR("Configured ReadTree 'root' is %lu bytes.  "
                             "Max length is %u", root_len, (unsigned)PATH_MAX);

        if(!(conf.root = strdup(conf.root)))
                PANIC_NOMEM();

        if(!ptree)
                PANIC("'ptree' is null");
        Tree t = {
                .full_path = conf.root,
                .path = conf.root + strlen(conf.root),
        };
        if(!t.full_path) {
                PANIC_NOMEM();
        }
        Error *err = NULL;
        t.sub = read_tree_(&conf, conf.root, &t.nsub, &err);
        if(err) {
                free(conf.root);
                return err;
        }
        Tree *tree = malloc(sizeof(Tree));
        if(!tree) {
                destroy_tree_(t);
                PANIC_NOMEM();
        }
        *tree = t;
        *ptree = tree;
        return NULL;
}


