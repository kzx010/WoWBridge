#include "Service.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

namespace Service
{
	namespace
	{
		std::mutex                    g_lock;
		std::condition_variable       g_wake;
		std::deque<json>              g_outbox;                 // requests waiting to be sent
		std::vector<std::pair<std::uint64_t, json>> g_inbox;    // replies waiting for the main thread
		std::map<std::uint64_t, Reply> g_pending;               // main thread only
		std::uint64_t                 g_nextId = 1;             // main thread only
		std::atomic<bool>             g_connected = false;
		int                           g_port = 7878;

		void Fail(json a_request)
		{
			std::lock_guard lock(g_lock);
			g_inbox.emplace_back(a_request.value("id", std::uint64_t{ 0 }), json{ { "ok", false }, { "error", "SERVICE_OFFLINE" } });
		}

		SOCKET Connect()
		{
			SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (s == INVALID_SOCKET) {
				return s;
			}
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(static_cast<u_short>(g_port));
			inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
			if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
				::closesocket(s);
				return INVALID_SOCKET;
			}
			const DWORD timeout = 2000;   // a reply line takes milliseconds; two seconds means the service is gone
			::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
			return s;
		}

		bool SendLine(SOCKET a_s, const std::string& a_line)
		{
			for (std::size_t sent = 0; sent < a_line.size();) {
				const int n = ::send(a_s, a_line.data() + sent, static_cast<int>(a_line.size() - sent), 0);
				if (n <= 0) {
					return false;
				}
				sent += static_cast<std::size_t>(n);
			}
			return true;
		}

		bool ReadLine(SOCKET a_s, std::string& a_buffer, std::string& a_line)
		{
			for (;;) {
				if (const auto nl = a_buffer.find('\n'); nl != std::string::npos) {
					a_line = a_buffer.substr(0, nl);
					a_buffer.erase(0, nl + 1);
					return true;
				}
				char chunk[4096];
				const int n = ::recv(a_s, chunk, sizeof(chunk), 0);
				if (n <= 0) {
					return false;
				}
				a_buffer.append(chunk, static_cast<std::size_t>(n));
			}
		}

		// One request in flight at a time: the protocol is one line in, one line out, and casts are rare events.
		void Run()
		{
			WSADATA wsa;
			::WSAStartup(MAKEWORD(2, 2), &wsa);
			SOCKET      s = INVALID_SOCKET;
			std::string buffer;
			for (;;) {
				json request;
				{
					std::unique_lock lock(g_lock);
					g_wake.wait_for(lock, std::chrono::seconds(2), [] { return !g_outbox.empty(); });
					if (g_outbox.empty()) {
						continue;
					}
					request = std::move(g_outbox.front());
					g_outbox.pop_front();
				}
				if (s == INVALID_SOCKET) {
					s = Connect();
					buffer.clear();
					if (s != INVALID_SOCKET) {
						logger::info("service: connected on 127.0.0.1:{}", g_port);
					}
				}
				std::string line;
				const bool ok = s != INVALID_SOCKET && SendLine(s, request.dump() + "\n") && ReadLine(s, buffer, line);
				g_connected = ok;
				if (!ok) {
					if (s != INVALID_SOCKET) {
						::closesocket(s);
						s = INVALID_SOCKET;
						logger::warn("service: connection lost");
					}
					Fail(std::move(request));
					continue;
				}
				json reply = json::parse(line, nullptr, false);
				if (reply.is_discarded()) {
					reply = json{ { "ok", false }, { "error", "BAD_REPLY" } };
				}
				std::lock_guard lock(g_lock);
				g_inbox.emplace_back(request.value("id", std::uint64_t{ 0 }), std::move(reply));
			}
		}
	}

	void Start(int a_port)
	{
		g_port = a_port;
		std::thread(Run).detach();
	}

	void Request(json a_request, Reply a_onReply)
	{
		const auto id = g_nextId++;
		a_request["id"] = id;
		g_pending.emplace(id, std::move(a_onReply));
		{
			std::lock_guard lock(g_lock);
			g_outbox.push_back(std::move(a_request));
		}
		g_wake.notify_one();
	}

	void Pump()
	{
		std::vector<std::pair<std::uint64_t, json>> replies;
		{
			std::lock_guard lock(g_lock);
			replies.swap(g_inbox);
		}
		for (auto& [id, reply] : replies) {
			if (auto it = g_pending.find(id); it != g_pending.end()) {
				auto fn = std::move(it->second);
				g_pending.erase(it);
				fn(reply);
			}
		}
	}

	bool Connected() { return g_connected; }
}
