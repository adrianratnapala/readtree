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

#define LOG_ERR(...) LOG_F(err_log, __VA_ARGS__);
#if 0
#define LOG_DBG(...) LOG_F(null_log, __VA_ARGS__);
#else
#define LOG_DBG(...) LOG_F(dbg_log, __VA_ARGS__);
#endif

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

// Determine the Stub_.de_type of an filesystem object.
//
// If the dirent contains what we need, just us that, otherwise stat() the
// underlying object and convert the result.
static int de_type_(const char *full_path, const struct dirent *de)
{
        unsigned char de_type = de->d_type;
        if(de_type == DT_REG && de_type != DT_DIR)
                return de_type;

        struct stat st;
        if(0 >  stat(full_path, &st))
                return -errno;
        LOG_DBG("stat(%s) returns mode %0x", full_path, S_IFBLK);
        switch(st.st_mode  & S_IFMT) {
        case S_IFDIR: return DT_DIR;
        case S_IFREG: return DT_REG;
        case S_IFLNK:
                PANIC("stat of %s returned S_IFLINK!", full_path);
        default:
                LOG_ERR("Unknown filetype %x from stat() of %s!",
                        (unsigned)(st.st_mode  & S_IFMT), full_path);
                return -EINVAL;
        }
}

// Convert a directory + dirent into a Stub_
static Error *make_stub(
        const char *full_dir_path,
        const struct dirent *de,
        Stub_ *pret)
{
        const char *de_fname = de->d_name;

        size_t nf = strlen(de_fname);
        size_t nd = strlen(full_dir_path);
        if(full_dir_path[nd-1] == '/')
                PANIC("read_tree allowed an untrimmed root directory");

        char *full_path = MALLOC(nf + nd + 2);
        char *name = full_path + nd + 1;
        memcpy(full_path, full_dir_path, nd);
        full_path[nd] = '/';
        memcpy(name, de_fname, nf + 1);

        int de_type = de_type_(full_path, de);
        if(de_type < 0) {
                Error *err = IO_ERROR(full_path, -de_type,
                        "While getting file-type of directory entry");
                free(full_path);
                return err;
        }

        *pret = (Stub_) {
                .full_path = full_path,
                .name = name,
                .de_type = de_type,
        };

        return NULL;
}

static FileNode *read_tree_(const ReadTreeConf*, const char*, unsigned*, Error**);

// Reads the content of a file into a buffer you can free().
static char *read_file_(const char *full_path, unsigned *psize, Error **perr)
{
        errno = 0;
        size_t used = 0, block_size = MIN_READ + 1;
        char *block = NULL;
        assert(full_path);
        int fd = open(full_path, O_RDONLY);
        if(fd < 0) {
                *perr = IO_ERROR(full_path, errno, "Opening file");
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
                        *perr = IO_ERROR(full_path, errno, "Reading file");
                        goto error;
                }
                LOG_DBG("Read %ld bytes from %s", n, full_path);
                if(n == 0) {
                        goto eof;
                }

                used += n;
                if(block_size - used >= (1 + MIN_READ)) {
                        continue;
                }

                if((block_size *= 2) > UINT_MAX) {
                        *perr = IO_ERROR(full_path, EINVAL,
                                "Reading too big a file");
                        goto error;
                }

                block = realloc(block, block_size *= 2);
        };

eof:
        block = realloc(block, used + 1);
        block[used] = 0;
        do close(fd); while(errno == EINTR);
        if(errno) {
                *perr = IO_ERROR(full_path, errno, "Closing file");
                free(block);
                return NULL;
        }
        *psize = used;
        LOG_DBG("Successfully read file %s (%u bytes).", full_path, *psize);
        return block;

error:
        assert(fd >= 0);
        do close(fd); while(errno == EINTR);
        free(block);
        return NULL;
}

