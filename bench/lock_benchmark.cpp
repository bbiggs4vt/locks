// Throughput benchmark: CSpinlock / CRecursiveSpinlock vs. regular mutexes.
//
// For every (lock type, thread count, critical-section size) combination,
// N threads each perform a fixed number of lock -> work -> unlock cycles on a
// single shared lock. The wall-clock time of the whole run is divided by the
// total number of acquisitions to give ns per acquisition (lower is better).
// Each configuration is repeated and the median is reported.
//
// Usage:
//   lock_benchmark [--ops N] [--reps N] [--threads 1,2,4,8] [--csv]

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

struct Candidate
{
	const char *name;
	bool recursive;
	std::function<double(unsigned, uint64_t, const Workload &)> run;
};

template <typename Lock>
Candidate make(const char *name, bool recursive)
{
	return Candidate{name, recursive, &runOnce<Lock>};
}

double median(std::vector<double> v)
{
	std::sort(v.begin(), v.end());
	const size_t n = v.size();
	return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
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
		else if (!std::strcmp(argv[i], "--csv"))
		{
			o.csv = true;
		}
		else
		{
			std::fprintf(stderr, "usage: %s [--ops N] [--reps N] [--threads 1,2,4,8] [--csv]\n", argv[0]);
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
		make<boost::mutex>("boost::mutex", false),
		make<std::mutex>("std::mutex", false),
		make<locks::CRecursiveSpinlock>("CRecursiveSpinlock", true),
		make<boost::recursive_mutex>("boost::recursive_mutex", true),
		make<std::recursive_mutex>("std::recursive_mutex", true),
	};

	const std::vector<Workload> workloads = {
		{"tiny", 0, false},       // just an increment: pure lock overhead
		{"medium", 100, false},   // ~100 dependent multiply-adds held under the lock
		{"nested", 0, true},      // lock taken twice per op (recursive locks only)
	};

	if (opt.csv)
	{
		std::printf("workload,lock,threads,ns_per_op,mops_per_sec\n");
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
			std::printf("\n== workload: %s ==\n%-24s", w.name, "lock \\ threads");
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
				std::printf("%-24s", c->name);
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
					std::printf("%s,%s,%u,%.2f,%.3f\n", w.name, c->name, t, ns, 1000.0 / ns);
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
	return EXIT_SUCCESS;
}
