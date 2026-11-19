/* PS1 memory cards for libmcrd (MemCard*), stored as standard 128 KB raw card images
 * (the .mcr / .mcd / .srm format used by ePSXe, DuckStation, RetroArch), so saves can be
 * moved between this port and emulators.
 *
 * Card layout: 16 blocks x 8 KB. Block 0 = directory: frame 0 "MC" header, frames 1..15 one
 * entry per data block (state, size, next, name, XOR checksum in byte 0x7F), frames 16..35
 * broken-sector list. Blocks 1..15 hold file data; a file spans a chain of blocks.
 *
 * Slot 1 (chan 0x00) = <pref>/memcard1.mcd, created formatted if missing.
 * Slot 2 (chan 0x10) = <pref>/memcard2.mcd, present only if that file exists.
 * Env overrides (tests): BRIG_MEMCARD1 / BRIG_MEMCARD2 = path. */
#include "hle.h"
#include "state.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define CARD_SIZE  0x20000
#define BLOCK_SIZE 0x2000
#define FRAME      0x80

enum { McFuncExist = 1, McFuncAccept = 2, McFuncReadFile = 3, McFuncWriteFile = 4 };
enum { McErrNone = 0, McErrCardNotExist = 1, McErrCardInvalid = 2, McErrNewCard = 3, McErrNotFormat = 4,
       McErrFileNotExist = 5, McErrAlreadyExist = 6, McErrBlockFull = 7 };

typedef struct {
    int present, loaded, announced_new;
    char path[1024];
    u8 data[CARD_SIZE];
} Card;

static Card g_card[2];
static int g_pending;            /* an async command finished and has not been synced yet */
static u32 g_last_cmd, g_last_result;
static int g_trace = -1;

#define MTRACE(...) do { if (g_trace < 0) g_trace = getenv("BRIG_MCTRACE") != NULL; if (g_trace) rt_log(__VA_ARGS__); } while (0)

static u8 *frame(Card *c, int block, int f) { return c->data + block * BLOCK_SIZE + f * FRAME; }

static void set_checksum(u8 *fr) {
    u8 x = 0;
    for (int i = 0; i < FRAME - 1; i++) x ^= fr[i];
    fr[FRAME - 1] = x;
}

static void format_card(Card *c) {
    memset(c->data, 0, CARD_SIZE);
    u8 *h = frame(c, 0, 0);
    h[0] = 'M'; h[1] = 'C';
    set_checksum(h);
    for (int i = 1; i <= 15; i++) {          /* free directory entries */
        u8 *e = frame(c, 0, i);
        e[0] = 0xA0;
        e[8] = 0xFF; e[9] = 0xFF;
        set_checksum(e);
    }
    for (int i = 16; i <= 35; i++) {         /* empty broken-sector list */
        u8 *e = frame(c, 0, i);
        memset(e, 0xFF, 4);
        e[8] = 0xFF; e[9] = 0xFF;
        set_checksum(e);
    }
    memcpy(frame(c, 0, 63), frame(c, 0, 0), FRAME);   /* write-test frame mirrors the header */
}

static int is_formatted(Card *c) { return c->data[0] == 'M' && c->data[1] == 'C'; }

static void save_card(Card *c) {
    FILE *f = fopen(c->path, "wb");
    if (!f) { rt_log("[memcard] cannot write %s", c->path); return; }
    fwrite(c->data, 1, CARD_SIZE, f);
    fflush(f);
    fclose(f);
}

static Card *card_for(u32 chan) {
    int slot = (chan & 0x10) ? 1 : 0;
    Card *c = &g_card[slot];
    if (c->loaded) return c->present ? c : NULL;
    c->loaded = 1;
    const char *env = getenv(slot ? "BRIG_MEMCARD2" : "BRIG_MEMCARD1");
    if (env) snprintf(c->path, sizeof c->path, "%s", env);
    else {
        char *dir = SDL_GetPrefPath("PortGamePC", "Brigandine");
        snprintf(c->path, sizeof c->path, "%smemcard%d.mcd", dir ? dir : "", slot + 1);
        if (dir) SDL_free(dir);
    }
    FILE *f = fopen(c->path, "rb");
    if (f) {
        size_t n = fread(c->data, 1, CARD_SIZE, f);
        fclose(f);
        if (n == CARD_SIZE) { c->present = 1; rt_log("[memcard] slot %d: %s", slot + 1, c->path); }
        else rt_log("[memcard] slot %d: %s is not a 128 KB card image, ignored", slot + 1, c->path);
    } else if (slot == 0) {                 /* slot 1 always has a card: create a formatted one */
        format_card(c);
        c->present = 1;
        save_card(c);
        rt_log("[memcard] slot 1: created new card %s", c->path);
    }
    return c->present ? c : NULL;
}

