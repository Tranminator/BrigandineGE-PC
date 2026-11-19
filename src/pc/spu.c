/* Software SPU + libsnd backend: VAB banks, SEQ sequencer, SsUtKeyOn voices.
 * Runs inside the SDL audio callback (spu_render); API calls from the game thread
 * take the audio lock. Output 44100 Hz = native SPU rate.
 * Reverb: algorithmic (Freeverb-style) approximation of the SPU presets, fed only by
 * voices whose tone has the reverb flag (VagAtr.mode & 4), like the real SPU. */
#include "spu.h"
#include "audio.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define SPU_RAM_SIZE 0x80000
#define NVOICES 24
#define NVABS 16
#define NSEQS 16

static u8 g_spu_ram[SPU_RAM_SIZE];
static u32 g_spu_alloc = 0x1010;   /* first free byte (after reserved area) */
static int g_master_l = 127, g_master_r = 127;
static int g_mono;   /* SsSetMono: ignore panning for SPU voices (game option Sound: Mono) */
extern float g_bgm_gain, g_sfx_gain;   /* audio.c */

/* ------------------------------------------------------------------ VAB */
typedef struct {
    int used;
    u8 *hdr;          /* host copy of VH */
    int ps;
    int mvol, mpan;
    u32 base;         /* VB location in SPU RAM */
    u32 vag_off[256]; /* absolute SPU RAM byte offsets */
    int tone_block[128];
} Vab;
static Vab g_vab[NVABS];

typedef struct {   /* VagAtr, 32 bytes */
    u8 prior, mode, vol, pan, center, shift, min, max;
    u8 vibW, vibT, porW, porT, pbmin, pbmax, r1, r2;
    u16 adsr1, adsr2;
    s16 prog, vag;
    s16 res[4];
} ToneAtr;

static const u8 *prog_atr(Vab *v, int p) { return v->hdr + 32 + p * 16; }
static const ToneAtr *tone_atr(Vab *v, int p, int t) {
    if (v->tone_block[p] < 0) return NULL;
    return (const ToneAtr *)(v->hdr + 32 + 128 * 16 + v->tone_block[p] * 512 + t * 32);
}

/* ------------------------------------------------------------------ voices */
enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
typedef struct {
    int active;
    u32 addr, loop_addr;     /* SPU RAM byte offsets of current / loop block */
    s16 buf[28 + 3];         /* decoded block (+ history for interpolation) */
    int buf_valid;
    s32 hist[2];
    s16 ring[16];            /* continuous history of decoded samples (across ADPCM blocks) */
    u32 ring_n;              /* samples produced so far */
    int blk_i;               /* next sample index inside buf */
    u32 pos;                 /* fractional position (low 16 bits) */
    u32 step;                /* 16.16 pitch step */
    double base_ratio;       /* without bend */
    int pbmin, pbmax;
    int env, phase, env_ctr;
    u16 adsr1, adsr2;
    float vol_l, vol_r;      /* static part */
    int owner;               /* seq id (0..15) or -1 for SsUtKeyOn */
    int channel, note;
    u32 age;
    int end_flag;
    int reverb;              /* tone mode bit 2 */
} Voice;
static Voice g_voice[NVOICES];
static u32 g_age;

static const int VK0[5] = { 0, 60, 115, 98, 122 };
static const int VK1[5] = { 0, 0, -52, -55, -60 };

static void voice_decode_block(Voice *v) {
    const u8 *b = g_spu_ram + (v->addr & (SPU_RAM_SIZE - 1));
    int shift = b[0] & 15, filt = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        s32 s = (s32)(s16)(nib << 12) >> shift;
        s += (v->hist[0] * VK0[filt] + v->hist[1] * VK1[filt] + 32) / 64;
        if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        v->hist[1] = v->hist[0]; v->hist[0] = s;
        v->buf[i] = (s16)s;
    }
    u8 flags = b[1];
    if (flags & 4) v->loop_addr = v->addr;
    v->end_flag = flags & 3;
    v->buf_valid = 1;
}

