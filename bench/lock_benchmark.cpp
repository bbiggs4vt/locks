// Throughput benchmark: CSpinlock / CRecursiveSpinlock vs. regular mutexes.
//
// For every (lock type, thread count, critical-section size) combination,
// N threads each perform a fixed number of lock -> work -> unlock cycles on a
// single shared lock. The wall-clock time of the whole run is divided by the
// total number of acquisitions to give ns per acquisition (lower is better).
// Each configuration is repeated and the median is reported.
//
// A second "fairness" phase runs every lock for a fixed duration with the
// tiny workload and counts how many acquisitions each thread got. It runs
// twice: with no gap between unlock and the next lock (the worst case, where
// the releasing thread can immediately re-acquire), and with a gap of ~100
// multiply-adds of thread-local work, closer to how real code uses a lock. A lock can
// post great throughput by letting one thread re-acquire it over and over
// while the others starve; the min/mean share exposes that (1.00 = perfectly
// even, 0.00 = some thread never got the lock).
//
// The previous spinlock implementation (bench/spinlock_v1.hpp) is included
// as "v1::..." for comparison, plus spin-policy variants of the current
// locks ("[hint]" / "[backoffN]") for tuning on a given target.
//
// Usage:
//   lock_benchmark [--ops N] [--reps N] [--threads 1,2,4,8] [--fairness-ms N] [--csv]

#include "spinlock_candidates.hpp"
#include "spinlock_v1.hpp"

#include <locks/spinlock.hpp>

#include <boost/thread/mutex.hpp>
#include <boost/thread/recursive_mutex.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct Options
{
	uint64_t opsPerThread = 200000;
	unsigned reps = 5;
	unsigned fairnessMs = 300; // 0 disables the fairness phase
	std::vector<unsigned> threads;
	bool csv = false;
};

// Amount of work done while holding the lock.
struct Workload
{
	const char *name;
	unsigned spinIters; // iterations of a tiny LCG inside the critical section
	bool nested;        // re-acquire the lock inside the critical section (recursive locks only)
};

// Shared state touched inside the critical section. Kept on its own cache line.
struct alignas(64) Shared
{
	uint64_t counter = 0;
	uint64_t value = 1;
};

inline void doWork(Shared &s, unsigned iters)
{
	++s.counter;
	uint64_t v = s.value;
	for (unsigned i = 0; i < iters; ++i)
	{
		v = v * 6364136223846793005ULL + 1442695040888963407ULL;
	}
	s.value = v;
}

