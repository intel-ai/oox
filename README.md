# OOX
Out-of-Order Executor library. Yet another approach to efficient and scalable tasking API and task scheduling.

## Try it
* **Requirements**: Install cmake, make, googletest, google benchmark, and optionally TBB (recommended), and TaskFlow libraries into your environment.
* **Build & run**: `make`
* **Install**: `make install`

## Continuation-focus design
With nested parallelism, blocking style programming is deadlock-prone and has latency problems. OOX provides semantic way out of these issues.

`std::future` is not intended for continuation tasks. Even with existing proposals like `.then()`, the continuation-style is limited - while OOX offers:
- Implicitly collapse template recursion of 'futures': `future x = async(async(async(…)));`
- Implicitly unpack 'futures' to a value in arguments: `async([](int a){}, async(…));`
- Implicit value conversion for a 'future' variable. e.g.: `future<int> x{2};`
- Implicitly build dependencies based on arguments: No blocking synchronization in the algorithm

This approach enables beautifully concise recursive parallelism programming like:
```C++
oox::var<int> Fib(int n) {         // OOX: High-level continuation style programming
    if(n < 2) return n;
    auto left = oox::run(Fib, n-1);
    return oox::run(std::plus<int>(), std::move(left), Fib(n-2));
}
```
In contrast, both Intel Threading Building Blocks (TBB) and TaskFlow examples do block, which makes them slower than OOX besides requiring verboser coding:
```C++
int Fib(int n) {                    // TBB: High-level blocking style programming
    if(n < 2) return n;
    int left, right;
    tbb::parallel_invoke(           // blocks here
        [&] { left = Fib(n-1); },
        [&] { right = Fib(n-2); }
    );
    return left + right;
}
```
and
```C++
int Fib(int n, tf::Subflow& sbf) {  // TaskFlow: High-level blocking style programming
    if (n < 2) return n;
    int res1, res2;
    sbf.emplace([&res1, n] (tf::Subflow& sbf) { res1 = Fib(n - 1, sbf); } );
    sbf.emplace([&res2, n] (tf::Subflow& sbf) { res2 = Fib(n - 2, sbf); } );
    sbf.join();                     // blocks here
    return res1 + res2;
}
```

## Quick Reference
- `oox::var<T>`: Basic representation of data in the OOX graph. In concept, a new form of `std::future` for continuations. It carries both: a value and dependency info
  - `using oox::node = oox::var<void>`: carries solely dependency info
- `oox::var<T> oox::run(T(Func&)(...), Args...)`: Basic tasking API, spawns a task when arguments are ready and returns `oox::var` as a promise to provide the result of Func in future. If there are `oox::var` arguments, which are not ready yet (i.e. they are "promises" themselves), it makes a continuation task, which depends on completion of pending `oox::var` arguments.

## Design
Pillars:
- Abstract user functor from async dependencies: `oox::run([](functor args...){}, dependency args...)`
- Reuse functor and runner arg types matching for dependency type specification
  - Flow, output, and anti-dependencies are deduced
  - Makes duplication of arguments finally useful
- Have clear serialization semantics for ease of debugging

Matching rules:
- plain arguments:
  - Follow C++ rules: everything is passed as decay copy
  - Use `std::ref` and `std::cref` for passing by reference (and take responsibility for the lifetime)
- `oox::var` arguments:
  - Similar to std::ref: usually passed by reference but it cares about lifetime and access sync
  - `oox::var` usage has to be indifferent from plain types

Stored types:
- `oox::run` returns `oox::var` for decay type of functor return type, copy- or move-initialized.
- `oox::var` doesn't store references. Use `std::reference_wrapper` or pointer types instead
- Don't compile oox_var<T> if not `is_same<T, std::decay<T>::type>`
- `oox::var` is reference-counted, it is safe for end of scope

Access types of `oox::var`'s stored value:
- **Read-Write**: *Exclusive* access: both matching arguments are passed by reference
- **Read-Only**: *Shared* access: `auto f = [](const A&){}` with `oox::var<A>& a` or `const oox::var<A>& a` while running `oox::run(f, a)`
- **Copy-Only**: *Copy* access: like read-only but does not hold off following read-write access after a copy is done, `auto f = [](A){}` or `auto f = [](A&&){}` with `oox::var<A>& a` or `const oox::var<A>& a` while running `oox::run(f, a)`
- **Final**: dispose after use: `oox::var<A>&& a`

## More resources
* Blog: https://habr.com/en/company/intel/blog/542908/
* Slides: https://www.slideshare.net/secret/ifHWb6mqkpBOn2

