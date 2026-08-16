CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -g -O2
LDLIBS ?= -lm
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

SRCS := src/main.c src/token.c src/lexer.c src/ast.c src/parser.c src/bytecode.c src/vm.c
OBJS := $(SRCS:.c=.o)
HDRS := $(wildcard src/*.h)

aurascript: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

install: aurascript
	mkdir -p $(BINDIR)
	ln -sf $(CURDIR)/aurascript $(BINDIR)/aurascript

uninstall:
	rm -f $(BINDIR)/aurascript

clean:
	rm -f aurascript $(OBJS)

.PHONY: clean install uninstall