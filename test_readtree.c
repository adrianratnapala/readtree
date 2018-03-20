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

#include "elm0/elm.h"
#include "elm0/0unit.h"

#define CHK_STR_EQ(A, B)\
        CHKV((A) && (B) && !strcmp((A),(B)), \
                "("#A")'%s' != ("#B")'%s'", (A), (B))

#define MAX_IN_DIR 1000000

// FIX: increase these
#define MIN_READ 1
#define MIN_READ_DIR 1

// FIX: tests should have no output
// FIX: separate test binary from libreadtree

typedef struct Tree {
        char *full_path;
        const char *path;

        unsigned size;
        char *content;

        unsigned nsub;
        struct Tree *sub;
} Tree;

typedef struct {
        char *path;
        int de_type;
        const char *name;
} Stub_;

typedef struct ReadTreeConf ReadTreeConf;

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

static bool read_tree_accept_all_(const void *arg, const char *path, const char *name)
{
        return true;
}

typedef struct {
        bool (*fun_)(const void *, const char *, const char *);
        void *arg_;
} AcceptClosure;

#define READ_TREE_ACCEPT_SUFFIX(suff) { \
        read_tree_accept_suffix_, (suff) }
#define READ_TREE_ACCEPT_ALL() { \
        read_tree_accept_all_}

struct ReadTreeConf {
        char *root;
        AcceptClosure accept_dir, accept_file;
        const void *accept_dir_arg, *accept_file_arg;
};


static Tree *read_tree_(
        const ReadTreeConf *conf,
        const char *root,
        unsigned *pnsub,
        Error **perr);

static char *path_join_(const char *base, const char *stem)
{
        size_t nb = strlen(base);
        size_t ns = strlen(stem);
        while(base[nb-1] == '/')
                nb--;
        char *ret = MALLOC(nb + ns + 2);
        memcpy(ret, base, nb);
        ret[nb] = '/';
        memcpy(ret + nb + 1, stem, ns + 1);
        return ret;
}

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
                PANIC("lstat of %s returned S_IFLINK!", path);
        default: return DT_UNKNOWN;
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
                PANIC("NULL name from scandir of %s!", stub.path);

        assert(stub.path[root_len] == '/');
        Tree r = {
                .full_path = stub.path,
                .path = stub.path + root_len + 1,
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
                *perr = IO_ERROR(stub.path, EINVAL, "Unknown filetype");
                return false;
        }

        return closure.fun_(closure.arg_, stub.path, stub.name);
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
        const char *dirname,
        DIR *dir)
{
        struct dirent *de;
        if(!(de = readdir(dir))) {
                if(!errno) {
                        *pstub = (Stub_){0};
                        return NULL;
                }
                IO_PANIC(dirname, errno,
                        "readdir() failed after opendir()");
        }

        if(reject_early(conf, de->d_name)) {
                return next_stub_(conf, pstub, dirname, dir);
        }

        Stub_ tde;
        Error *err = NULL;

        tde.path = path_join_(dirname, de->d_name);
        int de_type = de_type_(tde.path, de);
        if(de_type < 0) {
                err = IO_ERROR(tde.path, errno,
                        "While getting file-type of directory entry");
                goto done;
        }
        tde.de_type = de_type;

        int name_len = strnlen(de->d_name, NAME_MAX + 1);
        if(name_len > NAME_MAX)
                PANIC("filename under %s longer than %d bytes",
                        dirname, NAME_MAX);
        tde.name = tde.path + strlen(tde.path) - name_len;

        if(accept(conf, tde, &err)) {
                assert(!err);
                *pstub = tde;
                return NULL;
        }
        free(tde.path);

done:
        if(err) {
                *pstub = (Stub_){0};
                return err;
        }
        return next_stub_(conf, pstub, dirname, dir);
}