static void voice_next_block(Voice *v) {
    int f = v->end_flag;
    if (f & 1) {               /* loop end */
        if (f & 2) v->addr = v->loop_addr;
        else { v->active = 0; v->phase = ENV_OFF; return; }
    } else v->addr += 16;
    voice_decode_block(v);
}

/* ADSR stepping (psx-spx algorithm), one call per output sample */
static void env_tick(Voice *v) {
    int mode_exp, decrease, shift, step, target = -1;
    switch (v->phase) {
    case ENV_ATTACK:
        mode_exp = v->adsr1 >> 15; decrease = 0; shift = (v->adsr1 >> 10) & 31; step = 7 - ((v->adsr1 >> 8) & 3);
        break;
    case ENV_DECAY:
        mode_exp = 1; decrease = 1; shift = (v->adsr1 >> 4) & 15; step = -8;
        target = ((v->adsr1 & 15) + 1) * 0x800;
        break;
    case ENV_SUSTAIN:
        mode_exp = v->adsr2 >> 15; decrease = (v->adsr2 >> 14) & 1; shift = (v->adsr2 >> 8) & 31;
        step = decrease ? -8 + ((v->adsr2 >> 6) & 3) : 7 - ((v->adsr2 >> 6) & 3);
        break;
    case ENV_RELEASE:
        mode_exp = (v->adsr2 >> 5) & 1; decrease = 1; shift = v->adsr2 & 31; step = -8;
        break;
    default: return;
    }
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);
    int st = step << (shift < 11 ? 11 - shift : 0);
    if (mode_exp && !decrease && v->env > 0x6000) cycles *= 4;
    if (mode_exp && decrease) st = (st * v->env) >> 15;  /* arithmetic shift floors: never stalls at 0 */
    if (++v->env_ctr < cycles) return;
    v->env_ctr = 0;
    v->env += st;
    if (v->env > 0x7FFF) v->env = 0x7FFF;
    if (v->env < 0) v->env = 0;
    if (v->phase == ENV_ATTACK && v->env >= 0x7FFF) v->phase = ENV_DECAY;
    else if (v->phase == ENV_DECAY && v->env <= target) v->phase = ENV_SUSTAIN;
    else if (v->phase == ENV_RELEASE && v->env == 0) { v->active = 0; v->phase = ENV_OFF; }
}

static Voice *alloc_voice(void) {
    Voice *best = &g_voice[0];
    for (int i = 0; i < NVOICES; i++) {
        if (!g_voice[i].active) return &g_voice[i];
        /* steal: prefer releasing voices, then oldest */
        Voice *v = &g_voice[i];
        int vr = v->phase == ENV_RELEASE, br = best->phase == ENV_RELEASE;
        if ((vr && !br) || (vr == br && v->age < best->age)) best = v;
    }
    return best;
}

static void voice_set_pitch(Voice *v, int bend14) {
    double semis = 0;
    if (bend14 >= 0) {
        double b = (bend14 - 8192) / 8192.0;
        semis = b >= 0 ? b * v->pbmax : b * v->pbmin;
    }
    double r = v->base_ratio * pow(2.0, semis / 12.0);
    if (r > 4.0) r = 4.0;   /* SPU pitch register maximum 0x3FFF = 4x */
    v->step = (u32)(r * 65536.0);
}

