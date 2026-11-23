/* Brigandine GE native port — entry point.
 * usage: brigandine <path to Disc 1 .bin> [Disc 2 .bin]  (Disc 2 is found next to Disc 1 when omitted) */
#include "hle.h"
#include "gpu.h"
#include "audio.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>

void f_main_8006C4EC(Ctx *c); /* __SN_ENTRY_POINT */

static Ctx g_ctx;
u32 g_exe_crc;   /* CRC32 of the loaded EXE: save states only load on the same disc/patch */

static int load_exe(void) {
    u32 lba, size;
    /* the EXE of the disc in the drive: Disc 2's differs in 5 bytes (current disc 0x800A5FCC and two
     * per-disc LBAs), so the game knows which disc it booted from */
    if (!disc_find_file(disc_current() ? "SLPS_026.62" : "SLPS_026.61", &lba, &size) &&
        !disc_find_file("SLPS_026.61", &lba, &size) && !disc_find_file("SLPS_026.62", &lba, &size)) {
        fprintf(stderr, "EXE not found on disc\n");
        return 0;
    }
    u8 *buf = malloc(size + 2048);
    for (u32 i = 0; i < (size + 2047) / 2048; i++) disc_read_sector(lba + i, buf + i * 2048);
    if (memcmp(buf, "PS-X EXE", 8) != 0) { fprintf(stderr, "bad EXE header\n"); return 0; }
    u32 t_addr, t_size;
    memcpy(&t_addr, buf + 0x18, 4);
    memcpy(&t_size, buf + 0x1C, 4);
    memcpy(ram_ptr(t_addr), buf + 0x800, t_size);
    /* the CRC is taken as if it were Disc 1's EXE, so save states load whichever disc was booted */
    static const struct { u32 off; u8 d1, d2; } disc_bytes[] = {
        { 0x5C7DC, 198, 200 }, { 0x5C7DD, 224, 39 }, { 0x5C94C, 170, 113 }, { 0x5C94D, 114, 117 }, { 0x95FCC, 0, 1 },
    };
    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < t_size; i++) {
        u8 v = buf[0x800 + i];
        for (unsigned k = 0; k < sizeof disc_bytes / sizeof disc_bytes[0]; k++)
            if (i == disc_bytes[k].off && v == disc_bytes[k].d2) v = disc_bytes[k].d1;
        crc ^= v;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    g_exe_crc = ~crc;
    free(buf);
    return 1;
}

int main(int argc, char **argv) {
    if (getenv("BRIG_KANJI_TEST")) { void kanji_selftest(void); kanji_selftest(); return 0; }   /* debug */
    if (argc < 2) {
        fprintf(stderr, "usage: %s <Brigandine GE Disc 1 .bin> [Disc 2 .bin]\n", argv[0]);
        return 1;
    }
    settings_load();   /* before the disc (the last disc is booted) and the window (VSync / frame skip) */
    if (!disc_open(argv[1])) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    if (argc > 2 && !disc_open_second(argv[2])) fprintf(stderr, "cannot open %s (Disc 2)\n", argv[2]);
    if (!load_exe()) return 1;
    if (!platform_init("Brigandine: Grand Edition")) { fprintf(stderr, "SDL init failed\n"); return 1; }
    if (!audio_init()) fprintf(stderr, "audio init failed (continuing silent)\n");
    gpu_reset();
    g_ctx.r[29] = 0x801FFF00u;
    void interp_run(Ctx *c, u32 pc);
    if (setjmp(g_resume_jb) == 0) {
        f_main_8006C4EC(&g_ctx);                      /* normal boot */
    } else {
        /* resumed (game longjmp or save-state load): run the interrupted guest function to
         * its return, then keep unwinding through the callers' saved return addresses */
        for (;;) {
            interp_run(&g_ctx, g_resume_pc);
            g_resume_pc = g_ctx.r[31];
        }
    }
    fprintf(stderr, "entry point returned\n");
    return 0;
}