template <typename Lock>
double runOnce(unsigned threads, uint64_t opsPerThread, const Workload &w)
{
	Lock lock;
	Shared shared;
	std::atomic<unsigned> ready{0};
	std::atomic<bool> go{false};

	auto body = [&] {
		ready.fetch_add(1);
		while (!go.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		for (uint64_t i = 0; i < opsPerThread; ++i)
		{
			lock.lock();
			if (w.nested)
			{
				lock.lock();
				doWork(shared, w.spinIters);
				lock.unlock();
			}
			else
			{
				doWork(shared, w.spinIters);
			}
			lock.unlock();
		}
	};

	std::vector<std::thread> pool;
	pool.reserve(threads);
	for (unsigned t = 0; t < threads; ++t)
	{
		pool.emplace_back(body);
	}
	while (ready.load() != threads)
	{
		std::this_thread::yield();
	}

	const auto start = std::chrono::steady_clock::now();
	go.store(true, std::memory_order_release);
	for (auto &th : pool)
	{
		th.join();
	}
	const auto end = std::chrono::steady_clock::now();

	const uint64_t total = uint64_t(threads) * opsPerThread;
	if (shared.counter != total)
	{
		std::fprintf(stderr, "ERROR: lost updates (%llu != %llu)\n",
					 (unsigned long long)shared.counter, (unsigned long long)total);
		std::exit(EXIT_FAILURE);
	}

	const double ns = std::chrono::duration<double, std::nano>(end - start).count();
	return ns / double(total);
}

std::atomic<uint64_t> gSink{0};

struct FairnessResult
{
	double mopsPerSec;
	double minShare; // min per-thread acquisitions / mean
	double maxShare; // max per-thread acquisitions / mean
};

// Run for a fixed duration; each thread counts its own acquisitions.
template <typename Lock>
FairnessResult runFairness(unsigned threads, unsigned ms, unsigned gapIters)
{
	Lock lock;
	Shared shared;
	std::atomic<unsigned> ready{0};
	std::atomic<bool> go{false};
	std::atomic<bool> stop{false};

	struct alignas(64) PerThread
	{
		uint64_t count = 0;
	};
	std::vector<PerThread> counts(threads);

	auto body = [&](unsigned idx) {
		ready.fetch_add(1);
		while (!go.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		uint64_t n = 0;
		uint64_t local = idx + 1;
		while (!stop.load(std::memory_order_relaxed))
		{
			lock.lock();
			doWork(shared, 0);
			lock.unlock();
			++n;
			// Thread-local work between acquisitions (outside the lock).
			for (unsigned i = 0; i < gapIters; ++i)
			{
				local = local * 6364136223846793005ULL + 1442695040888963407ULL;
			}
		}
		counts[idx].count = n;
		gSink.fetch_add(local, std::memory_order_relaxed); // keep the gap loop alive
	};

	std::vector<std::thread> pool;
	for (unsigned t = 0; t < threads; ++t)
	{
		pool.emplace_back(body, t);
	}
	while (ready.load() != threads)
	{
		std::this_thread::yield();
	}
	const auto start = std::chrono::steady_clock::now();
	go.store(true, std::memory_order_release);
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	stop.store(true);
	for (auto &th : pool)
	{
		th.join();
	}
	const auto end = std::chrono::steady_clock::now();

	uint64_t total = 0, mn = UINT64_MAX, mx = 0;
	for (const auto &c : counts)
	{
		total += c.count;
		mn = std::min(mn, c.count);
		mx = std::max(mx, c.count);
	}
	if (shared.counter != total)
	{
		std::fprintf(stderr, "ERROR: lost updates in fairness run\n");
		std::exit(EXIT_FAILURE);
	}
	const double mean = double(total) / threads;
	const double us = std::chrono::duration<double, std::micro>(end - start).count();
	return FairnessResult{double(total) / us, mean > 0 ? mn / mean : 0.0, mean > 0 ? mx / mean : 0.0};
}

struct Candidate
{
	const char *name;
	bool recursive;
	std::function<double(unsigned, uint64_t, const Workload &)> run;
	std::function<FairnessResult(unsigned, unsigned, unsigned)> fairness;
};

template <typename Lock>
Candidate make(const char *name, bool recursive)
{
	return Candidate{name, recursive, &runOnce<Lock>, &runFairness<Lock>};
}

double median(std::vector<double> v)
{
	std::sort(v.begin(), v.end());
	const size_t n = v.size();
	return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

void printBuildInfo()
{
#if defined(__aarch64__)
	const char *arch = "aarch64";
#elif defined(__arm__)
	const char *arch = "arm32";
#elif defined(__x86_64__)
	const char *arch = "x86_64";
#elif defined(__i386__)
	const char *arch = "x86";
#else
	const char *arch = "unknown";
#endif
#if defined(__ARM_FEATURE_ATOMICS)
	const char *atomics = "LSE (inline swp/cas)";
#elif defined(__aarch64__)
	const char *atomics = "no LSE at compile time (LL/SC loops, or outline-atomics calls if enabled)";
#elif defined(__arm__)
	const char *atomics = "LL/SC (ldrex/strex)";
#else
	const char *atomics = "native";
#endif
#if defined(__VERSION__)
	const char *compiler = __VERSION__;
#else
	const char *compiler = "unknown";
#endif
#if defined(__ARM_ARCH)
	const int armArch = __ARM_ARCH;
#else
	const int armArch = 0;
#endif
	std::printf("# build: arch=%s arm_arch=%d compiler=\"%s\" atomics=%s default_hint=%s max_backoff=%u\n",
				arch, armArch, compiler, atomics, locks::relax::Default::name(), unsigned(LOCKS_SPIN_MAX_BACKOFF));
}

std::vector<unsigned> parseList(const char *s)
{
	std::vector<unsigned> out;
	std::string str(s);
	size_t pos = 0;
	while (pos < str.size())
	{
		size_t comma = str.find(',', pos);
		if (comma == std::string::npos)
		{
			comma = str.size();
		}
		out.push_back(unsigned(std::stoul(str.substr(pos, comma - pos))));
		pos = comma + 1;
	}
	return out;
}

Options parseArgs(int argc, char **argv)
{
	Options o;
	for (int i = 1; i < argc; ++i)
	{
		if (!std::strcmp(argv[i], "--ops") && i + 1 < argc)
		{
			o.opsPerThread = std::strtoull(argv[++i], nullptr, 10);
		}
		else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc)
		{
			o.reps = unsigned(std::strtoul(argv[++i], nullptr, 10));
		}
		else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc)
		{
			o.threads = parseList(argv[++i]);
		}
		else if (!std::strcmp(argv[i], "--fairness-ms") && i + 1 < argc)
		{
			o.fairnessMs = unsigned(std::strtoul(argv[++i], nullptr, 10));
		}
		else if (!std::strcmp(argv[i], "--csv"))
		{
			o.csv = true;
		}
		else
		{
			std::fprintf(stderr, "usage: %s [--ops N] [--reps N] [--threads 1,2,4,8] [--fairness-ms N] [--csv]\n", argv[0]);
			std::exit(EXIT_FAILURE);
		}
	}
	if (o.threads.empty())
	{
		// 1 (uncontended), powers of two up to the core count, then 2x
		// oversubscription, which is where spinlocks typically suffer.
		const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
		for (unsigned t = 1; t < hw; t *= 2)
		{
			o.threads.push_back(t);
		}
		o.threads.push_back(hw);
		o.threads.push_back(hw * 2);
	}
	return o;
}
} // namespace

