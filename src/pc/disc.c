/* Raw MODE2/2352 disc image access + ISO9660 lookup.
 * Two images: Disc 1 and Disc 2. The game asks for the other disc itself (main func 8004C64C, the
 * "Insert disk N" routine); the port then swaps the image in at once (disc_select), so nothing has
 * to be done by the player. Both discs hold the same code and data; they differ in their movies,
 * XA/MUSIC0.XA and the EXE name the game checks (SLPS_026.61 / .62). */
#include "hle.h"
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>

static FILE *g_imgs[2];       /* Disc 1, Disc 2 (NULL: not found) */
static FILE *g_disc;          /* the one "in the drive" */
static int g_cur;             /* 0 = Disc 1, 1 = Disc 2 */
static char g_paths[2][1024];

static int open_slot(int i, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (g_imgs[i]) fclose(g_imgs[i]);
    g_imgs[i] = f;
    snprintf(g_paths[i], sizeof g_paths[i], "%s", path);
    return 1;
}

/* Disc 2 next to Disc 1: the same path with "Disc 1" -> "Disc 2" (file, or the first .bin in that
 * folder), as in "iso/Disc 1/...(Disc 1).bin" + "iso/Disc 2/...(Disc 2).bin" */
static void find_disc2(const char *d1) {
    char p[1024];
    snprintf(p, sizeof p, "%s", d1);
    char *hit = NULL;
    for (char *q = strstr(p, "Disc 1"); q; q = strstr(q + 1, "Disc 1")) { q[5] = '2'; hit = q; }
    if (!hit) return;
    if (open_slot(1, p)) return;
    char dir[1024]; snprintf(dir, sizeof dir, "%s", p);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".bin")) {
            char f[2048]; snprintf(f, sizeof f, "%s/%s", dir, e->d_name);
            if (open_slot(1, f)) break;
        }
    }
    closedir(d);
}

int disc_open(const char *path) {
    if (!open_slot(0, path)) return 0;
    g_disc = g_imgs[0]; g_cur = 0;
    if (!g_imgs[1]) find_disc2(path);
    rt_log("[disc] Disc 1: %s", g_paths[0]);
    rt_log("[disc] Disc 2: %s", g_imgs[1] ? g_paths[1] : "(not found)");
    if (getenv("BRIG_START_DISC") && atoi(getenv("BRIG_START_DISC")) == 2) disc_select(1);   /* tests */
    return 1;
}

int disc_open_second(const char *path) { return open_slot(1, path); }

int disc_current(void) { return g_cur; }

int disc_select(int i) {
    if (i < 0 || i > 1 || !g_imgs[i]) return 0;
    if (i != g_cur) rt_log("[disc] Disc %d in the drive", i + 1);
    g_disc = g_imgs[i]; g_cur = i;
    return 1;
}

int disc_read_sector(u32 lba, u8 *dst) {
    u8 raw[2352];
    if (!g_disc || fseek(g_disc, (long)lba * 2352, SEEK_SET) != 0 || fread(raw, 1, 2352, g_disc) != 2352) {
        memset(dst, 0, 2048);
        return 0;
    }
    memcpy(dst, raw + 24, 2048);
    return 1;
}

int disc_read_raw(u32 lba, u8 *dst2352) {
    if (!g_disc || fseek(g_disc, (long)lba * 2352, SEEK_SET) != 0 || fread(dst2352, 1, 2352, g_disc) != 2352) {
        memset(dst2352, 0, 2352);
        return 0;
    }
    return 1;
}

static int name_eq(const u8 *rec_name, int len, const char *want, int wlen) {
    /* compare ignoring ";1" version suffix and case */
    int rl = len;
    for (int i = 0; i < len; i++) if (rec_name[i] == ';') { rl = i; break; }
    int wl = wlen;
    for (int i = 0; i < wlen; i++) if (want[i] == ';') { wl = i; break; }
    if (rl != wl) return 0;
    for (int i = 0; i < rl; i++) if (toupper(rec_name[i]) != toupper((unsigned char)want[i])) return 0;
    return 1;
}

static int find_in_dir(u32 lba, u32 size, const char *name, int nlen, u32 *out_lba, u32 *out_size, int *is_dir) {
    u8 sec[2048];
    for (u32 s = 0; s < (size + 2047) / 2048; s++) {
        disc_read_sector(lba + s, sec);
        int pos = 0;
        while (pos < 2048 && sec[pos]) {
            const u8 *r = sec + pos;
            if (r[32] > 1 || (r[33] > 1)) {
                if (name_eq(r + 33, r[32], name, nlen)) {
                    memcpy(out_lba, r + 2, 4);
                    memcpy(out_size, r + 10, 4);
                    *is_dir = (r[25] & 2) != 0;
                    return 1;
                }
            }
            pos += r[0];
        }
    }
    return 0;
}

int disc_find_file(const char *path, u32 *lba, u32 *size) {
    u8 pvd[2048];
    disc_read_sector(16, pvd);
    u32 cur_lba, cur_size;
    memcpy(&cur_lba, pvd + 156 + 2, 4);
    memcpy(&cur_size, pvd + 156 + 10, 4);
    const char *p = path;
    while (*p == '\\' || *p == '/') p++;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\\' && *e != '/') e++;
        int is_dir = 0;
        if (!find_in_dir(cur_lba, cur_size, p, (int)(e - p), &cur_lba, &cur_size, &is_dir)) return 0;
        p = e;
        while (*p == '\\' || *p == '/') p++;
    }
    *lba = cur_lba; *size = cur_size;
    return 1;
}
