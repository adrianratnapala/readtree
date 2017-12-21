# ELM0 Makefile (requires GNU make)
#
# See README for an explanation of what ELM0 is.
#
#    make  		builds libelm.a  in $(BUILD_DIR)
#    make test 		builds elm and runs full n0run unit tests.
#    make clean         deletes all built files
#    make install       install files into $(INSTALL_DIR)/include
#                       and                $(INSTALL_DIR)/lib
#
# Copyright (C) 2012, Adrian Ratnapala, under the ISC license. See file LICENSE.
#


BUILD_DIR ?= .
INSTALL_DIR ?= $(BUILD_DIR)

LIBS=elm
TEST_PROGS=elm-test elm-fail

OPTFLAGS ?= -g -Werror
CFLAGS = -std=c99 $(OPTFLAGS) -Wall -Wno-parentheses
LDFLAGS= $(LDOPTFLAGS)

TEST_TARGETS = $(TEST_PROGS:%=$(BUILD_DIR)/%)
LIB_TARGETS = $(LIBS:%=$(BUILD_DIR)/lib%.a)


all: dirs $(LIB_TARGETS)
test_progs: dirs $(TEST_TARGETS)

$(BUILD_DIR)/%-fail.o: %.c
	$(CC) $(CFLAGS) -DFAKE_FAIL=1 -c -o $@ $^

$(BUILD_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%-test: %.o test_%.o
	$(CC) $(LDFLAGS)  -o $@ $^

%-fail: %-fail.o test_%-fail.o
	$(CC) $(LDFLAGS)  -o $@ $^

clean:
	rm -f $(TEST_TARGETS) $(LIB_TARGETS)
	rm -f $(BUILD_DIR)/*.o

test: test_progs
	TEST_DIR=$(BUILD_DIR) ./n0run.py test_elm.c ./elm-test &&\
	TEST_DIR=$(BUILD_DIR) ./elm-fail-run.py

lib%.a: %.o
	ar rcs $@ $^

$(BUILD_DIR)/*.o: 0unit.h elm.h

dirs:
	mkdir -p $(BUILD_DIR)

install: all
	mkdir -p $(INSTALL_DIR)/lib
	mkdir -p $(INSTALL_DIR)/include
	install -m 664 -t $(INSTALL_DIR)/lib $(LIB_TARGETS)
	install -m 664 -t $(INSTALL_DIR)/include elm.h 0unit.h

