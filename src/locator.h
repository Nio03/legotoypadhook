// Runtime locator for RPCS3's LEGO Dimensions toypad symbols.
//
// RPCS3 ships no plugin API and auto-updates almost daily, so hard-coded
// offsets are useless. Instead we find a handful of functions in the loaded
// rpcs3.exe image at run time, the same way a disassembler would:
//   * the emulated toypad's C++ RTTI name -> its vtable -> interrupt_transfer
//   * interrupt_transfer's first reference to the g_dimensionstoypad global
//   * a unique log string -> dimensions_figure::save -> its caller remove_figure
//   * a unique Qt-dialog string -> the load_figure call site
//
// Everything is pure RVA arithmetic over an image-mapped module base, so the
// exact same code can run against rpcs3.exe mapped as a data image (for the
// offline self-test) or against GetModuleHandle(nullptr) inside the process.
#pragma once
#include <cstdint>

struct ToypadSymbols
{
	const uint8_t* base = nullptr; // module base (image-aligned)
	uint32_t interrupt_transfer = 0; // RVA, intermediate only
	uint32_t g_dimensionstoypad = 0; // RVA of the global toypad object
	uint32_t load_figure = 0;        // RVA
	uint32_t remove_figure = 0;      // RVA
	uint32_t save = 0;               // RVA, intermediate only

	bool complete() const { return g_dimensionstoypad && load_figure && remove_figure; }
};

// Walks the PE image at `base` and fills `out`. `log` receives human-readable
// progress/diagnostics (may be null). Returns true if all callable symbols were
// found. Never throws; on any malformed structure it logs and returns false.
bool locate_toypad_symbols(const uint8_t* base, ToypadSymbols& out, void (*log)(const char*));
