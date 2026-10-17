#pragma once

// Process-lifetime main-thread watchdog. Start on kDataLoaded, after the frame hook is installed.
namespace Hang
{
	void Start() noexcept;
	void Heartbeat() noexcept;
	void Checkpoint(const char* a_name) noexcept;
}
