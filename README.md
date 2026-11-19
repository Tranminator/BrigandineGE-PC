# Brigandine: Grand Edition — native PC port (work in progress)

Native build of the PS1 game (SLPS-02661/02662 + English patch) — no emulator.
The original MIPS code is **statically recompiled to C** (`tools/recomp.py`), and the
Sony PsyQ libraries that touch hardware are replaced by native implementations (HLE)
on top of SDL2. Game data is read from **your own disc image** at runtime; nothing
from the game is stored in this repository.

## What you need
- **Your own copy** of *Brigandine: Grand Edition* (PS1, SLPS-02661/02662) as a raw `.bin/.cue`
  (MODE2/2352) image, with the community English translation patch applied
  (the port is built against the patched EXE — the patch inserts code into it).
- macOS (Apple Silicon or Intel). The code is portable C + SDL2; Windows/Linux builds are planned.

> **Legal:** this repository contains only original code, tools and documentation.
> It does not include any game code, data, graphics, audio or video. Disassembly (`decomp/asm/`)
> and recompiled C (`build/gen/`) are generated locally from *your* disc and must not be redistributed.
> Brigandine is a trademark of its respective owners; this project is not affiliated with them.

## Status
Boots and plays natively: title, menus, story/scenario, strategy map, unit organisation, dialogue,
FMV movies (with voice), XA + sequenced music, sound effects, reverb, sound options (F2).
Not done yet: memory card saves (do not pick CONTINUE), battles not fully tested, Disc 2 swap,
high-resolution renderer, Windows build.

## Build (macOS)
```sh
brew install cmake sdl2          # Dear ImGui is fetched automatically by CMake
python3 -m venv .venv && .venv/bin/pip install splat64[mips] spimdisasm m2c   # analysis tools
# 0) put your disc images in iso/  (e.g. "iso/Disc 1/<disc1>.bin" + .cue)
# 1) extract the disc once (needed by the recompiler: EXE + OVER*.BIN)
python3 -I tools/extract_disc.py "iso/Disc 1/<disc1>.bin" extracted/disc1
# 2) disassemble for function boundaries / names
(cd decomp && ../.venv/bin/splat split brigandine.yaml && for n in 1 2 3 4 5; do ../.venv/bin/splat split over$n.yaml; done)
# 3) recompile + build
python3 -I tools/recomp.py .
cmake -S . -B build/cmake && cmake --build build/cmake -j8
```

## Run
```sh
./build/cmake/brigandine "iso/Disc 1/<disc1>.bin"
```
| PS1 | Keyboard | Controller |
|---|---|---|
| D-pad | Arrow keys | D-pad |
| ○ (confirm) | X | B |
| × (cancel) | Z | A |
| △ | S | Y |
| □ | A | X |
| L1 / R1 | Q / E | LB / RB |
| L2 / R2 | 1 / 3 | LT / RT |
| Start / Select | Enter / Backspace | Start / Back |

`F1` shows all of VRAM (debug). Hold `Tab` to fast-forward.

Settings: press `F2` (mouse, keyboard or controller; `F2`/`Esc` closes and saves). Game input is
paused while the window is open.
- **Graphics** — VSync (on: no tearing; off: best for variable-refresh displays), Frame skip
  (Off / Auto / draw 1 of 2–4: keeps normal game speed on slow machines), performance overlay.
- **Sound** — Music 30%, Sound effects 100%, Movies 50% by default (music level matches the
  emulator balance between music and menu sounds).
- **Controls** — key / controller reference.

The game logic always runs at the PS1's 59.94 Hz (it counts frames), whatever the display refresh.
Saved to `~/Library/Application Support/PortGamePC/Brigandine/settings.cfg` (Windows: `%APPDATA%`).
Environment variables `BRIG_BGM_VOLUME`, `BRIG_SFX_VOLUME`, `BRIG_MOVIE_VOLUME` (percent),
`BRIG_VSYNC` (0/1) and `BRIG_FRAMESKIP` (0–4) override the file.

Debug env vars: `BRIG_TRACE=1` (per-second GPU stats), `BRIG_SHOT_DIR=<dir> BRIG_SHOT_EVERY=<n>`
(BMP snapshots), `BRIG_INPUT="frame:hexmask,..."` (scripted pad input), `BRIG_SHOW_VRAM=1`,
`BRIG_CDTRACE=1` (CD commands), `BRIG_SNDTRACE=1` (libsnd calls + active SPU voices), `BRIG_WAV=<file.wav>`
(record audio output), `BRIG_MUTE_XA=1` (SPU only), `BRIG_REVERB_ONLY=1` (reverb wet signal only), `BRIG_MUTE_SPU=1` (XA only), `BRIG_KEYS="frame:KeyName,..."` (scripted keyboard keys), `BRIG_FAST=1` (unthrottled), `BRIG_SKIP_MOVIES=1`,
`BRIG_SHOT_FROM`/`BRIG_SHOT_TO` (snapshot range), `BRIG_SLOW_MS=<n>` (simulate a slow machine), `BRIG_UI_TAB=<0-2>`.

## Layout
- `tools/recomp.py` — MIPS → C recompiler (function discovery, jump tables, patch trampolines, overlays)
- `src/pc/` — runtime (`rt.c`), fallback interpreter (`interp.c`), GTE (`gte.c`), software GPU (`gpu.c`),
  audio mixer + XA-ADPCM (`audio.c`), SPU synth + VAB/SEQ player + reverb (`spu.c`),
  STR movie player (`movie.c`) + MDEC/BS v2 decoder (`mdec.c`), HLE (`hle_*.c`), SDL (`platform.c`)
- `src/pc/hle_list.txt` — which PsyQ/game functions are replaced natively
- `decomp/` — splat configs, symbols, struct headers, first hand-decompiled C
- `docs/` — plan, disc layout, findings
- `tools/seq_render.c` — offline renderer for the game's music (A/B against recordings)

## License
The code in this repository is released under the [MIT License](LICENSE).
It covers only this project's own code and tools — not the game, which you must own.
