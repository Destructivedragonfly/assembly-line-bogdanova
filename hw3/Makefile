CFLAGS ?= -std=gnu11 -Wall -Wextra -O2
SRC := $(wildcard src/*.c)


# MODE=named|unnamed|shm|mq
build/line: $(SRC) $(wildcard headers/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -Iheaders $(SRC) -o $@ -lrt -pthread

MODE ?= named
run: build/line
	build/line --mode $(MODE) $(if $(SEED),--seed $(SEED)) $(if $(DELAY),--delay $(DELAY))

clean:
	rm -rf build

.PHONY: run clean
