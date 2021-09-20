#include <benchmark/benchmark.h>
#include <oox/oox.h>

constexpr int FibN=28;

namespace Serial {
    int Fib(volatile int n) {
        if(n < 2) return n;
        return Fib(n-1) + Fib(n-2);
    }
}
static void Fib_Serial(benchmark::State& state) {
  for (auto _ : state)
    Serial::Fib(FibN);
}
// Register the function as a benchmark
BENCHMARK(Fib_Serial)->Unit(benchmark::kMillisecond)->UseRealTime();


namespace OOX {
    oox::var<int> Fib(int n) {
        if(n < 2) return n;
        auto right = oox::run(Fib, n-2);
        return oox::run(std::plus<int>(), Fib(n-1), std::move(right));
    }
}
// Define another benchmark
static void Fib_OOX(benchmark::State& state) {
  for (auto _ : state)
    oox::wait_and_get(OOX::Fib(FibN));
}
BENCHMARK(Fib_OOX)->Unit(benchmark::kMillisecond)->UseRealTime();

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
BENCHMARK(Fib_TBB)->Unit(benchmark::kMillisecond)->UseRealTime();

#endif //HAVE_TBB

#if HAVE_TF

#include <taskflow/taskflow.hpp>
#include <tbb/info.h>
const int nThreads = tbb::info::default_concurrency(); // respect affinity mask

namespace TF {
    int spawn(int n, tf::Subflow& sbf) {
        if (n < 2) return n;
        int res1, res2;

        // compute f(n-1)
        sbf.emplace([&res1, n] (tf::Subflow& sbf) { res1 = spawn(n - 1, sbf); } );
            //.name(std::to_string(n-1));

        // compute f(n-2)
        sbf.emplace([&res2, n] (tf::Subflow& sbf) { res2 = spawn(n - 2, sbf); } );
            //.name(std::to_string(n-2));

        sbf.join();
        return res1 + res2;
    }

    void Fib(int N) {
        tf::Executor executor(nThreads);
        tf::Taskflow taskflow("fibonacci");
        int res;  // result
        taskflow.emplace([&res, N] (tf::Subflow& sbf) { 
            res = spawn(N, sbf);  
        }); //.name(std::to_string(N));

        executor.run(taskflow).wait();        
    }
}
static void Fib_TF(benchmark::State& state) {
  for (auto _ : state)
    TF::Fib(FibN);
}
BENCHMARK(Fib_TF)->Unit(benchmark::kMillisecond)->UseRealTime();
#endif //HAVE_TF

BENCHMARK_MAIN();
