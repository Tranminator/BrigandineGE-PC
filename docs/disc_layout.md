# Brigandine GE — Bản đồ đĩa (bản J + English Patch Subbed)

Nguồn: `iso/Disc 1`, `iso/Disc 2` (MODE2/2352, 1 track). Tách bằng `tools/extract_disc.py` → `extracted/disc1|disc2`.
Volume ID: `BRIGANDINE_GE`. File Form2 (XA/STR) được lưu **raw 2352 byte/sector** để giữ subheader.

## Kết luận nhanh
- Hai đĩa dùng **chung toàn bộ file code + dữ liệu**. Chỉ khác `SYSTEM.CNF`, tên EXE, `XA/MUSIC0.XA`, danh sách `MOVIE/*.STR`, file đệm `DUMMY*.DAT`.
- `SLPS_026.61` vs `SLPS_026.62`: khác **5 byte** (ID đĩa) → chỉ decompile **một** EXE.
- Bản patch EN **sửa trực tiếp EXE** (text mô tả tướng/quái nằm trong EXE) → decompile bản đã patch.
- PsyQ SDK: `sys.c 1998/01/12`, `bios.c 1997/03/28`, `intr.c 1997/02/07` → khoảng PsyQ 4.3–4.4. Có libmcrd (memory card).

## EXE chính `SLPS_026.61`
| Trường | Giá trị |
|---|---|
| Entry (pc0) | `0x8006C4EC` |
| Load addr | `0x80010000` |
| Text size | `0x96800` (kết thúc `0x800A6800`) |
| Stack | `0x801FFF00` (SYSTEM.CNF), header `0x801FFFF0` |
| gp | 0 (không dùng $gp-relative) |
| SHA1 | `20845e0a661ceb9cb07ddff41e053d29792f68ad` |

## Overlay — tất cả nạp tại `0x800B6838` (dùng chung 1 slot)
Word đầu = ID overlay (5..9), tiếp theo là bảng con trỏ.
| File | ID | Kích thước | Kết thúc | Vai trò |
|---|---|---|---|---|
| OVER1.BIN | 5 | 0x26590 | 0x800DCDC8 | Giai đoạn chiến lược (bản đồ, di chuyển/tấn công thành, chọn quân) |
| OVER2.BIN | 6 | 0x31D18 | 0x800E8550 | Trận đánh (hex, phép, AUTO, Death Match) |
| OVER3.BIN | 7 | 0x10350 | 0x800C6B88 | Chọn chế độ / multiplayer tournament (6/4/3 người, ULTRA) |
| OVER4.BIN | 8 | 0xC800  | 0x800C3038 | Phần phụ: Music Memories, xem phim, credits |
| OVER5.BIN | 9 | 0x9D5C  | 0x800C0594 | Kết thúc / lịch sử / thống kê |

Vùng `0x800A6800–0x800B6838` nằm giữa EXE và overlay → nhiều khả năng là BSS/biến toàn cục;
các địa chỉ cheat (`0x800A79D8…`, `0x800ADD40…`) rơi đúng vùng này.

## File dữ liệu (đoán theo tên — cần xác nhận định dạng)
| Nhóm | File | Ghi chú |
|---|---|---|
| Âm thanh | `SE/VB.BIN`, `SE/SE.BIN`, `SE/BGMSEQ.BIN` | VAB (sample) + SEQ (nhạc MIDI-like) + hiệu ứng |
| | `XA/MUSIC0.XA`, `MUSIC1.XA`, `VOICE.XA` | XA-ADPCM nhạc nền + lồng tiếng (VOICE 313 MB) |
| Video | `MOVIE/BUG*.STR`, `BRGBOSS*.STR` | MDEC/STR; đĩa 1 & 2 có bộ phim khác nhau |
| Bản đồ | `MAP/MAP.BIN` (8.8 MB), `SR_MAP.BIN`, `CELANM.BIN`, `CLOUD*.TIM` | Chiến trường hex + bản đồ chiến lược |
| Nhân vật | `ETC/ACT.BIN` (1.8 MB), `ETC/FACE.BIN` (2.2 MB), `ETC/SPR.BIN` | Animation trận đánh, chân dung, sprite |
| Màn hình | `ETC/TITLE.BIN`, `OPEN_BG.BIN`, `LORDSEL.BIN`, `WALL.TIM`, `VSFONT.TIM` | Tiêu đề, chọn lãnh chúa, font |
| Sự kiện | `ETC/EVENT.BIN`, `ENTRY0..5.BIN`, `QUEST*.BIN`, `DISPUTE*.BIN`, `DOWNFALL.BIN`, `RUIN.BIN` | Kịch bản theo 6 nước, quest |
| Hiệu ứng | `ETC/EFF.BIN`, `EFFECT.TIM` | Hiệu ứng phép |
| Khác | `ETC/VISUAL.BIN` (19 MB), `STAFF_X.BIN` (8 MB), `STAY.BIN`, `TUTORIAL.BIN`, `MULTI*.BIN` | |

## Bố cục EXE (đã xác định)
| Vùng | Nội dung | Ghi chú |
|---|---|---|
| `0x80010000–0x8004D05C` | **Code game** — 409 hàm (`decomp/asm/game.s`) | cần decompile |
| `0x8004D05C–0x8006FC00` | PsyQ 4.6/4.7 — 543 hàm, ~466 đã đặt tên bằng chữ ký (`asm/psyq.s`) | thay bằng backend PC |
| `0x8006FC00–0x800A6800` | Dữ liệu tĩnh (text, bảng, bảng tướng `0x800973B0`) | `asm/data/` |
| `0x800A6800–0x800B6838` | BSS / biến toàn cục (`$gp = 0x800A5FC8`) | |
| `0x800B6838–…` | Slot overlay (OVER1–5: 222/163/24/21/31 hàm) | cần decompile |

Tổng code riêng của game ≈ **870 hàm**.

## Hệ thống đã hiểu
- `main` = `0x8001A4E0` (gọi từ `__SN_ENTRY_POINT`). State machine: `D_800A67D0` = chế độ game, `D_800A67BC` = cờ toàn cục, `setjmp(&D_800A7978)` làm điểm reset.
- `LoadOverlay(n)` = `0x8001A6BC`: đọc OVER(n+1) **theo LBA** từ bảng `g_OverlayLba` (`0x8006C680`: 539, 616, 716, 749, 774) và `g_OverlaySize` (`0x8006C694`) qua `func_8001F924` (hàm đọc CD).
  ⇒ Backend CD trên PC phải hỗ trợ **đọc theo LBA** (map LBA → file qua `_manifest.json`, hoặc đọc thẳng ảnh đĩa).
- Thư viện dùng: LIBGPU, LIBGTE, LIBCD, LIBSND, LIBSPU, LIBPRESS (MDEC), LIBMCRD/LIBCARD, LIBPAD, LIBAPI, LIBETC, LIBC2.
- Struct đã biết (`decomp/include/brigandine.h`): `Kingdom` (0x9E byte, mảng 6 tại `0x800ADD38`), `Knight` (layout từ bảng CE).

## Trạng thái
- splat: `decomp/brigandine.yaml` (EXE) + `over1..5.yaml`. Chạy: `cd decomp && ../.venv/bin/splat split <yaml>`.
- Tên ký hiệu: `decomp/symbol_addrs.txt` (PsyQ + tên tự đặt).
- C đầu tiên: `decomp/src/main.c` (m2c thô, chưa matching).