static Error *load_stubv_(
        const ReadTreeConf *conf,
        const char *dirname,
        unsigned *pnstub,
        Stub_ **pstubv)
{
        assert(dirname);
        errno = 0;
        DIR *dir = opendir(dirname);
        if(!dir) {
                return IO_ERROR(dirname, errno, "Readtree opening dir");
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
                err = next_stub_(conf, &stub, dirname, dir);
                if(!stub.path)
                        break;

                assert(used < alloced);
                stubv[used++] = stub;
                if(used >= MAX_IN_DIR) {
                        err = ERROR("Directory %s has > %d entries!",
                                dirname, MAX_IN_DIR);
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
        const char *root,
        unsigned *pnsub,
        Error **perr)
{
        Stub_ *stub;
        unsigned n;
        Error * err = load_stubv_(conf, root, &n, &stub);
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
                        free(stub[k].path);
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

void destroy_src_tree(Tree *tree)
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


#define TEST_UMASK 022

typedef const struct TestFile {
        const char *path;
        const char *content;
        const char *symlink;
        bool expect_dropped;
        bool explicit_mode;
        mode_t mode;
        unsigned int dev_major, dev_minor;
} TestFile;

typedef const struct{
        ReadTreeConf conf;
        TestFile *files;
} TestCase;

static Error *make_symlink_(const char *src, const char *tgt)
{
        const int max_symlink_len = 200;
        assert(src && *src);
        assert(tgt && *tgt);

        int tgt_len = strnlen(tgt, max_symlink_len + 2);
        if(tgt_len > max_symlink_len) {
                return ERROR(
                        "Symlink target %.20s... is too long (max is %d bytes)",
                         tgt, max_symlink_len);
        }

        if(!symlink(tgt, src)) {
                return NULL;
        }

        int errn = errno;

        if(errn != EEXIST) {
                LOG_F(err_log, "symlink() failed: %s", strerror(errn));
                assert(*src);
                return IO_ERROR(src, errno,
                        "Creating readtree test-case symlink to '%s'", tgt);
        }

        char buf[max_symlink_len + 1];
        ssize_t n = readlink(src, buf, sizeof(buf));
        if(0 > n) {
                return IO_ERROR(src, errno,
                        "Checking existing readtree test-case symlink");
        }
        buf[n] = 0;
        if(strcmp(buf, tgt)) {
                return ERROR("Incorrect existing readtree "
                        "test-case symlink %s \n"
                        "  points to: %s\n"
                        "  should be: %s",
                        src, buf, tgt);
        }

        return NULL;
}

static Error *make_test_symlink_(const char *root, TestFile *tf)
{
        assert(tf);
        assert(root && *root);

        char *src = path_join_(root, tf->path);
        const char *tgt = tf->symlink;

        //LOG_F(dbg_log, "Making test-tgt symlink %s -> %s", src, tgt);
        Error *err = make_symlink_(src, tgt);
        free(src);

        return err;
}

static Error *make_node_(const char *path, mode_t mode, dev_t dev)
{

        if(!mknod(path, mode, dev)) {
                return NULL;
        }
        if(errno != EEXIST) {
                return IO_ERROR(path, errno,
                        "Test mknod(mode = %o, major=%s, minor=%s)",
                        mode, major(dev), minor(dev));
        }

        struct stat st;
        if(stat(path, &st)) {
                return IO_ERROR(path, errno,
                        "Can't stat existing test after "
                        "mknod(mode = %o, major=%s, minor=%s)",
                        mode, major(dev), minor(dev));
        }

        if(st.st_mode != (mode & ~TEST_UMASK)) {
                return IO_ERROR(path, EEXIST,
                        "Existing node with unexpected mode %o != %o",
                        st.st_mode, mode);
        }

        dev_t sdev = st.st_rdev;
        if(major(sdev) != major(dev)) {
                return IO_ERROR(path, EEXIST,
                        "Existing node with unexpected dev major type %d != %d",
                        major(sdev) != major(dev));
        }
        if(minor(sdev) != minor(dev)) {
                return IO_ERROR(path, EEXIST,
                        "Existing node with unexpected dev minor type %d != %d",
                        minor(sdev) != minor(dev));
        }

        // Existing node seems to match what we want.
        return NULL;
}


static Error *make_test_node_(const char *root, TestFile *tf)
{
        assert(tf);
        assert(tf->explicit_mode);
        assert(!tf->symlink);

        char *path = path_join_(root, tf->path);
        dev_t dev = makedev(tf->dev_major, tf->dev_minor);
        Error *err = make_node_(path, tf->mode, dev);
        free(path);
        return err;
}

static Error *make_test_file_(const char *root, TestFile *tf)
{
        assert(tf);
        assert(root);
        assert(tf->path);
        assert(tf->content);
        Error *err = NULL;
        char *path = path_join_(root, tf->path);
        FILE *f = fopen(path, "w");
        if(!f) {
                err = IO_ERROR(path, errno,
                        "Creating readtree test-case file");
        }
        else if(EOF == fputs(tf->content, f)) {
                err = IO_ERROR(path, errno,
                        "Writing content of readtree test-case file");
        }
        else if(fclose(f)) {
                err = IO_ERROR(path, errno,
                        "Closing readtree test-case file (bad nework fs?)");
        }

        free(path);
        return err;
}

static Error *make_dir_(const char *path)
{
        const int mkdir_mode = 0755;
        assert((mkdir_mode & ~TEST_UMASK) == mkdir_mode);
        assert(path && *path);
        if(!mkdir(path, mkdir_mode)) {
                return NULL;
        }
        int ern = errno;

        struct stat st;
        if(ern != EEXIST)
                return IO_ERROR(path, ern, "Creating readtree test-case dir");
        if(stat(path, &st))
                return IO_ERROR(path, ern, "Statting readtree test-case dir");
        if((st.st_mode & S_IFMT) != S_IFDIR) {
                return IO_ERROR(path, ern,
                        "readtree test-case dir already exists, "
                        "but is not a directory!");
        }
        else if((st.st_mode & 0777) != mkdir_mode) {
                return IO_ERROR(path, ern,
                        "readtree test-case dir already exists, "
                        "with permissions mode %o != %o",
                        st.st_mode & 0777, mkdir_mode);
        }
        return NULL;
}


static Error *make_test_dir_(const char *root, TestFile *tf)
{
        assert(tf);
        assert(root);
        assert(tf->path);
        assert(!tf->content);

        char *path = path_join_(root, tf->path);
        Error *err = make_dir_(path);
        free(path);
        return err;
}

static int noerror(Error *err) {
        if(!err)
                return 1;
        log_error(dbg_log, err);
        return 0;
}


int make_test_tree(const char *root, TestFile *tf0)
{
        //CHK(noerror(make_dir_(root)));
        mode_t old_umask = umask(TEST_UMASK);

        for(TestFile *tf = tf0; tf->path; tf++) {
                Error *e;
                //CHK(*tf->path);
                if(tf->explicit_mode)
                        e = make_test_node_(root, tf);
                else if(tf->symlink != NULL)
                        e = make_test_symlink_(root, tf);
                else if(tf->content == NULL)
                        e = make_test_dir_(root, tf);
                else
                        e = make_test_file_(root, tf);
                if(!e)
                        continue;
                fprintf(stderr, "Error generating dirtree test-case: ");
                error_fwrite(e, stderr);
                fputc('\n', stderr);
                goto fail;
        }

        CHK(TEST_UMASK == umask(old_umask));
        return 1;
fail:
        umask(old_umask);
        return 0;
}

static TestFile *chk_tree_equal(const char *root, TestFile *tfp, Tree *tree) {
        TestFile tf = *tfp++;

        if(tf.expect_dropped) {
                return chk_tree_equal(root, tfp, tree);
        }

        CHK(tf.path);
        CHK(tree->full_path);

        //LOG_F(dbg_log, "Comparing: %s with %s", tf.path, tree->full_path);

        {
                char *full_path;
                if(*tf.path)
                        CHK(0 < asprintf(&full_path, "%s/%s", root, tf.path));
                else
                        full_path = strdup(root);
                CHK_STR_EQ(tree->full_path, full_path);
                free(full_path);
                CHK_STR_EQ(tree->path, tf.path);
        }
        if(tf.content) {
                CHK_STR_EQ(tf.content, tree->content);
        } else {
                CHK(!tree->content);
        }

        Tree *sub0 = tree->sub, *subE = sub0 + tree->nsub;
        for(Tree *src = sub0; src < subE; src++) {
                CHK(tfp = chk_tree_equal(root, tfp, src));
        }

        return tfp;
fail:
        return NULL;
}

static int chk_test_tree(TestFile *tf, const ReadTreeConf *conf)
{
        Tree *tree;
        CHK(noerror(read_tree(conf, &tree)));

        CHK(tf = chk_tree_equal(conf->root, tf, tree));
        for(; tf->expect_dropped; tf++) { }
        CHKV(tf->path == NULL, "Expected files/dirs missing from tree read: "
                         "%s, ...", tf->path);


        destroy_src_tree(tree);

        PASS_QUIETLY();
}

static int test_read_tree_case(TestCase tc)
{
        TestFile *tf =  tc.files;
        const char *name = tc.conf.root;
        CHKV(make_test_tree(name, tf),
                "failed to make dirtree for test case %s", name);
        CHKV(chk_test_tree(tf, &tc.conf),
                "read-and-compare failed for test case %s", name);
        PASSV("%s(%s)", __func__, name);
}

static const char more_bigger_text[] =
        "This file is slightly bigger than\n"
        "the others, but still not very big.\n"

        "But still not very big.\n"
        "But still not very bug.\n"
        "But still not very bog.\n"
        "But still not very bag.\n"
        "But still not very beg.\n"
        "Bit still not very big.\n"
        "Bit still not very bug.\n"
        "Bit still not very bog.\n"
        "Bit still not very bag.\n"
        "Bit still not very beg.\n"
        "Bet still not very big.\n"
        "Bet still not very bug.\n"
        "Bet still not very bog.\n"
        "Bet still not very bag.\n"
        "Bet still not very beg.\n"
        "Bat still not very big.\n"
        "Bat still not very bug.\n"
        "Bat still not very bog.\n"
        "Bat still not very bag.\n"
        "Bat still not very beg.\n"

        "But still net very big.\n"
        "But still net very bug.\n"
        "But still net very bog.\n"
        "But still net very bag.\n"
        "But still net very beg.\n"
        "Bit still net very big.\n"
        "Bit still net very bug.\n"
        "Bit still net very bog.\n"
        "Bit still net very bag.\n"
        "Bit still net very beg.\n"
        "Bet still net very big.\n"
        "Bet still net very bug.\n"
        "Bet still net very bog.\n"
        "Bet still net very bag.\n"
        "Bet still net very beg.\n"
        "Bat still net very big.\n"
        "Bat still net very bug.\n"
        "Bat still net very bog.\n"
        "Bat still net very bag.\n"
        "Bat still net very beg.\n"

        "But still nit very big.\n"
        "But still nit very bug.\n"
        "But still nit very bog.\n"
        "But still nit very bag.\n"
        "But still nit very beg.\n"
        "Bit still nit very big.\n"
        "Bit still nit very bug.\n"
        "Bit still nit very bog.\n"
        "Bit still nit very bag.\n"
        "Bit still nit very beg.\n"
        "Bet still nit very big.\n"
        "Bet still nit very bug.\n"
        "Bet still nit very bog.\n"
        "Bet still nit very bag.\n"
        "Bet still nit very beg.\n"
        "Bat still nit very big.\n"
        "Bat still nit very bug.\n"
        "Bat still nit very bog.\n"
        "Bat still nit very bag.\n"
        "Bat still nit very beg.\n"
        ;

#define DIR01_CONTENT(R) \
        {R "/deeper_file", "content file 0.0.0"}


#define DIR0_CONTENT(R) \
        {R"/dir01", NULL}, \
        DIR01_CONTENT(R"/dir01"), \
        {R"/file0", "content of file 0.0"}, \
        {R"/file1", "content of file 0.1"}, \
        {R"/link", more_bigger_text, "../more_bigger"}

static TestCase tc_main_test_tree_ = {
        .conf = {
                .root = "test_dir_tree",
                //.root = "main_test_tree",
        },
        .files = (TestFile[]){
                {"", NULL},
                {"dir0", NULL},
                DIR0_CONTENT("dir0"),
                {"emptydir", NULL},
                {"emptyfile", ""},
                {"file0", "content of file 0"},
                {"file1", "content of file 1"},
                {"later_dir", NULL},
                {"later_dir/file0", "content of later file 0"},
                {"later_dir/file1", "content of later file 1"},
                {"later_dir/file3", "content of later file 3"},
                {"link_to_dir0", NULL, "dir0"},
                DIR0_CONTENT("link_to_dir0"),
                {"link_to_dir01", NULL, "dir0/dir01"},
                DIR01_CONTENT("link_to_dir01"),
                {"link_to_empty_dir", NULL, "emptydir"},
                {"link_to_link", NULL, "link_to_dir0"},
                DIR0_CONTENT("link_to_link"),
                {"more_bigger", more_bigger_text},
                {0},
        }
};

static TestCase tc_drop_files_without_suffix_ = {
        .conf = (ReadTreeConf){
                .root = "test_endings_filter",
                .accept_file = READ_TREE_ACCEPT_SUFFIX(".kept"),
                .root = "test_endings_filter",
        },
        .files = (TestFile[]){
                {"", NULL},
                {"a.kept", "a"},
                {"b.kept", "b"},
                {"dir_not_dropped"},
                {"dir_not_dropped/sub_a.kept", "aa"},
                {"dir_not_dropped/sub_b.kept", "bb"},
                {"dir_not_dropped/sub_dropped", "dd",
                        .expect_dropped = true},
                {"dropped", "d",
                        .expect_dropped = true},
                {0},
        }
};

static TestCase tc_drop_dirs_without_suffix_ = {
        .conf = (ReadTreeConf){
                .root = "test_endings.kepd",
                .accept_dir = READ_TREE_ACCEPT_SUFFIX(".kepd"),
        },
        .files = (TestFile[]){
                {"", NULL},
                {"drop.d", .expect_dropped = true},
                {"drop.d/orphan", "this file is never read",
                        .expect_dropped = true},
                {"file_kept_without_suffix", "fkws"},
                {0},
        }
};

static TestCase tc_bad_fifo_in_tree_ = {
        .conf = (ReadTreeConf){ .root = "fifo_in_tree", },
        .files = (TestFile[]){
                {"", NULL},
                {"bad_fifo", .explicit_mode = true, .mode = 0666 | S_IFIFO},
                {0},
        }
};

static TestCase tc_bad_no_permission_ = {
        .conf = (ReadTreeConf){ .root = "bad_no_permission", },
        .files = (TestFile[]){
                {"", NULL},
                {"no_permssion",
                        .explicit_mode = true, .mode = 0000 | S_IFREG},
                {0},
        }
};

static TestCase tc_bad_broken_link_ = {
        .conf = (ReadTreeConf){ .root = "bad_broken_link", },
        .files = (TestFile[]){
                {"", NULL},
                {"bad_broken_link", .symlink="non_existent_target"},
                {0},
        }
};

static TestCase tc_bad_cyclic_link_ = {
        .conf = (ReadTreeConf){ .root = "bad_cyclic_link", },
        .files = (TestFile[]){
                {"", NULL},
                {"bad_cyclic_link", .symlink="bad_cyclic_link"},
                {0},
        }
};

static int test_bad_case(TestCase tc)
{
        TestFile *tf =  tc.files;
        const char *name = tc.conf.root;
        CHKV(make_test_tree(name, tf),
                "failed to make dirtree for test case %s", name);

        Tree *tree = NULL;
        Error *err = NULL;
        CHKV(err = read_tree(&tc.conf, &tree),
                "Expected error missing in test tree %s", name);
        destroy_error(err);
        CHKV(!tree, "read_tree returned both a tree and an error");
        destroy_src_tree(tree);

        PASSV("%s(%s)", __func__, name);
}

int main(void)
{
        test_read_tree_case(tc_main_test_tree_);
        test_read_tree_case(tc_drop_files_without_suffix_);
        test_read_tree_case(tc_drop_dirs_without_suffix_);

        test_bad_case(tc_bad_cyclic_link_);
        test_bad_case(tc_bad_broken_link_);
        test_bad_case(tc_bad_fifo_in_tree_);
        test_bad_case(tc_bad_no_permission_);

        return zunit_report();
}