/* Start every tone of the program that covers `note`. Returns first voice index or -1. */
static int key_on(int vab, int prog, int tone_only, int note, int fine, float vl, float vr, int owner, int ch, int bend14) {
    if (vab < 0 || vab >= NVABS || !g_vab[vab].used || prog < 0 || prog > 127) return -1;
    Vab *vb = &g_vab[vab];
    const u8 *pa = prog_atr(vb, prog);
    int ntones = pa[0], pmvol = pa[1], pmpan = pa[4];
    int first = -1;
    for (int t = 0; t < ntones && t < 16; t++) {
        if (tone_only >= 0 && t != tone_only) continue;
        const ToneAtr *ta = tone_atr(vb, prog, t);
        if (!ta) break;
        if (tone_only < 0 && (note < ta->min || note > ta->max)) continue;
        if (ta->vag <= 0 || ta->vag > 255) continue;
        Voice *v = alloc_voice();
        memset(v, 0, sizeof *v);
        v->active = 1;
        v->addr = v->loop_addr = vb->vag_off[ta->vag];
        v->adsr1 = ta->adsr1; v->adsr2 = ta->adsr2;
        v->phase = ENV_ATTACK; v->env = 0;
        v->pbmin = ta->pbmin; v->pbmax = ta->pbmax;
        /* tone "shift" raises pitch in 1/128-semitone units (verified against the OST: in tune
         * within ~1 cent, whereas subtracting it left the music ~30 cents flat) */
        double semis = (note - ta->center) + (fine + ta->shift) / 128.0;
        v->base_ratio = pow(2.0, semis / 12.0);
        voice_set_pitch(v, bend14);
        float vol = (ta->vol / 127.0f) * (pmvol / 127.0f) * (vb->mvol / 127.0f);
        int pan = ta->pan + pmpan + vb->mpan - 128;
        if (pan < 0) pan = 0; if (pan > 127) pan = 127;
        float pl = pan <= 64 ? 1.0f : (127 - pan) / 63.0f, pr = pan >= 64 ? 1.0f : pan / 64.0f;
        v->vol_l = vol * vl * pl; v->vol_r = vol * vr * pr;
        v->owner = owner; v->channel = ch; v->note = note;
        v->reverb = (ta->mode & 4) != 0;
        v->age = ++g_age;
        voice_decode_block(v);
        if (first < 0) first = (int)(v - g_voice);
    }
    return first;
}

static void key_off_match(int owner, int ch, int note) {
    for (int i = 0; i < NVOICES; i++) {
        Voice *v = &g_voice[i];
        if (v->active && v->owner == owner && v->channel == ch && (note < 0 || v->note == note) && v->phase != ENV_RELEASE)
            { v->phase = ENV_RELEASE; v->env_ctr = 0; }
    }
}

/* ------------------------------------------------------------------ reverb */
#define NCOMB 8
#define NAP 4
static const int COMB_LEN[NCOMB] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int AP_LEN[NAP] = { 556, 441, 341, 225 };
#define STEREO_SPREAD 23
static float g_comb_buf[2][NCOMB][1617 + STEREO_SPREAD + 1];
static int g_comb_pos[2][NCOMB];
static float g_comb_lp[2][NCOMB];
static float g_ap_buf[2][NAP][556 + STEREO_SPREAD + 1];
static int g_ap_pos[2][NAP];
#define ECHO_MAX 44100
static float g_echo_buf[2][ECHO_MAX];
static int g_echo_pos;

static int g_rev_type;                  /* 0 = off, 1..9 like SsUtSetReverbType */
static float g_rev_depth_l, g_rev_depth_r;
static float g_rev_room, g_rev_damp, g_rev_gain;
static int g_rev_echo_len;
static float g_rev_echo_fb;

static void reverb_configure(int type) {
    g_rev_type = type;
    g_rev_echo_len = 0; g_rev_echo_fb = 0;
    /*            room   damp   gain   (Freeverb-style feel per SPU preset family) */
    switch (type) {
    case 1: g_rev_room = 0.55f; g_rev_damp = 0.50f; g_rev_gain = 0.9f; break;  /* room */
    case 2: g_rev_room = 0.62f; g_rev_damp = 0.45f; g_rev_gain = 0.9f; break;  /* studio small */
    case 3: g_rev_room = 0.70f; g_rev_damp = 0.40f; g_rev_gain = 0.9f; break;  /* studio medium */
    case 4: g_rev_room = 0.78f; g_rev_damp = 0.35f; g_rev_gain = 0.9f; break;  /* studio large */
    case 5: g_rev_room = 0.86f; g_rev_damp = 0.30f; g_rev_gain = 0.9f; break;  /* hall */
    case 6: g_rev_room = 0.93f; g_rev_damp = 0.20f; g_rev_gain = 0.8f; break;  /* space echo */
    case 7: g_rev_room = 0.0f; g_rev_echo_len = 44100 * 2 / 5; g_rev_echo_fb = 0.5f; g_rev_gain = 0.8f; break; /* echo */
    case 8: g_rev_room = 0.0f; g_rev_echo_len = 44100 / 4; g_rev_echo_fb = 0.0f; g_rev_gain = 0.8f; break;     /* delay */
    case 9: g_rev_room = 0.80f; g_rev_damp = 0.05f; g_rev_gain = 0.8f; break;  /* pipe */
    default: g_rev_type = 0; break;
    }
    /* Freeverb parameter mapping: feedback = room*0.28 + 0.7, damping = damp*0.4 */
    if (g_rev_room > 0) g_rev_room = g_rev_room * 0.28f + 0.7f;
    g_rev_damp *= 0.4f;
}

