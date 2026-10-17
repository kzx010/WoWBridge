#pragma once

#include <nlohmann/json.hpp>

// The gameplay service (service/server.py: CMaNGOS data and formulas) over loopback TCP, newline-delimited JSON.
// Requests never block the game: a background thread does the I/O and replies are handed back on the main thread
// (Pump, from the frame tick). While the service is down every request is answered {"ok": false, "error":
// "SERVICE_OFFLINE"}; the thread keeps trying to connect.
namespace Service
{
	using json = nlohmann::json;
	using Reply = std::function<void(const json&)>;

	void Start(int a_port = 7878);
	void Request(json a_request, Reply a_onReply);  // main thread
	void Pump();                                     // main thread: deliver replies
	bool Connected();
	bool CallbackIsolationForTesting(); // explicit-only: throwing replies followed by a good reply in one Pump batch
}
