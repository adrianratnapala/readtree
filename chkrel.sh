#!/bin/bash
# chkrel.sh  -- Checks that elm0 is ready for release.
#
# Copyright (C) 2012, Adrian Ratnapala, under the ISC license. See file LICENSE.
#
#
# To check elm0, run this program from OUTSIDE elm0's git repository with the
# command:
#
#            REPO/chkrel.sh REPO VERSION_NUM
#
# e.g.
#            elm0/chkrel.sh elm0  0.5.
#
# This:
# * Checks that your working directory is clean.  And is on branch
#   rel/VERSION_NUM.  (-b disables this check.)
# * Clones the repository, checking out rel/VERSION_NUM.
# * Checks that the unit tests pass. (-s disables this check)
# * Does "make install" into a temporary directory.
#   (building is in a different tempdir)
# * Checks that quick-and-dirty unit test passes (even with -s).
# * Checks that the version information compiled into the library is
#   as expected.  See elm.h for details, but this check includes:
#        - checking the version number
#        - checking that this is not a prerelease, unless -p was given
#          in which case it MUST be a prerelease.
#


ABS_BUILD=$PWD/build/
ABS_INST=$PWD/inst/
SRC=src

die() {
        err=$?
        echo '*********************************************************' >& 2
        echo "ERROR $err: " $@ >& 2
        exit 1
}

opts=`getopt pbs "$@"` || die "Bad command line options."
set -- $opts
while [ $# -gt 0 ]
do
        case "$1" in
        (-p) expect_prerelease="yes" ;;
        (-s) skip_unit_tests="yes" ;;
        (-b) ignore_work_tree="yes" ;;
        (--)
                shift
                break;;
        (-*)  die "$0: error - unrecognised option $1";;
        esac
        shift
done

ORIG=$1; shift
[ -z $ORIG ] && die "You must specify the original source repository."

VERNUM=$1; shift
[ -z $VERNUM ] && die "You must specify the version number."

ORIG_GIT="git --work-tree=$ORIG --git-dir=$ORIG/.git"

# -- check that work dir is on the expected branch, and is clean.
if [ "$ignore_work_tree" != "yes" ]
then
        xbranch=rel/$VERNUM
        $ORIG_GIT branch --list $xbranch | (
                read star branch
                [ "$star"  = '*' ] || die "Branch '$xbranch' is not checked out."
                [ "$branch" = "$xbranch" ] || die "Can't find branch '$xbranch'."
        ) || exit

        $ORIG_GIT status --porcelain | (
                bad_paths=0
                while read stat path
                do
                        echo "bad path $((++bad_paths)):" $stat $path
                done
                exit $bad_paths
        ) || die "Some files don't match the repository"
fi

rm -rf  $ABS_BUILD
rm -rf  $ABS_INST
rm -rf  $SRC

git clone $ORIG $SRC -b rel/$VERNUM &&
[ "$skip_unit_tests" = "yes" ] ||\
        BUILD_DIR=$ABS_BUILD make -C $SRC clean test ||\
        die "Unit tests failed"

BUILD_DIR=$ABS_BUILD INSTALL_DIR=$ABS_INST make -C $SRC install ||\
        die "Install failed"

cp $SRC/test_elm.c $ABS_INST && (
        cd $ABS_INST &&\
        CFLAGS="-std=c99 -I include/" LDFLAGS="-Llib" LDLIBS="-lelm" \
                make test_elm || die "Cannot build in install tree"
        ./test_elm  || die "Test inside install tree failed"
       )

ver_id=`strings $ABS_INST/lib/libelm.a | grep elm0-` || die "Can't find version string"
ver=`echo $ver_id | sed 's/ //g'`

if [ x"$expect_prerelease" = x"yes" ]
then
        xver="elm0-$VERNUM-"
        reltype="PRERELEASE"
else
        xver="elm0-$VERNUM."
        reltype="RELEASE"
fi

[ "$ver" == "$xver"  ] || die "Got version '$ver', expected '$xver'"


echo '---------------------------------------------------------'
echo "$reltype $xver is OK"