static void reverb_clear(void) {
    memset(g_comb_buf, 0, sizeof g_comb_buf); memset(g_comb_lp, 0, sizeof g_comb_lp);
    memset(g_ap_buf, 0, sizeof g_ap_buf); memset(g_echo_buf, 0, sizeof g_echo_buf);
}

/* in: mono reverb send; adds wet signal to out[2] */
static inline void reverb_sample(float in_l, float in_r, float *out) {
    if (!g_rev_type) return;
    float x[2] = { in_l, in_r };
    for (int c = 0; c < 2; c++) {
        float acc = 0, inp = x[c] * 0.015f;
        int spread = c ? STEREO_SPREAD : 0;
        if (g_rev_echo_len) {
            int rp = (g_echo_pos - g_rev_echo_len + ECHO_MAX) % ECHO_MAX;
            float y = g_echo_buf[c][rp];
            g_echo_buf[c][g_echo_pos] = x[c] + y * g_rev_echo_fb;
            acc = y;
        } else {
            for (int i = 0; i < NCOMB; i++) {
                int len = COMB_LEN[i] + spread;
                float *b = g_comb_buf[c][i];
                float y = b[g_comb_pos[c][i]];
                g_comb_lp[c][i] = y * (1 - g_rev_damp) + g_comb_lp[c][i] * g_rev_damp;
                b[g_comb_pos[c][i]] = inp + g_comb_lp[c][i] * g_rev_room;
                if (++g_comb_pos[c][i] >= len) g_comb_pos[c][i] = 0;
                acc += y;
            }
            for (int i = 0; i < NAP; i++) {
                int len = AP_LEN[i] + spread;
                float *b = g_ap_buf[c][i];
                float bo = b[g_ap_pos[c][i]];
                float y = -acc + bo;
                b[g_ap_pos[c][i]] = acc + bo * 0.5f;
                if (++g_ap_pos[c][i] >= len) g_ap_pos[c][i] = 0;
                acc = y;
            }
            acc *= 2.2f;   /* wet level tuned to sit ~6-7 dB under the dry signal at depth 60 */
        }
        out[c] += acc * g_rev_gain * (c ? g_rev_depth_r : g_rev_depth_l);
    }
    if (g_rev_echo_len && ++g_echo_pos >= ECHO_MAX) g_echo_pos = 0;
}

void spu_set_reverb_type(int type) { audio_lock(); if (type != g_rev_type) { reverb_configure(type); reverb_clear(); } audio_unlock(); }
void spu_set_reverb_depth(int l, int r) {
    audio_lock(); g_rev_depth_l = (l & 127) / 127.0f; g_rev_depth_r = (r & 127) / 127.0f; audio_unlock();
}

/* ------------------------------------------------------------------ SEQ */
typedef struct {
    int open, playing;
    u32 data;          /* PSX address of SEQ */
    int vab;
    u32 pos;           /* PSX address of next event's delta */
    u32 loop_start;
    int loop_count, loop_inf;
    double spt;        /* samples per tick */
    double wait;       /* samples until next event */
    int resolution;
    u8 running;
    int l_count, plays;
    int nrpn_msb, nrpn_lsb;
    float vol_l, vol_r;
    struct { int prog, vol, pan, expr, bend; } ch[16];
} Seq;
static Seq g_seq[NSEQS];

static u32 read_varlen(u32 *p) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        u8 b = (u8)rd8((*p)++);
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

