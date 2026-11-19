/* Save states: one symmetric serialiser per module (the same function saves and loads). */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "rt.h"
#include <stdio.h>

typedef struct { FILE *f; int load; int err; } StateIO;

static inline void st_io(StateIO *s, void *p, size_t n) {
    if (s->err) return;
    size_t r = s->load ? fread(p, 1, n, s->f) : fwrite(p, 1, n, s->f);
    if (r != n) s->err = 1;
}
#define ST(s, x) st_io((s), &(x), sizeof(x))

/* per-module serialisers */
void rt_state(StateIO *s);       /* RAM, scratchpad, jmp slots, BIOS heap */
void gpu_state(StateIO *s);      /* VRAM + GPU registers */
void etc_state(StateIO *s);      /* vblank counter, VSync callback, pad buffers */
void cd_state(StateIO *s);       /* virtual CD drive */
void audio_state(StateIO *s);    /* CD audio volume, XA decoder */
void spu_state(StateIO *s);      /* SPU RAM, banks, voices, sequences, reverb */
void memcard_state(StateIO *s);  /* pending async command (card data itself lives on disk) */
void kanji_state(StateIO *s);    /* BIOS-font replacement glyph slots */

/* slots: 1..10 manual, AUTO_BASE+1..AUTO_BASE+AUTO_COUNT autosaves */
#define AUTO_BASE  100
#define AUTO_COUNT 3
#define THUMB_W    160
#define THUMB_H    120

typedef struct {
    u32 exe_crc;                 /* refuse states from another disc / patch */
    s64 time;                    /* unix time of the save */
    u16 year, month;             /* in-game date (month 1-based) */
    u8  phase;                   /* 0 organize, 1 attack */
    u8  battle, in_game;
    u8  pad;
    u8  thumb[THUMB_W * THUMB_H * 3];   /* RGB */
} StateHeader;

/* requests from the UI / hotkeys, executed at the game's next VSync */
void state_request_save(int slot);
void state_request_load(int slot);
void state_request_delete(int slot);
int  state_slot(void);
void state_set_slot(int slot);
int  state_slot_exists(int slot);
int  state_read_info(int slot, StateHeader *out);   /* 1 ok, 0 empty, -1 other version */
void state_slot_name(int slot, char *out, size_t n);
int  state_files_version(void);
void state_reset_autosave_tracking(void);
void state_vsync_point(Ctx *c);  /* called from hle_VSync */
#ifdef __cplusplus
}
#endif
