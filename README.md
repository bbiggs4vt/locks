# locks

Header-only spinlocks built on Boost, plus tests and a benchmark against regular mutexes.

- `locks::CSpinlock`: a test-and-set spinlock that yields while it waits.
- `locks::CRecursiveSpinlock`: a recursive spinlock based on
  [Preshing's recursive mutex](https://preshing.com/20120305/implementing-a-recursive-mutex/).

Both provide `lock()`/`unlock()` and a nested `scoped_lock` RAII guard, the same as `boost::mutex::scoped_lock`.

```cpp
#include <locks/spinlock.hpp>

locks::CSpinlock lock;
{
    locks::CSpinlock::scoped_lock guard(lock);
    // critical section
}
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

## Benchmark

`lock_benchmark` starts N threads. Each thread repeatedly takes one shared lock, does some work, and releases it. The benchmark reports the median wall time per lock acquisition in nanoseconds (lower is better), across several repetitions.

It compares:

- `CSpinlock` with `boost::mutex` and `std::mutex`
- `CRecursiveSpinlock` with `boost::recursive_mutex` and `std::recursive_mutex`

There are three workloads:

| workload | critical section |
|---|---|
| `tiny`   | a single increment, which measures pure lock overhead |
| `medium` | about 100 dependent multiply-adds while holding the lock |
| `nested` | `tiny`, but the lock is taken twice per op (recursive locks only) |

By default the thread counts run 1, 2, 4, … up to the core count, then 2× the core count to show oversubscription.

Options:

- `--ops N`: operations per thread
- `--reps N`: repetitions
- `--threads 1,2,8`: thread counts to run
- `--csv`: machine-readable output

### Sample results

These come from a 4-vCPU cloud VM, GCC 13, Boost 1.83, run with `--ops 100000 --reps 3`. They are noisy, so treat them as indicative only.

```
== workload: tiny ==
lock \ threads                   1         2         4         8
CSpinlock                      8.6      29.4      58.9      12.7
boost::mutex                  19.5      75.9      47.8      49.9
std::mutex                    23.6      54.9      56.4      45.1
CRecursiveSpinlock            35.9      56.5     171.2      53.6
boost::recursive_mutex        32.2      95.9      47.8      54.7
std::recursive_mutex          21.2      67.6      52.9      47.6

== workload: medium ==
lock \ threads                   1         2         4         8
CSpinlock                    126.9     144.9     286.2     222.7
boost::mutex                 126.0     435.6     303.0     280.1
std::mutex                   136.5     414.2     304.3     284.9
CRecursiveSpinlock           125.7     187.4     564.2     300.6
boost::recursive_mutex       125.1     355.9     297.7     288.6
std::recursive_mutex         128.5     382.4     263.1     235.8

== workload: nested ==
lock \ threads                   1         2         4         8
CRecursiveSpinlock            46.7      74.2     447.1     156.4
boost::recursive_mutex        23.0      67.5      67.4      56.5
std::recursive_mutex          22.0      55.8      58.8      64.2
```

What the results show:

- **Uncontended (1 thread):** `CSpinlock` is about 2–3× cheaper than either mutex, because it needs only one atomic exchange and one store.
- **Light contention:** `CSpinlock` is competitive or ahead.
- **Throughput is not fairness:** the very low 8-thread `CSpinlock` number most likely comes from the waiting threads yielding. One thread re-acquires the lock many times in a row while the others are descheduled. That gives high throughput but poor fairness.
- **`CRecursiveSpinlock`:**
  - Uncontended, it is slower than the standard recursive mutexes because it does several seq_cst atomics per lock and unlock.
  - Under contention it degrades badly, about 5–8× worse at 4 threads in the `nested` workload.
  - The mutexes win there because they put waiters to sleep in the kernel instead of having every waiter hammer the same cache line with CAS.
