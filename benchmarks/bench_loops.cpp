#include <string>
#include <benchmark/benchmark.h>

#define STR_(x) #x
#define STR(x) STR_(x)
const std::string parallel_str = STR(PARALLEL);


#include "harness_parallel.h"


static void Loop1(benchmark::State& state) {
    volatile int x = 3;
    for (auto _ : state) {
        Harness::parallel_for(0, int(state.range(0)), 1, [&](int i) {
            volatile auto z = x*x; // basically nothing
        });
    }
}

// static void Loop2(benchmark::State& state) {
//     volatile int a[100004];
//     for (auto _ : state) {
//         Harness::parallel_for(0, 100000, 1, [&](int i) {
//             a[i] = a[i]*a[i+1]*a[i+2]*a[i+3];
//         });
//     }
// }

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;

    Harness::InitParallel();
    //printf("Initialized for %d threads\n", Harness::nThreads);

    benchmark::RegisterBenchmark((parallel_str+",Loop1").c_str(), Loop1)->UseRealTime()->Unit(benchmark::kMicrosecond)->Range(64, 262144);
    //benchmark::RegisterBenchmark((parallel_str+",Loop2").c_str(), Loop2)->UseRealTime()->Unit(benchmark::kMicrosecond)->Range(64, 262144);

    benchmark::RunSpecifiedBenchmarks();
    Harness::DestroyParallel();
}
