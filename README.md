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
FMV movies (with voice), XA + sequenced music, sound effects, reverb, settings window (F2), save / load.
Memory card saves (standard 128 KB card images, compatible with emulators). High-resolution renderer.
Both discs: the port swaps to Disc 2 / Disc 1 by itself when the game asks for it.
Not done yet: long play-through testing, widescreen, Windows build.

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
./build/cmake/brigandine "iso/Disc 1/<disc1>.bin"                       # Disc 2 is found in "iso/Disc 2/"
./build/cmake/brigandine "<disc1>.bin" "<disc2>.bin"                     # or name it explicitly
```
The game needs Disc 2 for some story movies, and asks for it ("Insert disk 2"). The port then puts the
Disc 2 image in the drive by itself (a short "Disc 2 inserted" message) and the game goes on; nothing has to be
done. Disc 2 is looked for at the Disc 1 path with "Disc 1" changed to "Disc 2" (that file, or the first `.bin`
in that folder). Without it the game stays on "Insert disk 2".
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

Mouse (`F2` → **Controls**): one pointer — the mouse drives the game's own cursor (captured, with an
adjustable speed so a high-DPI mouse stays usable on the 320×240 picture; released while `F2` is open or the
window is in the background). Battle map: move to a hex, click to select (the game still checks every
choice), push against a screen edge to scroll (it keeps scrolling at
the game's cursor speed until you move the mouse back); when the game moves its cursor itself (menu closed, keyboard,
pad) the mouse continues from there; attack / spell targets are picked the same way. Strategy map: the
quill follows the mouse (push past the edge to scroll, same as on the battle map), click a castle to open its menu. Icon menus
(battle commands, castle, Organize), the Organize squad / stock grids and the scrolling lists (attacks,
magic, skills, items, SUMMON monsters, UNITS knights...): point, click to choose, push past the last row or use the wheel to
scroll. Yes / No boxes: point at Yes or No (or wheel), click to answer. Unit status: point at an attack,
spell or skill to read its description, click for the next page.
In the MOVE / QUEST troop lists, Ctrl+click, Shift+click or middle click marks a troop (□ Select). Right click = × (back), middle click = △, wheel =
up/down; on other screens left click = ○. Start: right click on a map with the free cursor (strategy or
battle), or a side (back / forward) button anywhere. DOMAIN map: the box follows the mouse, click to jump
there. Events: click for the next line, hold the left button to fast-forward (Square); the
same while the enemy moves on the battle map.
Title screen: click
for PUSH START BUTTON, point and click in the menu. SELECT RULER: push the mouse sideways (or the wheel) for
the next ruler, click to pick. Organize EQUIP / ITEM: point at an item
(push past the grid or use the wheel to scroll), click. NAME: point at a letter (or
Kana / Eng. / Space / Cancel / Finish), click. SAVE / LOAD: point at a file, click. UNITS list: push the mouse sideways for the other columns
(without the game pointer: click the red corner arrows); push above the first knight for the header row, where
the mouse picks the column and a click sorts.

`F5` quick save, `F9` quick load (see below). `F1` shows all of VRAM (debug). Hold `Tab` to fast-forward.

Settings: press `F2` (mouse, keyboard or controller; `F2`/`Esc` closes and saves). Game input is
paused while the window is open.
- **Graphics** — VSync (on: no tearing; off: best for variable-refresh displays), Frame skip
  (Off / Auto / draw 1 of 2–4: keeps normal game speed on slow machines), display shader (below),
  performance overlay.
- **Sound** — Music 30%, Sound effects 100%, Movies 50% by default (music level matches the
  emulator balance between music and menu sounds).
- **Controls** — key / controller reference.
- **Save** — the save slots (below) and the Autosave switch.
- **System** — Loading: Original CD speed (default; loading screens last as long as on a PS1) or Fast.

The game logic always runs at the PS1's 59.94 Hz (it counts frames), whatever the display refresh.
Saved to `~/Library/Application Support/PortGamePC/Brigandine/settings.cfg` (Windows: `%APPDATA%`).
Environment variables `BRIG_BGM_VOLUME`, `BRIG_SFX_VOLUME`, `BRIG_MOVIE_VOLUME` (percent),
`BRIG_VSYNC` (0/1), `BRIG_FRAMESKIP` (0–4) and `BRIG_FAST_LOAD` (0/1) override the file.

Debug env vars: `BRIG_TRACE=1` (per-second GPU stats), `BRIG_SHOT_DIR=<dir> BRIG_SHOT_EVERY=<n>`
(BMP snapshots), `BRIG_INPUT="frame:hexmask,..."` (scripted pad input), `BRIG_SHOW_VRAM=1`,
`BRIG_CDTRACE=1` (CD commands), `BRIG_SNDTRACE=1` (libsnd calls + active SPU voices), `BRIG_WAV=<file.wav>`
(record audio output), `BRIG_MUTE_XA=1` (SPU only), `BRIG_REVERB_ONLY=1` (reverb wet signal only), `BRIG_MUTE_SPU=1` (XA only), `BRIG_KEYS="frame:KeyName,..."` (scripted keyboard keys), `BRIG_FAST=1` (unthrottled), `BRIG_SKIP_MOVIES=1`,
`BRIG_SHOT_FROM`/`BRIG_SHOT_TO` (snapshot range), `BRIG_SLOW_MS=<n>` (simulate a slow machine), `BRIG_UI_TAB=<0-4>`, `BRIG_STATE_DIR=<dir>` (save-slot folder, for tests), `BRIG_SHADER=<name>`, `BRIG_SHADER_CFG=<file>`, `BRIG_SCALE=<1-8>` (internal resolution), `BRIG_NO_CAPTURE=1` / `BRIG_FAKE_CAPTURE=1` (scripted mouse tests: OS pointer / relative motion `Rel=dx;dy`, buttons `LClick` / `RBtn` / `MBtn` / `CClick` = Ctrl+click), `BRIG_POKE="frame:addr=value"` (write RAM), `BRIG_RAMDUMP="frame,..."` (2 MB RAM to `BRIG_DEBUG_DIR/ram_<frame>.bin`). `F10` (hardware renderer only) writes renderer diagnostics to
`<pref>/debug/` (scaled vs software VRAM images) and `states/debug.state` (`BRIG_DEBUG_DIR` overrides the folder).

## Renderer (internal resolution)
`F2` → **Graphics** → **Internal resolution**:
- **Native** (default) — the software renderer, pixel-exact PS1 picture (320×240, dithered).
- **2x … 8x** — the hardware renderer: the graphics card draws every polygon, sprite and line
  again at N× resolution (sharper polygon edges and 3D, smooth colour gradients without dither
  noise). Textures, palettes and all four PS1 semi-transparency modes are reproduced; the software
  renderer keeps running underneath as the reference VRAM, so the game logic sees no difference.
  Movies are shown from the software image. Measured cost on an Apple M1: about +1 ms per frame at 4x, +2.5 ms at 8x.
- Scanline / CRT shaders are designed around 240 lines and look best at Native.

## Display shaders
The image is drawn with OpenGL 3.3 through a shader, chosen in `F2` → **Graphics**:
- **Sharp** (default) — crisp pixels with even widths at any window size.
- **Pixel** — raw nearest-neighbour. **Smooth** — bilinear.
- **Scanlines** — TV-style line gaps. **CRT** — curved screen, scanlines, shadow mask, glow.

Each shader's `#pragma parameter` values appear as sliders and are saved in `shaders.cfg`.
**Your own shaders:** shaders use RetroArch's single-pass `.glsl` format (one file with `VERTEX` /
`FRAGMENT` sections), so most single-pass shaders from the RetroArch / libretro GLSL collection work
as they are. Put them in `~/Library/Application Support/PortGamePC/Brigandine/shaders/` (the
**Open shaders folder** button) or in `shaders/` next to the executable, then press **Reload**.
Multi-pass presets (`.glslp`) are not supported yet.

