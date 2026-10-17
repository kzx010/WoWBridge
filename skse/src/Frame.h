#pragma once

// Per-frame dispatcher driven by PlayerCharacter::Update (main thread, only while a game is loaded).
namespace Frame
{
	using Task = std::function<void()>;

	void Tick(float a_delta);

	// Seconds of unpaused game play since the first tick (the single gameplay clock).
	double Now();
	std::uint64_t Count();

	// Run a_task on the main thread once Now() >= current + a_delay.
	void After(double a_delay, Task a_task);
	void After(double a_delay, const char* a_name, Task a_task);
	// Run a_task every frame until it returns true.
	void Until(std::function<bool()> a_task);
	void Until(const char* a_name, std::function<bool()> a_task);
	// Main-thread diagnostic label, copied immediately (temporary strings are safe). Does not reset the heartbeat.
	void Checkpoint(const char* a_name) noexcept;
}
