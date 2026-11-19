/* Save states (quick save / quick load).
 *
 * A state is taken at the game's VSync call, the one point every frame passes through
 * where all live state belongs to the emulated PS1: RAM (including the PS1 call stack
 * with every saved return address), CPU/GTE registers, VRAM/GPU, SPU, CD drive, pads.
 * Loading restores it and resumes guest execution right after that VSync call through
 * rt_resume_at(), so a state does not depend on the host C stack and stays valid after
 * the app is restarted.
 *
 * Files: <pref>/states/slot<N>.state  (N = 1..10). */
#include "state.h"
#include "hle.h"
#include "audio.h"
#include <SDL.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define MAGIC   "BRIGSTAT"
#define VERSION 1

void ui_toast(const char *msg);   /* ui.cpp */

static int g_slot = 1;
static int g_req_save = -1, g_req_load = -1;

int state_slot(void) { return g_slot; }
void state_set_slot(int slot) { g_slot = slot < 1 ? 1 : slot > 10 ? 10 : slot; }   /* manual slots 1..10 */
void state_request_save(int slot) { g_req_save = slot; }
void state_request_load(int slot) { g_req_load = slot; }

static void slot_path(int slot, char *out, size_t n) {
    const char *env = getenv("BRIG_STATE_DIR");          /* tests */
    char *dir = env ? NULL : SDL_GetPrefPath("PortGamePC", "Brigandine");
    snprintf(out, n, "%s%sslot%d.state", env ? env : dir ? dir : "", env ? "/" : "states/", slot);
    if (dir) SDL_free(dir);
}

int state_slot_exists(int slot) {
    char p[1024]; slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
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

static void do_save(Ctx *c, int slot) {
    char path[1024], dir[1024];
    slot_path(slot, path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0;
#ifdef _WIN32
        _mkdir(dir);
#else
        mkdir(dir, 0755);
#endif
    }
    char tmp[1040]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { ui_toast("Save state failed (cannot write file)"); return; }
    StateIO s = { f, 0, 0 };
    fwrite(MAGIC, 1, 8, f);
    u32 ver = VERSION; ST(&s, ver);
    Ctx saved = *c;
    saved.r[2] = 0;                    /* VSync's return value when the state resumes */
    all_modules(&s, &saved);
    fclose(f);
    if (s.err) { remove(tmp); ui_toast("Save state failed"); return; }
    remove(path);
    rename(tmp, path);
    char msg[64]; snprintf(msg, sizeof msg, "State %d saved", slot);
    ui_toast(msg);
    rt_log("[state] saved slot %d -> %s", slot, path);
}

static void do_load(Ctx *c, int slot) {
    char path[1024]; slot_path(slot, path, sizeof path);
    FILE *f = fopen(path, "rb");
    char msg[64];
    if (!f) { snprintf(msg, sizeof msg, "State %d is empty", slot); ui_toast(msg); return; }
    char magic[8]; u32 ver = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, MAGIC, 8) || fread(&ver, 4, 1, f) != 1 || ver != VERSION) {
        fclose(f); snprintf(msg, sizeof msg, "State %d is from another version", slot); ui_toast(msg); return;
    }
    StateIO s = { f, 1, 0 };
    audio_lock();
    all_modules(&s, c);
    audio_unlock();
    fclose(f);
    if (s.err) rt_fatal("save state %s is truncated", path);   /* state already partly applied */
    audio_flush_xa();
    platform_set_movie_frame(NULL, 0, 0);
    snprintf(msg, sizeof msg, "State %d loaded", slot);
    ui_toast(msg);
    rt_log("[state] loaded slot %d", slot);
    rt_resume_at(c, c->r[31]);         /* continue right after the VSync call that was saved */
}

void state_vsync_point(Ctx *c) {
    if (g_req_save >= 0) { int sl = g_req_save; g_req_save = -1; do_save(c, sl); }
    if (g_req_load >= 0) { int sl = g_req_load; g_req_load = -1; do_load(c, sl); }
}
