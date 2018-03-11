B=b
CFLAGS=-std=c99 -Wall -Werror -g -O0
LDFLAGS=-L $(B)
LDLIBS=-lelm

test: $B $B/dirw
	cd $B && ./dirw


$B/dirw: $B/libelm.a

$B/%.o: %.c
	$(CC) -c $< $(CPPFLAGS) $(CFLAGS) -o $@


$B/libelm.a:
	BUILD_DIR=../$B make -C elm0/

$B:
	mkdir -p $@

clean:
	rm -rf $B
