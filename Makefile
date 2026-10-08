# Two independent programs. Note there is no shared object file and no
# common include path: server/ and client/ each compile their own codec.
# That separation is the point of the exercise (see README).

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2
LDFLAGS ?=

SERVER_SRC = server/bserve.c server/bframe.c
CLIENT_SRC = client/bcurl.c  client/wire.c

all: bserve bcurl

bserve: $(SERVER_SRC)
	$(CC) $(CFLAGS) -Iserver -o $@ $(SERVER_SRC) $(LDFLAGS)

bcurl: $(CLIENT_SRC)
	$(CC) $(CFLAGS) -Iclient -o $@ $(CLIENT_SRC) $(LDFLAGS)

# Address and UB sanitizers. A protocol parser reads attacker-controlled
# lengths, so building it clean under ASan is not optional.
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
asan: clean all

test: all
	./tests/conformance.sh

clean:
	rm -f bserve bcurl
	rm -rf *.dSYM tests/tmp

.PHONY: all asan test clean
