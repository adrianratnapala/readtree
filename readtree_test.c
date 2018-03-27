#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/sysmacros.h>
#include <sys/stat.h>

#include "elm0/0unit.h"
#include "readtree.h"

#define TEST_UMASK 0022

#define CHK_STR_EQ(A, B)\
        CHKV((A) && (B) && !strcmp((A),(B)), \
                "("#A")'%s' != ("#B")'%s'", (A), (B))

static char *path_join_(const char *base, const char *tip)
{
        char *ret;
        if(!*tip) {
                // Don't gratuitously append '/' if tip is ""
                return strdup(base);
        }
        if(0 > asprintf(&ret, "%s/%s", base, tip))
                PANIC_NOMEM();
        return ret;
}

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

// Integrity tests for a FileTree, i.e. is it valid, not is it right.
static int chk_tree_ok(const ReadTreeConf *conf, const FileNode *tree)
{
        CHK(tree->path);

        char *xfull_path = path_join_(conf->root_path, tree->path);
        CHK_STR_EQ(xfull_path, tree->full_path);
        free(xfull_path);

        if(tree->content) {
                CHKV(tree->content[tree->size] == '\0',
                        "File content for %s was not nul-terminated",
                        tree->path);
        } else {
                //LOG_F(dbg_log, "'%s' is not a file", tree->path);
                CHKV(tree->subv, "Node is neither a file or directory!");

                FileNode sentry = tree->subv[tree->nsub];
                CHK(!sentry.path);
                CHK(!sentry.full_path);
                CHK(!sentry.content);
        }

        for(unsigned k = 0; k < tree->nsub; k++) {
                CHK(chk_tree_ok(conf, tree->subv + k));
        }

        PASS_QUIETLY();
}


// Compares a FileTree against against test data
static TestFile *chk_tree_equal(const char *root, TestFile *tfp, FileNode *tree) {
        TestFile tf = *tfp++;

        if(tf.expect_dropped) {
                return chk_tree_equal(root, tfp, tree); }
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

        FileNode *sub0 = tree->subv, *subE = sub0 + tree->nsub;
        for(FileNode *src = sub0; src < subE; src++) {
                CHK(tfp = chk_tree_equal(root, tfp, src));
        }

        return tfp;
fail:
        return NULL;
}

// Calls read_tree() validates the result and compares it to test data.
static int chk_test_tree(TestFile *tf, const ReadTreeConf *conf)
{
        FileTree tree = {.conf = *conf};
        CHK(tree.conf.root_path);
        CHK(noerror(read_tree(&tree)));
        CHK(chk_tree_ok(&tree.conf, &tree.root));

        CHK(tf = chk_tree_equal(conf->root_path, tf, &tree.root));
        for(; tf->expect_dropped; tf++) { }
        CHKV(tf->path == NULL, "Expected files/dirs missing from tree read: "
                         "%s, ...", tf->path);

        destroy_tree(&tree);

        PASS_QUIETLY();
}


// Happy-path test runner.  Generates a files from test data, calls
// read_trees() on it and then checks the result is correct.
static int test_happy_case(TestCase tc)
{
        TestFile *tf =  tc.files;
        const char *name = tc.conf.root_path;
        CHKV(make_test_tree(name, tf),
                "failed to make dirtree for test case %s", name);
        CHKV(chk_test_tree(tf, &tc.conf),
                "read-and-compare failed for test case %s", name);
        PASSV("%s(%s)", __func__, name);
}

// Sad-path test runner.  Generates files from test data, calls read_tree() and
// checks there is an error (but doesn't not examine the details).
static int test_sad_case(TestCase tc)
{
        TestFile *tf =  tc.files;
        const char *name = tc.conf.root_path;
        CHKV(make_test_tree(name, tf),
                "failed to make dirtree for test case %s", name);

        FileTree tree = {.conf = tc.conf};
        Error *err = NULL;
        CHKV(err = read_tree(&tree),
                "Expected error missing in test tree %s", name);
        destroy_error(err);
        CHKV(!tree.root.subv, "read_tree returned both a tree and an error");
        destroy_tree(&tree);

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
                .root_path ="test_dir_tree",
                //.root_path ="main_test_tree",
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
                .root_path ="test_endings_filter",
                .accept_file = READ_TREE_ACCEPT_SUFFIX(".kept"),
                .root_path ="test_endings_filter",
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
                .root_path ="test_endings.kepd",
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

static TestCase tc_sad_fifo_in_tree_ = {
        .conf = (ReadTreeConf){ .root_path ="fifo_in_tree", },
        .files = (TestFile[]){
                {"", NULL},
                {"bad_fifo", .explicit_mode = true, .mode = 0666 | S_IFIFO},
                {0},
        }
};

static TestCase tc_sad_no_permission_ = {
        .conf = (ReadTreeConf){ .root_path ="bad_no_permission", },
        .files = (TestFile[]){
                {"", NULL},
                {"no_permssion",
                        .explicit_mode = true, .mode = 0000 | S_IFREG},
                {0},
        }
};

static TestCase tc_sad_broken_link_ = {
        .conf = (ReadTreeConf){ .root_path ="bad_broken_link", },
        .files = (TestFile[]){
                {"", NULL},
                {"bad_broken_link", .symlink="non_existent_target"},
                {0},
        }
};

static TestCase tc_sad_cyclic_link_ = {
        .conf = (ReadTreeConf){ .root_path ="bad_cyclic_link", },
        .files = (TestFile[]){
                {"", NULL},
                {"bad_cyclic_link", .symlink="bad_cyclic_link"},
                {0},
        }
};

static TestCase tc_sad_root_is_file_ = {
        .conf = (ReadTreeConf){ .root_path ="bad_root_is_file", },
        .files = (TestFile[]){
                {"", "Having conent, I am a file, not a directory" },
                {0},
        }
};

static TestCase tc_sad_root_does_not_exist_ = {
        .conf = (ReadTreeConf){ .root_path = "root_does_not_exist", },
        .files = (TestFile[]){
                {0},
        }
};


int main(void)
{
        test_happy_case(tc_main_test_tree_);
        test_happy_case(tc_drop_files_without_suffix_);
        test_happy_case(tc_drop_dirs_without_suffix_);
        test_happy_case(tc_drop_dirs_without_suffix_);

        test_sad_case(tc_sad_root_does_not_exist_);
        test_sad_case(tc_sad_root_is_file_);
        test_sad_case(tc_sad_cyclic_link_);
        test_sad_case(tc_sad_broken_link_);
        test_sad_case(tc_sad_fifo_in_tree_);
        test_sad_case(tc_sad_no_permission_);

        return zunit_report();
}

