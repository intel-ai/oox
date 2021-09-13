.NOTPARALLEL:
export SHELL=bash

.PHONY: all
all: release test

release:
	cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -C build -j$(shell nproc)

debug:
	cmake -B build_debug -S . -DCMAKE_BUILD_TYPE=Debug && make -C build_debug -j$(shell nproc)

clean:
	rm -rf build; rm -rf build_debug

test:
	cd build/; ctest

bench_loops:
	@echo ---------------------------------------------------------------
	@echo -e "Benchmark                \tIters\tTime\tCPU\tUnit"
	@echo ---------------------------------------------------------------
	@for x in \" $(shell ls -1 build/benchmarks/bench_loops_*) \"; do numactl -N 0 $$x --benchmark_format=csv 2>/dev/null | grep Loop | tr , '\t'; done

bench_fib:
	numactl -N 0 build/benchmarks/bench_fib

bench: bench_fib bench_loops
