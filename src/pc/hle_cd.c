/* HLE for libcd.
 * Data is copied immediately, but CdReadSync reports "busy" until the time a real
 * 2x drive would need (seek + 150 sectors/s), so loading screens (castle name,
 * "Now Loading") stay up as long as on hardware. A virtual read head advances in
 * real time while a ReadN/ReadS streams (XA music polls it with CdlGetlocL). */
#include "hle.h"
#include "state.h"
#include "audio.h"
#include "settings.h"
#include <stdlib.h>

static u32 g_cd_pos;          /* LBA of the next sector the head will read */
static u8  g_cd_mode;
static int g_cd_reading;      /* 1 while ReadN/ReadS streams */
static u32 g_cd_frac;         /* sub-sector accumulator, units of 1/60 sector */
static u8  g_cd_lastcom;
static u8  g_xa_file, g_xa_chan;  /* CdlSetfilter */
static u8  g_cd_result[8];
static u32 g_head;            /* where the drive head is after the last data read */
static u32 g_read_done_vb;    /* vblank count at which the pending CdRead completes */
static u32 g_read_last_poll;  /* vblank count of the previous CdReadSync poll */
static int g_read_sectors_per_vb10;   /* sectors per vblank x10 (25 at 2x, 12 at 1x) */

static int bcd2i(u8 b) { return (b >> 4) * 10 + (b & 15); }
static u8 i2bcd(int i) { return (u8)(((i / 10) << 4) | (i % 10)); }

static u32 loc_to_lba(u32 a) {
    return (u32)((bcd2i(rd8(a)) * 60 + bcd2i(rd8(a + 1))) * 75 + bcd2i(rd8(a + 2)) - 150);
}

static void lba_to_bcd(u32 lba, u8 *out) {
    u32 l = lba + 150;
    out[0] = i2bcd(l / 75 / 60); out[1] = i2bcd((l / 75) % 60); out[2] = i2bcd(l % 75);
}

/* Called once per vblank from hle_etc.c */
void cd_vblank(void) {
    if (!g_cd_reading) return;
    g_cd_frac += (g_cd_mode & 0x80) ? 150 : 75;
    while (g_cd_frac >= 60) {
        g_cd_frac -= 60;
        if (g_cd_mode & 0x40) {  /* XA-ADPCM playback enabled */
            u8 raw[2352];
            disc_read_raw(g_cd_pos, raw);
            u8 file = raw[16], chan = raw[17], sub = raw[18];
            int match = !(g_cd_mode & 0x08) || (file == g_xa_file && chan == g_xa_chan);
            if ((sub & 0x24) == 0x24 && match) {   /* form2 audio sector */
                static s16 pcm[18 * 28 * 8 * 2];
                int rate, n = xa_decode_sector(raw, pcm, &rate);
                if (n > 0) audio_push_xa(pcm, n, rate);
            }
        }
        g_cd_pos++;
    }
}

static void cd_command(Ctx *c) {
    u32 com = c->r[4] & 0xFF, param = c->r[5], result = c->r[6];
    memset(g_cd_result, 0, sizeof g_cd_result);
    g_cd_result[0] = g_cd_reading ? 0x22 : 0x02; /* stat: motor on (+reading) */
    switch (com) {
    case 0x02: if (param) g_cd_pos = loc_to_lba(param); break;                 /* Setloc */
    case 0x0E: if (param) g_cd_mode = (u8)rd8(param); break;                   /* Setmode */
    case 0x0D: if (param) { g_xa_file = (u8)rd8(param); g_xa_chan = (u8)rd8(param + 1); } break; /* Setfilter */
    case 0x06: case 0x1B:                                                      /* ReadN / ReadS */
        if (param) g_cd_pos = loc_to_lba(param);
        g_cd_reading = 1; g_cd_frac = 0; xa_reset(); break;
    case 0x09: case 0x08: g_cd_reading = 0; audio_flush_xa(); break;           /* Pause / Stop */
    case 0x10: {                                                               /* GetlocL */
        u8 sec[2048];
        lba_to_bcd(g_cd_pos, g_cd_result);
        g_cd_result[3] = 2;                                                    /* mode 2 */
        (void)sec;
        break;
    }
    case 0x11: {                                                               /* GetlocP */
        u8 b[3]; lba_to_bcd(g_cd_pos, b);
        g_cd_result[0] = 1; g_cd_result[1] = 1;
        g_cd_result[2] = b[0]; g_cd_result[3] = b[1]; g_cd_result[4] = b[2];
        g_cd_result[5] = b[0]; g_cd_result[6] = b[1]; g_cd_result[7] = b[2];
        break;
    }
    case 0x03: rt_log("CdlPlay (CD-DA) ignored"); break;
    default: break;
    }
    if (getenv("BRIG_CDTRACE") && com != 0x10 && com != 0x01)
        rt_log("[cd] vb=%u com=%02X mode=%02X pos=%u reading=%d", g_vblank_count, com, g_cd_mode, g_cd_pos, g_cd_reading);
    g_cd_lastcom = (u8)com;
    if (result) for (int i = 0; i < 8; i++) wr8(result + i, g_cd_result[i]);
    c->r[2] = 1;
}

