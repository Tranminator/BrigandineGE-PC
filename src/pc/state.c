/* Save states: the port's own save system (quick save / quick load / autosave).
 *
 * A state is taken at the game's VSync call, the one point every frame passes through
 * where all live state belongs to the emulated PS1: RAM (including the PS1 call stack
 * with every saved return address), CPU/GTE registers, VRAM/GPU, SPU, CD drive, pads.
 * Loading restores it and resumes guest execution right after that VSync call through
 * rt_resume_at(), so a state does not depend on the host C stack and stays valid after
 * the app is restarted. Memory-card saves are untouched (kept as the portable backup).
 *
 * Slots: 1..10 manual (<pref>/states/slot<N>.state), AUTO_BASE+1..+3 autosaves
 * (auto<N>.state, rotating) written when the game enters the attack phase, starts a
 * new turn (organize phase), or starts a battle.
 *
 * File: "BRIGSTAT" | version | StateHeader (info + 160x120 thumbnail) | module data. */
#include "state.h"
#include "hle.h"
#include "audio.h"
#include "settings.h"
#include <SDL.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define MAGIC   "BRIGSTAT"
#define VERSION 2

/* game RAM locations (found by diffing states across phases; year from the cheat table) */
#define RAM_YEAR    0x800A68E8u   /* e.g. 215 */
#define RAM_PHASE   0x800A6900u   /* 0 = organize, 1 = attack */
#define RAM_MONTH0  0x800A6930u   /* month, 0-based (the rune on the map banner) */
#define OVL_ID_ADDR 0x800B6838u   /* overlay id word: 5 = strategy (OVER1), 6 = battle (OVER2) */

void ui_toast(const char *msg);                       /* ui.cpp */
void platform_thumbnail(u8 *rgb, int w, int h);       /* platform.c */
extern u32 g_exe_crc;                                 /* main.c */

static int g_req_save = -1, g_req_load = -1, g_req_delete = -1;
static int g_files_version;                           /* bumps whenever a state file changes */

int state_slot(void) { return g_settings.slot < 1 || g_settings.slot > 10 ? 1 : g_settings.slot; }
void state_set_slot(int slot) { g_settings.slot = slot < 1 ? 1 : slot > 10 ? 10 : slot; }

/* Left Shift + Left / Right: previous / next slot (wraps), with a toast describing it */
void state_cycle_slot(int dir) {
    int sl = (state_slot() - 1 + dir + 10) % 10 + 1;
    state_set_slot(sl);
    if (!getenv("BRIG_STATE_DIR")) settings_save();   /* tests leave the real settings alone */
    StateHeader *h = (StateHeader *)malloc(sizeof *h);
    char msg[128];
    int r = h ? state_read_info(sl, h) : 0;
    if (r == 1) {
        time_t t = (time_t)h->time; struct tm *lt = localtime(&t); char when[32] = "";
        if (lt) strftime(when, sizeof when, "%Y-%m-%d %H:%M", lt);
        if (h->in_game) snprintf(msg, sizeof msg, "Slot %d  -  %s  -  Year %u, Month %u", sl, when, h->year, h->month);
        else snprintf(msg, sizeof msg, "Slot %d  -  %s", sl, when);
    } else snprintf(msg, sizeof msg, r < 0 ? "Slot %d  -  other version" : "Slot %d  -  empty", sl);
    free(h);
    ui_toast(msg);
}
void state_request_save(int slot) { g_req_save = slot; }
void state_request_load(int slot) { g_req_load = slot; }
int state_load_pending(void) { return g_req_load >= 0; }
void state_request_delete(int slot) { g_req_delete = slot; }
int state_files_version(void) { return g_files_version; }

static void state_dir(char *out, size_t n) {
    const char *env = getenv("BRIG_STATE_DIR");          /* tests */
    if (env) { snprintf(out, n, "%s/", env); return; }
    char *dir = SDL_GetPrefPath("PortGamePC", "Brigandine");
    snprintf(out, n, "%sstates/", dir ? dir : "");
    if (dir) SDL_free(dir);
}

