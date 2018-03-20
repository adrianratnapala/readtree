B=b
CFLAGS=-std=c99 -Wall -Werror -g -O0
LDFLAGS=-L $(B)
LDLIBS=-lelm -lreadtree

all: test

test: $B $B/test_readtree
	cd $B && ./test_readtree

$B/test_readtree: $B/test_readtree.o $B/libreadtree.a $B/libelm.a

$B/libreadtree: readtree.c

$B/lib%.a: $B/%.o
	ar rcs $@ $^

$B/%.o: %.c
	$(CC) -c $< $(CPPFLAGS) $(CFLAGS) -o $@

$B/libelm.a:
	BUILD_DIR=../$B make -C elm0/

$B/test_readtree.o: readtree.h
$B/readtree: readtree.h

$B:
	mkdir -p $@

clean:
	rm -rf $B
