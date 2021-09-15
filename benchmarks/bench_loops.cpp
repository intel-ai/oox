#include <string>
#include <benchmark/benchmark.h>

#define STR_(x) #x
#define STR(x) STR_(x)
const std::string parallel_str = STR(PARALLEL);


#include "harness_parallel.h"


// Define another benchmark
static void Loop1(benchmark::State& state) {
    volatile int x = 3;
    for (auto _ : state) {
        Harness::parallel_for(0, 100000, 1, [&](int i) {
            BENCHMARK_UNUSED(x*x*x*x*x*x*x*x*x*x);
        });
    }
}

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;

    benchmark::RegisterBenchmark(("Loop1/"+parallel_str).c_str(), Loop1)->UseRealTime()->Unit(benchmark::kMicrosecond);

    Harness::InitParallel();
    //printf("Initialized for %d threads\n", Harness::nThreads);
    benchmark::RunSpecifiedBenchmarks();
    Harness::DestroyParallel();
}
