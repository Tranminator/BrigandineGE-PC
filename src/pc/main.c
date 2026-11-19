/* Brigandine GE native port — entry point.
 * usage: brigandine <path to Disc 1 .bin> */
#include "hle.h"
#include "gpu.h"
#include "audio.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>

void f_main_8006C4EC(Ctx *c); /* __SN_ENTRY_POINT */

static Ctx g_ctx;

static int load_exe(void) {
    u32 lba, size;
    if (!disc_find_file("SLPS_026.61", &lba, &size) && !disc_find_file("SLPS_026.62", &lba, &size)) {
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
    free(buf);
    return 1;
}

int main(int argc, char **argv) {
    if (getenv("BRIG_KANJI_TEST")) { void kanji_selftest(void); kanji_selftest(); return 0; }   /* debug */
    if (argc < 2) {
        fprintf(stderr, "usage: %s <Brigandine GE Disc 1 .bin>\n", argv[0]);
        return 1;
    }
    if (!disc_open(argv[1])) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    if (!load_exe()) return 1;
    settings_load();   /* before the window: VSync / frame skip apply at startup */
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
