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

#include <linux/limits.h> // FIX: handle NAME_MAX better.
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

typedef struct Tree {
        char *path;

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

struct ReadTreeConf {
        bool (*accept_dir)(const ReadTreeConf *, const char *, const char *);
        bool (*accept_file)(const ReadTreeConf *, const char *, const char *);
        //FIX: implement allow_hidden_files;
};


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
                *perr = IO_ERROR(path, errno, "Opening source file");
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
                        *perr = IO_ERROR(path, errno, "Reading source file");
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
                        *perr = IO_ERROR(path, EINVAL, "Reading too big a source file");
                        goto error;
                }

                block = realloc(block, block_size *= 2);
        };

eof:
        block = realloc(block, used + 1);
        block[used] = 0;
        do close(fd); while(errno == EINTR);
        if(errno) {
                *perr = IO_ERROR(path, errno, "Closing source file");
                free(block);
                return NULL;
        }
        *psize = used;
        LOG_F(dbg_log, "Successfully read file %s (%u bytes).", path, *psize);
        return block;

error:
        assert(fd >= 0);
        do close(fd); while(errno == EINTR);
        free(block);
        return NULL;
}

static unsigned char de_type_(const char *path, const struct dirent *de)
{
        unsigned char de_type = de->d_type;
        if(de_type != DT_UNKNOWN && de_type != DT_LNK)
                return de_type;

        struct stat st;
        // FIX: confirm that stat recurively chases links.
        if(0 >  stat(path, &st)) {
                return DT_UNKNOWN;
        }
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

static Error *from_typed_de(
        const ReadTreeConf *conf,
        const char *root,
        Tree *pret,
        const Stub_ tde)
{
        assert(pret);
        assert(root);
        const char *name = tde.name;
        if(!name)
                PANIC("NULL name from scandir of %s!", root);

        Tree r = {.path = tde.path};
        Error *err = NULL;

        switch(tde.de_type) {
        case DT_DIR:
                r.sub = read_tree_(conf, r.path, &r.nsub, &err);
                break;
        case DT_LNK:
                r.content = read_file_(r.path, &r.size, &err);
                break;
        case DT_REG:
                r.content = read_file_(r.path, &r.size, &err);
                break;
        default:
                return IO_ERROR(r.path, EINVAL,
                "Reading something that is neither a file nor directory.");
        }

        if(err)
                return err;
        *pret = r;
        return NULL;
}

static void destroy_tree_(Tree t)
{
        free(t.path);
        free(t.content);
        for(unsigned k = 0; k < t.nsub; k++) {
                destroy_tree_(t.sub[k]);
        }
        free(t.sub);
}

static bool accept_all_(
        const ReadTreeConf *conf,
        const char *path,
        const char *name)
{
        return true;
}

static bool reject_early(const ReadTreeConf *conf, const char *name)
{
        assert(name);
        return *name == '.';
}

static bool accept(const ReadTreeConf *conf, Stub_ stub)
{
        switch(stub.de_type) {
        case DT_DIR: return conf->accept_dir(conf, stub.path, stub.name);
        case DT_REG: return conf->accept_file(conf, stub.path, stub.name);
        default:
                // FIX: we rely on this error being caught later, but code is
                // clearer if we don't do that.
                return false;
        }
}


static int qsort_fun_(const void *va, const void *vb, void *arg)
{
        const Stub_ *a = va, *b = vb;
        const char *name_a = a->name;
        const char *name_b = b->name;
        return strcmp(name_a, name_b);
}

static Stub_ next_stub_(const ReadTreeConf *conf, const char *dirname, DIR *dir)
{
        struct dirent *de;
        if(!(de = readdir(dir))) {
                if(!errno)
                        return (Stub_){0};
                IO_PANIC(dirname, errno,
                        "readdir() failed after opendir()");
        }

        if(reject_early(conf, de->d_name)) {
                return next_stub_(conf, dirname, dir);
        }

        Stub_ tde;
        int path_len = asprintf(&tde.path, "%s/%s", dirname, de->d_name);
        if(0 > path_len)
                PANIC_NOMEM();
        tde.de_type = de_type_(tde.path, de);

        int name_len = strnlen(de->d_name, NAME_MAX + 1);
        if(name_len > NAME_MAX)
                PANIC("filename under %s longer than %d bytes",
                        dirname, NAME_MAX);
        tde.name = tde.path + path_len - name_len;

        if(accept(conf, tde)) {
                return tde;
        }
        free(tde.path);
        return next_stub_(conf, dirname, dir);
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
        int used = 0, alloced = 0;

        for(;;) {
                if(used == alloced) {
                        if(!(alloced *= 2))
                                alloced = 1;
                        LOG_F(dbg_log, "allocating %d dirent ptrs", alloced);
                        stubv = realloc(stubv, alloced * sizeof stubv[0]);
                        if(!stubv) {
                                PANIC_NOMEM();
                        }
                }

                Stub_ tde = next_stub_(conf, dirname, dir);
                if(!tde.path)
                        break;

                assert(used < alloced);
                stubv[used++] = tde;
                if(used >= MAX_IN_DIR)
                        PANIC("Directory %s has > %d entries!",
                                dirname, MAX_IN_DIR);
        }


        LOG_F(dbg_log, "trimming alloc to %d dirent ptrs", used);
        stubv = realloc(stubv, used * sizeof stubv[0]);
        if(!stubv && used) {
                PANIC_NOMEM();
        }

        qsort_r(stubv, used, sizeof stubv[0], qsort_fun_, NULL);
        *pstubv = stubv;
        *pnstub = used;
        closedir(dir);
        return NULL;
}

static Tree *read_tree_(
        const ReadTreeConf *conf,
        const char *root,
        unsigned *pnsub,
        Error **perr)
{
        // FIX: write our own, deterministic alternative to alphasort.
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
        for(nconverted = 0; nconverted < n; nconverted++) {
                err = from_typed_de(
                        conf, root, subv+nconverted, stub[nconverted]);
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
        if(!conf->accept_dir)
                conf->accept_dir = accept_all_;
        if(!conf->accept_file)
                conf->accept_file = accept_all_;
        return NULL;
}

Error *read_source_tree(
        const ReadTreeConf *pconf,
        const char *root,
        Tree **ptree)
{
        ReadTreeConf conf = {0};
        if(pconf)
                conf = *pconf;
        fill_out_config_(&conf);

        // FIX: reduce the boilerplate.
        if(!root)
                PANIC("'root' is null");
        if(!ptree)
                PANIC("'ptree' is null");
        assert(root);
        assert(ptree);
        Tree t = {.path = strdup(root)};
        if(!t.path) {
                PANIC_NOMEM();
        }
        Error *err = NULL;
        t.sub = read_tree_(&conf, root, &t.nsub, &err);
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


typedef const struct TestFile {
        const char *name;
        const char *content;
        const char *symlink;
        bool expect_dropped;
} TestFile;

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

// FIX: test empty files
// All directories should be sorted, in naive byte-for-byte order.
static TestFile test_dir_tree_[] = {
        {"test_dir_tree", NULL},
        {"test_dir_tree/dir0", NULL},
        DIR0_CONTENT("test_dir_tree/dir0"),
        {"test_dir_tree/emptydir", NULL},
        {"test_dir_tree/file0", "content of file 0"},
        {"test_dir_tree/file1", "content of file 1"},
        {"test_dir_tree/later_dir", NULL},
        {"test_dir_tree/later_dir/file0", "content of later file 0"},
        {"test_dir_tree/later_dir/file1", "content of later file 1"},
        {"test_dir_tree/later_dir/file3", "content of later file 3"},
        {"test_dir_tree/link_to_dir0", NULL, "dir0"},
        DIR0_CONTENT("test_dir_tree/link_to_dir0"),
        {"test_dir_tree/link_to_dir01", NULL, "dir0/dir01"},
        DIR01_CONTENT("test_dir_tree/link_to_dir01"),
        {"test_dir_tree/link_to_empty_dir", NULL, "emptydir"},
        {"test_dir_tree/link_to_link", NULL, "link_to_dir0"},
        DIR0_CONTENT("test_dir_tree/link_to_link"),
        {"test_dir_tree/more_bigger", more_bigger_text},
        {0},
};

static Error *make_test_symlink_(TestFile *tf)
{
        const int max_symlink_len = 200;
        const char *tgt, *src;
        assert(tf);
        assert((src = tf->name));
        assert(*src);
        assert((tgt = tf->symlink));
        LOG_F(dbg_log, "Making test-tgt symlink %s -> %s", src, tgt);

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
        LOG_F(err_log, "symlink() failed: %s", strerror(errn));

        if(errn != EEXIST) {
                assert(*src);
                return IO_ERROR(src, errno,
                        "Creating readtree test-case symlink to '%s'",
                        tf->symlink);
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


static Error *make_test_file_(TestFile *tf)
{
        assert(tf);
        assert(tf->name);
        assert(tf->content);
        FILE *f = fopen(tf->name, "w");
        if(!f) {
                return IO_ERROR(tf->name, errno,
                        "Creating readtree test-case file");
        }
        if(EOF == fputs(tf->content, f)) {
                return IO_ERROR(tf->name, errno,
                        "Writing content of readtree test-case file");
        }
        if(fclose(f)) {
                return IO_ERROR(tf->name, errno,
                        "Closing readtree test-case file (bad nework fs?)");
        }
        return NULL;
}

static Error *make_test_dir_(TestFile *tf)
{
        const int mkdir_mode = 0755;
        assert(tf);
        assert(tf->name);
        assert(!tf->content);
        if(!mkdir(tf->name, mkdir_mode)) {
                return NULL;
        }

        int ern = errno;
        struct stat st;
        if(ern == EEXIST && ! stat(tf->name, &st)) {
                if((st.st_mode & S_IFMT) != S_IFDIR) {
                        return IO_ERROR(tf->name, ern,
                                "readtree test-case dir already exists, "
                                "but is not a directory!");
                }

                // FIX: this must be run with a sufficiently permissive umask.
                if((st.st_mode & 0777) != mkdir_mode) {
                        return IO_ERROR(tf->name, ern,
                                "readtree test-case dir already exists, "
                                "with permissions mode %o != %o",
                                st.st_mode & 0777, mkdir_mode);
                }
                return NULL;
        }


        return IO_ERROR(tf->name, ern, "Creating readtree test-case dir");
}

int make_test_tree(TestFile *tf0)
{
        for(TestFile *tf = tf0; tf->name; tf++) {
                Error *e;
                CHK(*tf->name);
                if(tf->symlink != NULL)
                        e = make_test_symlink_(tf);
                else if(tf->content == NULL)
                        e = make_test_dir_(tf);
                else
                        e = make_test_file_(tf);
                if(!e)
                        continue;
                fprintf(stderr, "Error generating dirtree test-case: ");
                error_fwrite(e, stderr);
                fputc('\n', stderr);
                goto fail;
        }

        return 1;
fail:
        return 0;
}

static TestFile *chk_tree_equal(TestFile *tfp, Tree *tree) {
        TestFile tf = *tfp++;

        CHK(tf.name);
        CHK(tree->path);
        CHK_STR_EQ(tree->path, tf.name);
        if(tf.content) {
                CHK_STR_EQ(tf.content, tree->content);
        } else {
                CHK(!tree->content);
        }

        Tree *sub0 = tree->sub, *subE = sub0 + tree->nsub;
        for(Tree *src = sub0; src < subE; src++) {
                CHK(tfp = chk_tree_equal(tfp, src));
        }

        return tfp;
fail:
        return NULL;
}

static int noerror(Error *err) {
        if(!err)
                return 1;
        log_error(dbg_log, err);
        return 0;
}


static int chk_test_tree(TestFile *tf, const ReadTreeConf *conf)
{
        const char *name = tf->name;
        Tree *tree;
        CHK(noerror(read_source_tree(conf, name, &tree)));

        CHK(tf = chk_tree_equal(tf, tree));
        CHKV(tf->name == NULL, "Expected files/dirs missing from tree read: "
                         "%s, ...", tf->name);

        destroy_src_tree(tree);

        PASS_QUIETLY();
}

static int test_dir_tree(TestFile *tf)
{
        CHK(make_test_tree(tf));
        CHK(chk_test_tree(tf, NULL));
        PASS();
}

static TestFile test_drop_files_without_extension_[] = {
        {"test_endings_filter"},
        {"test_endings_filter/a.kept", "a"},
        {"test_endings_filter/b.kept", "b"},
        {"test_endings_filter/dir_not_dropped"},
        {"test_endings_filter/dir_not_dropped/sub_a.kept", "aa"},
        {"test_endings_filter/dir_not_dropped/sub_b.kept", "bb"},
        {"test_endings_filter/dir_not_dropped/sub_dropped", "dd",
                .expect_dropped = true},
        {"test_endings_filter/dropped", "d",
                .expect_dropped = true},
        {0},
};

int main(void)
{
        test_dir_tree(test_dir_tree_);
        test_dir_tree(test_drop_files_without_extension_);

        // FIX: we need bad-path tests, e.g. cyclic symlinks, FIFOs in the tree
        return zunit_report();
}

