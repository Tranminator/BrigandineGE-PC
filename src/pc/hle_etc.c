/* HLE for libetc (VSync/callbacks), libpad, libc I/O, and temporary stubs for
 * libsnd / libmcrd / libpress / streaming until the real backends exist. */
#include "hle.h"
#include "state.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

u32 g_vblank_count;
void cd_vblank(void);
static u32 g_vsync_cb;
static u32 g_pad_buf[2];
static int g_in_vblank_cb;

static void call_guest(Ctx *c, u32 fn) {
    Ctx saved = *c;
    c->r[29] -= 0x100; /* keep the callback's frame clear of the caller's */
    rt_call(c, fn);
    *c = saved;
}

static void update_pads(void) {
    u16 pressed = platform_pad_buttons();
    u16 b = (u16)~pressed;
    if (pressed && getenv("BRIG_TRACE"))
        rt_log("[pad] vb=%u pressed=%04X buf=%08X game_btn(800A67A4)=%08X", g_vblank_count, pressed, g_pad_buf[0],
               rd32(0x800A67A4));
    if (g_pad_buf[0]) {
        wr8(g_pad_buf[0] + 0, 0x00);  /* ok */
        wr8(g_pad_buf[0] + 1, 0x41);  /* digital pad, 1 halfword */
        wr8(g_pad_buf[0] + 2, b & 0xFF);
        wr8(g_pad_buf[0] + 3, b >> 8);
    }
    if (g_pad_buf[1]) wr8(g_pad_buf[1], 0xFF); /* port 2: no controller */
}

#include "gpu.h"
extern int g_stat_otag, g_stat_pkts;
void hle_vblank_tick(Ctx *c) {
    if (getenv("BRIG_TRACE") && g_vblank_count % 60 == 0) {
        rt_log("[vb %u] otag=%d pkts=%d disp=(%d,%d %dx%d en=%d) area=(%d,%d)-(%d,%d) ofs=(%d,%d) ra=%08X",
               g_vblank_count, g_stat_otag, g_stat_pkts, g_gpu.disp_x, g_gpu.disp_y, g_gpu.disp_w, g_gpu.disp_h,
               g_gpu.disp_enabled, g_gpu.area_x0, g_gpu.area_y0, g_gpu.area_x1, g_gpu.area_y1, g_gpu.ofs_x, g_gpu.ofs_y,
               c->r[31]);
        g_stat_otag = g_stat_pkts = 0;
    }
    if (getenv("BRIG_SNDTRACE") && g_vblank_count % 60 == 0) { void spu_debug_dump(u32); spu_debug_dump(g_vblank_count); }
    platform_vblank();
    if (platform_quit_requested()) exit(0);
    g_vblank_count++;
    cd_vblank();
    update_pads();
    if (g_vsync_cb && !g_in_vblank_cb) {
        g_in_vblank_cb = 1;
        call_guest(c, g_vsync_cb);
        g_in_vblank_cb = 0;
    }
}

/* ---------------------------------------------------------------- libetc */
void hle_ResetCallback(Ctx *c) { g_vsync_cb = 0; c->r[2] = 0; }
void hle_VSyncCallback(Ctx *c) { u32 old = g_vsync_cb; g_vsync_cb = c->r[4]; c->r[2] = old; }

void hle_VSync(Ctx *c) {
    s32 mode = (s32)c->r[4];
    if (mode < 0) { c->r[2] = g_vblank_count; return; }
    if (mode == 1) { c->r[2] = 0; return; }
    int n = mode == 0 ? 1 : mode;
    for (int i = 0; i < n; i++) hle_vblank_tick(c);
    c->r[2] = 0;
    {   static u32 last; if (getenv("BRIG_VSTRACE") && g_vblank_count - last > 30) { last = g_vblank_count; rt_log("[vsync] vb=%u mode=%d incb=%d ra=%08X", g_vblank_count, mode, g_in_vblank_cb, c->r[31]); } }
    if (!g_in_vblank_cb) state_vsync_point(c);   /* save/load requests run here (safe point) */
}

/* ---------------------------------------------------------------- libpad */
void hle_PadInitDirect(Ctx *c) { g_pad_buf[0] = c->r[4]; g_pad_buf[1] = c->r[5]; update_pads(); }
void hle_PadStartCom(Ctx *c) { (void)c; }
void hle_PadGetState(Ctx *c) { c->r[2] = (c->r[4] == 0) ? 6 : 0; /* PadStateStable / Discon */ }
void hle_PadInfoMode(Ctx *c) { c->r[2] = (c->r[5] == 1) ? 4 : 0; /* InfoModeCurID: digital */ }
void hle_PadSetAct(Ctx *c) { c->r[2] = 0; }
void hle_PadSetActAlign(Ctx *c) { c->r[2] = 1; }

/* ---------------------------------------------------------------- libc */
static void read_cstr(u32 a, char *dst, int max) {
    int i = 0;
    for (; i < max - 1; i++) { dst[i] = (char)rd8(a + i); if (!dst[i]) break; }
    dst[i] = 0;
}
void hle_printf(Ctx *c) { char s[256]; read_cstr(c->r[4], s, sizeof s); fprintf(stderr, "[game printf] %s", s); c->r[2] = 0; }
void hle_exit(Ctx *c) { rt_fatal("game called exit(%d)", (int)c->r[4]); }

