// Correctness tests for CSpinlock and CRecursiveSpinlock.
// Each test hammers a lock from several threads and checks that a plain
// (non-atomic) counter guarded by the lock ends up with the exact value.

#include <locks/spinlock.hpp>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>
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

void testScopedLockEarlyUnlock()
{
	// Previously the guard's destructor unlocked again after a manual
	// unlock(), releasing a lock that another thread had since acquired.
	locks::CSpinlock lock;
	bool stillHeldByOther = false;
	{
		locks::CSpinlock::scoped_lock guard(lock);
		guard.unlock();
		check(!guard.owns_lock(), "scoped_lock::owns_lock false after unlock");

		std::thread other([&] { lock.lock(); });
		other.join();
		// `other` exited holding the lock; the guard must not release it.
	}
	stillHeldByOther = !lock.try_lock();
	check(stillHeldByOther, "scoped_lock destructor does not unlock after early unlock()");
	lock.unlock();

	locks::CRecursiveSpinlock rlock;
	{
		locks::CRecursiveSpinlock::scoped_lock guard(rlock);
		guard.unlock();
		guard.lock();
		check(guard.owns_lock(), "recursive scoped_lock relock after unlock");
	}
	bool freeAfter = false;
	std::thread([&] {
		freeAfter = rlock.try_lock();
		if (freeAfter)
		{
			rlock.unlock();
		}
	}).join();
	check(freeAfter, "recursive scoped_lock releases exactly once");
}

template <typename Lock>
bool tryLockFromOtherThread(Lock &lock)
{
	bool got = false;
	std::thread([&] {
		got = lock.try_lock();
		if (got)
		{
			lock.unlock();
		}
	}).join();
	return got;
}

void testTryLock()
{
	locks::CSpinlock lock;
	check(lock.try_lock(), "CSpinlock::try_lock succeeds when free");
	check(!lock.try_lock(), "CSpinlock::try_lock fails when held (non-recursive)");
	check(!tryLockFromOtherThread(lock), "CSpinlock::try_lock fails from another thread when held");
	lock.unlock();
	check(tryLockFromOtherThread(lock), "CSpinlock::try_lock succeeds from another thread after unlock");

	locks::CRecursiveSpinlock rlock;
	check(rlock.try_lock(), "CRecursiveSpinlock::try_lock succeeds when free");
	check(rlock.try_lock(), "CRecursiveSpinlock::try_lock re-enters for owner");
	check(!tryLockFromOtherThread(rlock), "CRecursiveSpinlock::try_lock fails from another thread when held");
	rlock.unlock();
	check(!tryLockFromOtherThread(rlock), "CRecursiveSpinlock still held after one of two unlocks");
	rlock.unlock();
	check(tryLockFromOtherThread(rlock), "CRecursiveSpinlock free after matching unlocks");
}

void testStdLockCompatibility()
{
	locks::CSpinlock a;
	locks::CRecursiveSpinlock b;
	{
		std::unique_lock<locks::CSpinlock> ua(a, std::defer_lock);
		std::unique_lock<locks::CRecursiveSpinlock> ub(b, std::defer_lock);
		std::lock(ua, ub); // needs try_lock
		check(ua.owns_lock() && ub.owns_lock(), "std::lock works with both spinlocks");
	}
	check(tryLockFromOtherThread(a) && tryLockFromOtherThread(b), "std::unique_lock releases both spinlocks");
	check(alignof(locks::CSpinlock) == 64 && alignof(locks::CRecursiveSpinlock) == 64,
		  "spinlocks are cache-line aligned");
}

void testUnlockByNonOwnerAborts()
{
	// Run in a child process: unlocking from a thread that does not own the
	// lock must abort (in release builds too) instead of corrupting it.
	std::fflush(stdout);
	const pid_t pid = fork();
	if (pid == 0)
	{
		locks::CRecursiveSpinlock lock;
		lock.lock();
		std::thread([&] { lock.unlock(); }).join();
		_exit(0); // should not get here
	}
	int status = 0;
	waitpid(pid, &status, 0);
	check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
		  "CRecursiveSpinlock::unlock by non-owner aborts");
}
} // namespace

int main()
{
	testSpinlockMutualExclusion();
	testRecursiveSpinlockReentry();
	testRecursiveSpinlockMutualExclusion();
	testScopedLockEarlyUnlock();
	testTryLock();
	testStdLockCompatibility();
	testUnlockByNonOwnerAborts();

	if (gFailures != 0)
	{
		std::printf("%d test(s) failed\n", gFailures);
		return EXIT_FAILURE;
	}
	std::printf("All tests passed\n");
	return EXIT_SUCCESS;
}