static void seq_reset_channels(Seq *s) {
    for (int i = 0; i < 16; i++) { s->ch[i].prog = 0; s->ch[i].vol = 127; s->ch[i].pan = 64; s->ch[i].expr = 127; s->ch[i].bend = 8192; }
}

static void seq_rewind(Seq *s) {
    u32 d = s->data;
    s->resolution = (rd8(d + 8) << 8) | rd8(d + 9);
    u32 tempo = (rd8(d + 10) << 16) | (rd8(d + 11) << 8) | rd8(d + 12);
    if (!s->resolution) s->resolution = 480;
    if (!tempo) tempo = 500000;
    s->spt = tempo / 1e6 * 44100.0 / s->resolution;
    s->pos = d + 15;
    s->loop_start = s->pos;
    s->running = 0;
    s->wait = read_varlen(&s->pos) * s->spt;
    seq_reset_channels(s);
}

static void seq_stop_voices(int id) {
    for (int i = 0; i < NVOICES; i++)
        if (g_voice[i].active && g_voice[i].owner == id) { g_voice[i].phase = ENV_RELEASE; g_voice[i].env_ctr = 0; }
}

/* Process one event; returns 0 when the track ended and stopped. */
static int seq_event(int id) {
    Seq *s = &g_seq[id];
    u8 st = (u8)rd8(s->pos);
    /* Sony SEQ keeps running status for meta events too: a bare 0x51 after an FF 51 event
     * is another tempo change. (Treating it as channel data desynced songs with tempo changes.) */
    if (st & 0x80) { s->pos++; if (st != 0xF0 && st != 0xF7) s->running = st; }
    else st = s->running;
    int ch = st & 15;
    switch (st & 0xF0) {
    case 0x90: {
        int note = rd8(s->pos++), vel = rd8(s->pos++);
        if (vel) {
            float v = vel / 127.0f * s->ch[ch].vol / 127.0f * s->ch[ch].expr / 127.0f;
            int p = s->ch[ch].pan;
            float pl = p <= 64 ? 1.0f : (127 - p) / 63.0f, pr = p >= 64 ? 1.0f : p / 64.0f;
            key_on(s->vab, s->ch[ch].prog, -1, note, 0, v * pl * s->vol_l, v * pr * s->vol_r, id, ch, s->ch[ch].bend);
        } else key_off_match(id, ch, note);
        break;
    }
    case 0x80: { int note = rd8(s->pos++); s->pos++; key_off_match(id, ch, note); break; }
    case 0xB0: {
        int cc = rd8(s->pos++), val = rd8(s->pos++);
        if (cc == 7) s->ch[ch].vol = val;
        else if (cc == 10) s->ch[ch].pan = val;
        else if (cc == 11) s->ch[ch].expr = val;
        else if (cc == 98) s->nrpn_lsb = val;
        else if (cc == 99) {                                  /* Sony NRPN (MSB) */
            s->nrpn_msb = val;
            if (val == 20) s->loop_start = s->pos;            /* loop start marker */
            else if (val == 30) {                             /* loop end marker */
                if (s->loop_inf || s->loop_count-- > 0) s->pos = s->loop_start;
            }
        }
        else if (cc == 6) {                                   /* data entry for the current NRPN */
            if (s->nrpn_msb == 20) { s->loop_count = val; s->loop_inf = (val == 0 || val == 127); }
            else if (s->nrpn_msb == 16 && s->nrpn_lsb == 15) { if (val != g_rev_type) { reverb_configure(val); reverb_clear(); } }
            else if (s->nrpn_msb == 16 && s->nrpn_lsb == 16) { g_rev_depth_l = g_rev_depth_r = val / 127.0f; }
        }
        break;
    }
    case 0xC0: s->ch[ch].prog = rd8(s->pos++); break;
    case 0xE0: {
        int lsb = rd8(s->pos++), msb = rd8(s->pos++);
        s->ch[ch].bend = (msb << 7) | lsb;
        for (int i = 0; i < NVOICES; i++)
            if (g_voice[i].active && g_voice[i].owner == id && g_voice[i].channel == ch) voice_set_pitch(&g_voice[i], s->ch[ch].bend);
        break;
    }
    case 0xA0: case 0xD0: s->pos += (st & 0xF0) == 0xD0 ? 1 : 2; break;
    case 0xF0:
        if (st == 0xFF) {
            int type = rd8(s->pos++);
            if (type == 0x51) {
                u32 tempo = (rd8(s->pos) << 16) | (rd8(s->pos + 1) << 8) | rd8(s->pos + 2);
                s->pos += 3;
                if (tempo) s->spt = tempo / 1e6 * 44100.0 / s->resolution;
            } else if (type == 0x2F) {
                s->plays++;
                seq_stop_voices(id);
                if (s->l_count == 0 || s->plays < s->l_count) { seq_rewind(s); return 1; }
                s->playing = 0;
                return 0;
            } else {
                u32 len = read_varlen(&s->pos); s->pos += len;
            }
        } else { u32 len = read_varlen(&s->pos); s->pos += len; }
        break;
    default:
        s->pos++; /* corrupt / unknown: skip a byte */
    }
    s->wait += read_varlen(&s->pos) * s->spt;
    return 1;
}

