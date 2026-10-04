// Loopback TCP listener that speaks LegoToypad's wire protocol and drives
// RPCS3's own toypad object through the resolved load_figure / remove_figure.
//
// This is the exact protocol the Cemu/RPCS3/shadPS4 "seamless" builds expose,
// so the stock LegoToypad app talks to it unchanged. Minimal by design: no
// controller input muting and no live LED mirror (GET_LED answers a static
// "all off" snapshot so a polling client never stalls). MOVE is served from a
// per-slot tag cache this hook fills on LOAD, so move_figure is never needed.
#include "listener.h"
#include "rpcs3_toypad.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace
{
	constexpr size_t TAG_BYTES = 0x2D * 0x04; // 180

	ToypadApi g_api;
	void (*g_log)(const char*) = nullptr;
	std::atomic<bool> g_running{false};
	std::atomic<SOCKET> g_listen{INVALID_SOCKET};
	std::thread g_thread;

	std::mutex g_cache_mutex;
	struct Slot { bool present = false; std::array<uint8_t, TAG_BYTES> tag{}; };
	std::array<Slot, 7> g_slots;

	void logf(const char* fmt, ...)
	{
		if (!g_log) return;
		char buf[256];
		va_list ap; va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		g_log(buf);
	}

	bool recv_all(SOCKET s, uint8_t* p, size_t n)
	{
		while (n)
		{
			int r = ::recv(s, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
			if (r <= 0) return false;
			p += r; n -= static_cast<size_t>(r);
		}
		return true;
	}

	bool send_all(SOCKET s, const uint8_t* p, size_t n)
	{
		while (n)
		{
			int r = ::send(s, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
			if (r <= 0) return false;
			p += r; n -= static_cast<size_t>(r);
		}
		return true;
	}

	// An empty fs::file (null inner pointer). Oversized and zeroed so whatever
	// the callee moves out of it is a no-op; the real object is a single
	// pointer, we just give it room and never read it back.
	struct EmptyFile { alignas(16) uint8_t storage[32]; };

	void do_load(uint8_t pad, uint8_t index, const std::array<uint8_t, TAG_BYTES>& tag)
	{
		EmptyFile f{}; std::memset(f.storage, 0, sizeof(f.storage));
		g_api.remove(g_api.self, pad, index, /*full_remove*/1, /*lock*/1); // overwrite semantics
		std::memset(f.storage, 0, sizeof(f.storage));
		uint32_t id = g_api.load(g_api.self, tag.data(), &f, pad, index, /*lock*/1);
		logf("  -> load_figure returned id=0x%x (0 usually means an unreadable/blank tag)", id);
	}

	void handle_client(SOCKET c)
	{
		uint8_t h[5];
		if (!recv_all(c, h, sizeof(h))) { logf("client connected but sent no header"); return; }
		const uint8_t cmd = h[0], pad = h[1], index = h[2];
		logf("client msg cmd=0x%02x pad=%u index=%u", cmd, pad, index);

		if (cmd != 0x04 && (pad < 1 || pad > 3 || index >= 7))
		{
			logf("reject: cmd=0x%02x pad=%u index=%u", cmd, pad, index);
			return;
		}

		switch (cmd)
		{
		case 0x01: // LOAD: 180 tag bytes + u16 path_len + path (path ignored: in-memory session only)
		{
			std::array<uint8_t, TAG_BYTES> tag{};
			if (!recv_all(c, tag.data(), tag.size())) return;
			uint8_t lb[2];
			if (!recv_all(c, lb, 2)) return;
			uint16_t path_len = static_cast<uint16_t>(lb[0] | (lb[1] << 8));
			for (uint16_t left = path_len; left;)
			{
				uint8_t junk[256];
				uint16_t chunk = left < sizeof(junk) ? left : static_cast<uint16_t>(sizeof(junk));
				if (!recv_all(c, junk, chunk)) return;
				left -= chunk;
			}
			do_load(pad, index, tag);
			{ std::lock_guard<std::mutex> lk(g_cache_mutex); g_slots[index].present = true; g_slots[index].tag = tag; }
			logf("LOAD  pad=%u index=%u (path %u bytes ignored)", pad, index, path_len);
			break;
		}
		case 0x02: // REMOVE
		{
			g_api.remove(g_api.self, pad, index, 1, 1);
			{ std::lock_guard<std::mutex> lk(g_cache_mutex); g_slots[index].present = false; }
			logf("REMOVE pad=%u index=%u", pad, index);
			break;
		}
		case 0x03: // MOVE (served from the tag cache)
		{
			const uint8_t old_pad = h[3], old_index = h[4];
			if (old_pad < 1 || old_pad > 3 || old_index >= 7) { logf("reject MOVE src pad=%u index=%u", old_pad, old_index); return; }

			std::array<uint8_t, TAG_BYTES> tag{};
			bool have;
			{ std::lock_guard<std::mutex> lk(g_cache_mutex); have = g_slots[old_index].present; if (have) tag = g_slots[old_index].tag; }
			if (!have) { logf("MOVE ignored: slot %u not loaded by this hook", old_index); return; }

			g_api.remove(g_api.self, old_pad, old_index, 1, 1);
			do_load(pad, index, tag);
			{
				std::lock_guard<std::mutex> lk(g_cache_mutex);
				g_slots[old_index].present = false;
				g_slots[index].present = true; g_slots[index].tag = tag;
			}
			logf("MOVE  %u/%u -> %u/%u", old_pad, old_index, pad, index);
			break;
		}
		case 0x04: // GET_LED: static all-off v2 snapshot (40 bytes) so LED-polling clients don't stall
		{
			uint8_t r[4 + 3 * 12] = {};
			r[0] = 0x4C; r[1] = 0x00; r[2] = 0x02; r[3] = 0x03; // 'L', serial, version 2, 3 regions
			const uint8_t region_pad[3] = {1, 2, 3};
			for (int i = 0; i < 3; ++i) r[4 + i * 12 + 0] = region_pad[i]; // pad id; mode + colours stay 0 (off)
			if (send_all(c, r, sizeof(r))) logf("GET_LED served (all off, v2)");
			else logf("GET_LED send failed");
			break;
		}
		default:
			logf("unknown cmd 0x%02x", cmd);
			break;
		}
	}

	void run(uint16_t port)
	{
		WSADATA w{};
		WSAStartup(MAKEWORD(2, 2), &w);

		SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (ls == INVALID_SOCKET) { logf("socket() failed"); return; }
		// Mark the socket non-inheritable. When RPCS3 auto-updates it relaunches
		// itself; an inheritable listen socket would leak into the new process
		// and keep the port occupied as a ghost after the old one exits. This
		// keeps restarts clean.
		SetHandleInformation(reinterpret_cast<HANDLE>(ls), HANDLE_FLAG_INHERIT, 0);
		// Deliberately NO SO_REUSEADDR: on Windows it lets a second process
		// duplicate-bind the same port, and RPCS3's frequent auto-update
		// restarts would then pile up ghost listeners that steal and hang the
		// app's connections. Without it, a stale socket simply makes bind fail
		// and we retry until the old instance's port is released.

		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(port);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only

		bool bound = false;
		for (int attempt = 0; attempt < 20 && g_running; ++attempt)
		{
			if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && ::listen(ls, 4) == 0) { bound = true; break; }
			if (attempt == 0) logf("bind on 127.0.0.1:%u busy (stale instance?), retrying...", port);
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		if (!bound)
		{
			logf("could not bind 127.0.0.1:%u after retries -- close other RPCS3 instances", port);
			::closesocket(ls);
			WSACleanup();
			return;
		}
		g_listen = ls;
		logf("Toypad listener active on 127.0.0.1:%u", port);

		while (g_running)
		{
			SOCKET c = ::accept(ls, nullptr, nullptr);
			if (c == INVALID_SOCKET) break; // closed by listener_stop()
			handle_client(c);
			::closesocket(c);
		}
		WSACleanup();
		logf("listener stopped");
	}

	uint16_t resolve_port(uint16_t fallback)
	{
		if (const char* e = std::getenv("RPCS3_TOYPAD_PORT"))
		{
			int p = std::atoi(e);
			if (p >= 1 && p <= 65535) return static_cast<uint16_t>(p);
		}
		return fallback;
	}
}

bool listener_start(const ToypadSymbols& symbols, void (*log)(const char*), uint16_t port)
{
	g_log = log;
	if (!g_api.bind(symbols)) { logf("listener: incomplete symbols, not starting"); return false; }
	if (g_running.exchange(true)) return true;
	g_thread = std::thread(run, resolve_port(port));
	return true;
}

void listener_stop()
{
	if (!g_running.exchange(false)) return;
	SOCKET ls = g_listen.exchange(INVALID_SOCKET);
	if (ls != INVALID_SOCKET) ::closesocket(ls); // unblocks accept()
	if (g_thread.joinable()) g_thread.join();
}
