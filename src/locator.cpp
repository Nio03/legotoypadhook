#include "locator.h"

#include <windows.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace
{
	struct Range { uint32_t lo = 0, hi = 0; bool has(uint32_t r) const { return r >= lo && r < hi; } };
	struct RF { uint32_t beg, end, unw; };

	struct Ctx
	{
		const uint8_t* img = nullptr;
		uint64_t image_base = 0;
		Range text, rdata, data;
		std::vector<RF> rf;
		void (*log)(const char*) = nullptr;

		void logf(const char* fmt, ...) const
		{
			if (!log) return;
			char buf[256];
			va_list ap; va_start(ap, fmt);
			vsnprintf(buf, sizeof(buf), fmt, ap);
			va_end(ap);
			log(buf);
		}

		uint32_t u32(uint32_t rva) const { uint32_t v; std::memcpy(&v, img + rva, 4); return v; }
		uint64_t u64(uint32_t rva) const { uint64_t v; std::memcpy(&v, img + rva, 8); return v; }
		int32_t  i32(uint32_t rva) const { int32_t v; std::memcpy(&v, img + rva, 4); return v; }

		// All occurrences of `needle` (len bytes) within [lo,hi).
		std::vector<uint32_t> find_all(const void* needle, size_t len, const Range& r) const
		{
			std::vector<uint32_t> out;
			if (r.hi <= r.lo || len == 0) return out;
			for (uint32_t i = r.lo; i + len <= r.hi; ++i)
				if (std::memcmp(img + i, needle, len) == 0) out.push_back(i);
			return out;
		}

		const RF* rf_of(uint32_t rva) const
		{
			size_t lo = 0, hi = rf.size();
			while (lo < hi) { size_t m = (lo + hi) / 2; if (rf[m].beg <= rva) lo = m + 1; else hi = m; }
			if (lo == 0) return nullptr;
			const RF& c = rf[lo - 1];
			return (c.beg <= rva && rva < c.end) ? &c : nullptr;
		}

		// Follow UNWIND chaininfo to the primary (outermost) function; returns its start RVA (0 if unknown).
		uint32_t primary(uint32_t rva) const
		{
			const RF* e = rf_of(rva);
			for (int guard = 0; e && guard < 16; ++guard)
			{
				uint8_t flags = img[e->unw] >> 3;
				if (!(flags & 4)) return e->beg; // UNW_FLAG_CHAININFO not set
				uint8_t cnt = img[e->unw + 2];
				uint32_t off = e->unw + 4 + ((cnt + 1) & ~1u) * 2;
				e = rf_of(u32(off));
			}
			return e ? e->beg : 0;
		}

		// A `lea r64,[rip+disp32]` whose 3rd byte matches a modrm with rm=101 (rip) and mod=00.
		bool is_lea_rip(uint32_t a, uint32_t& target) const
		{
			uint8_t b0 = img[a], b1 = img[a + 1], b2 = img[a + 2];
			if ((b0 == 0x48 || b0 == 0x4c) && b1 == 0x8d && (b2 & 0xc7) == 0x05)
			{
				target = a + 7 + static_cast<uint32_t>(i32(a + 3));
				return true;
			}
			return false;
		}

		// Instruction RVAs of every `lea r64,[rip+X]` in .text where X resolves to target_rva.
		std::vector<uint32_t> lea_xrefs(uint32_t target_rva) const
		{
			std::vector<uint32_t> out;
			for (uint32_t a = text.lo; a + 7 <= text.hi; ++a)
			{
				uint32_t t;
				if (is_lea_rip(a, t) && t == target_rva) out.push_back(a);
			}
			return out;
		}

		// Call sites (`E8 rel32`) in [lo,hi) and the target each one calls.
		void calls_in(uint32_t lo, uint32_t hi, std::vector<std::pair<uint32_t, uint32_t>>& out) const
		{
			for (uint32_t a = lo; a + 5 <= hi; ++a)
				if (img[a] == 0xe8)
					out.emplace_back(a, a + 5 + static_cast<uint32_t>(i32(a + 1)));
		}

		// First `lea rcx,[rip+X]` in [lo,hi) whose target lands in .data (that is g_dimensionstoypad).
		uint32_t first_data_lea_rcx(uint32_t lo, uint32_t hi) const
		{
			for (uint32_t a = lo; a + 7 <= hi; ++a)
				if (img[a] == 0x48 && img[a + 1] == 0x8d && img[a + 2] == 0x0d)
				{
					uint32_t t = a + 7 + static_cast<uint32_t>(i32(a + 3));
					if (data.has(t)) return t;
				}
			return 0;
		}

		// Does [lo,hi) contain a `lea rcx,[rip+X]` targeting g?
		bool refs_g(uint32_t lo, uint32_t hi, uint32_t g) const
		{
			for (uint32_t a = lo; a + 7 <= hi; ++a)
				if (img[a] == 0x48 && img[a + 1] == 0x8d && img[a + 2] == 0x0d &&
					a + 7 + static_cast<uint32_t>(i32(a + 3)) == g)
					return true;
			return false;
		}

		bool contains(uint32_t lo, uint32_t hi, const void* n, size_t len) const
		{
			for (uint32_t a = lo; a + len <= hi; ++a)
				if (std::memcmp(img + a, n, len) == 0) return true;
			return false;
		}

		// RVA of a C string living in .rdata that actually begins there (preceded by a NUL).
		std::vector<uint32_t> cstr(const char* s) const
		{
			size_t len = std::strlen(s) + 1; // include terminator
			auto hits = find_all(s, len, rdata);
			std::vector<uint32_t> out;
			for (uint32_t h : hits) if (h == rdata.lo || img[h - 1] == 0) out.push_back(h);
			return out;
		}
	};
}