/* ------------------------------------------------------------------ directory helpers */
static void read_name(u32 addr, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { out[i] = (char)rd8(addr + i); if (!out[i]) break; }
    out[i] = 0;
}

static int find_file(Card *c, const char *name) {        /* returns first block (1..15) or 0 */
    for (int b = 1; b <= 15; b++) {
        u8 *e = frame(c, 0, b);
        if (e[0] == 0x51 && !strncmp((const char *)e + 0x0A, name, 20)) return b;
    }
    return 0;
}

static int next_block(Card *c, int b) {
    u8 *e = frame(c, 0, b);
    u16 n = (u16)(e[8] | e[9] << 8);
    return n == 0xFFFF ? 0 : n + 1;
}

static u32 file_size(Card *c, int first) { u8 *e = frame(c, 0, first); return (u32)(e[4] | e[5] << 8 | e[6] << 16 | e[7] << 24); }

/* copy between RAM and a file at byte offset ofs */
static int file_io(Card *c, int first, u32 adrs, u32 ofs, u32 bytes, int write) {
    u32 size = file_size(c, first);
    if (ofs + bytes > size) return 0;
    for (u32 done = 0; done < bytes; ) {
        u32 pos = ofs + done, blk_idx = pos / BLOCK_SIZE, in_blk = pos % BLOCK_SIZE;
        int b = first;
        for (u32 k = 0; k < blk_idx && b; k++) b = next_block(c, b);
        if (!b) return 0;
        u32 n = BLOCK_SIZE - in_blk;
        if (n > bytes - done) n = bytes - done;
        u8 *p = c->data + b * BLOCK_SIZE + in_blk;
        if (write) memcpy(p, ram_ptr(adrs + done), n); else memcpy(ram_ptr(adrs + done), p, n);
        done += n;
    }
    return 1;
}

static int wildcard(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') { for (;; s++) { if (wildcard(pat + 1, s)) return 1; if (!*s) return 0; } }
        if (!*s || (*pat != '?' && *pat != *s)) return 0;
    }
    return *s == 0;
}

static void finish(u32 cmd, u32 result) { g_last_cmd = cmd; g_last_result = result; g_pending = 1; }

/* ------------------------------------------------------------------ libmcrd HLE */
#define A(n) (c->r[4 + (n)])
#define STK(n) (rd32(c->r[29] + 0x10 + 4 * (n)))

void hle_MemCardInit(Ctx *c) { c->r[2] = 0; }
void hle_MemCardStart(Ctx *c) { c->r[2] = 0; }
void hle_MemCardStop(Ctx *c) { c->r[2] = 0; }

/* MemCardExist(chan): async. A card seen for the first time reports McErrNewCard once. */
void hle_MemCardExist(Ctx *c) {
    Card *cd = card_for(A(0));
    u32 r = !cd ? McErrCardNotExist : (!cd->announced_new ? (cd->announced_new = 1, McErrNewCard) : McErrNone);
    MTRACE("[mc] Exist(%02X) -> %u", A(0), r);
    finish(McFuncExist, r);
    c->r[2] = 1;
}

/* MemCardAccept(chan): async, validates the card. */
void hle_MemCardAccept(Ctx *c) {
    Card *cd = card_for(A(0));
    u32 r = !cd ? McErrCardNotExist : is_formatted(cd) ? McErrNone : McErrNotFormat;
    MTRACE("[mc] Accept(%02X) -> %u", A(0), r);
    finish(McFuncAccept, r);
    c->r[2] = 1;
}

/* MemCardReadFile(chan, name, adrs, ofs, bytes): async */
void hle_MemCardReadFile(Ctx *c) {
    Card *cd = card_for(A(0));
    char name[24]; read_name(A(1), name, sizeof name);
    u32 r = McErrCardNotExist;
    if (cd) {
        int b = find_file(cd, name);
        r = !b ? McErrFileNotExist : file_io(cd, b, A(2), A(3), STK(0), 0) ? McErrNone : McErrFileNotExist;
    }
    MTRACE("[mc] ReadFile(%02X, %s, ofs %u, %u bytes) -> %u", A(0), name, A(3), STK(0), r);
    finish(McFuncReadFile, r);
    c->r[2] = 1;
}

