#pragma once

// Candidate fixes for CSpinlock's starvation of waiters on few cores
// (measured on a dual Cortex-A9: least-served thread got 4% of its fair
// share). Kept out of include/ until benchmarks on the target pick one.
//
// Cause: with test-and-test-and-set the releasing thread can re-acquire the
// lock a few cycles after unlocking, while it still holds the cache line.
// A waiter only reads the lock, so by the time it sees it free and issues
// its exchange the lock has been taken again ("barging").

#include <locks/spinlock.hpp>

namespace locks
{
	namespace candidates
	{
		//! FIFO ticket lock. Each locker takes a ticket and waits until it is
		//! being served, so no thread can barge ahead of a waiter.
		//! Downside: if the thread whose turn it is gets descheduled, every
		//! later waiter waits for it too (bad when threads > cores).
		template <typename Policy = SpinPolicy<> >
		class alignas(64) TicketSpinlock : boost::noncopyable
		{
		public:
			typedef detail::ScopedLock<TicketSpinlock> scoped_lock;

			TicketSpinlock()
				: mNext(0)
				, mServing(0)
			{
			}

			void lock()
			{
				const uint32_t ticket = mNext.fetch_add(1, boost::memory_order_relaxed);
				if (mServing.load(boost::memory_order_acquire) == ticket)
				{
					return;
				}
				detail::Backoff<Policy> backoff;
				while (mServing.load(boost::memory_order_acquire) != ticket)
				{
					backoff.pause();
				}
			}

			bool try_lock()
			{
				// Free exactly when no ticket is outstanding (next == serving).
				uint32_t serving = mServing.load(boost::memory_order_relaxed);
				uint32_t expected = serving;
				return mNext.compare_exchange_strong(expected, serving + 1,
													 boost::memory_order_acquire,
													 boost::memory_order_relaxed);
			}

			void unlock()
			{
				// Only the holder writes mServing.
				const uint32_t serving = mServing.load(boost::memory_order_relaxed);
				mServing.store(serving + 1, boost::memory_order_release);
			}

		private:
			boost::atomic<uint32_t> mNext;    //!< Next ticket to hand out
			boost::atomic<uint32_t> mServing; //!< Ticket currently allowed to hold the lock
		};

		//! CSpinlock plus a waiter count. A thread arriving while others are
		//! already waiting does not grab the lock immediately: it first gives
		//! the waiters up to GraceSpins relax() calls to take it, and only
		//! competes if none did. Waiters themselves retry as before.
		//! The uncontended fast path costs one extra relaxed load.
		template <typename Policy = SpinPolicy<>, uint32_t GraceSpins = 256>
		class alignas(64) NoBargeSpinlock : boost::noncopyable
		{
		public:
			typedef detail::ScopedLock<NoBargeSpinlock> scoped_lock;

			NoBargeSpinlock()
				: mLocked(false)
				, mWaiters(0)
			{
			}

			void lock()
			{
				if (mWaiters.load(boost::memory_order_relaxed) == 0 &&
					!mLocked.exchange(true, boost::memory_order_acquire))
				{
					return;
				}

				if (mWaiters.load(boost::memory_order_relaxed) != 0)
				{
					// Let an existing waiter take a free lock first.
					for (uint32_t i = 0; i < GraceSpins && !mLocked.load(boost::memory_order_relaxed); ++i)
					{
						Policy::relax_type::relax();
					}
				}

				mWaiters.fetch_add(1, boost::memory_order_relaxed);
				detail::Backoff<Policy> backoff;
				for (;;)
				{
					while (mLocked.load(boost::memory_order_relaxed))
					{
						backoff.pause();
					}
					if (!mLocked.exchange(true, boost::memory_order_acquire))
					{
						break;
					}
				}
				mWaiters.fetch_sub(1, boost::memory_order_relaxed);
			}

			bool try_lock()
			{
				return !mLocked.load(boost::memory_order_relaxed) &&
					   !mLocked.exchange(true, boost::memory_order_acquire);
			}

			void unlock()
			{
				mLocked.store(false, boost::memory_order_release);
			}

		private:
			boost::atomic<bool> mLocked;
			boost::atomic<uint32_t> mWaiters; //!< Threads in the slow path
		};
	} // namespace candidates
} // namespace locks