static void seq_advance(int frames) {
    for (int id = 0; id < NSEQS; id++) {
        Seq *s = &g_seq[id];
        if (!s->open || !s->playing) continue;
        s->wait -= frames;
        int guard = 0;
        while (s->playing && s->wait <= 0 && guard++ < 4096)
            if (!seq_event(id)) break;
    }
}

/* ------------------------------------------------------------------ interpolation
 * 8-tap Blackman-windowed sinc, cutoff lowered with the playback step so samples played
 * above their native rate do not alias (the hardware's 4-tap gaussian also low-passes). */
#define NPHASE 64
#define NCUT 16
static float g_sinc[NCUT][NPHASE][8];
static int g_sinc_init;

static void sinc_init(void) {
    for (int c = 0; c < NCUT; c++) {
        double fc = 0.95 * (1.0 - c * (0.75 / (NCUT - 1)));     /* 0.95 .. 0.2375 of Nyquist */
        for (int ph = 0; ph < NPHASE; ph++) {
            double frac = (double)ph / NPHASE, sum = 0, h[8];
            for (int k = 0; k < 8; k++) {
                double x = (k - 3) - frac, sx = fc * x;
                double sinc = fabs(sx) < 1e-9 ? 1.0 : sin(M_PI * sx) / (M_PI * sx);
                double w = fabs(x) >= 4 ? 0 : 0.42 + 0.5 * cos(M_PI * x / 4) + 0.08 * cos(2 * M_PI * x / 4);
                h[k] = fc * sinc * w; sum += h[k];
            }
            for (int k = 0; k < 8; k++) g_sinc[c][ph][k] = (float)(h[k] / sum);
        }
    }
    g_sinc_init = 1;
}

static inline int cut_bucket(u32 step) {
    double r = step / 65536.0;                     /* playback rate relative to 44.1 kHz */
    if (r <= 1.0) return 0;
    double fc = 0.95 / r;                          /* wanted cutoff */
    int c = (int)((1.0 - fc / 0.95) * (NCUT - 1) / 0.75 + 0.5);
    return c < 0 ? 0 : c >= NCUT ? NCUT - 1 : c;
}

static inline void voice_fetch(Voice *v) {
    if (v->blk_i >= 28) { voice_next_block(v); v->blk_i = 0; if (!v->active) return; }
    v->ring[v->ring_n++ & 15] = v->buf[v->blk_i++];
}

