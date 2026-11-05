# locks

Header-only spinlocks built on Boost, plus tests and a benchmark against regular mutexes.

- `locks::CSpinlock`: a non-recursive spinlock.
  - It uses test-and-test-and-set: waiters spin on a plain read and only attempt the atomic exchange when the lock looks free.
  - It waits with exponential backoff using the CPU pause instruction, then falls back to `yield()`.
- `locks::CRecursiveSpinlock`: a recursive spinlock that the owning thread may lock more than once.
  - Acquiring is one CAS of the owner id, releasing is one store, and re-entering involves no atomic operations.
  - It is loosely based on [Preshing's recursive mutex](https://preshing.com/20120305/implementing-a-recursive-mutex/).

Both classes:

- provide `lock()`, `try_lock()` and `unlock()`, so they work with `std::lock_guard`, `std::unique_lock`, `std::lock` and `boost::unique_lock`;
- provide a nested `scoped_lock` guard. It tracks ownership, so calling `unlock()` early is safe;
- are aligned to a 64-byte cache line (`alignas(64)`) to avoid false sharing. Each lock is therefore 64 bytes, which matters if you have many of them.

`CRecursiveSpinlock::unlock()` from a thread that doesn't hold the lock calls `std::abort()`, in release builds too.

```cpp
#include <locks/spinlock.hpp>

locks::CSpinlock lock;
{
    locks::CSpinlock::scoped_lock guard(lock);   // or std::lock_guard<locks::CSpinlock>
    // critical section
}
```

Use them only for short critical sections, with fewer threads than cores. Otherwise use a regular mutex (see the results below).

### Spin policy

`CSpinlock` and `CRecursiveSpinlock` are typedefs for `BasicSpinlock<>` and `BasicRecursiveSpinlock<>`, using the default `SpinPolicy` for the CPU being compiled for. Because they are now typedefs, a forward declaration such as `class CSpinlock;` no longer compiles; include the header instead.

A `SpinPolicy<Relax, MaxBackoff>` sets two things:

- **The hint a waiter executes between checks:**

  | hint | instruction | default on |
  |---|---|---|
  | `relax::Pause` | x86 `pause` | x86 |
  | `relax::Isb` | AArch64 `isb sy` | AArch64 |
  | `relax::Yield` | ARM `yield` | 32-bit ARM |

  Most Cortex-A cores treat `yield` as a no-op, so it adds almost no delay. `isb` really stalls the core briefly, which is why it is the AArch64 default; Rust's spin loop uses it too. On a CPU a hint doesn't apply to, it compiles to a plain compiler barrier.
- **The backoff limit:** the number of hint calls per round before the waiter gives up and calls `yield()`, which is a syscall. Each round doubles until it reaches the limit. The default is 64. You can override it globally with `-DLOCKS_SPIN_MAX_BACKOFF=N`, or per lock:

```cpp
typedef locks::BasicSpinlock<locks::SpinPolicy<locks::relax::Yield, 256> > MySpinlock;
```

## Build

You need CMake 3.14 or later, a C++17 compiler, and Boost (`thread` and `atomic`). On Debian or Ubuntu:
`apt-get install libboost-thread-dev libboost-atomic-dev`.

```sh
cmake -S . -B build          # defaults to Release
cmake --build build -j
ctest --test-dir build       # correctness tests (asserts always enabled)
./build/lock_benchmark       # performance comparison
```

### Building on the target (e.g. ARM)

```sh
cmake -S . -B build -DLOCKS_NATIVE_CPU=ON   # -mcpu=native on ARM, -march=native on x86
cmake --build build -j
ctest --test-dir build
./build/lock_benchmark | tee results.txt     # or --csv
```

- **`LOCKS_NATIVE_CPU`** matters on ARM. On ARMv8.1+ cores (Cortex-A55/A76, Raspberry Pi 5 and newer) it lets atomic operations compile to single LSE instructions (`swp`/`cas`). On ARMv8.0 cores (Cortex-A53/A72, Raspberry Pi 3/4) those cores have no such instructions, and GCC's default `-moutline-atomics` turns every atomic operation into a library call that picks an implementation at runtime.
- **`-DLOCKS_SPIN_MAX_BACKOFF=N`** changes the default backoff limit.

The first line of the benchmark output records the architecture, compiler, atomic implementation, default hint and backoff limit, so keep it with any results you share.

## Benchmark

`lock_benchmark` has two phases.

**Throughput.** N threads each repeatedly take one shared lock, do some work, and release it. The benchmark reports the median wall time per lock acquisition in nanoseconds (lower is better), across several repetitions. There are three workloads:

| workload | critical section |
|---|---|
| `tiny`   | a single increment, which measures pure lock overhead |
| `medium` | about 100 dependent multiply-adds while holding the lock |
| `nested` | `tiny`, but the lock is taken twice per op (recursive locks only) |

**Fairness.** Every lock runs for a fixed time with the `tiny` workload, and each thread counts how many times it got the lock. A lock can show high throughput simply by letting one thread re-take it over and over while the others starve. The `min/mean` column shows the share of the least-served thread (1.00 = perfectly even).

It compares:

- `CSpinlock` with `boost::mutex` and `std::mutex`
- `CRecursiveSpinlock` with `boost::recursive_mutex` and `std::recursive_mutex`
- the previous implementation, as `v1::...` (`bench/spinlock_v1.hpp`). This is the code as originally provided, with one change: the race fix on `CRecursiveSpinlock`'s uncontended path. Without it the benchmark stops with "lost updates".
- spin-policy variants of the current locks:
  - `[backoff1024]`: a 16× longer backoff before yielding;
  - `[yield]`: on ARM only, the `yield` hint instead of the default `isb`.

By default the thread counts run 1, 2, 4, … up to the core count, then 2× the core count to show oversubscription.

Options:

- `--ops N`: operations per thread
- `--reps N`: repetitions
- `--threads 1,2,8`: thread counts to run
- `--fairness-ms N`: duration of each fairness run (0 skips the phase)
- `--csv`: machine-readable output

### Results

These come from a 4-vCPU x86 cloud VM (GCC 13, Boost 1.83), run with `--ops 100000 --reps 3`, before the spin-policy variants were added. There are no ARM results yet. The VM is noisy, so each cell shows the range across two full runs. Numbers are ns per acquisition, lower is better.

| | 1 thread | 2 | 4 | 8 |
|---|---|---|---|---|
| **tiny** | | | | |
| `CSpinlock` | 8.8–10.2 | 15–16 | 14–34 | 16–17 |
| `v1::CSpinlock` | 8.6–10.3 | 12–13 | 19–30 | 13–18 |
| `std::mutex` | 24–28 | 68–85 | 53–56 | 53 |
| `CRecursiveSpinlock` | **15** | 30–41 | 131–183 | **30** |
| `v1::CRecursiveSpinlock` | 28 | 52–56 | 124–133 | 57–62 |
| `std::recursive_mutex` | 21–23 | 46–87 | 55–69 | 49–64 |
| **medium** | | | | |
| `CSpinlock` | 133–139 | 144–160 | 185–342 | 186–217 |
| `v1::CSpinlock` | 127–130 | 134–173 | 283–286 | 176–205 |
| `std::mutex` | 127–153 | 388–408 | 293–309 | 273–274 |
| `CRecursiveSpinlock` | 127–136 | 135–280 | 373–465 | 178–337 |
| `v1::CRecursiveSpinlock` | 130–135 | 143–155 | 369–532 | 185–306 |
| `std::recursive_mutex` | 126–130 | 406–413 | 311–348 | 226–246 |
| **nested** | | | | |
| `CRecursiveSpinlock` | **14** | **22–25** | **64–85** | **46–53** |
| `v1::CRecursiveSpinlock` | 50–56 | 145–151 | 251–372 | 167–188 |
| `std::recursive_mutex` | 35–42 | 71–130 | 70–76 | 68–79 |

Fairness, as `min/mean` share (higher is fairer):

| | 2 threads | 4 | 8 |
|---|---|---|---|
| `CSpinlock` | 0.95–0.99 | 0.77–0.80 | 0.60–0.64 |
| `v1::CSpinlock` | 0.84–0.97 | 0.78–0.89 | 0.50–0.53 |
| `std::mutex` | 0.90–0.96 | 0.66–0.82 | 0.65–0.67 |
| `CRecursiveSpinlock` | 0.53–0.77 | 0.53–0.69 | 0.47–0.66 |
| `v1::CRecursiveSpinlock` | 0.81–0.82 | 0.74–0.76 | 0.26–0.35 |
| `std::recursive_mutex` | 0.65–0.97 | 0.88–0.92 | 0.81 |

What the results show:

- **`CRecursiveSpinlock`** improved the most.
  - Uncontended it is about 2× faster than v1 and also beats `std::recursive_mutex`.
  - In the `nested` workload it is 3–6× faster than v1 at every thread count, and now matches or beats `std::recursive_mutex` there.
  - Starvation under oversubscription dropped (8-thread `min/mean` 0.47–0.66, against 0.26–0.35 for v1).
- **Remaining weak spot for `CRecursiveSpinlock`:** with exactly as many threads as cores (4) on short critical sections it is still 2–3× slower than the mutexes. Results there are bimodal (either about 50 or about 180 ns per acquisition), and moving `mRecursion` to its own cache line did not change that reliably.
- **`CSpinlock`** speed is about the same as v1 on this machine. All differences are within run-to-run noise, and both are 2–5× faster than `std::mutex` on the `tiny` workload. Its rework mainly brought `try_lock`, the safe guard and somewhat less starvation at 8 threads. The cache-line benefit of test-and-test-and-set grows with core count, so a 4-vCPU VM is a poor place to see it.
- **Fairness:** the spinlocks are generally less fair than the mutexes, especially once there are more threads than cores.
