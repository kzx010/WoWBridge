#include "Frame.h"
#include "Hang.h"

namespace Frame
{
	namespace
	{
		double                                   g_now = 0.0;
		std::uint64_t                            g_count = 0;
		struct Timed { double at; std::string name; Task task; };
		struct Poll { std::string name; std::function<bool()> task; };
		std::vector<Timed> g_timed;
		std::vector<Poll> g_polls;
	}

	double Now() { return g_now; }
	std::uint64_t Count() { return g_count; }

	void After(double a_delay, Task a_task) { After(a_delay, "task", std::move(a_task)); }
	void After(double a_delay, const char* a_name, Task a_task) { g_timed.push_back({g_now + a_delay, a_name ? a_name : "task", std::move(a_task)}); }
	void Until(std::function<bool()> a_task) { Until("poll", std::move(a_task)); }
	void Until(const char* a_name, std::function<bool()> a_task) { g_polls.push_back({a_name ? a_name : "poll", std::move(a_task)}); }
	void Checkpoint(const char* a_name) noexcept { Hang::Checkpoint(a_name); }

	void Tick(float a_delta)
	{
		Hang::Heartbeat();
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
		for (auto& [at, name, task] : timed) {
			if (at <= g_now) {
				Checkpoint(name.c_str());
				guarded([&] { task(); });
			} else {
				g_timed.push_back({at, std::move(name), std::move(task)});
			}
		}
		auto polls = std::move(g_polls);
		g_polls.clear();
		for (auto& poll : polls) {
			Checkpoint(poll.name.c_str());
			if (!guarded([&] { return poll.task(); })) {
				g_polls.push_back(std::move(poll));
			}
		}
		Checkpoint("frame");
	}
}
