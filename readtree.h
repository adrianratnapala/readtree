#ifndef READTREE_H
#define READTREE_H

#include "elm0/elm.h"

// ReadTree recursively reads a directory tree into an in-memory FileNode.
typedef struct FileNode {
        // Full path to the this node.  This can be an absolute path or it can
        // be a path relative to `cwd()` at the time ReadTree was called.
        char *full_path;
        // Relative path from the tree root to this node. If this node is the
        // root, this is the empty string (not NULL).
        const char *path;

        // The size in bytes and the content of a file followed by a single 0
        // (NUL) byte. For a directory .size = 0, .content = NULL.
        unsigned size;
        char *content;

        // The sub-nodes node of this one, followed by an empty (default
        // initalized) "sentry" node.  For a file directory .nsub = 0, .sub =
        // NULL.
        unsigned nsub;
        struct FileNode *subv;
} FileNode;


// A closure you can define telling ReadTree whether to include a file or dir.
// N.B. All files with or directories with names beginning with '.' are
// excluded regardless of this function.  See readtree_test.c for examples
typedef struct {
        // ReadTree calls this for each candidate.  `arg` is user-suplied.
        // `full_path` and `path` are the path to the node (see ?FileNode).
        // FIX: remove the underscore.
        // FIX: is this really correct or is `path` just the filename part?
        bool (*fun_)(const void *arg, const char *full_path, const char *path);
        // An opaque pointer as `arg` to each invocation of `fun`.
        // FIX: remove the underscore.
        void *arg_;
} AcceptClosure;

// The configuration controlling ReadTree().
typedef struct {
        // Path to the root of the tree.  Can be an absolute path or realtive
        // to `cwd()` at the time read_tree() is called.
        char *root_path;


        // AcceptClosure for choosing files and directories.  The defaults
        // accept every candidate.  HOWEVER every node beginning with '.'
        // is exc

        // AcceptClosure for choosing files.  The default accepts all files.
        AcceptClosure accept_file;
        // AcceptClosure for choosing directories.  The default accepts all files.
        AcceptClosure accept_dir;
        const void *accept_dir_arg, *accept_file_arg;
} ReadTreeConf;

typedef struct {
        ReadTreeConf conf;
        FileNode root;
} FileTree;

// Read recursively tree reads a directory tree into memory as a FileTree.
//
// Allocate a FileTree object and set the `.conf` field as desired.  This
// function will mutate it, filling defaults in .conf and also reading a
// FileTree into `.root`.
extern Error *read_tree(FileTree *ptree);
// Cleans up internal data structures in *tree, but does no delete it.
extern void destroy_tree(FileTree *tree);

// Internal back-end for RAD_TREE_ACCEPT_SUFFIX, do no use directly.
extern bool read_tree_accept_all_(
        const void *arg,
        const char *path,
        const char *name);
// Internal back-end for RAD_TREE_ACCEPT_SUFFIX, do no use directly.
extern bool read_tree_accept_suffix_(
        const void *arg,
        const char *path,
        const char *fname);

// A C iniatizer for a AcceptClosure accepting only paths ending in `suff`.
#define READ_TREE_ACCEPT_SUFFIX(suff) { \
        read_tree_accept_suffix_, (suff) }
// A C iniatizer for a AcceptClosure accepting all candidates.
#define READ_TREE_ACCEPT_ALL() { \
        read_tree_accept_all_}


#endif // READTREE_H
