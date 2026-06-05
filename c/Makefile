CC := gcc
CFLAGS := -O3 -march=haswell -mavx2 -mfma -flto -Wall -Wextra -fno-plt
LDFLAGS := -O3 -flto

CORE := parser.o index.o search.o distance_avx2.o

all: rinha-server rinha-lb selftest

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

rinha-server: server.o $(CORE)
	$(CC) $(LDFLAGS) $^ -o $@ -lpthread -lm

rinha-lb: lb.c
	$(CC) $(CFLAGS) lb.c -o rinha-lb $(LDFLAGS)

selftest: selftest.o $(CORE)
	$(CC) $(LDFLAGS) $^ -o $@ -lm

bench: bench.o $(CORE)
	$(CC) $(LDFLAGS) $^ -o $@ -lm

# builder offline (precisa de zlib)
build_index: build_index.c constants.h
	$(CC) -O3 -march=x86-64-v3 build_index.c -o build_index -lz -lm

clean:
	rm -f *.o rinha-server rinha-lb selftest bench build_index

.PHONY: all clean
