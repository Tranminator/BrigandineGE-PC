/* Brigandine: Grand Edition — known structures.
 * Source: RAM anchors from the user's cheat tables (docs/cheats), cross-checked
 * against code references in the disassembly. Offsets marked (?) are unverified. */
#ifndef BRIGANDINE_H
#define BRIGANDINE_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed char    s8;
typedef signed short   s16;
typedef signed int     s32;

enum KingdomId {
    KINGDOM_ALMEKIA  = 0,
    KINGDOM_NORGARD  = 1,
    KINGDOM_LEONIA   = 2,
    KINGDOM_CAERLEON = 3,
    KINGDOM_ISCALIO  = 4,
    KINGDOM_ESGARES  = 5,
    KINGDOM_COUNT    = 6
};

#define ITEM_SLOT_COUNT 0x94

/* 0x9E bytes, array of 6 at 0x800ADD38 (D_800ADD38, 256 code refs). */
typedef struct Kingdom {
    /* 0x00 */ u16 mana;
    /* 0x02 */ u8  unk02[6];
    /* 0x08 */ u8  items[ITEM_SLOT_COUNT]; /* count per item id */
    /* 0x9C */ u8  unk9C[2];
} Kingdom; /* size 0x9E */

/* Knight / unit record (layout from Cheat Engine "Character Editor"). */
typedef struct Knight {
    /* 0x00 */ u8  classId;
    /* 0x01 */ u8  level;
    /* 0x02 */ u16 exp;
    /* 0x04 */ u16 hp;
    /* 0x06 */ u16 mp;
    /* 0x08 */ u8  str;
    /* 0x09 */ u8  intel;
    /* 0x0A */ u8  agi;
    /* 0x0B */ u8  runeArea;
    /* 0x0C */ u16 runePower;
    /* 0x0E */ u8  unk0E[6];
    /* 0x14 */ u8  classLevels[4]; /* expert class & sub-classes */
    /* 0x18 */ u8  weapon;
    /* 0x19 */ u8  item;
    /* 0x1A */ u8  unk1A[2];
    /* 0x1C */ u8  kingdom;
    /* 0x1D */ u8  location;    /* castle id */
    /* 0x1E */ u8  unk1E;
    /* 0x1F */ u8  action;
    /* 0x20 */ u8  unk20;
    /* 0x21 */ u8  questDays;
    /* 0x22 */ u8  status;
    /* size unknown (?) */
} Knight;

extern Kingdom g_Kingdoms[KINGDOM_COUNT]; /* 0x800ADD38 */
extern u8      g_KnightTable[];           /* 0x800973B0 (491 refs) — element type TBD */
extern u8      g_MonsterTable[];          /* 0x800A79D0 (186 refs) — per-kingdom stride 0xC08 (?) */

#endif
