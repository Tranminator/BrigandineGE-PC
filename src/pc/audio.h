#pragma once
#include "rt.h"

int  audio_init(void);
void audio_lock(void);
void audio_unlock(void);
void audio_set_cd_volume(int l, int r);   /* 0..127 */
void audio_push_xa(const s16 *pcm, int frames, int src_rate);
void audio_flush_xa(void);
void xa_reset(void);
int  xa_decode_sector(const u8 *raw2352, s16 *out_stereo, int *rate);
void audio_set_xa_movie(int on);   /* movie XA audio bypasses the music volume */
