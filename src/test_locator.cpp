// Offline self-test: maps rpcs3.exe as an image (without running it) and runs
// the exact locator the DLL uses, printing the RVAs it resolves. Compare these
// against a disassembler to confirm the locator is correct for a given build.
//
//   test_locator <path-to-rpcs3.exe>
#include "locator.h"
#include <windows.h>
#include <cstdio>

static void logline(const char* m) { std::printf("  %s\n", m); }

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2) { std::printf("usage: test_locator <rpcs3.exe>\n"); return 2; }

	HMODULE h = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
	if (!h) { std::printf("LoadLibraryEx failed: %lu\n", GetLastError()); return 1; }

	const uint8_t* base = reinterpret_cast<const uint8_t*>(reinterpret_cast<ULONG_PTR>(h) & ~static_cast<ULONG_PTR>(3));
	std::printf("mapped at %p\n", static_cast<const void*>(base));

	ToypadSymbols s;
	bool ok = locate_toypad_symbols(base, s, logline);

	std::printf("\nresult: %s\n", ok ? "OK" : "FAILED");
	std::printf("  interrupt_transfer  rva=%08x\n", s.interrupt_transfer);
	std::printf("  g_dimensionstoypad  rva=%08x\n", s.g_dimensionstoypad);
	std::printf("  save                rva=%08x\n", s.save);
	std::printf("  remove_figure       rva=%08x\n", s.remove_figure);
	std::printf("  load_figure         rva=%08x\n", s.load_figure);
	return ok ? 0 : 1;
}
