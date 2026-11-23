/* Native replacement for the game's movie player strNext (0x8004519C).
 *   strNext(MovieDesc *m, int skippable) -> 1 if the player skipped it, else 0
 *   MovieDesc: +0 u32 start LBA, +6 u16 frame count, +0xC u8 CD (XA) volume
 * Reads the STR stream at the real 2x CD rate (150 sectors/s = 2.5 per vblank),
 * decodes video sectors with mdec.c and interleaved XA audio (voice + music) with audio.c. */
#include "hle.h"
#include "audio.h"
#include "mdec.h"
#include "state.h"
#include <stdlib.h>

#define MAX_W 640
#define MAX_H 480

static u8 g_bs[MAX_W * MAX_H * 2];
static u8 g_rgb[2][MAX_W * MAX_H * 3];

void hle_strNext(Ctx *c) {
    u32 desc = c->r[4];
    int skippable = c->r[5] != 0;
    u32 lba = rd32(desc);
    int frames = (int)rd16(desc + 6);
    int vol = (int)rd8(desc + 12);
    if (getenv("BRIG_TRACE")) rt_log("[movie] lba=%u frames=%d vol=%d skippable=%d", lba, frames, vol, skippable);
    if (getenv("BRIG_SKIP_MOVIES")) { c->r[2] = 0; return; }   /* debug: stable timing for scripted tests */

    audio_flush_xa();
    xa_reset();
    audio_set_xa_movie(1);
    audio_set_cd_volume(vol ? vol : 127, vol ? vol : 127);

    u32 sec = lba;
    int cur_frame = -1, have = 0, need = 0, w = 0, h = 0, shown = -1, done = 0, skipped = 0, idle = 0;
    int xa_file = -1, xa_chan = -1, frac = 0, front = 0, decoded_frames = 0;
    u16 prev_btn = platform_pad_buttons();
    int had_frame = 0;
    while (!done) {
        frac += 5;                                   /* 2.5 sectors per vblank */
        while (frac >= 2 && !done) {
            frac -= 2;
            u8 raw[2352];
            if (!disc_read_raw(sec++, raw)) { done = 1; break; }
            u8 sub = raw[18];
            const u8 *d = raw + 24;
            if (sub & 0x04) {                        /* XA audio sector */
                if (xa_file < 0) { xa_file = raw[16]; xa_chan = raw[17]; }
                if (raw[16] == xa_file && raw[17] == xa_chan) {
                    static s16 pcm[18 * 28 * 8 * 2];
                    int rate, n = xa_decode_sector(raw, pcm, &rate);
                    if (n > 0) audio_push_xa(pcm, n, rate);
                }
                continue;
            }
            if (d[0] != 0x60 || d[1] != 0x01 || d[2] != 0x01 || d[3] != 0x80) {   /* not a video sector */
                if (++idle > 32) done = 1;
                continue;
            }
            idle = 0;
            int secn = d[4] | d[5] << 8, nsec = d[6] | d[7] << 8;
            int fr = d[8] | d[9] << 8 | d[10] << 16 | d[11] << 24;
            if (fr != cur_frame) { cur_frame = fr; have = 0; need = nsec; }
            w = d[16] | d[17] << 8; h = d[18] | d[19] << 8;
            if (w > MAX_W || h > MAX_H || secn >= 2 * MAX_W * MAX_H / 2016) continue;
            memcpy(g_bs + secn * 2016, d + 32, 2016);
            if (++have == need) {
                int fsize = d[12] | d[13] << 8 | d[14] << 16;
                if (mdec_decode_frame(g_bs, fsize, w, h, g_rgb[front ^ 1])) {
                    front ^= 1;
                    shown = fr;
                    had_frame = 1;
                    decoded_frames++;
                }
                if (frames > 0 && fr >= frames) done = 1;
            }
        }
        if (had_frame) platform_set_movie_frame(g_rgb[front], w, h);
        platform_vblank();
        if (platform_quit_requested()) exit(0);
        u16 btn = platform_pad_buttons();
        if (skippable && (btn & 0x08) && !(prev_btn & 0x08)) { skipped = 1; done = 1; }  /* Start */
        /* a quick load (F9 ...) happens at the game's next VSync, which never comes while a movie
         * plays: end the movie so the load is not held back until it finishes */
        if (state_load_pending()) { skipped = skippable; done = 1; }
        prev_btn = btn;
    }
    platform_set_movie_frame(NULL, 0, 0);
    audio_flush_xa();
    audio_set_xa_movie(0);
    if (getenv("BRIG_TRACE")) rt_log("[movie] end: %d frames decoded, last=%d, skipped=%d", decoded_frames, shown, skipped);
    c->r[2] = (u32)skipped;
}
