/* Offline SEQ renderer using the port's own SPU synth (src/pc/spu.c).
 * Loads EXE + SE/VB.BIN (+ an overlay) from an extracted disc, plays one SEQ into a WAV.
 *
 * build: clang -O2 -Isrc/pc -o build/seq_render tools/seq_render.c src/pc/spu.c
 * usage: seq_render <extracted_disc_dir> <seq_ram_addr_hex> <overlay_file|FILE@offset|-> <seconds> <out.wav> [reverb 0/1]
 *   e.g. seq_render extracted/disc1 800BE830 OVER3.BIN 86 out.wav */
#include "spu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

u8 g_ram[RAM_SIZE];
u8 g_scratch[0x400];
u32 mem_rd_slow(u32 a, int size) { (void)a; (void)size; return 0; }
void mem_wr_slow(u32 a, u32 v, int size) { (void)a; (void)v; (void)size; }
void rt_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); }
float g_bgm_gain = 1.0f, g_sfx_gain = 1.0f;
void audio_lock(void) {}
void audio_unlock(void) {}
void spu_render(s32 *mix, int frames, int rate);

static long load(const char *path, u32 addr, long skip) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, skip, SEEK_SET);
    long n = (long)fread(ram_ptr(addr), 1, RAM_SIZE - (addr & (RAM_SIZE - 1)), f);
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s disc_dir seq_addr overlay|- seconds out.wav [reverb]\n", argv[0]); return 1; }
    char p[1024];
    snprintf(p, sizeof p, "%s/SLPS_026.61", argv[1]); load(p, 0x80010000u, 0x800);
    snprintf(p, sizeof p, "%s/SE/VB.BIN", argv[1]);   load(p, 0x80100000u, 0);
    u32 seq = (u32)strtoul(argv[2], NULL, 16);
    const char *at = strchr(argv[3], '@');
    if (at) {   /* "FILE@offset": load that file region at the SEQ address (e.g. SE/BGMSEQ.BIN@0x25800) */
        char fn[512]; snprintf(fn, sizeof fn, "%.*s", (int)(at - argv[3]), argv[3]);
        snprintf(p, sizeof p, "%s/%s", argv[1], fn); load(p, seq, strtol(at + 1, NULL, 0));
    } else if (strcmp(argv[3], "-")) { snprintf(p, sizeof p, "%s/%s", argv[1], argv[3]); load(p, 0x800B6838u, 0); }
    int secs = atoi(argv[4]);
    int reverb = argc > 6 ? atoi(argv[6]) : 1;
    int vab = spu_vab_open(0x8008B138u, 0x80100000u, 0);
    int id = spu_seq_open(seq, vab);
    if (!reverb) setenv("BRIG_NO_REVERB", "1", 1);
    if (getenv("BRIG_MONO")) spu_set_mono(1);   /* test the game's Sound: Mono option */
    spu_seq_play(id, 1, 0);
    FILE *o = fopen(argv[5], "wb");
    u32 frames = (u32)secs * 44100, data = frames * 4, riff = 36 + data, fmt = 16, rate = 44100, br = 44100 * 4;
    u16 pcm = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF", 1, 4, o); fwrite(&riff, 4, 1, o); fwrite("WAVEfmt ", 1, 8, o); fwrite(&fmt, 4, 1, o);
    fwrite(&pcm, 2, 1, o); fwrite(&ch, 2, 1, o); fwrite(&rate, 4, 1, o); fwrite(&br, 4, 1, o);
    fwrite(&ba, 2, 1, o); fwrite(&bits, 2, 1, o); fwrite("data", 1, 4, o); fwrite(&data, 4, 1, o);
    static s32 mix[1024 * 2];
    static s16 out[1024 * 2];
    for (u32 done = 0; done < frames; done += 1024) {
        memset(mix, 0, sizeof mix);
        spu_render(mix, 1024, 44100);
        for (int i = 0; i < 2048; i++) out[i] = (s16)(mix[i] < -32768 ? -32768 : mix[i] > 32767 ? 32767 : mix[i]);
        fwrite(out, 4, 1024, o);
    }
    fclose(o);
    return 0;
}
