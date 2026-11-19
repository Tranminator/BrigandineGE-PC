/* main.c — game entry & mode state machine. Raw m2c output (NON-MATCHING), cleanup pending.
 * D_800A67D0 = game mode, D_800A67BC = global flags. LoadOverlay(n) loads OVER(n+1).BIN by LBA. */
#include "brigandine.h"

? D_80015648();                                     /* static */
? func_8001F924(s32, s32, s32);                     /* static */
extern ? g_OverlayLba;
extern ? g_OverlaySize;

void LoadOverlay(s32 arg0) {
    s32 temp_a2;

    temp_a2 = arg0 * 4;
    func_8001F924(*(temp_a2 + &g_OverlayLba), D_80015648, *(temp_a2 + &g_OverlaySize));
}

? ResetCallback();                                  /* extern */
? SetDispMask(?);                                   /* extern */
? __main();                                         /* extern */
? func_800B6D7C();                                  /* extern */
? func_800B7704();                                  /* extern */
? func_800B793C();                                  /* extern */
? func_800B7B9C();                                  /* extern */
? func_800B7E5C();                                  /* extern */
? func_800BA784();                                  /* extern */
? func_800BDEF8();                                  /* extern */
s32 setjmp(? *);                                    /* extern */
? LoadOverlay(?);                                   /* static */
? func_8001A708();                                  /* static */
? func_8001A938();                                  /* static */
? func_8001AAD4();                                  /* static */
? func_8001AB3C();                                  /* static */
? func_8001B574();                                  /* static */
extern s32 D_800A63D4;
extern s32 D_800A63E4;
extern s32 D_800A6724;
extern s32 D_800A67A4;
extern s32 D_800A67BC;
extern s32 D_800A67D0;
extern s32 D_800A6A0C;
extern s32 D_800A6A5C;
extern ? D_800A7978;

void main(void) {
    s32 temp_v0;

    __main();
    ResetCallback();
    func_8001A938();
loop_1:
    D_800A6724 = 0;
    D_800A67BC = 0;
    func_8001B574();
    if (D_800A67A4 == 0xC) {
        D_800A67BC |= 2;
    }
    if (setjmp(&D_800A7978) != 0) {
        func_8001AAD4();
    }
    D_800A67D0 = 2;
    func_8001A708();
    func_8001AB3C();
    SetDispMask(1);
    if (!(D_800A67BC & 0x30000)) {
        do {
            switch (D_800A67D0) {                   /* irregular */
            case 1:
                if (!(D_800A67BC & 0x2000)) {
                    LoadOverlay(1);
                }
                func_800B7B9C();
                if (D_800A67BC & 0x800000) {
                    LoadOverlay(4);
                    func_800BDEF8();
                }
                LoadOverlay(0);
                break;
            case 2:
                func_800B7704();
                break;
            }
            D_800A6A0C = -1;
            D_800A6A5C = -1;
            temp_v0 = D_800A67BC & ~0x400;
            D_800A67BC = temp_v0;
        } while (!(temp_v0 & 0x30000));
    }
    LoadOverlay(4);
    if (D_800A63D4 & 0x100000) {
        func_800BA784();
        goto loop_1;
    }
    if (D_800A67BC & 0x20000) {
        func_800B7E5C();
        goto loop_1;
    }
    if (D_800A63E4 == 0) {
        func_800B793C();
        goto loop_1;
    }
    func_800B6D7C();
    goto loop_1;
}