/* MemCardWriteFile(chan, name, adrs, ofs, bytes): async; the card file is saved right away */
void hle_MemCardWriteFile(Ctx *c) {
    Card *cd = card_for(A(0));
    char name[24]; read_name(A(1), name, sizeof name);
    u32 r = McErrCardNotExist;
    if (cd) {
        int b = find_file(cd, name);
        r = !b ? McErrFileNotExist : file_io(cd, b, A(2), A(3), STK(0), 1) ? McErrNone : McErrFileNotExist;
        if (r == McErrNone) save_card(cd);
    }
    MTRACE("[mc] WriteFile(%02X, %s, ofs %u, %u bytes) -> %u", A(0), name, A(3), STK(0), r);
    finish(McFuncWriteFile, r);
    c->r[2] = 1;
}

/* MemCardCreateFile(chan, name, blocks): sync, returns an McErr code */
void hle_MemCardCreateFile(Ctx *c) {
    Card *cd = card_for(A(0));
    char name[24]; read_name(A(1), name, sizeof name);
    int blocks = (int)A(2);
    u32 r;
    if (!cd) r = McErrCardNotExist;
    else if (find_file(cd, name)) r = McErrAlreadyExist;
    else {
        int freeb[15], nf = 0;
        for (int b = 1; b <= 15 && nf < blocks; b++) if ((frame(cd, 0, b)[0] & 0xF0) == 0xA0) freeb[nf++] = b;
        if (blocks < 1 || nf < blocks) r = McErrBlockFull;
        else {
            for (int i = 0; i < blocks; i++) {
                u8 *e = frame(cd, 0, freeb[i]);
                memset(e, 0, FRAME);
                e[0] = i == 0 ? 0x51 : (i == blocks - 1 ? 0x53 : 0x52);
                if (i == 0) { u32 sz = (u32)blocks * BLOCK_SIZE; e[4] = sz; e[5] = sz >> 8; e[6] = sz >> 16; e[7] = sz >> 24; }
                u16 nx = i == blocks - 1 ? 0xFFFF : (u16)(freeb[i + 1] - 1);
                e[8] = nx & 0xFF; e[9] = nx >> 8;
                if (i == 0) strncpy((char *)e + 0x0A, name, 20);
                set_checksum(e);
                memset(cd->data + freeb[i] * BLOCK_SIZE, 0, BLOCK_SIZE);
            }
            save_card(cd);
            r = McErrNone;
        }
    }
    MTRACE("[mc] CreateFile(%02X, %s, %d blocks) -> %u", A(0), name, blocks, r);
    c->r[2] = r;
}

/* MemCardFormat(chan): sync */
void hle_MemCardFormat(Ctx *c) {
    Card *cd = card_for(A(0));
    if (cd) { format_card(cd); save_card(cd); }
    MTRACE("[mc] Format(%02X)", A(0));
    c->r[2] = cd ? McErrNone : McErrCardNotExist;
}

/* MemCardGetDirentry(chan, name, DIRENTRY *dir, long *files, ofs, max): sync.
 * DIRENTRY = { char name[20]; long attr; long size; DIRENTRY *next; long head; char system[4]; } (40 bytes) */
void hle_MemCardGetDirentry(Ctx *c) {
    Card *cd = card_for(A(0));
    char pat[24]; read_name(A(1), pat, sizeof pat);
    u32 dir = A(2), files = A(3), ofs = STK(0), max = STK(1);
    u32 n = 0, idx = 0;
    if (!cd) { if (files) wr32(files, 0); c->r[2] = McErrCardNotExist; return; }
    for (int b = 1; b <= 15; b++) {
        u8 *e = frame(cd, 0, b);
        if (e[0] != 0x51) continue;
        char nm[21]; memcpy(nm, e + 0x0A, 20); nm[20] = 0;
        if (!wildcard(pat, nm)) continue;
        if (idx++ < ofs) continue;
        if (n >= max) break;
        u32 d = dir + n * 40;
        for (int i = 0; i < 20; i++) wr8(d + i, (u8)nm[i]);
        wr32(d + 20, e[0]);
        wr32(d + 24, file_size(cd, b));
        wr32(d + 28, 0);
        wr32(d + 32, (u32)b);
        wr32(d + 36, 0);
        n++;
    }
    if (files) wr32(files, n);
    MTRACE("[mc] GetDirentry(%02X, %s) -> %u files", A(0), pat, n);
    c->r[2] = McErrNone;
}

/* MemCardSync(mode, long *cmds, long *result): 1 = finished (cmds/result filled), -1 = nothing pending */
void hle_MemCardSync(Ctx *c) {
    if (!g_pending) { c->r[2] = (u32)-1; return; }
    if (A(1)) wr32(A(1), g_last_cmd);
    if (A(2)) wr32(A(2), g_last_result);
    g_pending = 0;
    c->r[2] = 1;
}

/* save state: only the pending async result (card contents live on disk and are not rolled back) */
void memcard_state(StateIO *s) {
    ST(s, g_pending); ST(s, g_last_cmd); ST(s, g_last_result);
}
