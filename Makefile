B=b
CFLAGS=-std=c99 -Wall -Werror -g -O0
LDFLAGS=-L $(B)
LDLIBS=-lelm -lreadtree

all: test

test: $B $B/readtree_test
	cd $B && ./readtree_test

$B/readtree_test: $B/readtree_test.o $B/libreadtree.a $B/libelm.a

$B/libreadtree: readtree.c

$B/lib%.a: $B/%.o
	ar rcs $@ $^

$B/%.o: %.c
	$(CC) -c $< $(CPPFLAGS) $(CFLAGS) -o $@

$B/libelm.a:
	BUILD_DIR=../$B make -C elm0/

$B/readtree_test.o: readtree.h
$B/readtree: readtree.h

$B:
	mkdir -p $@

clean:
	rm -rf $B