## Quick save / load (save anywhere)
The port's own save system works at any moment — on the map, in menus, in the middle of a battle —
and loads instantly, without going back to the title screen.
- `F5` saves to the selected slot, `F9` loads it (controller: hold Back + LB / RB). `Left Shift` + `←` / `→` (controller: hold Back + D-pad
  ← / →; a Back tap alone is still Select) changes the slot (a message shows
  its date and in-game month); the selected slot is remembered. `F2` → **Save** shows 10 slots with a screenshot,
  date / time and the in-game year, month and phase; click a slot to select it, or use its
  Save / Load / Delete buttons.
- **Autosave** (on by default): 3 rotating slots, written when you finalize the Organize phase,
  when you finalize the Attack phase (new month), and when a battle starts.
- Files: `~/Library/Application Support/PortGamePC/Brigandine/states/` (`slot1..10.state`,
  `auto1..3.state`, about 3.7 MB each). They keep working after the app is restarted, but only with
  the same disc / patch and the same version of the port.
- These are a snapshot of the whole game, not the game's own save: to take a save to an emulator,
  also save to the memory card from the game's menu.

## Memory cards (save / load)
The game saves to virtual memory cards stored as **standard 128 KB raw PS1 card images** — the same
format as ePSXe `.mcr`, DuckStation `.mcd` and RetroArch/Beetle PSX `.srm` — so saves can be moved
between this port and emulators.
- Slot 1: `~/Library/Application Support/PortGamePC/Brigandine/memcard1.mcd` (Windows: `%APPDATA%\PortGamePC\Brigandine\`),
  created (formatted, empty) the first time the game accesses the card.
- Slot 2: `memcard2.mcd` in the same folder, used only if you put a card file there.
- To continue an emulator save: copy that 128 KB card file over `memcard1.mcd` (back yours up first).
- Every write goes to disk immediately.

## Layout
- `tools/recomp.py` — MIPS → C recompiler (function discovery, jump tables, patch trampolines, overlays)
- `src/pc/` — runtime (`rt.c`), fallback interpreter (`interp.c`), GTE (`gte.c`), software GPU (`gpu.c`),
  audio mixer + XA-ADPCM (`audio.c`), SPU synth + VAB/SEQ player + reverb (`spu.c`),
  STR movie player (`movie.c`) + MDEC/BS v2 decoder (`mdec.c`), HLE (`hle_*.c`), SDL (`platform.c`)
- `src/pc/hle_list.txt` — which PsyQ/game functions are replaced natively
- `decomp/` — splat configs, symbols, struct headers, first hand-decompiled C
- `docs/` — plan, disc layout, findings
- `tools/seq_render.c` — offline renderer for the game's music (A/B against recordings)

## Third-party assets
- `assets/fonts/DotGothic16-Regular.ttf` — [DotGothic16](https://github.com/fontworks-fonts/DotGothic16)
  (Google Fonts), © The DotGothic16 Project Authors, SIL Open Font License 1.1
  (`assets/fonts/OFL-DotGothic16.txt`). Used to draw the text the English patch renders through the
  PS1 BIOS font.
- Dear ImGui (MIT) is fetched by CMake at build time.

## License
The code in this repository is released under the [MIT License](LICENSE).
It covers only this project's own code and tools — not the game, which you must own.
