#pragma once
#include "rt.h"

int  spu_vab_open(u32 vh, u32 vb, int want_id);
void spu_vab_close(int id);
int  spu_seq_open(u32 addr, int vab);
void spu_seq_close(int id);
void spu_seq_play(int id, int mode, int l_count);
void spu_seq_stop(int id);
void spu_seq_pause(int id);
void spu_seq_set_vol(int id, int l, int r);
int  spu_seq_playing(int id);
int  spu_key_on(int vab, int prog, int tone, int note, int fine, int voll, int volr);
void spu_all_key_off(void);
void spu_set_master(int l, int r);
void spu_set_reverb_type(int type);
void spu_set_reverb_depth(int l, int r);
void spu_set_mono(int mono);   /* SsSetMono / SsSetStereo */
