#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
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

#define MAX_LINE_LENGTH 80


typedef struct SrcTree {
        char *root;
} SrcTree;

Error *walk_source_tree(const char *root, SrcTree **pstree)
{
        assert(pstree);
        struct SrcTree *stree = MALLOC(sizeof(SrcTree));
        *stree = (SrcTree) {
                .root = strdup(root),
        };

        *pstree = stree;
        return NULL;
}

Error *destroy_src_tree(SrcTree *stree)
{
        assert(stree && stree->root);
        free(stree->root);
        free(stree);
        return NULL;
}

typedef const struct TestFile {
        const char *name;
        const char *content;
} TestFile;


// All directories should be sorted, in naive byte-for-byte order.
static TestFile test_dir_tree_[] = {
        {"test_dir_tree", NULL},
        {"test_dir_tree/dir0", NULL},
        {"test_dir_tree/dir0/file0", "content of file 0.0"},
        {"test_dir_tree/dir0/file1", "content of file 0.1"},
        {"test_dir_tree/dir0/dir01", NULL},
        {"test_dir_tree/dir0/dir01/deeper_file", "content file 0.0.0"},
        {"test_dir_tree/emptydir", NULL},
        {"test_dir_tree/file0", "content of file 0"},
        {"test_dir_tree/file1", "content of file 1"},
        {"test_dir_tree/later_dir", NULL},
        {"test_dir_tree/later_dir/file0", "content of later file 0"},
        {"test_dir_tree/later_dir/file1", "content of later file 1"},
        {"test_dir_tree/later_dir/file3", "content of later file 3"},
        {"test_dir_tree/big",
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
        },
        {0},
};

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
                if(tf->content == NULL)
                        e = make_test_dir_(tf);
                else
                        e = make_test_file_(tf);
                if(!e)
                        continue;
                fprintf(stderr, "Error generating dirtree test-case: ");
                error_fwrite(e, stderr);
                exit(1);
        }

        return tf0;
}

static TestFile *chk_prefix_walk(TestFile *tf, SrcTree *stree) {
        CHK(!tf->content);
        CHK(!strcmp(tf->name, stree->root));
        while(tf->name)
                tf++;
        return tf;
fail:
        return NULL;
}

static int test_dir_tree()
{
        TestFile *tf = make_test_dir_tree();

        SrcTree *stree;
        CHK(!walk_source_tree("test_dir_tree", &stree));

        CHK(tf = chk_prefix_walk(tf, stree));
        CHK(tf->name == NULL);

        destroy_src_tree(stree);

        PASS();
}

int main(void)
{
        test_dir_tree();
        return zunit_report();
}