void hle_CdInit(Ctx *c) { c->r[2] = 1; }
void hle_CdControl(Ctx *c) { cd_command(c); }
void hle_CdControlB(Ctx *c) { cd_command(c); }
void hle_CdControlF(Ctx *c) { cd_command(c); }
void hle_CdSync(Ctx *c) {
    if (c->r[5]) for (int i = 0; i < 8; i++) wr8(c->r[5] + i, g_cd_result[i]);
    c->r[2] = 2; /* CdlComplete */
}
void hle_CdLastCom(Ctx *c) { c->r[2] = g_cd_lastcom; }
void hle_CdReady(Ctx *c) { c->r[2] = 2; }
void hle_CdDiskReady(Ctx *c) { c->r[2] = 2; }
void hle_CdGetDiskType(Ctx *c) { c->r[2] = 2; /* PlayStation disc */ }
void hle_CdPlay(Ctx *c) { c->r[2] = 0; }

void hle_CdRead(Ctx *c) {
    int n = (int)c->r[4];
    u32 buf = c->r[5];
    u8 sec[2048];
    for (int i = 0; i < n; i++) {
        disc_read_sector(g_cd_pos + i, sec);
        memcpy(ram_ptr(buf + i * 2048), sec, 2048);
    }
    /* timing of a real drive: seek (distance based, ~0.1-0.4 s) + transfer */
    u32 start = g_cd_pos;
    u32 dist = start > g_head ? start - g_head : g_head - start;
    int speed2x = (g_cd_mode & 0x80) != 0;
    g_read_sectors_per_vb10 = speed2x ? 25 : 12;
    u32 seek_vb = (dist < 8) ? 0 : 6 + dist * 18 / 330000;
    u32 xfer_vb = (u32)(n * 10 + g_read_sectors_per_vb10 - 1) / (u32)g_read_sectors_per_vb10;
    g_read_done_vb = g_settings.fast_load ? g_vblank_count : g_vblank_count + seek_vb + xfer_vb;
    g_read_last_poll = g_vblank_count - 1;
    g_head = start + n;
    g_cd_pos += n;
    g_cd_reading = 0;
    g_cd_lastcom = 0x06;
    if (getenv("BRIG_CDTRACE")) rt_log("[cd] vb=%u CdRead %d sectors @%u: seek %u + transfer %u vblanks", g_vblank_count, n, start, seek_vb, xfer_vb);
    c->r[2] = 1;
}

void hle_CdRead2(Ctx *c) { rt_log("CdRead2 (streaming) mode=%X — not implemented", c->r[4]); c->r[2] = 1; }

/* CdReadSync(mode, result): 0 = done, >0 = sectors still to come. mode 0 blocks.
 * A game spinning on CdReadSync(1) without calling VSync still gets vblanks (display,
 * callbacks, input) because each busy poll advances one vblank, like the hardware's
 * VSync interrupt during a read. */
void hle_CdReadSync(Ctx *c) {
    u32 mode = c->r[4];
    if (c->r[5]) wr8(c->r[5], 0x02);
    for (;;) {
        if ((s32)(g_vblank_count - g_read_done_vb) >= 0) { c->r[2] = 0; return; }
        if (mode == 0 || g_vblank_count == g_read_last_poll) {
            Ctx saved = *c;
            hle_vblank_tick(c);      /* let a frame pass */
            *c = saved;
        }
        g_read_last_poll = g_vblank_count;
        if (mode != 0) {
            s32 left_vb = (s32)(g_read_done_vb - g_vblank_count);
            if (left_vb <= 0) { c->r[2] = 0; return; }
            c->r[2] = (u32)((left_vb * g_read_sectors_per_vb10 + 9) / 10);
            return;
        }
    }
}

/* CdlFILE { CdlLOC pos; u32 size; char name[16]; } */
void hle_CdSearchFile(Ctx *c) {
    u32 fp = c->r[4];
    char name[64] = {0};
    for (int i = 0; i < 63; i++) { name[i] = (char)rd8(c->r[5] + i); if (!name[i]) break; }
    u32 lba, size;
    if (!disc_find_file(name, &lba, &size)) {
        rt_log("CdSearchFile(%s): not found", name);
        c->r[2] = 0;
        return;
    }
    u8 b[3]; lba_to_bcd(lba, b);
    wr8(fp + 0, b[0]); wr8(fp + 1, b[1]); wr8(fp + 2, b[2]); wr8(fp + 3, 0);
    wr32(fp + 4, size);
    for (int i = 0; i < 16; i++) wr8(fp + 8 + i, i < (int)strlen(name) ? (u8)name[i] : 0);
    c->r[2] = fp;
}

/* save state */
void cd_state(StateIO *s) {
    ST(s, g_cd_pos); ST(s, g_cd_mode); ST(s, g_cd_reading); ST(s, g_cd_frac); ST(s, g_cd_lastcom);
    ST(s, g_xa_file); ST(s, g_xa_chan); ST(s, g_cd_result); ST(s, g_head); ST(s, g_read_done_vb);
    ST(s, g_read_last_poll); ST(s, g_read_sectors_per_vb10);
}
