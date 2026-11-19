/* Audio output: SDL device + mixer. Sources: XA-ADPCM stream (CD audio) and the
 * SPU synth (spu.c). Sample rate 44100 Hz stereo s16. */
#include "audio.h"
#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

#define OUT_RATE 44100
#define XA_RING (OUT_RATE * 2)   /* 2 s of stereo frames */

static SDL_AudioDeviceID g_dev;
static s16 g_xa_ring[XA_RING * 2];
static _Atomic u32 g_xa_w, g_xa_r;     /* frame indices; w published with release, read with acquire */
static volatile int g_cd_vol_l = 127, g_cd_vol_r = 127;
volatile u32 g_stat_xa_underruns, g_stat_clipped;   /* debug counters */
/* BRIG_BGM_VOLUME / BRIG_SFX_VOLUME (percent). Music default 30%: matches the music/menu-sound
 * balance of the game on a PS1 emulator (compared by ear by the user). */
float g_bgm_gain = 0.30f, g_sfx_gain = 1.0f, g_movie_gain = 0.50f;   /* set from settings.c */
static volatile int g_xa_movie;                       /* movie audio uses its own volume, not music */
void audio_set_xa_movie(int on) { g_xa_movie = on; }

void spu_render(s32 *mix, int frames, int rate); /* spu.c */

/* debug: BRIG_WAV=<path> records the mixed output */
static FILE *g_wav;
static u32 g_wav_frames;
static void wav_header(void) {
    u32 data = g_wav_frames * 4, riff = 36 + data, fmt = 16, rate = OUT_RATE, br = OUT_RATE * 4;
    u16 pcm = 1, ch = 2, ba = 4, bits = 16;
    fseek(g_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav); fwrite("WAVEfmt ", 1, 8, g_wav);
    fwrite(&fmt, 4, 1, g_wav); fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav); fwrite(&rate, 4, 1, g_wav);
    fwrite(&br, 4, 1, g_wav); fwrite(&ba, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 1, 4, g_wav); fwrite(&data, 4, 1, g_wav);
    fseek(g_wav, 0, SEEK_END);
}
static void wav_write(const s16 *p, int frames) {
    if (!g_wav) return;
    fwrite(p, 4, (size_t)frames, g_wav);
    g_wav_frames += frames;
    if ((g_wav_frames & 0xFFFF) < (u32)frames) wav_header();
}

static void callback(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int frames = len / 4;
    {   /* debug: how far ahead of real time we have handed audio to the OS (= queued latency) */
        static Uint64 t0, produced; static Uint32 last_log; static int trace = -1;
        if (trace < 0) trace = getenv("BRIG_TRACE") != NULL;
        Uint64 now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
        if (!t0) t0 = now;
        double ahead_ms = produced * 1000.0 / OUT_RATE - (double)(now - t0) * 1000.0 / f;
        produced += frames;
        if (trace && SDL_GetTicks() - last_log > 1000) {
            last_log = SDL_GetTicks();
            rt_log("[audio] callback %d frames, queued ahead of real time: %.1f ms", frames, ahead_ms + frames * 1000.0 / OUT_RATE);
        }
    }
    s16 *out = (s16 *)stream;
    static s32 mix[4096 * 2];
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        memset(mix, 0, (size_t)n * 2 * sizeof(s32));
        /* XA: wait for ~150 ms of data before (re)starting, so bursty sector delivery never underruns */
        static int xa_playing;
        u32 xw = atomic_load_explicit(&g_xa_w, memory_order_acquire);
        u32 xr = atomic_load_explicit(&g_xa_r, memory_order_relaxed);
        u32 avail = xw - xr;
        if (!xa_playing && avail >= OUT_RATE * 15 / 100) xa_playing = 1;
        if (avail == 0 && xa_playing) { xa_playing = 0; g_stat_xa_underruns++; }
        static int mute_xa = -1;
        if (mute_xa < 0) mute_xa = getenv("BRIG_MUTE_XA") != NULL;
        float xg = g_xa_movie ? g_movie_gain : g_bgm_gain;
        for (int i = 0; xa_playing && i < n; i++) {
            if (xr != xw) {
                u32 r = xr % XA_RING;
                if (!mute_xa) {
                    /* /256: CD audio goes through the SPU mixer at the same 0.5 full-scale as voices,
                     * keeping music below menu sound effects as on hardware */
                    mix[i * 2] += (s32)(g_xa_ring[r * 2] * g_cd_vol_l / 256 * xg);
                    mix[i * 2 + 1] += (s32)(g_xa_ring[r * 2 + 1] * g_cd_vol_r / 256 * xg);
                }
                xr++;
            }
        }
        atomic_store_explicit(&g_xa_r, xr, memory_order_release);
        static int mute_spu = -1;
        if (mute_spu < 0) mute_spu = getenv("BRIG_MUTE_SPU") != NULL;   /* debug */
        if (!mute_spu) spu_render(mix, n, OUT_RATE);
        for (int i = 0; i < n * 2; i++) {
            s32 v = mix[i];
            if (v < -32768 || v > 32767) g_stat_clipped++;
            out[i] = (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        wav_write(out, n);
        out += n * 2;
        frames -= n;
    }
}

int audio_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return 0;
    const char *wp = getenv("BRIG_WAV");
    if (wp && (g_wav = fopen(wp, "wb"))) wav_header();
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE; want.format = AUDIO_S16SYS; want.channels = 2;
    want.samples = 512;   /* ~11.6 ms per callback: keeps sound-effect latency low */
    want.callback = callback;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) return 0;
    if (getenv("BRIG_TRACE")) rt_log("[audio] device %d Hz, %d frames per callback (%.1f ms)", have.freq, have.samples, have.samples * 1000.0 / have.freq);
    SDL_PauseAudioDevice(g_dev, 0);
    return 1;
}

