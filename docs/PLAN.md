# Brigandine: Grand Edition — Native PC Port

Mục tiêu: bản PC chạy native (macOS / Windows / Linux), không giả lập, dùng dữ liệu từ đĩa của chính người chơi
(bản J + English patch v8). Repo chỉ chứa code và công cụ, **không** chứa file game.

## Chiến lược: Decompile + thay lớp phần cứng (kiểu REDRIVER2 / PsyCross)

```
 ISO (đã patch EN) ──► tách file ──► EXE + overlay ──► Ghidra/splat ──► C (m2c + sửa tay)
                                                                         │
                     PsyQ libs (libgpu/libgte/libspu/libcd/libpad)  ◄────┘ thay bằng
                     PC backend: SDL2 + OpenGL + đọc file từ ISO
```

### Giai đoạn
| # | Việc | Đầu ra |
|---|------|--------|
| 0 | ✅ Toolchain + skeleton | `.venv` (splat64, spimdisasm, m2c), mipsel binutils, cmake, SDL2 (Ghidra: chưa cài — brew build JDK từ source quá lâu, sẽ dùng bản zip + JDK dựng sẵn) |
| 1 | ✅ Tách ISO, liệt kê file, xác định EXE (`SLPS_026.61`), overlay, định dạng dữ liệu | `docs/disc_layout.md` |
| 2 | ✅ (phần lớn) Nhận diện hàm PsyQ (signature), đặt tên biến bằng điểm neo từ cheat | Ghidra project, `symbols.txt` |
| 3 | splat config, disassemble toàn bộ; build lại EXE khớp byte (matching) để kiểm chứng | `make` ra EXE giống hệt gốc |
| 4 | Decompile dần sang C: ưu tiên main loop, file I/O, render, input | `src/game/*.c` |
| 5 | Backend PC: GPU (OT/primitives → OpenGL), GTE (phần mềm), SPU/XA/SEQ (âm thanh), CD (đọc ISO), pad (SDL) | `src/pc/*` |
| 6 | Chạy được màn tiêu đề → bản đồ chiến lược → trận đánh → toàn game | Bản native |
| 7 | Cải tiến: độ phân giải cao, widescreen, 60fps UI, phím tắt, save file | |

### Điểm neo RAM từ bộ cheat (docs/cheats)
Bộ cheat do người dùng tạo (qua một AI agent khác) cho ePSXe (`.CT`, Cheat Engine) và RetroArch (`.cht`, core PSX).
Địa chỉ trong `.cht` là địa chỉ RAM PS1 thật nên dùng trực tiếp được cho reverse.
Code GameShark `30XXXXXX` = ghi 8-bit tại `0x80XXXXXX`.
- Túi đồ (potion…): `0x800ADD40` – `0x800ADDE5`…
- Cheat Engine table (ePSXe) có: `char`, `basechar`, `basemonster_<nước>` x6 (bước 0xC08), `basekingdom`, `baseyear`.
  Cần quy đổi offset ePSXe → địa chỉ PSX (giả thuyết base RAM ePSXe 2.0.5 = `ePSXe.exe+A82020`
  ⇒ `char ≈ 0x800973B4`, `basemonster_almekia ≈ 0x800A79D8`) — **chưa kiểm chứng**, sẽ xác nhận khi có ISO/savestate.

### Rủi ro chính
- Overlay nhiều (chiến lược / trận đánh / FMV) → splat config phức tạp.
- Patch EN có thể sửa EXE (font/VWF/pointer) → decomp trực tiếp bản đã patch.
- Định dạng FMV (STR/MDEC) và nhạc (XA, SEQ/VAB) cần decoder riêng ở backend.

## Cấu trúc thư mục
```
iso/         # ĐẶT ISO VÀO ĐÂY (git-ignored)
extracted/   # file tách từ đĩa (git-ignored)
tools/       # script phân tích
docs/        # tài liệu, cheat, bản đồ đĩa
src/         # code port (game/ = decomp, pc/ = backend)
```

## Cập nhật 2026-11-18 — đổi chiến lược: Recompile + HLE (đã chạy được)
Thay vì chờ decompile tay ~870 hàm, toàn bộ code MIPS (EXE + 5 overlay + PsyQ) được **recompile tĩnh sang C**
(`tools/recomp.py`), thư viện chạm phần cứng được **thay bằng code native** (`src/pc/hle_*.c`, danh sách trong
`src/pc/hle_list.txt`). Decompile tay vẫn tiếp tục song song để code dễ đọc/sửa.

**Đã chạy native trên macOS:** màn E3 Staff → title → menu → chọn chế độ → chọn lãnh chúa → bản đồ chiến lược
(lời dẫn mở đầu). Chữ tiếng Anh hiển thị đúng.

Phát hiện quan trọng khi làm:
- Patch tiếng Anh chèn code vào `0x8006C620–0x8006FC00` và dùng **trampoline** (code gốc `j` sang patch, patch `j`
  ngược vào giữa hàm gốc) → recompiler hỗ trợ "secondary entry" (22 điểm trong EXE).
- Nhạc nền là **XA stream**; màn title chờ vị trí đầu đọc (CdlGetlocL) vượt 35 sector → HLE CD có đầu đọc ảo theo thời gian thật.
- `CdSync` = `0x80066F1C`, `CdLastCom` = `0x80066ECC` (không có trong chữ ký PsyQ).
- Trình phát phim game = `strNext` `0x8004519C` (đang HLE bỏ qua).
- Nút xác nhận là ○ (game Nhật).

### Việc tiếp theo (theo thứ tự đề xuất)
1. ✅ Âm thanh: XA-ADPCM (nhạc CD, stream theo đầu đọc ảo) + SPU mềm (VAG/ADSR/pitch) + VAB/SEQ (libsnd HLE). Reverb: thuật toán xấp xỉ (Freeverb) theo loại preset; game đặt Hall(5)/type 4 depth ~60 qua SsUtSetReverbType + NRPN 16/15,16/16 trong SEQ. Vibrato/portamento: không cần — không tone/SEQ nào dùng (đã quét 46 VAB, 31 SEQ).
2. ✅ Phim: `strNext` thay bằng trình phát native (movie.c): STR v2 320x224/320x192, MDEC mềm, XA lồng tiếng, 150 sector/s, START để bỏ qua, letterbox 240 dòng.
3. Memory card → file save.
4. Chơi thử sâu: trận đánh (OVER2), các sự kiện, đổi đĩa 2.
5. Renderer OpenGL độ phân giải cao, widescreen.
6. Build Windows (mingw-w64) để test.
7. Decompile tay dần các hệ thống chính (thay code recompile bằng C dễ đọc).
