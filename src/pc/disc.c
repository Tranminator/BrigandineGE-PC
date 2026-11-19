/* Raw MODE2/2352 disc image access + ISO9660 lookup. */
#include "hle.h"
#include <stdio.h>
#include <ctype.h>

static FILE *g_disc;

int disc_open(const char *path) {
    if (g_disc) fclose(g_disc);
    g_disc = fopen(path, "rb");
    return g_disc != NULL;
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
