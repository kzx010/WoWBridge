#pragma once

// ContainerMenu stays as an invisible lifecycle/crime backend. Its living-actor modes stay native.
namespace Loot
{
	void Init();
	void Reset();
	void Tick();
	void Take(std::uint32_t a_session, std::string a_key, int a_count);
	void Deposit(std::uint32_t a_session, std::string a_key, int a_count);
	void Close(std::uint32_t a_session);
}
