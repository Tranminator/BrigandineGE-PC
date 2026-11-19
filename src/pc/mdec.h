#pragma once
#include "rt.h"

/* Decode one demuxed STR frame (BS v2) of w x h pixels into packed RGB24. Returns 1 on success. */
int mdec_decode_frame(const u8 *bs, int len, int w, int h, u8 *rgb);
