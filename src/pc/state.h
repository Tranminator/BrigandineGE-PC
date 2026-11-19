/* Save states: one symmetric serialiser per module (the same function saves and loads). */
#pragma once
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

/* requests from the UI / hotkeys, executed at the game's next VSync */
void state_request_save(int slot);
void state_request_load(int slot);
int  state_slot(void);
void state_set_slot(int slot);
int  state_slot_exists(int slot);
void state_vsync_point(Ctx *c);  /* called from hle_VSync */
