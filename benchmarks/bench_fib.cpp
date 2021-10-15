// Copyright (C) 2021 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#define TBB_PREVIEW_TASK_GROUP_EXTENSIONS 1

#include <benchmark/benchmark.h>
#include "../examples/fibonacci.h"
using namespace Fibonacci;

constexpr int FibN=28;

static void Fib_Serial(benchmark::State& state) {
  for (auto _ : state)
    Serial::Fib(FibN);
}
// Register the function as a benchmark
BENCHMARK(Fib_Serial)->Unit(benchmark::kMillisecond)->UseRealTime();

static void Fib_OOX1(benchmark::State& state) {
  for (auto _ : state)
    oox::wait_and_get(OOX1::Fib(FibN));
}
BENCHMARK(Fib_OOX1)->Unit(benchmark::kMillisecond)->UseRealTime();

static void Fib_OOX2(benchmark::State& state) {
  for (auto _ : state)
    oox::wait_and_get(OOX2::Fib(FibN));
}
BENCHMARK(Fib_OOX2)->Unit(benchmark::kMillisecond)->UseRealTime();

#if HAVE_TBB
#include <tbb/tbb.h>
namespace TBB1 {
    int Fib(int n, tbb::task_group_context &ctx) {                  // TBB: High-level blocking style
        if(n < 2) return n;
        int left, right;
        tbb::parallel_invoke(
            [&] { left = Fib(n-1, ctx); },
            [&] { right = Fib(n-2, ctx); },
            ctx
        );
        return left + right;
    }
}

static void Fib_TBB1(benchmark::State& state) {
  for (auto _ : state) {
    tbb::task_group_context ctx;
    TBB1::Fib(FibN, ctx);
  }
}
BENCHMARK(Fib_TBB1)->Unit(benchmark::kMillisecond)->UseRealTime();

#if TBB_INTERFACE_VERSION >= 12030
namespace TBB2 {
    int Fib(int n, tbb::task_group_context &ctx) {                  // TBB: High-level blocking style
        if(n < 2) return n;
        int left, right;
        tbb::task_group tg(ctx);
        tg.run( [&] { right = Fib(n-2, ctx); } );
        left = Fib(n-1, ctx);
        tg.wait();
        return left + right;
    }
}

static void Fib_TBB2(benchmark::State& state) {
  for (auto _ : state) {
    tbb::task_group_context ctx;
    TBB2::Fib(FibN, ctx);
  }
}
BENCHMARK(Fib_TBB2)->Unit(benchmark::kMillisecond)->UseRealTime();
#endif

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
