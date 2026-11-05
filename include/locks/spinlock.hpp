#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>

#include <boost/atomic.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread/thread.hpp>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define LOCKS_ARCH_X86 1
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define LOCKS_ARCH_AARCH64 1
#elif defined(__arm__) || defined(_M_ARM)
#define LOCKS_ARCH_ARM32 1
#endif

//! Upper bound of the exponential spin backoff, in relax() calls per round,
//! before a waiter falls back to yielding its time slice (a syscall).
//! Override with -DLOCKS_SPIN_MAX_BACKOFF=N to tune for a target.
#ifndef LOCKS_SPIN_MAX_BACKOFF
#define LOCKS_SPIN_MAX_BACKOFF 64
#endif

namespace locks
{
	//! Spin-wait hints: what a waiter executes between checks of the lock.
	namespace relax
	{
		//! x86 PAUSE. Saves power, frees resources for a sibling hyper-thread
		//! and avoids the memory-order mis-speculation penalty on loop exit.
		//! Elsewhere falls back to a compiler barrier.
		struct Pause
		{
			static const char *name() { return "pause"; }
			static void relax()
			{
#if defined(LOCKS_ARCH_X86)
				_mm_pause();
#else
				__asm__ __volatile__("" ::: "memory");
#endif
			}
		};

		//! ARM YIELD. Architecturally a hint for SMT cores; most Cortex-A
		//! cores execute it as a NOP, so it adds almost no delay.
		struct Yield
		{
			static const char *name() { return "yield"; }
			static void relax()
			{
#if defined(LOCKS_ARCH_AARCH64) || defined(LOCKS_ARCH_ARM32)
				__asm__ __volatile__("yield" ::: "memory");
#else
				__asm__ __volatile__("" ::: "memory");
#endif
			}
		};

		//! AArch64 ISB SY. Flushes the pipeline, so unlike YIELD it actually
		//! stalls the core for a short, roughly fixed time (Rust's
		//! core::hint::spin_loop uses this on AArch64).
		struct Isb
		{
			static const char *name() { return "isb"; }
			static void relax()
			{
#if defined(LOCKS_ARCH_AARCH64)
				__asm__ __volatile__("isb sy" ::: "memory");
#elif defined(LOCKS_ARCH_ARM32)
				__asm__ __volatile__("yield" ::: "memory");
#else
				__asm__ __volatile__("" ::: "memory");
#endif
			}
		};

		//! Best hint for the architecture being compiled for.
#if defined(LOCKS_ARCH_AARCH64)
		typedef Isb Default;
#elif defined(LOCKS_ARCH_ARM32)
		typedef Yield Default;
#else
		typedef Pause Default;
#endif
	} // namespace relax

	//! Spin-wait policy: which hint to execute and how far to back off
	//! before yielding the time slice.
	template <typename Relax = relax::Default, uint32_t MaxBackoff = LOCKS_SPIN_MAX_BACKOFF>
	struct SpinPolicy
	{
		typedef Relax relax_type;
		static const uint32_t kMaxBackoff = MaxBackoff;
	};

	namespace detail
	{
		//! Exponential backoff with Policy::relax_type::relax() for a bounded
		//! number of rounds, then fall back to yielding the time slice.
		//! Yielding is a syscall, so it is only used once spinning has failed
		//! for a while (the holder is probably descheduled).
		template <typename Policy>
		class Backoff
		{
		public:
			void pause()
			{
				if (mSpins <= Policy::kMaxBackoff)
				{
					for (uint32_t i = 0; i < mSpins; ++i)
					{
						Policy::relax_type::relax();
					}
					mSpins <<= 1;
				}
				else
				{
					boost::this_thread::yield();
				}
			}

		private:
			uint32_t mSpins = 1;
		};

		//! Generic RAII guard used as Lock::scoped_lock.
		//! It tracks ownership, so an early unlock() is not repeated by the
		//! destructor.
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

	//! BasicSpinlock is a lightweight, non-recursive mutex.
	//! Use the CSpinlock typedef unless you are tuning the spin policy.
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
	template <typename Policy = SpinPolicy<> >
	class alignas(64) BasicSpinlock : boost::noncopyable
	{
	public:
		typedef detail::ScopedLock<BasicSpinlock> scoped_lock;

		BasicSpinlock()
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

			detail::Backoff<Policy> backoff;
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

	//! BasicRecursiveSpinlock is a spinlock that the owning thread may lock
	//! multiple times; it must unlock it the same number of times.
	//! Use the CRecursiveSpinlock typedef unless you are tuning the spin policy.
	//!
	//! The lock is the CAS of mOwner from "no thread" to the caller's id.
	//! Only the owning thread ever stores its own id, so a thread can test
	//! "do I already own it?" with a relaxed load, and mRecursion is only ever
	//! touched by the owner (ordered by the acquire CAS / release store).
	//!
	//! Cost: one CAS to acquire, a plain store to release, no atomics at all
	//! for a re-entrant lock/unlock.
	template <typename Policy = SpinPolicy<> >
	class alignas(64) BasicRecursiveSpinlock : boost::noncopyable
	{
	public:
		typedef detail::ScopedLock<BasicRecursiveSpinlock> scoped_lock;

		BasicRecursiveSpinlock()
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

			detail::Backoff<Policy> backoff;
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

	//! The spinlocks with the default policy for the target architecture.
	typedef BasicSpinlock<> CSpinlock;
	typedef BasicRecursiveSpinlock<> CRecursiveSpinlock;
} // namespace locks