/* ------------------------------------------------------------------ render */
void spu_render(s32 *mix, int frames, int rate) {
    (void)rate;
    int done = 0;
    static float send[64 * 2];
    if (!g_sinc_init) sinc_init();
    while (done < frames) {
        int n = frames - done > 64 ? 64 : frames - done;   /* sequencer granularity ~1.5 ms */
        seq_advance(n);
        memset(send, 0, sizeof send);
        for (int vi = 0; vi < NVOICES; vi++) {
            Voice *v = &g_voice[vi];
            if (!v->active) continue;
            for (int i = 0; i < n && v->active; i++) {
                env_tick(v);
                /* output point sits between ring samples n-5 and n-4 (4-sample latency) */
                const float *h = g_sinc[cut_bucket(v->step)][(v->pos & 0xFFFF) >> 10];
                u32 n0 = v->ring_n - 8;
                float acc = 0;
                for (int k = 0; k < 8; k++) acc += v->ring[(n0 + k) & 15] * h[k];
                s32 e = (s32)acc * v->env / 0x7FFF;
                /* 0.5 = SPU voice volume register full scale (0x3FFF of 0x8000) */
                float g = v->owner >= 0 ? g_bgm_gain : g_sfx_gain;   /* SEQ voices = music, SsUtKeyOn = effects */
                float vl = v->vol_l, vr = v->vol_r;
                if (g_mono) vl = vr = (vl + vr) * 0.5f;   /* same loudness, centred */
                float ol = e * vl * g_master_l / 254 * g, orr = e * vr * g_master_r / 254 * g;
                static int wet_only = -1;
                if (wet_only < 0) wet_only = getenv("BRIG_REVERB_ONLY") != NULL;   /* debug */
                if (!wet_only) { mix[(done + i) * 2] += (s32)ol; mix[(done + i) * 2 + 1] += (s32)orr; }
                if (v->reverb) { send[i * 2] += ol; send[i * 2 + 1] += orr; }
                v->pos += v->step;
                while ((v->pos >> 16) && v->active) { v->pos -= 1 << 16; voice_fetch(v); }
            }
        }
        static int no_rev = -1;
        if (no_rev < 0) no_rev = getenv("BRIG_NO_REVERB") != NULL;   /* debug */
        if (g_rev_type && !no_rev) {
            for (int i = 0; i < n; i++) {
                float wet[2] = { 0, 0 };
                reverb_sample(send[i * 2], send[i * 2 + 1], wet);
                mix[(done + i) * 2] += (s32)wet[0];
                mix[(done + i) * 2 + 1] += (s32)wet[1];
            }
        }
        done += n;
    }
}

/* ------------------------------------------------------------------ API (game thread) */
int spu_vab_open(u32 vh, u32 vb, int want_id) {
    if (rd32(vh) != 0x56414270u) { rt_log("SsVabTransfer: bad VAB header at %08X", vh); return -1; }
    audio_lock();
    int id = want_id;
    if (id < 0 || id >= NVABS || g_vab[id].used) for (id = 0; id < NVABS && g_vab[id].used; id++);
    if (id >= NVABS) { audio_unlock(); return -1; }
    Vab *v = &g_vab[id];
    int ps = rd16(vh + 18);
    u32 hsize = 32 + 128 * 16 + ps * 512 + 512;
    v->hdr = malloc(hsize);
    memcpy(v->hdr, ram_ptr(vh), hsize);
    v->ps = ps; v->mvol = v->hdr[24]; v->mpan = v->hdr[25];
    if (!v->mvol) v->mvol = 127;
    if (!v->mpan) v->mpan = 64;
    int k = 0;
    for (int p = 0; p < 128; p++) v->tone_block[p] = (v->hdr[32 + p * 16] > 0 && k < ps) ? k++ : -1;
    const u16 *vt = (const u16 *)(v->hdr + 32 + 128 * 16 + ps * 512);
    u32 total = 0;
    for (int i = 1; i < 256; i++) total += (u32)vt[i] * 8;
    v->base = g_spu_alloc;
    if (v->base + total > SPU_RAM_SIZE) { rt_log("SPU RAM full, resetting allocator"); v->base = g_spu_alloc = 0x1010; }
    memcpy(g_spu_ram + v->base, ram_ptr(vb), total);
    u32 off = v->base;
    v->vag_off[0] = off;
    for (int i = 1; i < 256; i++) { v->vag_off[i] = off; off += (u32)vt[i] * 8; }
    g_spu_alloc = (off + 15) & ~15u;
    v->used = 1;
    audio_unlock();
    return id;
}

