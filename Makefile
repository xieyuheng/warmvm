CC      ?= gcc
CFLAGS   = -O2 -Wall -Wextra -std=gnu11

all: wvm.exe wvmbench.exe fib.wvm

wvm.exe: bench.c wvm.c wvm.h
	$(CC) $(CFLAGS) -o $@ bench.c wvm.c

wvmbench.exe: bench.c wvm.c wvm.h
	$(CC) $(CFLAGS) -DWVM_STATS -o $@ bench.c wvm.c

wvmbench-safe.exe: bench.c wvm.c wvm.h
	$(CC) $(CFLAGS) -DWVM_SAFE -DWVM_STATS -o $@ bench.c wvm.c

fib.wvm: fib.asm warmasm.py
	python3 warmasm.py -o fib.wvm fib.asm

clean:
	rm -f wvm.exe wvmbench.exe wvmbench-safe.exe fib.wvm

.PHONY: all clean

# ---- interaction nets 验证 ----
inet.wvm: inet.asm warmasm.py
	python3 warmasm.py -o inet.wvm inet.asm

inet-run.exe: inet-run.c wvm.c wvm.h
	$(CC) $(CFLAGS) -o $@ inet-run.c wvm.c

inet-runbench.exe: inet-run.c wvm.c wvm.h
	$(CC) $(CFLAGS) -DWVM_STATS -o $@ inet-run.c wvm.c

# ---- 并行验证 ----
inet-par.wvm: inet-par.asm warmasm.py
	python3 warmasm.py -o inet-par.wvm inet-par.asm

par-run.exe: par-run.c wvm.c wvm.h
	$(CC) $(CFLAGS) -pthread -o $@ par-run.c wvm.c
