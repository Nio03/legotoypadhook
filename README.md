# legotoypadhook

**A single drop-in `version.dll` that makes stock, unmodified RPCS3 work with
[LegoToypad](https://github.com/harrysof/LegoToypad) — no custom emulator build
required.**

Today, using LegoToypad with RPCS3 means downloading and running a whole
[custom RPCS3 build](https://github.com/NeverCookFirst/RPCS3-Seamless-Toypad-Build).
This project does the same thing with one file you drop next to `rpcs3.exe`.
You keep your normal RPCS3 install, it keeps auto-updating, and LegoToypad talks
to it unchanged.

Delete the file and RPCS3 is byte-for-byte stock again.

## What it does

- Opens the same loopback listener on `127.0.0.1:9191` that LegoToypad already
  speaks to, and drives RPCS3's **own** emulated LEGO Dimensions toypad.
- Load / remove / move figures from the app, controller in hand — no alt-tab,
  no mouse, no modified emulator.
- Works with the stock `LegoToypad.exe`; the app doesn't know the difference.

### Intentionally minimal

This implements the core of the "seamless" protocol, not every extra:

- **No controller input muting** while the picker overlay is open.
- **No live LED mirror** (a `GET_LED` poll gets a static "all off" reply so an
  LED-polling client never stalls).
- **Move** is served from a small tag cache this hook fills when *it* loads a
  figure. A figure loaded through RPCS3's own Dimensions Manager won't move from
  the app — load it from the app instead.

## Install

1. Build `version.dll` (see below), or grab it from a release.
2. Copy `version.dll` into your RPCS3 folder, next to `rpcs3.exe`.
3. Launch RPCS3 and start LEGO Dimensions as usual.
4. Run `LegoToypad.exe` with `[Listener] Port=9191` in its `LegoToypad.ini`.
5. Open the picker and load a figure — it appears in game.

A `legotoypad_hook.log` is written next to `version.dll`. A healthy run ends with:

```
Toypad listener active on 127.0.0.1:9191
```

and shows a `client msg cmd=0x01 ...` line each time the app loads a figure.

## How it works

RPCS3 has no plugin API and updates almost daily, so hard-coded addresses are
useless. Two ideas make a drop-in file work anyway:

1. **Getting loaded.** `rpcs3.exe` imports `VERSION.dll`, and that name is not a
   Windows "KnownDLL", so a `version.dll` sitting beside the executable is loaded
   instead of the system one. This proxy forwards all of RPCS3's `version.dll`
   imports to the real `C:\Windows\System32\version.dll`, so nothing else
   changes — it just also gets a chance to run at startup.

2. **Finding the toypad.** On a background thread it locates three functions in
   the loaded image the way a disassembler would — from the emulated toypad's
   C++ RTTI name to its vtable and `interrupt_transfer`, then to the
   `g_dimensionstoypad` global, `load_figure` and `remove_figure`. Because it
   keys off structure, not offsets, it keeps working across RPCS3 updates (it has
   been verified to resolve the correct functions across different builds, and
   confirmed working after a live auto-update mid-session).

The listener socket is created non-inheritable and without `SO_REUSEADDR`, so
RPCS3's self-restart on update never leaves a ghost socket on the port.

```
LegoToypad  --TCP 127.0.0.1:9191-->  version.dll listener  -->  g_dimensionstoypad
  (unchanged)                         (this project)             (RPCS3's own toypad)
```

## Build

Needs MinGW-w64 `g++` on PATH. No Visual Studio, no Qt.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Output lands in `build/`:

- `version.dll` — the hook; copy into the RPCS3 folder.
- `test_locator.exe` — optional diagnostic: `test_locator.exe <path\to\rpcs3.exe>`
  prints the functions it resolves. It maps the exe as data and does **not** run it.

## Status

Verified: symbol detection across RPCS3 builds (including after a live
auto-update), self-contained DLL (UCRT + ws2_32 only), correct forwarding to the
real `version.dll`, and clean idle when loaded into a non-RPCS3 process.
Confirmed end-to-end loading figures into a running LEGO Dimensions session.

Windows x64 only. The DLL is unsigned, so SmartScreen may warn on first use.

## Credits

- [RPCS3](https://github.com/RPCS3/rpcs3) — the emulator and its toypad
  implementation this calls into.
- [harrysof/LegoToypad](https://github.com/harrysof/LegoToypad) — the companion
  app and the wire protocol.
- [NeverCookFirst/RPCS3-Seamless-Toypad-Build](https://github.com/NeverCookFirst/RPCS3-Seamless-Toypad-Build)
  — the seamless build whose protocol this re-implements as a drop-in.

## License

RPCS3 is GPLv2; this hook is distributed under the same license (see `LICENSE`).
Not affiliated with RPCS3, Sony, LEGO, or Warner Bros. Bring your own game dump
and your own tag dumps.
