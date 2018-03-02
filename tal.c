#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "elm0/elm.h"
#include "elm0/0unit.h"

#define CHK_STR_EQ(A, B)\
        CHKV((A) && (B) && !strcmp((A),(B)), \
                "("#A")'%s' != ("#B")'%s'", (A), (B))

#define MIN_READ 1

typedef struct SrcTree {
        char *path;

        unsigned size;
        char *content;

        unsigned nsub;
        struct SrcTree *sub;
} SrcTree;

static SrcTree *walk_tree_(const char *root, unsigned *pnsub, Error **perr);

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

static Error *from_dirent_(
        const char *root,
        SrcTree *pret,
        const struct dirent *de)
{
        assert(de);
        assert(pret);
        assert(root);
        const char *name = de->d_name;
        if(!name)
                PANIC("NULL name from scandir of %s!", root);

        SrcTree r = {0};

        if(0 > asprintf(&r.path, "%s/%s", root, name))
                PANIC_NOMEM();
        Error *err = NULL;

        switch(de->d_type) {
        case DT_DIR:
                r.sub = walk_tree_(r.path, &r.nsub, &err);
                break;
        case DT_LNK:
        case DT_REG:
                r.content = read_file_(r.path, &r.size, &err);
                break;
        default:
                return IO_ERROR(r.path, EINVAL,
                "Dirwalking something that is neither a file nor directory.");
        }

        if(err)
                return err;
        *pret = r;
        return NULL;
}

static void destroy_tree_(SrcTree t)
{
        free(t.path);
        free(t.content);
        for(unsigned k; k < t.nsub; k++) {
                destroy_tree_(t.sub[k]);
        }
        free(t.sub);
}

static int filter(const struct dirent *de)
{
        const char *name = de->d_name;
        if(!name)
                PANIC("scandir called filter with NULL name!");
        return *name != '.';
}

static SrcTree *walk_tree_(const char *root, unsigned *pnsub, Error **perr)
{
        // FIX: write our own, deterministic alternative to alphasort.
        struct dirent **direntv;
        int n = scandir(root, &direntv, filter, alphasort);
        if(n < 0) {
                *perr = IO_ERROR(root, errno, "walking directory");
                return NULL;
        }
        assert(direntv || !n);
        struct SrcTree *subv = MALLOC(sizeof(SrcTree)*n);

        Error *err = NULL;
        int nconverted;
        for(nconverted = 0; nconverted < n; nconverted++) {
                err = from_dirent_(root, subv+nconverted, direntv[nconverted]);
                if(err)
                        break;
        }

        // Clear up the dirent whether or not there was an error.
        for(int k = 0; k < n; k++) {
                free(direntv[k]);
        }
        free(direntv);

        // Happy path: just return the subv.
        assert(n >= 0);
        if(!err) {
                *pnsub = (unsigned)n;
                return subv;
        }

        // Sad path: clean up whatever trees were already converted.
        for(int k = 0; k < nconverted; k++) {
                destroy_tree_(subv[k]);
        }
        free(subv);

        *perr = err;
        return NULL;
}

void destroy_src_tree(SrcTree *stree)
{
        if(!stree)
                return;
        destroy_tree_(*stree);
        free(stree);
}

Error *walk_source_tree(const char *root, SrcTree **pstree)
{
        // FIX: reduce the boilerplate.
        if(!root)
                PANIC("'root' is null");
        if(!pstree)
                PANIC("'pstree' is null");
        assert(root);
        assert(pstree);
        SrcTree t = {.path = strdup(root)};
        if(!t.path) {
                PANIC_NOMEM();
        }
        Error *err = NULL;
        t.sub = walk_tree_(root, &t.nsub, &err);
        if(err) {
                return err;
        }
        SrcTree *stree = malloc(sizeof(SrcTree));
        if(!stree) {
                destroy_tree_(t);
                PANIC_NOMEM();
        }
        *stree = t;
        *pstree = stree;
        return NULL;
}


typedef const struct TestFile {
        const char *name;
        const char *content;
        const char *symlink;
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

// FIX: test empty files
// All directories should be sorted, in naive byte-for-byte order.
static TestFile test_dir_tree_[] = {
        {"test_dir_tree", NULL},
        {"test_dir_tree/dir0", NULL},
        {"test_dir_tree/dir0/dir01", NULL},
        {"test_dir_tree/dir0/dir01/deeper_file", "content file 0.0.0"},
        {"test_dir_tree/dir0/file0", "content of file 0.0"},
        {"test_dir_tree/dir0/file1", "content of file 0.1"},
        {"test_dir_tree/dir0/link", more_bigger_text, "../more_bigger"},
        {"test_dir_tree/emptydir", NULL},
        {"test_dir_tree/file0", "content of file 0"},
        {"test_dir_tree/file1", "content of file 1"},
        {"test_dir_tree/later_dir", NULL},
        {"test_dir_tree/later_dir/file0", "content of later file 0"},
        {"test_dir_tree/later_dir/file1", "content of later file 1"},
        {"test_dir_tree/later_dir/file3", "content of later file 3"},
        {"test_dir_tree/more_bigger", more_bigger_text},
        {0},
};

static Error *make_test_symlink_(TestFile *tf)
{
        const int max_symlink_len = 200;
        const char *tgt, *src;
        assert(tf);
        assert((src = tf->name));
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
                return IO_ERROR(tf->name, errno,
                        "Creating dir-walk test-case symlink to '%s'",
                        tf->symlink);
        }

        char buf[max_symlink_len + 1];
        ssize_t n = readlink(src, buf, sizeof(buf));
        if(0 > n) {
                return IO_ERROR(src, errno,
                        "Checking existing dir-walk test-case symlink");
        }
        buf[n] = 0;
        if(strcmp(buf, tgt)) {
                return ERROR("Incorrect existing dir-walk "
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
                        "Creating dir-walk test-case file");
        }
        if(EOF == fputs(tf->content, f)) {
                return IO_ERROR(tf->name, errno,
                        "Writing content of dir-walk test-case file");
        }
        if(fclose(f)) {
                return IO_ERROR(tf->name, errno,
                        "Closing dir-walk test-case file (bad nework fs?)");
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
                                "dir-walk test-case dir already exists, "
                                "but is not a directory!");
                }

                // FIX: this must be run with a sufficiently permissive umask.
                if((st.st_mode & 0777) != mkdir_mode) {
                        return IO_ERROR(tf->name, ern,
                                "dir-walk test-case dir already exists, "
                                "with permissions mode %o != %o",
                                st.st_mode & 0777, mkdir_mode);
                }
                return NULL;
        }


        return IO_ERROR(tf->name, ern, "Creating dir-walk test-case dir");
}

