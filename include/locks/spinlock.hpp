#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>

#include <boost/atomic.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread/thread.hpp>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace locks
{
	namespace detail
	{
		//! Tell the CPU we are in a spin-wait loop (x86 PAUSE / ARM YIELD).
		//! Saves power, frees resources for a sibling hyper-thread and avoids
		//! the memory-order mis-speculation penalty when the loop exits.
		inline void cpuRelax()
		{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
			_mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
			__asm__ __volatile__("yield" ::: "memory");
#endif
		}

		//! Spin-wait strategy: exponential backoff with cpuRelax() for a
		//! bounded number of rounds, then fall back to yielding the time slice.
		//! Yielding is a syscall, so it is only used once spinning has failed
		//! for a while (the holder is probably descheduled).
		class Backoff
		{
		public:
			void pause()
			{
				if (mSpins <= kMaxSpins)
				{
					for (uint32_t i = 0; i < mSpins; ++i)
					{
						cpuRelax();
					}
					mSpins <<= 1;
				}
				else
				{
					boost::this_thread::yield();
				}
			}

		private:
			static const uint32_t kMaxSpins = 64;
			uint32_t mSpins = 1;
		};

		//! Generic RAII guard used as Lock::scoped_lock.
		//! Unlike the previous guard it tracks ownership, so an early
		//! unlock() is not repeated by the destructor.
		template <typename Lock>
		class ScopedLock : boost::noncopyable
		{
		public:
			explicit ScopedLock(Lock &lock)
				: mLock(lock)
				, mOwns(false)
			{
				this->lock();
			}

			~ScopedLock()
			{
				if (mOwns)
				{
					mLock.unlock();
				}
			}

			void lock()
			{
				assert(!mOwns && "scoped_lock already owns the lock");
				mLock.lock();
				mOwns = true;
			}

			bool try_lock()
			{
				assert(!mOwns && "scoped_lock already owns the lock");
				mOwns = mLock.try_lock();
				return mOwns;
			}

			void unlock()
			{
				assert(mOwns && "scoped_lock does not own the lock");
				mLock.unlock();
				mOwns = false;
			}

			bool owns_lock() const
			{
				return mOwns;
			}

		private:
			Lock &mLock;
			bool mOwns;
		};
	} // namespace detail

	//! CSpinlock is a lightweight, non-recursive mutex.
	//!
	//! Test-and-test-and-set with backoff: waiters spin on a plain load (which
	//! stays in their own cache) and only attempt the atomic exchange when the
	//! lock looks free, so they do not fight the holder for the cache line.
	//!
	//! Satisfies the Lockable concept (lock/try_lock/unlock), so it also works
	//! with std::lock_guard, std::unique_lock and boost::unique_lock.
	//!
	//! Aligned to a cache line to avoid false sharing with neighbouring data.
	//! Use for short critical sections with fewer threads than cores;
	//! otherwise prefer a regular mutex.
	class alignas(64) CSpinlock : boost::noncopyable
	{
	public:
		typedef detail::ScopedLock<CSpinlock> scoped_lock;

		CSpinlock()
			: mLocked(false)
		{
		}

		void lock()
		{
			// Fast path: uncontended.
			if (!mLocked.exchange(true, boost::memory_order_acquire))
			{
				return;
			}

			detail::Backoff backoff;
			for (;;)
			{
				while (mLocked.load(boost::memory_order_relaxed))
				{
					backoff.pause();
				}
				if (!mLocked.exchange(true, boost::memory_order_acquire))
				{
					return;
				}
			}
		}

		bool try_lock()
		{
			// Check first so a failing try_lock does not take the cache line exclusively.
			return !mLocked.load(boost::memory_order_relaxed) &&
				   !mLocked.exchange(true, boost::memory_order_acquire);
		}

		void unlock()
		{
			mLocked.store(false, boost::memory_order_release);
		}

	private:
		boost::atomic<bool> mLocked;
	};

	//! CRecursiveSpinlock is a spinlock that the owning thread may lock
	//! multiple times; it must unlock it the same number of times.
	//!
	//! The lock is the CAS of mOwner from "no thread" to the caller's id.
	//! Only the owning thread ever stores its own id, so a thread can test
	//! "do I already own it?" with a relaxed load, and mRecursion is only ever
	//! touched by the owner (ordered by the acquire CAS / release store).
	//!
	//! Cost: one CAS to acquire, a plain store to release, no atomics at all
	//! for a re-entrant lock/unlock.
	class alignas(64) CRecursiveSpinlock : boost::noncopyable
	{
	public:
		typedef detail::ScopedLock<CRecursiveSpinlock> scoped_lock;

		CRecursiveSpinlock()
			: mOwner(boost::thread::id())
			, mRecursion(0)
		{
		}

		void lock()
		{
			const boost::thread::id tid = boost::this_thread::get_id();
			if (mOwner.load(boost::memory_order_relaxed) == tid)
			{
				++mRecursion;
				return;
			}

			detail::Backoff backoff;
			for (;;)
			{
				boost::thread::id unowned;
				if (mOwner.compare_exchange_weak(unowned, tid,
												 boost::memory_order_acquire,
												 boost::memory_order_relaxed))
				{
					break;
				}
				while (mOwner.load(boost::memory_order_relaxed) != boost::thread::id())
				{
					backoff.pause();
				}
			}
			mRecursion = 1;
		}

		bool try_lock()
		{
			const boost::thread::id tid = boost::this_thread::get_id();
			const boost::thread::id owner = mOwner.load(boost::memory_order_relaxed);
			if (owner == tid)
			{
				++mRecursion;
				return true;
			}
			if (owner != boost::thread::id())
			{
				return false;
			}
			boost::thread::id unowned;
			if (mOwner.compare_exchange_strong(unowned, tid,
											   boost::memory_order_acquire,
											   boost::memory_order_relaxed))
			{
				mRecursion = 1;
				return true;
			}
			return false;
		}

		void unlock()
		{
			// Unlocking a lock this thread does not hold would corrupt it for
			// everyone. The check is a relaxed load plus a compare, so it is
			// enforced in release builds too rather than only by assert.
			if (mOwner.load(boost::memory_order_relaxed) != boost::this_thread::get_id())
			{
				assert(!"CRecursiveSpinlock::unlock called by a thread that does not own it");
				std::abort();
			}
			if (--mRecursion == 0)
			{
				mOwner.store(boost::thread::id(), boost::memory_order_release);
			}
		}

	private:
		// Without BOOST_THREAD_PROVIDES_BASIC_THREAD_ID (older Boost / some
		// configs) thread::id wraps a shared_ptr and the atomic would be
		// implemented with a hidden internal lock.
		static_assert(boost::atomic<boost::thread::id>::is_always_lock_free,
					  "boost::atomic<boost::thread::id> must be lock-free");

		boost::atomic<boost::thread::id> mOwner; //!< Owning thread, or default id when free
		uint32_t mRecursion; //!< Times the owner has entered; only accessed by the owner
	};
} // namespace locks
