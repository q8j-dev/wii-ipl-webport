// KPR, the SDK's keyboard character processing (not part of the
// decompilation). The software keyboard passes every character through a
// KPR queue: dead keys (combining accents, used by the European layouts)
// are composed with the next letter, and in Japanese the romaji typed on the
// QWERTY keyboard are converted to kana.
//
// The queue holds the characters ready for output first (oCount of them),
// followed by the input still being composed (iCount); the keyboard code
// reads and edits these counts directly.

#include "web.h"

#include <string.h>

typedef struct {
    u16 text[5];  // 0x00
    u32 mode;     // 0x0C
    u8 oCount;    // 0x10
    u8 iCount;    // 0x11
    u32 altVal;   // 0x14
} KPRQueue;

enum {
    KPR_MODE_DEADKEY = 1 << 1,
    KPR_MODE_JP_ROMAJI_HIRAGANA = 1 << 2,
    KPR_MODE_JP_ROMAJI_KATAKANA = 1 << 3,
};

#define KPR_FLUSH 0xFFFF
#define KPR_SIZE 5

// ---------------------------------------------------------------------------
// Queue

static void put_output(KPRQueue* q, u16 c) {
    if (q->oCount + q->iCount >= KPR_SIZE)
        return;
    // Output goes before the pending input.
    memmove(&q->text[q->oCount + 1], &q->text[q->oCount], q->iCount * sizeof(u16));
    q->text[q->oCount++] = c;
}

static u16* pending(KPRQueue* q) { return &q->text[q->oCount]; }

static void drop_pending(KPRQueue* q, int n) {
    u16* p = pending(q);
    memmove(p, p + n, (q->iCount - n) * sizeof(u16));
    q->iCount -= n;
}

static void push_pending(KPRQueue* q, u16 c) {
    if (q->oCount + q->iCount < KPR_SIZE)
        q->text[q->oCount + q->iCount++] = c;
}

// ---------------------------------------------------------------------------
// Dead keys

static u16 spacing_accent(u16 mark) {
    switch (mark) {
        case 0x300: return 0x60;
        case 0x301: return 0xB4;
        case 0x302: return 0x5E;
        case 0x303: return 0x7E;
        case 0x308: return 0xA8;
        case 0x30D: return 0x27;
        case 0x30E: return 0x22;
        case 0x327: return 0xB8;
    }
    return mark;
}

static int is_dead(u16 c) { return c >= 0x300 && c <= 0x36F; }

static const struct {
    u16 mark;
    const char* base;
    u16 composed[12];
} kCompose[] = {
    {0x300, "AEIOUaeiou", {0xC0, 0xC8, 0xCC, 0xD2, 0xD9, 0xE0, 0xE8, 0xEC, 0xF2, 0xF9}},
    {0x301, "AEIOUYaeiouy", {0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xDD, 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xFD}},
    {0x30D, "AEIOUYaeiouy", {0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xDD, 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xFD}},
    {0x302, "AEIOUaeiou", {0xC2, 0xCA, 0xCE, 0xD4, 0xDB, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB}},
    {0x303, "ANOano", {0xC3, 0xD1, 0xD5, 0xE3, 0xF1, 0xF5}},
    {0x308, "AEIOUYaeiouy", {0xC4, 0xCB, 0xCF, 0xD6, 0xDC, 0x178, 0xE4, 0xEB, 0xEF, 0xF6, 0xFC, 0xFF}},
    {0x30E, "AEIOUYaeiouy", {0xC4, 0xCB, 0xCF, 0xD6, 0xDC, 0x178, 0xE4, 0xEB, 0xEF, 0xF6, 0xFC, 0xFF}},
    {0x327, "Cc", {0xC7, 0xE7}},
};

static u16 compose(u16 mark, u16 c) {
    for (u32 i = 0; i < sizeof(kCompose) / sizeof(kCompose[0]); i++) {
        if (kCompose[i].mark != mark)
            continue;
        for (int j = 0; kCompose[i].base[j]; j++)
            if ((u16)(u8)kCompose[i].base[j] == c)
                return kCompose[i].composed[j];
    }
    return 0;
}

