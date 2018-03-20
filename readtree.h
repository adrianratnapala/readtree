#ifndef READTREE_H
#define READTREE_H

#include "elm0/elm.h"

typedef struct Tree {
        char *full_path;
        const char *path;

        unsigned size;
        char *content;

        unsigned nsub;
        struct Tree *sub;
} Tree;


typedef struct {
        bool (*fun_)(const void *, const char *, const char *);
        void *arg_;
} AcceptClosure;

typedef struct {
        char *root;
        AcceptClosure accept_dir, accept_file;
        const void *accept_dir_arg, *accept_file_arg;
} ReadTreeConf;

extern Error *read_tree(const ReadTreeConf *pconf, Tree **ptree);
extern void destroy_tree(Tree *tree);

extern bool read_tree_accept_all_(
        const void *arg,
        const char *path,
        const char *name);
extern bool read_tree_accept_suffix_(
        const void *arg,
        const char *path,
        const char *fname);

#define READ_TREE_ACCEPT_SUFFIX(suff) { \
        read_tree_accept_suffix_, (suff) }
#define READ_TREE_ACCEPT_ALL() { \
        read_tree_accept_all_}



#endif // READTREE_H