/* ---------------------------------------------------------------- libsnd -> spu.c */
#include "spu.h"
#define STUB(name, ret) void hle_##name(Ctx *c) { c->r[2] = (ret); }
#define A(n) ((s32)(s16)c->r[4 + (n)])
#define STK(n) ((s32)(s16)rd32(c->r[29] + 0x10 + 4 * (n)))
static int g_snd_trace = -1;
#define STRACE(...) do { if (g_snd_trace < 0) g_snd_trace = getenv("BRIG_SNDTRACE") != NULL; \
                         if (g_snd_trace) rt_log(__VA_ARGS__); } while (0)

STUB(SsInit, 0) STUB(SsEnd, 0) STUB(SsStart, 0) STUB(SsSetTableSize, 0) STUB(SsSetTickMode, 0)
STUB(SsSetSerialAttr, 0)
STUB(SsUtGetVBaddrInSB, 0x1010)

void hle_SsUtSetReverbType(Ctx *c) { STRACE("[snd] SsUtSetReverbType(%d)", A(0)); spu_set_reverb_type(A(0)); c->r[2] = 0; }
void hle_SsUtSetReverbDepth(Ctx *c) { STRACE("[snd] SsUtSetReverbDepth(%d,%d)", A(0), A(1)); spu_set_reverb_depth(A(0), A(1)); c->r[2] = 0; }
void hle_SsSetMono(Ctx *c) { STRACE("[snd] SsSetMono"); spu_set_mono(1); c->r[2] = 0; }
void hle_SsSetStereo(Ctx *c) { STRACE("[snd] SsSetStereo"); spu_set_mono(0); c->r[2] = 0; }
void hle_SsSetMVol(Ctx *c) { spu_set_master(A(0), A(1)); c->r[2] = 0; }
void hle_SsVabTransfer(Ctx *c) {
    int id = spu_vab_open(c->r[4], c->r[5], A(2));
    STRACE("[snd] SsVabTransfer(vh=%08X vb=%08X id=%d) -> %d", c->r[4], c->r[5], A(2), id);
    c->r[2] = (u32)id;
}
void hle_SsVabClose(Ctx *c) { STRACE("[snd] SsVabClose(%d)", A(0)); spu_vab_close(A(0)); c->r[2] = 0; }
void hle_SsSeqOpen(Ctx *c) {
    int id = spu_seq_open(c->r[4], A(1));
    STRACE("[snd] SsSeqOpen(%08X, vab=%d) -> %d", c->r[4], A(1), id);
    c->r[2] = (u32)id;
}
void hle_SsSeqClose(Ctx *c) { STRACE("[snd] SsSeqClose(%d)", A(0)); spu_seq_close(A(0)); c->r[2] = 0; }
void hle_SsSeqPlay(Ctx *c) { STRACE("[snd] SsSeqPlay(%d, mode=%d, loops=%d)", A(0), A(1), A(2)); spu_seq_play(A(0), A(1), A(2)); c->r[2] = 0; }
void hle_SsSeqStop(Ctx *c) { STRACE("[snd] SsSeqStop(%d)", A(0)); spu_seq_stop(A(0)); c->r[2] = 0; }
void hle_SsSeqPause(Ctx *c) { STRACE("[snd] SsSeqPause(%d)", A(0)); spu_seq_pause(A(0)); c->r[2] = 0; }
void hle_SsSeqSetVol(Ctx *c) { STRACE("[snd] SsSeqSetVol(%d, %d, %d)", A(0), A(1), A(2)); spu_seq_set_vol(A(0), A(1), A(2)); c->r[2] = 0; }
void hle_SsIsEos(Ctx *c) { c->r[2] = (u32)spu_seq_playing(A(0)); }
void hle_SsUtAllKeyOff(Ctx *c) { spu_all_key_off(); c->r[2] = 0; }
void hle_SsUtKeyOn(Ctx *c) {
    /* SsUtKeyOn(vabId, prog, tone, note, fine, voll, volr) */
    int v = spu_key_on(A(0), A(1), A(2), A(3), STK(0), STK(1), STK(2));
    if (getenv("BRIG_TRACE")) rt_log("[sfx] t=%u ms vb=%u note=%d vol=%d", SDL_GetTicks(), g_vblank_count, A(3), STK(1));
    STRACE("[snd] vb=%u SsUtKeyOn(vab=%d prog=%d tone=%d note=%d fine=%d vol=%d,%d) -> %d caller=%08X", g_vblank_count, A(0), A(1), A(2), A(3), STK(0), STK(1), STK(2), v, rd32(c->r[29] + 0x40));
    c->r[2] = (u32)v;
}

/* SsSetSerialVol(s_num, voll, volr): s_num 0 = CD audio (XA / CD-DA) */
#include "audio.h"
void hle_SsSetSerialVol(Ctx *c) {
    if (c->r[4] == 0) audio_set_cd_volume((int)(c->r[5] & 0x7F), (int)(c->r[6] & 0x7F));
    c->r[2] = 0;
}

/* libmcrd (memory cards): see memcard.c */

/* ---------------------------------------------------------------- libpress / streaming
 * Only reachable from the game's movie player, which is replaced as a whole (movie.c). */
STUB(DecDCTReset, 0) STUB(DecDCTvlcBuild, 0) STUB(DecDCTin, 0) STUB(DecDCTout, 0) STUB(DecDCToutCallback, 0)
STUB(DecDCTvlc2, 0) STUB(StSetRing, 0) STUB(StSetStream, 0) STUB(StGetNext, 1) STUB(StFreeRing, 0)
STUB(StUnSetRing, 0) STUB(StCdInterrupt, 0) STUB(StGetBackloc, 0)

/* save state */
void etc_state(StateIO *s) {
    ST(s, g_vblank_count); ST(s, g_vsync_cb); ST(s, g_pad_buf);
    if (s->load) g_in_vblank_cb = 0;
}