void audio_lock(void) { if (g_dev) SDL_LockAudioDevice(g_dev); }
void audio_unlock(void) { if (g_dev) SDL_UnlockAudioDevice(g_dev); }

void audio_set_cd_volume(int l, int r) { g_cd_vol_l = l; g_cd_vol_r = r; }

/* Push decoded XA PCM (stereo interleaved, at src_rate) resampled to OUT_RATE. */
void audio_push_xa(const s16 *pcm, int frames, int src_rate) {
    static double pos;      /* fractional read position carried across calls */
    static s16 prev[2];
    double step = (double)src_rate / OUT_RATE;
    u32 w0 = atomic_load_explicit(&g_xa_w, memory_order_relaxed);
    if (w0 - atomic_load_explicit(&g_xa_r, memory_order_acquire) > XA_RING - 8192) return; /* overrun: drop */
    u32 wi = w0;
    while (pos < frames) {
        int i = (int)pos;
        double f = pos - i;
        s16 l0 = i == 0 ? prev[0] : pcm[(i - 1) * 2], r0 = i == 0 ? prev[1] : pcm[(i - 1) * 2 + 1];
        s16 l1 = pcm[i * 2], r1 = pcm[i * 2 + 1];
        u32 w = wi % XA_RING;
        g_xa_ring[w * 2] = (s16)(l0 + (l1 - l0) * f);
        g_xa_ring[w * 2 + 1] = (s16)(r0 + (r1 - r0) * f);
        wi++;
        pos += step;
    }
    atomic_store_explicit(&g_xa_w, wi, memory_order_release);   /* publish only after the samples are written */
    pos -= frames;
    prev[0] = pcm[(frames - 1) * 2]; prev[1] = pcm[(frames - 1) * 2 + 1];
}

void audio_flush_xa(void) {
    audio_lock();
    atomic_store(&g_xa_r, atomic_load(&g_xa_w));
    audio_unlock();
}

/* ------------------------------------------------------------------ XA-ADPCM decoder */
static const int K0[4] = { 0, 60, 115, 98 };
static const int K1[4] = { 0, 0, -52, -55 };
static s32 g_hist[2][2];

void xa_reset(void) { memset(g_hist, 0, sizeof g_hist); }

static inline s16 clamp16(s32 v) { return (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* raw: full 2352-byte sector. Returns frames written to out (stereo interleaved). */
int xa_decode_sector(const u8 *raw, s16 *out, int *rate) {
    u8 coding = raw[19];
    int stereo = coding & 1, half = (coding >> 2) & 1, bits8 = (coding >> 4) & 1;
    *rate = half ? 18900 : 37800;
    const u8 *data = raw + 24;
    s16 mono[18 * 28 * 8];
    int n_l = 0, n_r = 0, n_m = 0;
    static s16 left[18 * 28 * 4], right[18 * 28 * 4];
    for (int g = 0; g < 18; g++) {
        const u8 *grp = data + g * 128;
        int units = bits8 ? 4 : 8;
        for (int u = 0; u < units; u++) {
            u8 h = grp[4 + u];
            int shift = h & 15, filt = (h >> 4) & 3;
            if (shift > 12) shift = 9;
            int ch = stereo ? (u & 1) : 0;
            s32 *hs = g_hist[ch];
            for (int j = 0; j < 28; j++) {
                s32 s;
                if (bits8) s = (s32)(s8)grp[16 + j * 4 + u] << 8;
                else {
                    u8 b = grp[16 + j * 4 + u / 2];
                    s = (s32)(s16)((((u & 1) ? b >> 4 : b) & 15) << 12);
                }
                s >>= shift;
                s += (hs[0] * K0[filt] + hs[1] * K1[filt] + 32) / 64;
                s16 v = clamp16(s);
                hs[1] = hs[0]; hs[0] = v;
                if (!stereo) mono[n_m++] = v;
                else if (ch == 0) left[n_l++] = v;
                else right[n_r++] = v;
            }
        }
    }
    if (stereo) {
        int n = n_l < n_r ? n_l : n_r;
        for (int i = 0; i < n; i++) { out[i * 2] = left[i]; out[i * 2 + 1] = right[i]; }
        return n;
    }
    for (int i = 0; i < n_m; i++) { out[i * 2] = out[i * 2 + 1] = mono[i]; }
    return n_m;
}