static void slot_path(int slot, char *out, size_t n) {
    char dir[1024]; state_dir(dir, sizeof dir);
    if (slot == STATE_DEBUG_SLOT) snprintf(out, n, "%sdebug.state", dir);
    else if (slot > AUTO_BASE) snprintf(out, n, "%sauto%d.state", dir, slot - AUTO_BASE);
    else snprintf(out, n, "%sslot%d.state", dir, slot);
}

int state_slot_exists(int slot) {
    char p[1024]; slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Read only the header of a state file (for the save list). 1 ok, 0 none, -1 other version. */
int state_read_info(int slot, StateHeader *out) {
    char p[1024]; slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char magic[8]; u32 ver = 0;
    int ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, MAGIC, 8) && fread(&ver, 4, 1, f) == 1 &&
             ver == VERSION && fread(out, sizeof *out, 1, f) == 1;
    fclose(f);
    return ok ? 1 : -1;
}

static void all_modules(StateIO *s, Ctx *c) {
    st_io(s, c, sizeof *c);
    rt_state(s);
    gpu_state(s);
    etc_state(s);
    cd_state(s);
    audio_state(s);
    spu_state(s);
    memcard_state(s);
    kanji_state(s);
}

static void fill_header(StateHeader *h) {
    memset(h, 0, sizeof *h);
    h->exe_crc = g_exe_crc;
    h->time = (s64)time(NULL);
    u32 ovl = rd32(OVL_ID_ADDR);
    h->battle = ovl == 6;
    h->in_game = ovl == 5 || ovl == 6;
    if (h->in_game) {
        h->year = (u16)rd32(RAM_YEAR);
        h->month = (u16)(rd8(RAM_MONTH0) + 1);
        h->phase = (u8)rd8(RAM_PHASE);
    }
    platform_thumbnail(h->thumb, THUMB_W, THUMB_H);
}

static int write_state(Ctx *c, int slot) {
    char path[1024], dir[1024];
    slot_path(slot, path, sizeof path);
    state_dir(dir, sizeof dir);
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    char tmp[1040]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    StateIO s = { f, 0, 0 };
    fwrite(MAGIC, 1, 8, f);
    u32 ver = VERSION; ST(&s, ver);
    StateHeader h; fill_header(&h); ST(&s, h);
    Ctx saved = *c;
    saved.r[2] = 0;                    /* VSync's return value when the state resumes */
    all_modules(&s, &saved);
    fclose(f);
    if (s.err) { remove(tmp); return 0; }
    remove(path);
    rename(tmp, path);
    g_files_version++;
    rt_log("[state] saved %s", path);
    return 1;
}

void state_slot_name(int slot, char *out, size_t n) {
    if (slot == STATE_DEBUG_SLOT) snprintf(out, n, "Debug state");
    else if (slot > AUTO_BASE) snprintf(out, n, "Autosave %d", slot - AUTO_BASE);
    else snprintf(out, n, "Slot %d", slot);
}

static void do_load(Ctx *c, int slot) {
    char path[1024], name[32], msg[96];
    slot_path(slot, path, sizeof path);
    state_slot_name(slot, name, sizeof name);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, sizeof msg, "%s is empty", name); ui_toast(msg); return; }
    char magic[8]; u32 ver = 0; StateHeader h;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, MAGIC, 8) || fread(&ver, 4, 1, f) != 1 || ver != VERSION ||
        fread(&h, sizeof h, 1, f) != 1) {
        fclose(f); snprintf(msg, sizeof msg, "%s was made by another version of the port", name); ui_toast(msg); return;
    }
    if (h.exe_crc != g_exe_crc) {
        fclose(f); snprintf(msg, sizeof msg, "%s belongs to a different game disc / patch", name); ui_toast(msg); return;
    }
    StateIO s = { f, 1, 0 };
    audio_lock();
    all_modules(&s, c);
    audio_unlock();
    fclose(f);
    if (s.err) rt_fatal("save state %s is truncated", path);   /* state already partly applied */
    audio_flush_xa();
    disc_sync_from_ram();               /* the state may come from the other disc */
    platform_set_movie_frame(NULL, 0, 0);
    state_reset_autosave_tracking();
    snprintf(msg, sizeof msg, "%s loaded", name);
    ui_toast(msg);
    rt_log("[state] loaded %s", path);
    rt_resume_at(c, c->r[31]);         /* continue right after the VSync call that was saved */
}