static void put_deadkey(KPRQueue* q, u16 c) {
    if (q->iCount) {
        u16 mark = pending(q)[0];
        drop_pending(q, 1);
        if (c == KPR_FLUSH || c == ' ') {
            put_output(q, spacing_accent(mark));
            return;
        }
        u16 composed = compose(mark, c);
        if (composed) {
            put_output(q, composed);
            return;
        }
        put_output(q, spacing_accent(mark));
    }
    if (c == KPR_FLUSH)
        return;
    if (is_dead(c))
        push_pending(q, c);
    else
        put_output(q, c);
}

// ---------------------------------------------------------------------------
// Romaji to kana

static const struct {
    const char* roma;
    u16 kana[2];
} kRomaji[] = {
    {"a", {0x3042}}, {"i", {0x3044}}, {"u", {0x3046}}, {"e", {0x3048}}, {"o", {0x304A}},
    {"ka", {0x304B}}, {"ki", {0x304D}}, {"ku", {0x304F}}, {"ke", {0x3051}}, {"ko", {0x3053}},
    {"sa", {0x3055}}, {"si", {0x3057}}, {"shi", {0x3057}}, {"su", {0x3059}}, {"se", {0x305B}}, {"so", {0x305D}},
    {"ta", {0x305F}}, {"ti", {0x3061}}, {"chi", {0x3061}}, {"tu", {0x3064}}, {"tsu", {0x3064}}, {"te", {0x3066}},
    {"to", {0x3068}},
    {"na", {0x306A}}, {"ni", {0x306B}}, {"nu", {0x306C}}, {"ne", {0x306D}}, {"no", {0x306E}},
    {"ha", {0x306F}}, {"hi", {0x3072}}, {"hu", {0x3075}}, {"fu", {0x3075}}, {"he", {0x3078}}, {"ho", {0x307B}},
    {"ma", {0x307E}}, {"mi", {0x307F}}, {"mu", {0x3080}}, {"me", {0x3081}}, {"mo", {0x3082}},
    {"ya", {0x3084}}, {"yu", {0x3086}}, {"yo", {0x3088}},
    {"ra", {0x3089}}, {"ri", {0x308A}}, {"ru", {0x308B}}, {"re", {0x308C}}, {"ro", {0x308D}},
    {"wa", {0x308F}}, {"wo", {0x3092}}, {"nn", {0x3093}}, {"n'", {0x3093}},
    {"ga", {0x304C}}, {"gi", {0x304E}}, {"gu", {0x3050}}, {"ge", {0x3052}}, {"go", {0x3054}},
    {"za", {0x3056}}, {"zi", {0x3058}}, {"ji", {0x3058}}, {"zu", {0x305A}}, {"ze", {0x305C}}, {"zo", {0x305E}},
    {"da", {0x3060}}, {"di", {0x3062}}, {"du", {0x3065}}, {"de", {0x3067}}, {"do", {0x3069}},
    {"ba", {0x3070}}, {"bi", {0x3073}}, {"bu", {0x3076}}, {"be", {0x3079}}, {"bo", {0x307C}},
    {"pa", {0x3071}}, {"pi", {0x3074}}, {"pu", {0x3077}}, {"pe", {0x307A}}, {"po", {0x307D}},
    {"xa", {0x3041}}, {"xi", {0x3043}}, {"xu", {0x3045}}, {"xe", {0x3047}}, {"xo", {0x3049}},
    {"la", {0x3041}}, {"li", {0x3043}}, {"lu", {0x3045}}, {"le", {0x3047}}, {"lo", {0x3049}},
    {"xya", {0x3083}}, {"xyu", {0x3085}}, {"xyo", {0x3087}}, {"lya", {0x3083}}, {"lyu", {0x3085}}, {"lyo", {0x3087}},
    {"xtu", {0x3063}}, {"xtsu", {0x3063}}, {"ltu", {0x3063}}, {"ltsu", {0x3063}}, {"xwa", {0x308E}}, {"lwa", {0x308E}},
    {"kya", {0x304D, 0x3083}}, {"kyu", {0x304D, 0x3085}}, {"kyo", {0x304D, 0x3087}},
    {"gya", {0x304E, 0x3083}}, {"gyu", {0x304E, 0x3085}}, {"gyo", {0x304E, 0x3087}},
    {"sya", {0x3057, 0x3083}}, {"syu", {0x3057, 0x3085}}, {"syo", {0x3057, 0x3087}},
    {"sha", {0x3057, 0x3083}}, {"shu", {0x3057, 0x3085}}, {"she", {0x3057, 0x3047}}, {"sho", {0x3057, 0x3087}},
    {"ja", {0x3058, 0x3083}}, {"ju", {0x3058, 0x3085}}, {"je", {0x3058, 0x3047}}, {"jo", {0x3058, 0x3087}},
    {"jya", {0x3058, 0x3083}}, {"jyu", {0x3058, 0x3085}}, {"jyo", {0x3058, 0x3087}},
    {"zya", {0x3058, 0x3083}}, {"zyu", {0x3058, 0x3085}}, {"zyo", {0x3058, 0x3087}},
    {"tya", {0x3061, 0x3083}}, {"tyu", {0x3061, 0x3085}}, {"tyo", {0x3061, 0x3087}},
    {"cha", {0x3061, 0x3083}}, {"chu", {0x3061, 0x3085}}, {"che", {0x3061, 0x3047}}, {"cho", {0x3061, 0x3087}},
    {"dya", {0x3062, 0x3083}}, {"dyu", {0x3062, 0x3085}}, {"dyo", {0x3062, 0x3087}},
    {"nya", {0x306B, 0x3083}}, {"nyu", {0x306B, 0x3085}}, {"nyo", {0x306B, 0x3087}},
    {"hya", {0x3072, 0x3083}}, {"hyu", {0x3072, 0x3085}}, {"hyo", {0x3072, 0x3087}},
    {"bya", {0x3073, 0x3083}}, {"byu", {0x3073, 0x3085}}, {"byo", {0x3073, 0x3087}},
    {"pya", {0x3074, 0x3083}}, {"pyu", {0x3074, 0x3085}}, {"pyo", {0x3074, 0x3087}},
    {"mya", {0x307F, 0x3083}}, {"myu", {0x307F, 0x3085}}, {"myo", {0x307F, 0x3087}},
    {"rya", {0x308A, 0x3083}}, {"ryu", {0x308A, 0x3085}}, {"ryo", {0x308A, 0x3087}},
    {"fa", {0x3075, 0x3041}}, {"fi", {0x3075, 0x3043}}, {"fe", {0x3075, 0x3047}}, {"fo", {0x3075, 0x3049}},
    {"thi", {0x3066, 0x3043}}, {"dhi", {0x3067, 0x3043}}, {"wi", {0x3046, 0x3043}}, {"we", {0x3046, 0x3047}},
    {"va", {0x3094, 0x3041}}, {"vi", {0x3094, 0x3043}}, {"vu", {0x3094}}, {"ve", {0x3094, 0x3047}},
    {"vo", {0x3094, 0x3049}},
};

