// Copyright (C) 2021 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#include <oox/oox.h>

namespace Fibonacci {
namespace Serial { // Original problem statement

    int Fib(volatile int n) {
        if(n < 2) return n;
        return Fib(n-1) + Fib(n-2);
    }

}
namespace OOX1 { // Concise 2 lines OOX demonstration

    oox::var<int> Fib(int n) {
        if(n < 2) return n;
        return oox::run(std::plus<int>(), oox::run(Fib, n-1), oox::run(Fib, n-2) );
    }

}
namespace OOX2 { // Optimized number and order of tasks

    oox::var<int> Fib(int n) {                                         // OOX: High-level continuation style
        if(n < 2) return n;
        auto right = oox::run(Fib, n-2);                               // spawn right child
        return oox::run(std::plus<int>(), Fib(n-1), std::move(right)); // assign continuation
    }

}}