void spu_vab_close(int id) {
    if (id < 0 || id >= NVABS || !g_vab[id].used) return;
    audio_lock();
    for (int i = 0; i < NVOICES; i++) g_voice[i].active = 0;
    free(g_vab[id].hdr);
    memset(&g_vab[id], 0, sizeof g_vab[id]);
    /* simple allocator: when every bank is closed, reclaim SPU RAM */
    int any = 0;
    for (int i = 0; i < NVABS; i++) any |= g_vab[i].used;
    if (!any) g_spu_alloc = 0x1010;
    audio_unlock();
}

int spu_seq_open(u32 addr, int vab) {
    if (rd8(addr) != 'p' || rd8(addr + 1) != 'Q' || rd8(addr + 2) != 'E' || rd8(addr + 3) != 'S') {
        rt_log("SsSeqOpen: not a SEQ at %08X", addr); return -1;
    }
    audio_lock();
    int id;
    for (id = 0; id < NSEQS && g_seq[id].open; id++);
    if (id < NSEQS) {
        Seq *s = &g_seq[id];
        memset(s, 0, sizeof *s);
        s->open = 1; s->data = addr; s->vab = vab; s->vol_l = s->vol_r = 1.0f;
        seq_rewind(s);
    } else id = -1;
    audio_unlock();
    return id;
}

void spu_seq_close(int id) {
    if (id < 0 || id >= NSEQS) return;
    audio_lock(); seq_stop_voices(id); g_seq[id].open = g_seq[id].playing = 0; audio_unlock();
}

void spu_seq_play(int id, int mode, int l_count) {
    if (id < 0 || id >= NSEQS || !g_seq[id].open) return;
    audio_lock();
    Seq *s = &g_seq[id];
    if (!s->playing && s->plays == 0) { s->l_count = l_count; }
    s->playing = mode != 0;
    audio_unlock();
}

void spu_seq_stop(int id) {
    if (id < 0 || id >= NSEQS || !g_seq[id].open) return;
    audio_lock(); seq_stop_voices(id); g_seq[id].playing = 0; g_seq[id].plays = 0; seq_rewind(&g_seq[id]); audio_unlock();
}

void spu_seq_pause(int id) {
    if (id < 0 || id >= NSEQS) return;
    audio_lock(); g_seq[id].playing = 0; seq_stop_voices(id); audio_unlock();
}

void spu_seq_set_vol(int id, int l, int r) {
    if (id < 0 || id >= NSEQS) return;
    audio_lock(); g_seq[id].vol_l = l / 127.0f; g_seq[id].vol_r = r / 127.0f; audio_unlock();
}

int spu_seq_playing(int id) { return id >= 0 && id < NSEQS && g_seq[id].open && g_seq[id].playing; }

int spu_key_on(int vab, int prog, int tone, int note, int fine, int voll, int volr) {
    audio_lock();
    int v = key_on(vab, prog, tone, note, fine, voll / 127.0f, volr / 127.0f, -1, 0, -1);
    audio_unlock();
    return v;
}

void spu_all_key_off(void) {
    audio_lock();
    for (int i = 0; i < NVOICES; i++) if (g_voice[i].active) { g_voice[i].phase = ENV_RELEASE; g_voice[i].env_ctr = 0; }
    audio_unlock();
}

void spu_set_master(int l, int r) { g_master_l = l & 127; g_master_r = r & 127; }

/* debug: one-line summary of active voices */
void spu_debug_dump(u32 vb) {
    char line[1024]; int n = 0;
    n += snprintf(line + n, sizeof line - n, "[spu vb=%u]", vb);
    for (int i = 0; i < NVOICES; i++) {
        Voice *v = &g_voice[i];
        if (!v->active) continue;
        n += snprintf(line + n, sizeof line - n, " v%d(o%d c%d n%d ph%d env%04X loop%d)", i, v->owner, v->channel, v->note,
                      v->phase, v->env, (v->end_flag & 2) != 0);
        if (n > 900) break;
    }
    rt_log("%s", line);
}

void spu_set_mono(int mono) { g_mono = mono; }
