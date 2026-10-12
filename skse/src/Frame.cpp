#include "Frame.h"

namespace Frame
{
	namespace
	{
		double                                   g_now = 0.0;
		std::uint64_t                            g_count = 0;
		std::vector<std::pair<double, Task>>     g_timed;
		std::vector<std::function<bool()>>       g_polls;
	}

	double Now() { return g_now; }
	std::uint64_t Count() { return g_count; }

	void After(double a_delay, Task a_task) { g_timed.emplace_back(g_now + a_delay, std::move(a_task)); }
	void Until(std::function<bool()> a_task) { g_polls.push_back(std::move(a_task)); }

	void Tick(float a_delta)
	{
		// The engine delta already excludes menu pauses; clamp hitches so timers cannot jump.
		g_now += std::clamp(static_cast<double>(a_delta), 0.0, 0.25);
		++g_count;

		auto timed = std::move(g_timed);
		g_timed.clear();
		auto guarded = [](auto&& a_fn) {
			try {
				return a_fn();
			} catch (const std::exception& e) {
				logger::error("frame task threw: {}", e.what());  // never let an exception unwind into the engine
				return decltype(a_fn())();
			}
		};
		for (auto& [at, task] : timed) {
			if (at <= g_now) {
				guarded([&] { task(); });
			} else {
				g_timed.emplace_back(at, std::move(task));
			}
		}
		auto polls = std::move(g_polls);
		g_polls.clear();
		for (auto& poll : polls) {
			if (!guarded([&] { return poll(); })) {
				g_polls.push_back(std::move(poll));
			}
		}
	}
}
