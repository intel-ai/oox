#include <benchmark/benchmark.h>
#include "harness_parallel.h"


// Define another benchmark
static void Loop1(benchmark::State& state) {
    volatile int x = 3;
    for (auto _ : state) {
        Harness::parallel_for(0, 1000000, 1, [&](int i) {
            x = x*x*x*x*x*x*x*x*x;
        });
    }
}

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;

    benchmark::RegisterBenchmark("Loop1/" MACRO_STRING(PARALLEL), Loop1)->UseRealTime()->Unit(benchmark::kMillisecond);

    Harness::InitParallel();
    benchmark::RunSpecifiedBenchmarks();
    Harness::DestroyParallel();
}