// Read the content of a Stub_
//
// * For a file, this means read the bytes into pret->content.
// * For a directory, this means recursively read it into pr->sub.
// * Any other kind of file-system node results in an error.
static Error *from_stub_(
        const ReadTreeConf *conf,
        unsigned root_len, // precomputed strlen(conf->root)
        FileNode *pr,
        const Stub_ stub)
{
        assert(pr);
        const char *name = stub.name;
        if(!name)
                PANIC("NULL name from scandir of %s!", stub.full_path);

        assert(stub.full_path[root_len] == '/');
        FileNode r = {
                .full_path = stub.full_path,
                .path = stub.full_path + root_len + 1,
        };
        Error *err = NULL;

        switch(stub.de_type) {
        case DT_DIR:
                r.sub = read_tree_(conf, r.full_path, &r.nsub, &err);
                assert(err || r.sub);
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

// Filter stubs, use hard-coded dot-file exclusion and the configured acceptor.
static bool accept_stub_(const ReadTreeConf *conf, Stub_ stub)
{
        assert(stub.name && stub.full_path);
        // We must exclude at least '.' and '..'; here we exclude all dotfiles.
        if(stub.name[0] == '.')
                return false;

        AcceptClosure closure;
        switch(stub.de_type) {
        case DT_DIR: closure = conf->accept_dir; break;
        case DT_REG: closure = conf->accept_file; break;
        default:
                PANIC("accept_stub() called with bad filetype %d", stub.de_type);
        }
        return closure.fun_(closure.arg_, stub.full_path, stub.name);
}

// Iterate the dirstream `dir` to the next non-ignored object (Stub_).
static Error *next_stub_(
        const ReadTreeConf *conf,
        Stub_ *pstub,
        const char *full_dir_path,
        DIR *dir)
{
        struct dirent *de;
        if(!(de = readdir(dir))) {
                if(!errno) {
                        *pstub = (Stub_){0};
                        return NULL;
                }
                IO_PANIC(full_dir_path, errno,
                        "readdir() failed after opendir()");
        }

        Stub_ tde;
        Error *err = make_stub(full_dir_path, de, &tde);
        if(err) {
                *pstub = (Stub_){0};
                return err;
        }

        if(accept_stub_(conf, tde)) {
                *pstub = tde;
                return NULL;
        }

        free(tde.full_path);
        return next_stub_(conf, pstub, full_dir_path, dir);
}

static int qsort_stub_cmp_(const void *va, const void *vb, void *arg)
{
        const Stub_ *a = va, *b = vb;
        const char *name_a = a->name;
        const char *name_b = b->name;
        return strcmp(name_a, name_b);
}

// Non-recursively a read directory into a sorted array of Stub_s.
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
                return IO_ERROR(full_dir_path, errno, "read_tree opening dir");
        }

        Stub_ *stubv = NULL;
        Error *err = NULL;
        int used = 0, alloced = 0;

        for(;;) {
                if(used == alloced) {
                        if(!(alloced *= 2))
                                alloced = 1;
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


        // Clean-up stage, on the good and bad paths both.
        //
        // Bad (err != NULL) implies used == 0, but that also happens on the
        // good path.
        stubv = realloc(stubv, used * sizeof stubv[0]);
        if(!stubv) {
                if(used)
                        PANIC_NOMEM();
                LOG_DBG("nothing found in drectory, return NULL");
        }

        qsort_r(stubv, used, sizeof stubv[0], qsort_stub_cmp_, NULL);
        *pstubv = stubv;
        *pnstub = used;
        closedir(dir);
        return err;
}

// Destroy the *content* of `t`.  Recurses over all sub-nodes.
//
// This is in internal helper, both used by the (root-only) public interface
// destroy_tree() and to clean up after errors in read_tree_().
static void destroy_tree_(FileNode t)
{
        free(t.full_path);
        free(t.content);
        for(unsigned k = 0; k < t.nsub; k++) {
                destroy_tree_(t.sub[k]);
        }
        free(t.sub);
}

// Recursively read read a directory into a sorted array of FileNodes.
static FileNode *read_tree_(
        const ReadTreeConf *conf,
        const char *full_dir_path,
        unsigned *pnsub,
        Error **perr)
{
        // Read it as stubs.
        Stub_ *stub;
        unsigned n;
        Error * err = load_stubv_(conf, full_dir_path, &n, &stub);
        if(err) {
                *perr = err;
                return NULL;
        }
        assert(stub || !n);

        // Recursively expand each stub.
        struct FileNode *subv = MALLOC(sizeof(FileNode)*(n+1));
        subv[n] = (FileNode){0};
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

        // Clean up.
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

// Modify a conf in-place to make it ready for use (expans out defaults etc).
Error *fill_out_config_(ReadTreeConf *conf)
{
        if(!conf->accept_dir.fun_)
                conf->accept_dir = (AcceptClosure)READ_TREE_ACCEPT_ALL();
        if(!conf->accept_file.fun_)
                conf->accept_file = (AcceptClosure)READ_TREE_ACCEPT_ALL();
        return NULL;
}

// -- Public -----------------------------------------------------------

// See read_tree.h?read_tree
Error *read_tree(const ReadTreeConf *pconf, FileNode **ptree)
{
        if(!ptree)
                PANIC("'ptree' is null");
        if(!pconf)
                PANIC("ReadTreeConf is NULL");
        if(!pconf->root)
                PANIC("Configured ReadTree 'root' is null");
        size_t root_len = strnlen(pconf->root, PATH_MAX + 1);
        if(root_len > PATH_MAX)
                PANIC("Configured ReadTree 'root' is %lu bytes.  "
                             "Max length is %u", root_len, (unsigned)PATH_MAX);

        ReadTreeConf conf = *pconf;
        fill_out_config_(&conf);

        // Keep a private copy of the root, owned by to (root) FileNode object.
        if(!(conf.root = strdup(conf.root)))
                PANIC_NOMEM();

        FileNode t = {
                .full_path = conf.root,
                .path = conf.root + strlen(conf.root),
        };
        if(!t.full_path) {
                PANIC_NOMEM();
        }
        Error *err = NULL;

        // The top-level is always a directory, so just call read_tree_.
        t.sub = read_tree_(&conf, conf.root, &t.nsub, &err);
        assert(err || t.sub);
        if(err) {
                free(conf.root);
                return err;
        }
        FileNode *tree = malloc(sizeof(FileNode));
        if(!tree) {
                destroy_tree_(t);
                PANIC_NOMEM();
        }
        *tree = t;
        *ptree = tree;
        return NULL;
}

// See read_tree.h?destroy_tree
void destroy_tree(FileNode *tree)
{
        if(!tree)
                return;
        destroy_tree_(*tree);
        free(tree);
}

