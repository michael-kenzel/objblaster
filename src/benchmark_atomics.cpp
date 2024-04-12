#include <atomic>
#include <thread>
#include <latch>
#include <chrono>
#include <numeric>
#include <iostream>
#include <iomanip>

using namespace std::literals;


std::atomic<unsigned int> a;

void test(double& ops_per_sec, int i, std::latch& start)
{
	start.arrive_and_wait();

	long long ops = 0;

	auto t_start = std::chrono::steady_clock::now();

	while (std::chrono::steady_clock::now() < t_start + 2s)
	{
		for (int i = 0; i < 4096; ++i)
		{
			a.fetch_or(i, std::memory_order::relaxed);
			++ops;
		}
	}

	auto t = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_start).count();

	ops_per_sec = ops * 1000000.0 / t;
}

int main()
{
	constexpr int max_threads = 16;
	std::thread threads[max_threads - 1];
	double ops_per_sec[max_threads];

	std::cout << "threads        Mops/s\n";

	for (int N = 1; N < max_threads; ++N)
	{
		std::cout << std::setw(7) << N << ": ";

		std::latch start(N);

		for (int i = 1; i < N; ++i)
			threads[i] = std::thread(test, std::ref(ops_per_sec[i]), i, std::ref(start));

		test(ops_per_sec[0], 0, start);

		for (int i = 1; i < N; ++i)
			threads[i].join();

		for (int i = 0; i < N; ++i)
			std::cout << " " << std::fixed << std::setprecision(1) << std::setw(5) << ops_per_sec[i] * 1e-6;
		std::cout << " = " << std::reduce(std::begin(ops_per_sec), std::begin(ops_per_sec) + N) * 1e-6
		          << '\n';
	}
}
