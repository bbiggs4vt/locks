// Correctness tests for CSpinlock and CRecursiveSpinlock.
// Each test hammers a lock from several threads and checks that a plain
// (non-atomic) counter guarded by the lock ends up with the exact value.

#include <locks/spinlock.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace
{
int gFailures = 0;

void check(bool ok, const char *what)
{
	std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
	if (!ok)
	{
		++gFailures;
	}
}

template <typename Fn>
void runThreads(unsigned threads, Fn fn)
{
	std::vector<std::thread> pool;
	for (unsigned t = 0; t < threads; ++t)
	{
		pool.emplace_back(fn);
	}
	for (auto &th : pool)
	{
		th.join();
	}
}

void testSpinlockMutualExclusion()
{
	locks::CSpinlock lock;
	uint64_t counter = 0;
	const unsigned threads = 8;
	const uint64_t iters = 100000;

	runThreads(threads, [&] {
		for (uint64_t i = 0; i < iters; ++i)
		{
			locks::CSpinlock::scoped_lock guard(lock);
			++counter;
		}
	});

	check(counter == threads * iters, "CSpinlock mutual exclusion");
}

void testRecursiveSpinlockReentry()
{
	locks::CRecursiveSpinlock lock;
	int depth = 0;
	{
		locks::CRecursiveSpinlock::scoped_lock a(lock);
		++depth;
		{
			locks::CRecursiveSpinlock::scoped_lock b(lock);
			++depth;
			{
				locks::CRecursiveSpinlock::scoped_lock c(lock);
				++depth;
			}
		}
	}
	check(depth == 3, "CRecursiveSpinlock single-thread re-entry");

	// After full release, another thread must be able to take it.
	bool acquired = false;
	std::thread other([&] {
		locks::CRecursiveSpinlock::scoped_lock g(lock);
		acquired = true;
	});
	other.join();
	check(acquired, "CRecursiveSpinlock released after nested unlocks");
}

void testRecursiveSpinlockMutualExclusion()
{
	locks::CRecursiveSpinlock lock;
	uint64_t counter = 0;
	const unsigned threads = 8;
	const uint64_t iters = 50000;

	runThreads(threads, [&] {
		for (uint64_t i = 0; i < iters; ++i)
		{
			locks::CRecursiveSpinlock::scoped_lock outer(lock);
			++counter;
			{
				// Re-enter while other threads are contending.
				locks::CRecursiveSpinlock::scoped_lock inner(lock);
				++counter;
			}
		}
	});

	check(counter == 2 * threads * iters, "CRecursiveSpinlock mutual exclusion with nesting");
}
} // namespace

int main()
{
	testSpinlockMutualExclusion();
	testRecursiveSpinlockReentry();
	testRecursiveSpinlockMutualExclusion();

	if (gFailures != 0)
	{
		std::printf("%d test(s) failed\n", gFailures);
		return EXIT_FAILURE;
	}
	std::printf("All tests passed\n");
	return EXIT_SUCCESS;
}