TestFile *make_test_dir_tree()
{
        TestFile *tf0 = test_dir_tree_;
        for(TestFile *tf = tf0; tf->name; tf++) {
                Error *e;
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
                exit(1);
        }

        return tf0;
}

static TestFile *chk_tree_equal(TestFile *tf, SrcTree *stree) {
        CHK(tf->name);
        CHK(stree->path);
        CHK_STR_EQ(stree->path, tf->name);
        if(tf->content) {
                CHK_STR_EQ(tf->content, stree->content);
        } else {
                CHK(!stree->content);
        }

        ++tf;

        SrcTree *sub0 = stree->sub, *subE = sub0 + stree->nsub;
        for(SrcTree *src = sub0; src < subE; src++) {
                CHK(tf = chk_tree_equal(tf, src));
        }

        return tf;
fail:
        return NULL;
}

static int noerror(Error *err) {
        if(!err)
                return 1;
        log_error(dbg_log, err);
        return 0;
}

static int test_dir_tree()
{
        TestFile *tf = make_test_dir_tree();

        SrcTree *stree;
        CHK(noerror(walk_source_tree("test_dir_tree", &stree)));

        CHK(tf = chk_tree_equal(tf, stree));
        CHKV(tf->name == NULL, "Expected files/dirs missing from tree walk: "
                         "%s, ...", tf->name);

        destroy_src_tree(stree);

        PASS();
}

int main(void)
{
        test_dir_tree();
        return zunit_report();
}

