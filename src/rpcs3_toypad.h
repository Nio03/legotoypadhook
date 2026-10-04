// Typed entry points into RPCS3's g_dimensionstoypad, resolved at run time.
//
// These are member functions of dimensions_toypad; on the Windows x64 ABI the
// implicit `this` is the first integer argument (rcx). We declare them as plain
// pointers and pass &g_dimensionstoypad as `self`. `fs::file` is a move-only
// class passed by value, which the MSVC ABI lowers to a hidden pointer in the
// r8 slot; we always hand it a pointer to a zeroed (empty) fs::file, so the
// figure lives in RPCS3's memory for the session and no file handle is touched.
#pragma once
#include <cstdint>
#include "locator.h"

#if defined(__GNUC__) || defined(__clang__)
#define TP_MSABI __attribute__((ms_abi)) // force the Microsoft x64 ABI explicitly
#else
#define TP_MSABI
#endif

// u32 dimensions_toypad::load_figure(const std::array<u8,180>& buf, fs::file in_file, u8 pad, u8 index, bool lock)
using load_figure_fn = uint32_t(TP_MSABI*)(void* self, const void* buf180, void* in_file, uint8_t pad, uint8_t index, bool lock);
// bool dimensions_toypad::remove_figure(u8 pad, u8 index, bool full_remove, bool lock)
using remove_figure_fn = int(TP_MSABI*)(void* self, uint8_t pad, uint8_t index, int full_remove, int lock);

struct ToypadApi
{
	void* self = nullptr;      // &g_dimensionstoypad
	load_figure_fn load = nullptr;
	remove_figure_fn remove = nullptr;

	bool bind(const ToypadSymbols& s)
	{
		if (!s.complete()) return false;
		self = const_cast<uint8_t*>(s.base) + s.g_dimensionstoypad;
		load = reinterpret_cast<load_figure_fn>(const_cast<uint8_t*>(s.base) + s.load_figure);
		remove = reinterpret_cast<remove_figure_fn>(const_cast<uint8_t*>(s.base) + s.remove_figure);
		return true;
	}
};
