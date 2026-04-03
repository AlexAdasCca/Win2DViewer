#pragma once

#include <functional>
#include <chrono>
#include <future>
#include <cstdio>

class CallBackTimer
{
public:
	CallBackTimer()
		:executeFlag(false)
	{}

	~CallBackTimer() {
		if (executeFlag.load(std::memory_order_acquire)) {
			stop();
		};
	}

	void stop()
	{
		executeFlag.store(false, std::memory_order_release);

		if (workerThread.joinable())
			workerThread.join();
	}

	void start(double interval, std::function<bool(void)> func)
	{
		if (executeFlag.load(std::memory_order_acquire)) {
			stop();
		};
		executeFlag.store(true, std::memory_order_release);
		workerThread = std::thread([this, interval, func]()
		{
			while (executeFlag.load(std::memory_order_acquire)) {
				if (!func())
				{
					executeFlag.store(false, std::memory_order_release);
					break;
				}
				std::this_thread::sleep_for(
					std::chrono::nanoseconds((int)(interval * 1000000.0)));
			}
		});
	}

	bool IsRunning() const noexcept {
		return (executeFlag.load(std::memory_order_acquire) &&
			workerThread.joinable());
	}

private:
	std::atomic<bool> executeFlag;
	std::thread workerThread;
};