int main(int argc, char **argv)
{
	const Options opt = parseArgs(argc, argv);

	const std::vector<Candidate> candidates = {
		make<locks::CSpinlock>("CSpinlock", false),
#if defined(__aarch64__)
		make<locks::BasicSpinlock<locks::SpinPolicy<locks::relax::Yield> > >("CSpinlock[yield]", false),
#elif defined(__arm__)
		make<locks::BasicSpinlock<locks::SpinPolicy<locks::relax::Isb> > >("CSpinlock[isb]", false),
#endif
		make<locks::BasicSpinlock<locks::SpinPolicy<locks::relax::Default, 1024> > >("CSpinlock[backoff1024]", false),
		make<locks::candidates::NoBargeSpinlock<> >("CSpinlock[nobarge]", false),
		make<locks::candidates::NoBargeSpinlock<locks::SpinPolicy<>, 32> >("CSpinlock[nobarge32]", false),
		make<locks::candidates::TicketSpinlock<> >("TicketSpinlock", false),
		make<locks::v1::CSpinlock>("v1::CSpinlock", false),
		make<boost::mutex>("boost::mutex", false),
		make<std::mutex>("std::mutex", false),
		make<locks::CRecursiveSpinlock>("CRecursiveSpinlock", true),
#if defined(__aarch64__)
		make<locks::BasicRecursiveSpinlock<locks::SpinPolicy<locks::relax::Yield> > >("CRecursiveSpinlock[yield]", true),
#elif defined(__arm__)
		make<locks::BasicRecursiveSpinlock<locks::SpinPolicy<locks::relax::Isb> > >("CRecursiveSpinlock[isb]", true),
#endif
		make<locks::BasicRecursiveSpinlock<locks::SpinPolicy<locks::relax::Default, 1024> > >("CRecursiveSpinlock[backoff1024]", true),
		make<locks::v1::CRecursiveSpinlock>("v1::CRecursiveSpinlock", true),
		make<boost::recursive_mutex>("boost::recursive_mutex", true),
		make<std::recursive_mutex>("std::recursive_mutex", true),
	};

	const std::vector<Workload> workloads = {
		{"tiny", 0, false},       // just an increment: pure lock overhead
		{"medium", 100, false},   // ~100 dependent multiply-adds held under the lock
		{"nested", 0, true},      // lock taken twice per op (recursive locks only)
	};

	printBuildInfo();
	if (opt.csv)
	{
		std::printf("kind,workload,lock,threads,ns_per_op,mops_per_sec,min_share,max_share\n");
	}
	else
	{
		std::printf("ops/thread=%llu reps=%u hardware_concurrency=%u (median ns per acquisition, lower is better)\n",
					(unsigned long long)opt.opsPerThread, opt.reps, std::thread::hardware_concurrency());
	}

	for (const auto &w : workloads)
	{
		std::vector<const Candidate *> active;
		for (const auto &c : candidates)
		{
			if (!w.nested || c.recursive)
			{
				active.push_back(&c);
			}
		}

		if (!opt.csv)
		{
			std::printf("\n== workload: %s ==\n%-32s", w.name, "lock \\ threads");
			for (unsigned t : opt.threads)
			{
				std::printf("%10u", t);
			}
			std::printf("\n");
		}

		for (const Candidate *c : active)
		{
			if (!opt.csv)
			{
				std::printf("%-32s", c->name);
				std::fflush(stdout);
			}
			for (unsigned t : opt.threads)
			{
				// Warm-up run, then measured reps.
				c->run(t, std::max<uint64_t>(1, opt.opsPerThread / 10), w);
				std::vector<double> samples;
				for (unsigned r = 0; r < opt.reps; ++r)
				{
					samples.push_back(c->run(t, opt.opsPerThread, w));
				}
				const double ns = median(samples);
				if (opt.csv)
				{
					std::printf("throughput,%s,%s,%u,%.2f,%.3f,,\n", w.name, c->name, t, ns, 1000.0 / ns);
				}
				else
				{
					std::printf("%10.1f", ns);
					std::fflush(stdout);
				}
			}
			if (!opt.csv)
			{
				std::printf("\n");
			}
		}
	}

	if (opt.fairnessMs > 0)
	{
		// Only contended thread counts are interesting here.
		std::vector<unsigned> fthreads;
		for (unsigned t : opt.threads)
		{
			if (t > 1)
			{
				fthreads.push_back(t);
			}
		}
		const unsigned gaps[] = {0, 100};
		for (unsigned gap : gaps)
		{
			if (!opt.csv)
			{
				std::printf("\n== fairness: tiny workload, gap=%u, %u ms per run ==\n"
							"cells: Mops/s total | min/mean per-thread share (1.00 = even)\n%-32s",
							gap, opt.fairnessMs, "lock \\ threads");
				for (unsigned t : fthreads)
				{
					std::printf("%16u", t);
				}
				std::printf("\n");
			}
			for (const auto &c : candidates)
			{
				if (!opt.csv)
				{
					std::printf("%-32s", c.name);
					std::fflush(stdout);
				}
				for (unsigned t : fthreads)
				{
					const FairnessResult r = c.fairness(t, opt.fairnessMs, gap);
					if (opt.csv)
					{
						std::printf("fairness,tiny+gap%u,%s,%u,%.2f,%.3f,%.3f,%.3f\n", gap, c.name, t,
									1000.0 / r.mopsPerSec, r.mopsPerSec, r.minShare, r.maxShare);
					}
					else
					{
						std::printf("%9.1f | %4.2f", r.mopsPerSec, r.minShare);
						std::fflush(stdout);
					}
				}
				if (!opt.csv)
				{
					std::printf("\n");
				}
			}
		}
	}
	return EXIT_SUCCESS;
}