bool locate_toypad_symbols(const uint8_t* base, ToypadSymbols& out, void (*log)(const char*))
{
	auto say = [log](const char* m) { if (log) log(m); };
	out = ToypadSymbols{};
	out.base = base;
	if (!base) { say("locator: null base"); return false; }

	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) { say("locator: bad DOS header"); return false; }
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
	{ say("locator: not a PE32+/x64 image"); return false; }

	Ctx c;
	c.img = base;
	c.image_base = nt->OptionalHeader.ImageBase;
	c.log = log;

	const auto* sec = IMAGE_FIRST_SECTION(nt);
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
	{
		char name[9] = {}; std::memcpy(name, sec[i].Name, 8);
		Range r{ sec[i].VirtualAddress, sec[i].VirtualAddress + sec[i].Misc.VirtualSize };
		if (!std::strcmp(name, ".text")) c.text = r;
		else if (!std::strcmp(name, ".rdata")) c.rdata = r;
		else if (!std::strcmp(name, ".data")) c.data = r;
	}
	if (!c.text.hi || !c.rdata.hi || !c.data.hi) { say("locator: missing a core section"); return false; }

	const auto& exd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
	for (uint32_t off = 0; off + 12 <= exd.Size; off += 12)
	{
		RF e{ c.u32(exd.VirtualAddress + off), c.u32(exd.VirtualAddress + off + 4), c.u32(exd.VirtualAddress + off + 8) };
		c.rf.push_back(e);
	}
	std::sort(c.rf.begin(), c.rf.end(), [](const RF& a, const RF& b) { return a.beg < b.beg; });
	c.logf("locator: sections ok, %zu runtime functions", c.rf.size());

	// --- 1. RTTI name -> type descriptor -> complete object locator -> vtable -> interrupt_transfer
	const char* rtti = ".?AVusb_device_dimensions@@";
	auto names = c.find_all(rtti, std::strlen(rtti) + 1, c.data);
	if (names.empty()) { say("locator: RTTI name for usb_device_dimensions not found"); return false; }
	uint32_t td = names[0] - 16; // TypeDescriptor = vftable(8) + spare(8) + name
	c.logf("locator: type descriptor rva=%x", td);

	uint32_t col = 0;
	{
		uint32_t key = td;
		for (uint32_t loc : c.find_all(&key, 4, c.rdata))
		{
			uint32_t cand = loc - 12; // pTypeDescriptor sits at col+12
			if (cand < c.rdata.lo) continue;
			if (c.u32(cand) == 1 && c.u32(cand + 4) == 0 && c.u32(cand + 20) == cand) { col = cand; break; }
		}
	}
	if (!col) { say("locator: complete object locator not found"); return false; }
	c.logf("locator: complete object locator rva=%x", col);

	uint32_t vt = 0;
	{
		uint64_t key = c.image_base + col;
		auto hits = c.find_all(&key, 8, c.rdata);
		if (!hits.empty()) vt = hits[0] + 8; // vtable[0] follows the meta pointer
	}
	if (!vt) { say("locator: vtable not found"); return false; }
	c.logf("locator: vtable rva=%x", vt);

	const uint8_t cmp_edx_20[3] = { 0x83, 0xfa, 0x20 }; // cmp edx, 0x20  == ensure(buf_size == 0x20)
	for (int k = 0; k < 16; ++k)
	{
		uint64_t va = c.u64(vt + 8 * k);
		if (va < c.image_base) break;
		uint32_t f = static_cast<uint32_t>(va - c.image_base);
		if (!c.text.has(f)) break;
		if (c.contains(f, f + 0x60, cmp_edx_20, 3)) { out.interrupt_transfer = f; c.logf("locator: interrupt_transfer rva=%x (vslot %d)", f, k); break; }
	}
	if (!out.interrupt_transfer) { say("locator: interrupt_transfer not identified in vtable"); return false; }

	// --- 2. g_dimensionstoypad: first data-pointing `lea rcx` inside interrupt_transfer
	out.g_dimensionstoypad = c.first_data_lea_rcx(out.interrupt_transfer, out.interrupt_transfer + 0x1000);
	if (!out.g_dimensionstoypad) { say("locator: g_dimensionstoypad reference not found"); return false; }
	c.logf("locator: g_dimensionstoypad rva=%x", out.g_dimensionstoypad);
	const uint32_t G = out.g_dimensionstoypad;

	// --- 3. dimensions_figure::save via its unique error string, then remove_figure as its caller
	for (uint32_t s : c.cstr("Tried to save infinity figure to file but no infinity figure is active!"))
		for (uint32_t x : c.lea_xrefs(s)) { uint32_t p = c.primary(x); if (p) { out.save = p; break; } }
	if (out.save)
	{
		c.logf("locator: save rva=%x", out.save);
		uint32_t it_primary = c.primary(out.interrupt_transfer);
		std::vector<std::pair<uint32_t, uint32_t>> calls;
		c.calls_in(c.text.lo, c.text.hi, calls);
		for (auto& cl : calls)
		{
			if (cl.second != out.save) continue;
			uint32_t p = c.primary(cl.first);
			if (!p || p == it_primary) continue;
			if (c.refs_g(p, p + 0x200, G)) { out.remove_figure = p; break; }
		}
	}
	if (!out.remove_figure) { say("locator: remove_figure not found"); return false; }
	c.logf("locator: remove_figure rva=%x", out.remove_figure);

	// --- 4. load_figure via the Qt dialog's unique error string and its call site
	const uint8_t set_lock_true[5] = { 0xc6, 0x44, 0x24, 0x28, 0x01 }; // mov byte [rsp+0x28], 1  (lock=true)
	std::vector<uint32_t> loads;
	for (uint32_t s : c.cstr("Failed to read the figure file!"))
		for (uint32_t x : c.lea_xrefs(s))
		{
			std::vector<std::pair<uint32_t, uint32_t>> calls;
			c.calls_in(x, x + 0x200, calls);
			for (auto& cl : calls)
			{
				uint32_t a = cl.first, t = cl.second;
				if (!c.text.has(t)) continue;
				uint32_t lo = a > 0x30 ? a - 0x30 : 0;
				if (c.contains(lo, a, set_lock_true, 5) && c.refs_g(t, t + 0x80, G))
					if (std::find(loads.begin(), loads.end(), t) == loads.end()) loads.push_back(t);
			}
		}
	if (loads.size() == 1) out.load_figure = loads[0];
	else c.logf("locator: load_figure ambiguous/not found (%zu candidates)", loads.size());
	if (!out.load_figure) { say("locator: load_figure not found"); return false; }
	c.logf("locator: load_figure rva=%x", out.load_figure);

	return out.complete();
}
