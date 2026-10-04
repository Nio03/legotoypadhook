// version.dll proxy for RPCS3.
//
// rpcs3.exe imports three functions from VERSION.dll. Windows resolves a DLL
// named `version.dll` sitting next to the executable before the system copy
// (version.dll is not a KnownDLL), so dropping this file into the RPCS3 folder
// makes rpcs3.exe load it automatically. We forward those imports to the real
// C:\Windows\System32\version.dll and, on a background thread, locate RPCS3's
// toypad functions in memory and open the LegoToypad listener. Nothing in the
// emulator is modified on disk; remove the file and RPCS3 is byte-for-byte
// stock again.
#include <windows.h>
#include <cstdio>
#include <mutex>

#include "locator.h"
#include "listener.h"

namespace
{
	std::mutex g_log_mutex;
	wchar_t g_log_path[MAX_PATH] = {};
	HMODULE g_self = nullptr;

	void hook_log(const char* msg)
	{
		std::lock_guard<std::mutex> lk(g_log_mutex);
		FILE* f = _wfopen(g_log_path, L"a");
		if (!f) return;
		SYSTEMTIME t; GetLocalTime(&t);
		std::fprintf(f, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, msg);
		std::fclose(f);
	}

	DWORD WINAPI init_thread(LPVOID)
	{
		hook_log("legotoypad hook loaded");
		const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
		ToypadSymbols sym;
		if (!locate_toypad_symbols(base, sym, hook_log))
		{
			hook_log("could not locate toypad symbols -- is this rpcs3.exe? hook idle.");
			return 0;
		}
		listener_start(sym, hook_log, 9191);
		return 0;
	}

	// ---- forwarding to the real version.dll -------------------------------
	HMODULE g_real = nullptr;
	std::once_flag g_real_once;

	FARPROC real(const char* name)
	{
		std::call_once(g_real_once, [] {
			wchar_t sys[MAX_PATH];
			UINT n = GetSystemDirectoryW(sys, MAX_PATH);
			if (!n || n >= MAX_PATH - 12) return;
			wcscat_s(sys, MAX_PATH, L"\\version.dll");

			// A module is keyed by its base name, so LoadLibrary("...\\version.dll")
			// would hand back THIS proxy (same base name) and the forwarders would
			// recurse forever. Load the system copy under a unique name instead.
			wchar_t tmp[MAX_PATH];
			if (GetTempPathW(MAX_PATH, tmp) && wcslen(tmp) < MAX_PATH - 32)
			{
				wcscat_s(tmp, MAX_PATH, L"legotoypad_version_orig.dll");
				CopyFileW(sys, tmp, FALSE); // best effort; overwrite each run
				g_real = LoadLibraryW(tmp);
			}
			if (!g_real) g_real = LoadLibraryW(sys); // last resort
			if (g_real == g_self) g_real = nullptr;  // never forward to ourselves
		});
		return g_real ? GetProcAddress(g_real, name) : nullptr;
	}
}

extern "C"
{
	BOOL WINAPI proxy_GetFileVersionInfoA(LPCSTR f, DWORD h, DWORD len, LPVOID data)
	{ auto fn = reinterpret_cast<BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID)>(real("GetFileVersionInfoA")); return fn ? fn(f, h, len, data) : FALSE; }

	BOOL WINAPI proxy_GetFileVersionInfoW(LPCWSTR f, DWORD h, DWORD len, LPVOID data)
	{ auto fn = reinterpret_cast<BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID)>(real("GetFileVersionInfoW")); return fn ? fn(f, h, len, data) : FALSE; }

	BOOL WINAPI proxy_GetFileVersionInfoExA(DWORD flags, LPCSTR f, DWORD h, DWORD len, LPVOID data)
	{ auto fn = reinterpret_cast<BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID)>(real("GetFileVersionInfoExA")); return fn ? fn(flags, f, h, len, data) : FALSE; }

	BOOL WINAPI proxy_GetFileVersionInfoExW(DWORD flags, LPCWSTR f, DWORD h, DWORD len, LPVOID data)
	{ auto fn = reinterpret_cast<BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID)>(real("GetFileVersionInfoExW")); return fn ? fn(flags, f, h, len, data) : FALSE; }

	DWORD WINAPI proxy_GetFileVersionInfoSizeA(LPCSTR f, LPDWORD h)
	{ auto fn = reinterpret_cast<DWORD(WINAPI*)(LPCSTR, LPDWORD)>(real("GetFileVersionInfoSizeA")); return fn ? fn(f, h) : 0; }

	DWORD WINAPI proxy_GetFileVersionInfoSizeW(LPCWSTR f, LPDWORD h)
	{ auto fn = reinterpret_cast<DWORD(WINAPI*)(LPCWSTR, LPDWORD)>(real("GetFileVersionInfoSizeW")); return fn ? fn(f, h) : 0; }

	DWORD WINAPI proxy_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR f, LPDWORD h)
	{ auto fn = reinterpret_cast<DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD)>(real("GetFileVersionInfoSizeExA")); return fn ? fn(flags, f, h) : 0; }

	DWORD WINAPI proxy_GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR f, LPDWORD h)
	{ auto fn = reinterpret_cast<DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD)>(real("GetFileVersionInfoSizeExW")); return fn ? fn(flags, f, h) : 0; }

	BOOL WINAPI proxy_VerQueryValueA(LPCVOID blk, LPCSTR sub, LPVOID* buf, PUINT len)
	{ auto fn = reinterpret_cast<BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT)>(real("VerQueryValueA")); return fn ? fn(blk, sub, buf, len) : FALSE; }

	BOOL WINAPI proxy_VerQueryValueW(LPCVOID blk, LPCWSTR sub, LPVOID* buf, PUINT len)
	{ auto fn = reinterpret_cast<BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT)>(real("VerQueryValueW")); return fn ? fn(blk, sub, buf, len) : FALSE; }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_self = inst;
		DisableThreadLibraryCalls(inst);
		GetModuleFileNameW(inst, g_log_path, MAX_PATH);
		if (wchar_t* slash = wcsrchr(g_log_path, L'\\')) slash[1] = 0;
		wcscat_s(g_log_path, MAX_PATH, L"legotoypad_hook.log");
		CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		listener_stop();
	}
	return TRUE;
}
