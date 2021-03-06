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
#if 1
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

// Use stat() to get the Stub_.de_type corresponding to a deirent.
//
// This function always calls stat() but returns a value as if it was a dirent
// d_type (which is also Stub_.de_type).  We only need to call this if our
// dirent doesn't give us the info we need.
static int de_type_from_stat_(const char *full_path)
{
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
static Error *stub_from_de_(
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

        int de_type = de->d_type;
        if(de_type != DT_REG && de_type != DT_DIR) {
                de_type = de_type_from_stat_(full_path);
        }
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

static Error *stub_from_path_(const char *full_path, Stub_ *pret)
{
        int de_type = de_type_from_stat_(full_path);
        if(de_type < 0) {
                return IO_ERROR(full_path, -de_type,
                        "While getting file-type of '%s'", full_path);
        }

        char *ret_full_path = strdup(full_path);
        if(!ret_full_path)
                PANIC_NOMEM();
        const char *last_slash = strrchr(ret_full_path, '/');
        *pret = (Stub_) {
                .full_path = ret_full_path,
                .name = last_slash ? (last_slash+1) : full_path,
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

        assert(stub.full_path[root_len] == '/' || !stub.full_path[root_len]);
        FileNode r = {
                .full_path = stub.full_path,
                .path = stub.full_path + root_len,
        };
        while(*r.path == '/') {
                r.path++;
        }
        Error *err = NULL;

        switch(stub.de_type) {
        case DT_DIR:
                r.subv = read_tree_(conf, r.full_path, &r.nsub, &err);
                assert(err || r.subv);
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
        return closure.fun(closure.arg, stub.full_path, stub.name);
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
        Error *err = stub_from_de_(full_dir_path, de, &tde);
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
                destroy_tree_(t.subv[k]);
        }
        free(t.subv);
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
        unsigned root_len = strlen(conf->root_path);
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
        if(!conf->accept_dir.fun)
                conf->accept_dir = (AcceptClosure)READ_TREE_ACCEPT_ALL();
        if(!conf->accept_file.fun)
                conf->accept_file = (AcceptClosure)READ_TREE_ACCEPT_ALL();
        return NULL;
}

// Trim all trailing slashes from `path`, unless it is made entirely of slashes.
char *trimmed_path_copy(const char *path)
{
        size_t n = strnlen(path, PATH_MAX + 1);
        if(n > PATH_MAX)
                PANIC("Path is %lu bytes.  Max length is %u", n,
                (unsigned)PATH_MAX);

        for(size_t k = n; k; k--) {
                if(path[k-1] == '/')
                        continue;
                n = k;
                break;
        }

        char *dest = MALLOC(n + 1);
        memcpy(dest, path, n);
        dest[n] = 0;
        return dest;
}

// -- Public -----------------------------------------------------------

// See read_tree.h?read_tree
Error *read_tree(FileTree *ptree)
{
        if(!ptree)
                PANIC("'ptree' is null");
        ReadTreeConf *pconf = &ptree->conf;

        if(!pconf->root_path)
                PANIC("Configured ReadTree 'root_path' is null");

        fill_out_config_(pconf);
        // Keep a private copy of the root, owned by to (root) FileNode object.

        // FIX: a file-as-root is allowed even if the incoming root-path ends in '/'.
        char *root_path = trimmed_path_copy(pconf->root_path);
        LOG_DBG("timmed path trimmage = %s -> %s", pconf->root_path, root_path);
        pconf->root_path = root_path;

        FileNode t = {
                .full_path = root_path,
                .path = root_path + strlen(root_path),
        };
        if(!t.full_path) {
                PANIC_NOMEM();
        }
        Error *err = NULL;

        Stub_ root_stub;
        err = stub_from_path_(root_path, &root_stub);
        if(!err) {
                err = from_stub_(pconf, strlen(root_path), &t, root_stub);
        }
        if(!err && !accept_stub_(pconf, root_stub)) {
                err = ERROR("ReadTree root is dropped");
        }
        if(err) {
                free(root_path);
                *ptree = (FileTree){0};
                return err;
        }

        ptree->root = t;
        return NULL;
}

// See read_tree.h?destroy_tree
void destroy_tree(FileTree *tree)
{
        if(!tree)
                return;
        destroy_tree_(tree->root);
}

