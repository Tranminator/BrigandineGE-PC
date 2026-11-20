/* Mouse control (mouse.c). */
#pragma once
#include <SDL.h>
#include <stdint.h>

int  mouse_event(const SDL_Event *e);     /* 1 if consumed */
void mouse_set_view(int x, int y, int w, int h, float px_per_pt);   /* image rect in drawable pixels */
void mouse_update(void);                  /* once per vblank */
uint16_t mouse_pad_bits(void);            /* virtual PS1 buttons to OR into the pad */
void mouse_cancel(void);                  /* keyboard / pad took over */