/* ------------------------------------------------------------------ autosave */
/* A turn change is a change of the phase byte: organize -> attack ("Finalize Organize")
 * and attack -> next month's organize ("Finalize Attack"). A battle start is the battle
 * overlay replacing the strategy one. Frames where neither overlay is resident (loading)
 * keep the previous observation, so a strategy -> loading -> battle switch is still seen. */
static int g_prev_valid, g_prev_phase, g_prev_battle;
static int g_auto_pending;           /* vblanks left before an autosave is written (0 = none) */
static int g_last_auto;              /* last autosave slot written (0 = unknown yet) */

void state_reset_autosave_tracking(void) { g_prev_valid = 0; g_auto_pending = 0; }

static int next_auto_slot(void) {    /* rotate 1 -> 2 -> 3 -> 1; first time: after the newest file */
    if (!g_last_auto) {
        s64 newest = -1; g_last_auto = AUTO_BASE + AUTO_COUNT;
        for (int i = 1; i <= AUTO_COUNT; i++) {
            StateHeader h;
            if (state_read_info(AUTO_BASE + i, &h) == 1 && h.time >= newest) { newest = h.time; g_last_auto = AUTO_BASE + i; }
        }
    }
    g_last_auto = g_last_auto >= AUTO_BASE + AUTO_COUNT ? AUTO_BASE + 1 : g_last_auto + 1;
    return g_last_auto;
}

static void autosave_watch(Ctx *c) {
    if (!g_settings.autosave) { g_prev_valid = 0; g_auto_pending = 0; return; }
    u32 ovl = rd32(OVL_ID_ADDR);
    if (ovl == 5 || ovl == 6) {
        int phase = rd8(RAM_PHASE), battle = ovl == 6;
        if (g_prev_valid) {
            int new_phase = !battle && !g_prev_battle && phase != g_prev_phase;
            int new_battle = battle && !g_prev_battle;
            if (new_phase || new_battle) {
                if (getenv("BRIG_TRACE")) rt_log("[state] autosave trigger: phase %d->%d battle %d->%d", g_prev_phase, phase, g_prev_battle, battle);
                g_auto_pending = 30;     /* let the transition settle (~0.5 s) */
            }
        }
        g_prev_valid = 1; g_prev_phase = phase; g_prev_battle = battle;
    }
    if (g_auto_pending && --g_auto_pending == 0) {
        int slot = next_auto_slot();
        if (write_state(c, slot)) ui_toast("Autosaved");
    }
}

void state_vsync_point(Ctx *c) {
    if (g_req_delete >= 0) {
        char p[1024]; slot_path(g_req_delete, p, sizeof p);
        remove(p); g_files_version++; g_req_delete = -1;
    }
    if (g_req_save >= 0) {
        int sl = g_req_save; g_req_save = -1;
        char name[32], msg[64]; state_slot_name(sl, name, sizeof name);
        snprintf(msg, sizeof msg, write_state(c, sl) ? "Saved to %s" : "Saving to %s failed", name);
        ui_toast(msg);
    }
    if (g_req_load >= 0) { int sl = g_req_load; g_req_load = -1; do_load(c, sl); }
    autosave_watch(c);
}
