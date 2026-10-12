#pragma once

// Hidden Skyrim TrainingMenu backend, published and executed only on the main thread.
namespace Trainers
{
    void Init();
    void Tick();
    void Reset();
    void Command(std::string a_action, std::uint32_t a_session, int a_current, int a_cost, int a_trained);
}
