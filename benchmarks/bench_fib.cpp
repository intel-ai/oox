#include <benchmark/benchmark.h>
#include <oox/oox.h>

constexpr int FibN=20;

namespace Serial {
    int Fib(int n) {
        if(n < 2) return n;
        return Fib(n-1) + Fib(n-2);
    }
}
static void Fib_Serial(benchmark::State& state) {
  for (auto _ : state)
    Serial::Fib(FibN);
}
// Register the function as a benchmark
BENCHMARK(Fib_Serial);


namespace OOX {
    oox_var<int> Fib(int n) {
        if(n < 2) return n;
        return oox_run(std::plus<int>(), Fib(n-1), oox_run(Fib, n-2));
    }
}
// Define another benchmark
static void Fib_OOX(benchmark::State& state) {
  for (auto _ : state)
    oox_wait_and_get(OOX::Fib(FibN));
}
BENCHMARK(Fib_OOX);

#if HAVE_TBB
#include <tbb/tbb.h>
namespace TBB {
    int Fib(int n) {                  // TBB: High-level blocking style
        if(n < 2) return n;
        int left, right;
        tbb::parallel_invoke(
            [&] { left = Fib(n-1); },
            [&] { right = Fib(n-2); }
        );
        return left + right;
    }
}

static void Fib_TBB(benchmark::State& state) {
  for (auto _ : state)
    TBB::Fib(FibN);
}
BENCHMARK(Fib_TBB);
#endif

BENCHMARK_MAIN();
