#pragma once

// Previous implementation (as originally provided, plus the race fix on the
// uncontended CRecursiveSpinlock path). Kept only so lock_benchmark can
// compare it against include/locks/spinlock.hpp. Do not use in new code.

#include <cassert>
#include <cstdint>

#include <boost/atomic.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread/thread.hpp>

namespace locks { namespace v1
{
	// Spin Lock skeleton taken from the boost::thread
	// examples
	//! CSpinlock is a lightweight mutex
	class CSpinlock : boost::noncopyable
	{
	private:
		//! Internal state
		enum LockState
		{
			Locked, //!< Lock is in use
			Unlocked, //!< Lock is available
		};
		boost::atomic<LockState> mState;

	public:
		//! RAII lock object.  Used like
		//! boost::mutex::scoped_lock
		struct scoped_lock : boost::noncopyable
		{
			CSpinlock &mSpinlock;

			scoped_lock(CSpinlock &spinlock)
				: mSpinlock(spinlock)
			{
				lock();
			}

			~scoped_lock()
			{
				unlock();
			}

			void lock()
			{
				mSpinlock.lock();
			}

			void unlock()
			{
				mSpinlock.unlock();
			}
		};

		CSpinlock()
			: mState(Unlocked)
		{
		}

		void lock()
		{
			while (mState.exchange(Locked, boost::memory_order_acquire) == Locked)
			{
				/* busy-wait */
				boost::this_thread::yield();
			}
		}
		void unlock()
		{
			mState.store(Unlocked, boost::memory_order_release);
		}
	};

	// Based on https://preshing.com/20120305/implementing-a-recursive-mutex/
	class CRecursiveSpinlock
	{
	private:
		//! Internal state
		const boost::thread::id mUnownedId; // The default constructor for ID is the invalid value given to no thread
		boost::atomic<boost::thread::id> mOwner; // ID of the thread in the lock
		boost::atomic<uint32_t> mCounter; // A combination of number of threads trying to own the lock plus the owner's recursion
		uint32_t mRecursion; // The number of times the owning thread has entered the lock

	public:
		//! RAII lock object.  Used like
		//! boost::mutex::scoped_lock
		struct scoped_lock
		{
			CRecursiveSpinlock &mSpinlock;

			scoped_lock(CRecursiveSpinlock &spinlock)
				: mSpinlock(spinlock)
			{
				lock();
			}

			~scoped_lock()
			{
				unlock();
			}

			void lock()
			{
				mSpinlock.lock();
			}

			void unlock()
			{
				mSpinlock.unlock();
			}

		private:
			scoped_lock & operator=(const scoped_lock &) = delete;
			scoped_lock(const scoped_lock &) = delete;
		};

		CRecursiveSpinlock()
			: mCounter(0)
			, mRecursion(0)
		{
		}

		void lock()
		{
			const auto tid = boost::this_thread::get_id();

			// Counter is a combination of the
			// recursion level and the number of
			// threads attempting to get the lock
			if (++mCounter > 1)
			{
				if (tid != mOwner)
				{
					// A thread wants to own the already locked lock
					// Only leave the loop when the lock is owned by this thread
					// Note that on a failed exchange
					// unowned is changed to the current mOwner
					auto unowned = mUnownedId;
					while (!mOwner.compare_exchange_strong(unowned, tid))
					{
						// Reset unowned for the next attempt
						unowned = mUnownedId;
						// Give up the rest of the quantum (maybe)
						boost::this_thread::yield();
					}
					// This thread is the new owner of the lock
				}
				else
				{
					// This thread already owns the lock
					// so continue on
				}
				assert(boost::this_thread::get_id() == mOwner);
				++mRecursion;
				return;
			}

			// The lock looked free, but ownership must still be claimed
			// with a CAS. A plain `mOwner = tid` here races with a
			// contender that incremented mCounter just after us: it sees
			// mOwner still unowned, wins its CAS, and then both threads
			// believe they hold the lock.
			auto unowned = mUnownedId;
			while (!mOwner.compare_exchange_strong(unowned, tid))
			{
				unowned = mUnownedId;
				boost::this_thread::yield();
			}
			++mRecursion;
		}
		void unlock()
		{
			// It's an error to have a different thread release the lock
			assert(boost::this_thread::get_id() == mOwner);
			// Update the accounting
			// If the recursion level drops to 0
			// this thread is releasing the lock
			uint32_t recur = --mRecursion;
			if (recur == 0)
			{
				mOwner = mUnownedId;
			}

			--mCounter;
		}

	private:
		CRecursiveSpinlock & operator=(const CRecursiveSpinlock &) = delete;
		CRecursiveSpinlock(const CRecursiveSpinlock &) = delete;
	};
} } // namespace locks::v1