#define ROMAJI_COUNT (sizeof(kRomaji) / sizeof(kRomaji[0]))

// 2: exact match (index in *match), 1: prefix of an entry, 0: neither.
static int romaji_lookup(const u16* p, int n, int* match) {
    int prefix = 0;
    for (u32 i = 0; i < ROMAJI_COUNT; i++) {
        const char* r = kRomaji[i].roma;
        int j = 0;
        while (j < n && r[j] && (u16)r[j] == p[j])
            j++;
        if (j < n)
            continue;
        if (!r[j]) {
            *match = (int)i;
            return 2;
        }
        prefix = 1;
    }
    return prefix;
}

static u16 to_kana(KPRQueue* q, u16 c) {
    // Katakana are the hiragana moved up by 0x60.
    if ((q->mode & KPR_MODE_JP_ROMAJI_KATAKANA) && c >= 0x3041 && c <= 0x3096)
        return c + 0x60;
    return c;
}

static int is_vowel(u16 c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

static void romaji_flush(KPRQueue* q) {
    while (q->iCount) {
        u16 c = pending(q)[0];
        drop_pending(q, 1);
        put_output(q, c == 'n' ? to_kana(q, 0x3093) : c);
    }
}

static void romaji_reduce(KPRQueue* q) {
    while (q->iCount) {
        u16* p = pending(q);
        int match;
        int r = romaji_lookup(p, q->iCount, &match);
        if (r == 2) {
            drop_pending(q, q->iCount);
            put_output(q, to_kana(q, kRomaji[match].kana[0]));
            if (kRomaji[match].kana[1])
                put_output(q, to_kana(q, kRomaji[match].kana[1]));
            return;
        }
        if (r == 1)
            return;
        if (q->iCount >= 2 && p[0] == 'n' && !is_vowel(p[1]) && p[1] != 'y') {
            // "n" before a consonant
            drop_pending(q, 1);
            put_output(q, to_kana(q, 0x3093));
            continue;
        }
        if (q->iCount >= 2 && p[0] == p[1] && !is_vowel(p[0])) {
            // Doubled consonant: small tsu
            drop_pending(q, 1);
            put_output(q, to_kana(q, 0x3063));
            continue;
        }
        u16 c = p[0];
        drop_pending(q, 1);
        put_output(q, c);
    }
}

static void put_romaji(KPRQueue* q, u16 c) {
    if (c == KPR_FLUSH) {
        romaji_flush(q);
        return;
    }
    if (c == '-') {
        romaji_flush(q);
        put_output(q, 0x30FC);
        return;
    }
    if (!((c >= 'a' && c <= 'z') || c == '\'')) {
        romaji_flush(q);
        put_output(q, c);
        return;
    }
    push_pending(q, c);
    romaji_reduce(q);
}

// ---------------------------------------------------------------------------

void KPRInitRegionUS(void) {}
void KPRInitRegionJP(void) {}
void KPRInitRegionEU(void) {}

void KPRClearQueue(KPRQueue* q) {
    q->oCount = 0;
    q->iCount = 0;
    q->altVal = 0;
}

void KPRInitQueue(KPRQueue* q) {
    memset(q, 0, sizeof(*q));
}

void KPRSetMode(KPRQueue* q, u32 mode) { q->mode = mode; }
u32 KPRGetMode(KPRQueue* q) { return q->mode; }

// Returns the number of characters ready for output.
u8 KPRPutChar(KPRQueue* q, u16 c) {
    if (q->mode & (KPR_MODE_JP_ROMAJI_HIRAGANA | KPR_MODE_JP_ROMAJI_KATAKANA))
        put_romaji(q, c);
    else if (q->mode & KPR_MODE_DEADKEY)
        put_deadkey(q, c);
    else if (c != KPR_FLUSH)
        put_output(q, c);
    return q->oCount;
}

// Returns the next output character, 0 if there is none.
u16 KPRGetChar(KPRQueue* q) {
    if (!q->oCount)
        return 0;
    u16 c = q->text[0];
    memmove(&q->text[0], &q->text[1], (q->oCount + q->iCount - 1) * sizeof(u16));
    q->oCount--;
    return c;
}

// Returns the number of characters still being composed and copies them.
u8 KPRLookAhead(KPRQueue* q, u16* out, u32 max) {
    if (out) {
        u32 n = q->iCount < max ? q->iCount : max;
        memcpy(out, pending(q), n * sizeof(u16));
        if (n < max)
            out[n] = 0;
    }
    return q->iCount;
}
