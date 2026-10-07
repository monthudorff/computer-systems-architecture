CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L

restaurant: main.c sim.c restaurant.h
	$(CC) $(CFLAGS) -o restaurant main.c sim.c

test: restaurant
	sh tests/run_tests.sh

clean:
	rm -f restaurant restaurant.log

.PHONY: test clean
