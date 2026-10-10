// IOS (the Starlet's operating system) at the IPC level.
//
// The SDK libraries (NAND, ISFS, ES, SC, NWC24, ...) run unchanged and send
// their requests through IOS_Open/Ioctl/Ioctlv/...; this file answers them.
// Synchronous requests complete immediately; asynchronous ones complete at
// the next interrupt point, like an IPC reply interrupt.
//
// Request structures are in big-endian memory and are read through the
// normal C types. Files live in the virtual NAND kept by web/js/nand.js.

#include "web.h"

#include <private/es/types.h>
#include <private/ios.h>
#include <private/ipc.h>
#include <revolution/os.h>

#include <stdlib.h>
#include <string.h>

enum {
    ISFS_OK = 0,
    ISFS_INVALID = -101,
    ISFS_ACCESS = -102,
    ISFS_EXISTS = -105,
    ISFS_NOEXISTS = -106,
    ISFS_MAXFD = -109,
    ISFS_NOTEMPTY = -115,
};

enum {
    KIND_NONE,
    KIND_FILE,
    KIND_FS,
    KIND_ES,
    KIND_DI,
    KIND_STM_IMMEDIATE,
    KIND_STM_EVENT,
    KIND_NCD,
    KIND_KD,
    KIND_STUB,
};

#define MAX_FDS 64

typedef struct {
    int kind;
    char path[128];
    u32 mode;
    u32 pos;
} IosFd;

static IosFd sFds[MAX_FDS];

// ---------------------------------------------------------------------------
// Virtual NAND helpers

static int fs_stat(const char* path, u32* type, u32* size) {
    u32 out[2];
    s32 r = webjs_fs_stat(path, out);
    if (r < 0)
        return r;
    *type = out[0];
    *size = out[1];
    return 0;
}

static int fs_exists(const char* path) {
    u32 t, s;
    return fs_stat(path, &t, &s) == 0;
}

static void parent_of(const char* path, char* out) {
    strcpy(out, path);
    char* slash = strrchr(out, '/');
    if (slash == out)
        out[1] = 0;
    else if (slash)
        *slash = 0;
}

// ---------------------------------------------------------------------------
// /dev/fs

int __web_nand_exists(const char* path) { return fs_exists(path); }

static s32 fs_create(const char* path, int dir) {
    if (path[0] != '/')
        return ISFS_INVALID;
    if (fs_exists(path))
        return ISFS_EXISTS;
    char parent[128];
    parent_of(path, parent);
    u32 t, s;
    if (fs_stat(parent, &t, &s) != 0 || t != 2)
        return ISFS_NOEXISTS;
    return webjs_fs_create(path, dir) < 0 ? ISFS_INVALID : ISFS_OK;
}

static s32 fs_ioctl(s32 cmd, u8* in, u32 inLen, u8* out, u32 outLen) {
    switch (cmd) {
        case 2: {  // GET_STATS
            u32* st = (u32*)out;
            st[0] = 0x4000;  // block size
            st[1] = 0x4000;  // free blocks
            st[2] = 0x200;   // used blocks
            st[3] = 0;
            st[4] = 0;
            st[5] = 0x1000;  // free inodes
            st[6] = 0x100;
            return ISFS_OK;
        }
        case 3:  // CREATE_DIR
        case 9: {  // CREATE_FILE
            const char* path = (const char*)in + 6;
            return fs_create(path, cmd == 3);
        }
        case 5:  // SET_ATTR
            return fs_exists((const char*)in + 6) ? ISFS_OK : ISFS_NOEXISTS;
        case 6: {  // GET_ATTR: in = path, out = ISFSPathAttrArgs
            const char* path = (const char*)in;
            if (!fs_exists(path))
                return ISFS_NOEXISTS;
            memset(out, 0, outLen);
            *(u32*)out = 0x1000;          // owner id
            *(u16*)(out + 4) = 1;         // group id
            strncpy((char*)out + 6, path, 64);
            out[0x46] = 3;  // owner rw
            out[0x47] = 3;
            out[0x48] = 3;
            out[0x49] = 0;
            return ISFS_OK;
        }
        case 7: {  // DELETE_PATH
            const char* path = (const char*)in;
            if (!fs_exists(path))
                return ISFS_NOEXISTS;
            return webjs_fs_delete(path) < 0 ? ISFS_ACCESS : ISFS_OK;
        }
        case 8: {  // RENAME_PATH
            const char* from = (const char*)in;
            const char* to = (const char*)in + 64;
            if (!fs_exists(from))
                return ISFS_NOEXISTS;
            return webjs_fs_rename(from, to) < 0 ? ISFS_ACCESS : ISFS_OK;
        }
        case 13:  // SHUTDOWN
            return ISFS_OK;
    }
    __web_report("[ios] /dev/fs ioctl %d unhandled", cmd);
    return ISFS_INVALID;
}

static char sDirBuf[0x10000];

static s32 fs_ioctlv(s32 cmd, u32 nIn, u32 nOut, IOSIoVector* v) {
    switch (cmd) {
        case 4: {  // READ_DIR
            const char* path = (const char*)v[0].base;
            u32 t, s;
            if (fs_stat(path, &t, &s) != 0)
                return ISFS_NOEXISTS;
            if (t != 2)
                return ISFS_INVALID;
            s32 n = webjs_fs_readdir(path, sDirBuf, sizeof(sDirBuf));
            if (n < 0)
                return ISFS_INVALID;
            if (nIn == 1) {
                *(u32*)v[1].base = n;
                return ISFS_OK;
            }
            u32 max = *(u32*)v[1].base;
            char* dst = (char*)v[2].base;
            const char* src = sDirBuf;
            u32 count = 0;
            for (s32 i = 0; i < n && count < max; i++) {
                u32 len = strlen(src);
                memcpy(dst, src, len + 1);
                dst += len + 1;
                src += len + 1;
                count++;
            }
            *(u32*)v[3].base = count;
            return ISFS_OK;
        }
        case 12: {  // GET_USAGE
            *(u32*)v[1].base = 1;
            *(u32*)v[2].base = 1;
            return ISFS_OK;
        }
    }
    __web_report("[ios] /dev/fs ioctlv %d unhandled", cmd);
    return ISFS_INVALID;
}

// ---------------------------------------------------------------------------
// /dev/es

#define SYSMENU_TID 0x0000000100000002ULL

typedef struct {
    ESTitleId tid;
    u8* tmd;  // raw signed TMD
    u32 tmdSize;
} Title;

#define MAX_TITLES 256
static Title sTitles[MAX_TITLES];
static int sNumTitles;
static int sTitlesScanned;

static ESTitleMetaHeader* tmd_head(Title* t) { return (ESTitleMetaHeader*)(t->tmd + 0x180); }
static ESContentMeta* tmd_contents(Title* t) { return (ESContentMeta*)(t->tmd + 0x1E4); }

static void title_dir(ESTitleId tid, char* out) {
    static const char hex[] = "0123456789abcdef";
    u32 hi = (u32)(tid >> 32), lo = (u32)tid;
    strcpy(out, "/title/");
    char* p = out + 7;
    for (int i = 7; i >= 0; i--)
        *p++ = hex[(hi >> (i * 4)) & 15];
    *p++ = '/';
    for (int i = 7; i >= 0; i--)
        *p++ = hex[(lo >> (i * 4)) & 15];
    *p = 0;
}

static u32 parse_hex(const char* s) {
    u32 v = 0;
    for (; *s; s++) {
        char c = *s;
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v |= c - 'A' + 10;
    }
    return v;
}

static void scan_titles(void) {
    if (sTitlesScanned)
        return;
    sTitlesScanned = 1;
    static char highs[4096], lows[4096];
    s32 nh = webjs_fs_readdir("/title", highs, sizeof(highs));
    const char* h = highs;
    for (s32 i = 0; i < nh; i++, h += strlen(h) + 1) {
        char dir[64];
        strcpy(dir, "/title/");
        strcat(dir, h);
        s32 nl = webjs_fs_readdir(dir, lows, sizeof(lows));
        const char* l = lows;
        for (s32 j = 0; j < nl; j++, l += strlen(l) + 1) {
            char path[96];
            strcpy(path, dir);
            strcat(path, "/");
            strcat(path, l);
            strcat(path, "/content/title.tmd");
            u32 t, size;
            if (fs_stat(path, &t, &size) != 0 || t != 1 || size < 0x1E4 || sNumTitles >= MAX_TITLES)
                continue;
            Title* ti = &sTitles[sNumTitles++];
            ti->tid = ((u64)parse_hex(h) << 32) | parse_hex(l);
            ti->tmd = (u8*)malloc(size);
            ti->tmdSize = size;
            webjs_fs_read(path, 0, ti->tmd, size);
        }
    }
}

static Title* find_title(ESTitleId tid) {
    scan_titles();
    for (int i = 0; i < sNumTitles; i++)
        if (sTitles[i].tid == tid)
            return &sTitles[i];
    return NULL;
}

static void forget_title(ESTitleId tid) {
    for (int i = 0; i < sNumTitles; i++)
        if (sTitles[i].tid == tid) {
            free(sTitles[i].tmd);
            sTitles[i] = sTitles[--sNumTitles];
            return;
        }
}

static u32 tmd_view_size(Title* t) { return 0x5C + tmd_head(t)->numContents * sizeof(ESCmdView); }

static void make_tmd_view(Title* t, ESTmdView* view) {
    ESTitleMetaHeader* h = tmd_head(t);
    memset(view, 0, tmd_view_size(t));
    view->head.version = h->version;
    view->head.sysVersion = h->sysVersion;
    view->head.titleId = h->titleId;
    view->head.titleType = h->type;
    view->head.groupId = h->groupId;
    memcpy(&view->head.reserved, &h->reserved, sizeof(h->reserved));
    view->head.titleVersion = h->titleVersion;
    view->head.numContents = h->numContents;
    ESContentMeta* c = tmd_contents(t);
    for (int i = 0; i < h->numContents; i++) {
        view->contents[i].cid = c[i].cid;
        view->contents[i].index = c[i].index;
        view->contents[i].type = c[i].type;
        view->contents[i].size = c[i].size;
    }
}

static void make_ticket_view(Title* t, ESTicketView* v) {
    memset(v, 0, sizeof(*v));
    v->version = 0;
    v->ticketId = 0x0001000000000000ULL | (t->tid & 0xFFFFFFFF);
    v->deviceId = 0;
    v->titleId = t->tid;
    v->accessMask[0] = v->accessMask[1] = 0xFF;
    v->ticketVersion = 0;
    v->license = 0;
    memset(&v->cidxMask, 0xFF, sizeof(v->cidxMask));
}

static ESTitleId sCurrentTitle = SYSMENU_TID;

static s32 content_path(Title* t, u32 index, char* out) {
    ESContentMeta* c = tmd_contents(t);
    for (int i = 0; i < tmd_head(t)->numContents; i++) {
        if (c[i].index != index)
            continue;
        static const char hex[] = "0123456789abcdef";
        title_dir(t->tid, out);
        strcat(out, "/content/");
        char name[16];
        for (int k = 0; k < 8; k++)
            name[k] = hex[(c[i].cid >> ((7 - k) * 4)) & 15];
        strcpy(name + 8, ".app");
        strcat(out, name);
        return 0;
    }
    return -1;
}

static s32 alloc_fd(int kind, const char* path, u32 mode) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (sFds[i].kind == KIND_NONE) {
            sFds[i].kind = kind;
            strncpy(sFds[i].path, path, sizeof(sFds[i].path) - 1);
            sFds[i].path[sizeof(sFds[i].path) - 1] = 0;
            sFds[i].mode = mode;
            sFds[i].pos = 0;
            return i;
        }
    }
    return -1;
}

static s32 open_content(Title* t, u32 index) {
    char path[128];
    if (!t || content_path(t, index, path) < 0)
        return -1017;  // ES_ERR_NOEXISTS-ish
    if (!fs_exists(path)) {
        __web_report("[es] content missing: %s", path);
        return -106;
    }
    s32 fd = alloc_fd(KIND_FILE, path, 1);
    return fd < 0 ? -109 : fd;
}

static s32 es_ioctlv(s32 cmd, u32 nIn, u32 nOut, IOSIoVector* v) {
    switch (cmd) {
        case 69:  // VERIFY_CK_2: succeeds only on consoles with the Korean common key
            return -1017;

        case 7:  // GET_DEVICE_ID
            *(u32*)v[0].base = 0x0403AC68;
            return 0;
        case 9:  // OPEN_CONTENT (current title)
            return open_content(find_title(sCurrentTitle), *(u32*)v[0].base);
        case 36:  // OPEN_TITLE_CONTENT_FILE
            return open_content(find_title(*(ESTitleId*)v[0].base), *(u32*)v[2].base);
        case 10: {  // READ_CONTENT
            s32 fd = *(s32*)v[0].base;
            if (fd < 0 || fd >= MAX_FDS || sFds[fd].kind != KIND_FILE)
                return -1017;
            s32 n = webjs_fs_read(sFds[fd].path, sFds[fd].pos, v[1].base, v[1].length);
            if (n > 0)
                sFds[fd].pos += n;
            return n;
        }
        case 35: {  // SEEK_CONTENT
            s32 fd = *(s32*)v[0].base;
            s32 off = *(s32*)v[1].base;
            u32 whence = *(u32*)v[2].base;
            if (fd < 0 || fd >= MAX_FDS || sFds[fd].kind != KIND_FILE)
                return -1017;
            u32 t, size;
            fs_stat(sFds[fd].path, &t, &size);
            s32 base = whence == 0 ? 0 : whence == 1 ? (s32)sFds[fd].pos : (s32)size;
            sFds[fd].pos = base + off;
            return sFds[fd].pos;
        }
        case 11: {  // CLOSE_CONTENT
            s32 fd = *(s32*)v[0].base;
            if (fd >= 0 && fd < MAX_FDS && sFds[fd].kind == KIND_FILE)
                sFds[fd].kind = KIND_NONE;
            return 0;
        }
        case 12:  // owned titles: count
        case 14: {  // titles on card: count
            scan_titles();
            *(u32*)v[0].base = sNumTitles;
            return 0;
        }
        case 13:
        case 15: {
            scan_titles();
            u32 max = *(u32*)v[0].base;
            ESTitleId* out = (ESTitleId*)v[1].base;
            for (u32 i = 0; i < max && i < (u32)sNumTitles; i++)
                out[i] = sTitles[i].tid;
            return 0;
        }
        case 16: {  // LIST_TITLE_CONTENTS (count)
            Title* t = find_title(*(ESTitleId*)v[0].base);
            if (!t)
                return -106;
            *(u32*)v[1].base = tmd_head(t)->numContents;
            return 0;
        }
        case 17: {
            Title* t = find_title(*(ESTitleId*)v[0].base);
            if (!t)
                return -106;
            u32 max = *(u32*)v[1].base;
            ESContentId* out = (ESContentId*)v[2].base;
            ESContentMeta* c = tmd_contents(t);
            for (u32 i = 0; i < max && i < tmd_head(t)->numContents; i++)
                out[i] = c[i].cid;
            return 0;
        }
        case 18: {  // GET_TICKET_VIEWS (count)
            Title* t = find_title(*(ESTitleId*)v[0].base);
            *(u32*)v[1].base = t ? 1 : 0;
            return t ? 0 : -106;
        }
        case 19: {
            Title* t = find_title(*(ESTitleId*)v[0].base);
            if (!t)
                return -106;
            if (*(u32*)v[1].base >= 1)
                make_ticket_view(t, (ESTicketView*)v[2].base);
            return 0;
        }
        case 20: {  // GET_TMDVIEW (size)
            Title* t = find_title(*(ESTitleId*)v[0].base);
            if (!t)
                return -106;
            *(u32*)v[1].base = tmd_view_size(t);
            return 0;
        }
        case 21: {
            Title* t = find_title(*(ESTitleId*)v[0].base);
            if (!t)
                return -106;
            make_tmd_view(t, (ESTmdView*)v[2].base);
            return 0;
        }
        case 52: {  // GET_TMD (size)
            Title* t = find_title(*(ESTitleId*)v[0].base);
            if (!t)
                return -106;
            *(u32*)v[1].base = t->tmdSize;
            return 0;
        }
        case 53: {
            Title* t = find_title(*(ESTitleId*)v[0].base);
            if (!t)
                return -106;
            u32 n = *(u32*)v[1].base;
            memcpy(v[2].base, t->tmd, n < t->tmdSize ? n : t->tmdSize);
            return 0;
        }
        case 22: {  // GET_CONSUMPTION
            *(u32*)v[2].base = 0;
            return 0;
        }
        case 29: {  // GET_DATA_DIR
            char dir[64];
            title_dir(*(ESTitleId*)v[0].base, dir);
            strcat(dir, "/data");
            strncpy((char*)v[1].base, dir, v[1].length);
            return 0;
        }
        case 32:  // GET_TITLE_ID
            *(ESTitleId*)v[0].base = sCurrentTitle;
            return 0;
        case 33:  // SET_UID
            return 0;
        case 23:    // DELETE_TITLE
        case 34: {  // DELETE_TITLE_CONTENT
            ESTitleId tid = *(ESTitleId*)v[0].base;
            char dir[64];
            title_dir(tid, dir);
            if (cmd == 34)
                strcat(dir, "/content");
            webjs_fs_delete(dir);
            if (cmd == 23)
                forget_title(tid);
            return 0;
        }
        case 24:  // DELETE_TICKET
        case 62:  // DELETE_CONTENT
            return 0;
        case 46:  // GET_BOOT2_VERSION
            *(u32*)v[0].base = 5;
            return 0;
        case 54:  // LIST_SHARED_CONTENTS (count)
            *(u32*)v[0].base = 0;
            return 0;
        case 55:
            return 0;
        case 30:  // GET_DEVICE_CERT
            memset(v[0].base, 0, v[0].length);
            return 0;
        case 8:  // LAUNCH_TITLE
            if (*(u64*)v[0].base == 0x0000000100000002ULL)  // the System Menu: restart it
                webjs_power(WEB_POWER_REBOOT);
            __web_fatal("Launching title %016llx is not supported in the web port.", *(u64*)v[0].base);
            return -1;
    }
    __web_report("[ios] /dev/es ioctlv %d unhandled", cmd);
    return -1017;
}

// ---------------------------------------------------------------------------
// /dev/net/ncd/manage: network configuration. There is no network adapter;
// the configuration itself is kept where IOS keeps it.

#define NCD_CONFIG_PATH "/shared2/sys/net/02/config.dat"

static s32 ncd_ioctlv(s32 cmd, u32 nIn, u32 nOut, IOSIoVector* v) {
    static const u8 kMac[6] = {0x00, 0x17, 0xAB, 0x5E, 0xB0, 0x01};
    u32* result;
    switch (cmd) {
        case 1:  // lock wireless driver: [result, id]
            result = (u32*)v[0].base;
            result[0] = 0;
            result[1] = 1;
            return 0;
        case 2:  // unlock wireless driver
            ((u32*)v[1].base)[0] = 0;
            return 0;
        case 3:  // get configuration
        case 5: {  // read configuration back from NAND
            memset(v[0].base, 0, v[0].length);
            webjs_fs_read(NCD_CONFIG_PATH, 0, v[0].base, v[0].length);
            ((u32*)v[1].base)[0] = 0;
            return 0;
        }
        case 4:  // set configuration
        case 6: {  // write configuration to NAND
            if (!fs_exists(NCD_CONFIG_PATH))
                fs_create(NCD_CONFIG_PATH, 0);
            webjs_fs_write(NCD_CONFIG_PATH, 0, v[0].base, v[0].length, 1);
            ((u32*)v[1].base)[0] = 0;
            return 0;
        }
        case 7:  // link status: [result, status]; no link
            result = (u32*)v[0].base;
            result[0] = 0;
            result[1] = 1;
            return 0;
        case 8:  // wireless MAC address
            ((u32*)v[0].base)[0] = 0;
            memcpy(v[1].base, kMac, 6);
            return 0;
        default:
            __web_report("[ios] ncd ioctlv %d (stubbed)", cmd);
            for (u32 i = nIn; i < nIn + nOut; i++)
                if (v[i].base)
                    memset(v[i].base, 0, v[i].length);
            return 0;
    }
}

// ---------------------------------------------------------------------------
// /dev/net/kd/request and /dev/net/kd/time: the WiiConnect24 daemon. There
// is no network, so mail is never sent or fetched, but the scheduler
// requests succeed and the console gets a WiiConnect24 user ID (memos are
// addressed to it). The NWC24 library itself keeps the mail boxes.

#define WC24_CONFIG_PATH "/shared2/wc24/nwc24msg.cfg"
#define WC24_CONFIG_BACKUP_PATH "/shared2/wc24/nwc24msg.cbk"
#define WC24_CONFIG_SIZE 0x400

enum {
    WC24_OK = 0,
    WC24_ERR_FAILED = -2,
    WC24_ERR_NETWORK = -31,
    WC24_ERR_ID_GENERATED = -35,
    WC24_ERR_ID_REGISTERED = -36,
};

static u8 u64_byte(u64 v, int i) { return (u8)(v >> (i * 8)); }
static u64 u64_set_byte(u64 v, int i, u8 b) { return (v & ~((u64)0xFF << (i * 8))) | ((u64)b << (i * 8)); }

// The user ID: console ID, counter, model and region scrambled around a CRC
// that the NWC24 library verifies.
static u64 wc24_make_user_id(u32 consoleId, u16 counter, u8 model, u8 area) {
    static const u8 kTable1[16] = {0x4, 0xB, 0x7, 0x9, 0xF, 0x1, 0xD, 0x3, 0xC, 0x2, 0x6, 0xE, 0x8, 0x0, 0xA, 0x5};
    static const u8 kTable2[8] = {0x1, 0x5, 0x0, 0x4, 0x2, 0x3, 0x6, 0x7};
    u64 id = ((u64)area << 50) | ((u64)model << 47) | ((u64)consoleId << 15) | ((u64)counter << 10);
    u64 plain = id;
    for (int i = 0; i <= 42; i++)
        if ((id >> (52 - i)) & 1)
            id ^= (u64)0x635 << (42 - i);
    id = (plain | (id & 0xFFFFFFFF)) ^ 0x0000B3B3B3B3B3B3ULL;
    id = (id >> 10) | ((id & 0x3FF) << (11 + 32));
    for (int i = 0; i <= 5; i++) {
        u8 b = u64_byte(id, i);
        id = u64_set_byte(id, i, (u8)((kTable1[b >> 4] << 4) | kTable1[b & 0xF]));
    }
    u64 shuffled = id;
    for (int i = 0; i <= 5; i++)
        id = u64_set_byte(id, kTable2[i], u64_byte(shuffled, i));
    id &= 0x001FFFFFFFFFFFFFULL;
    id = (id << 1) | ((id >> 52) & 1);
    id ^= 0x00005E5E5E5E5E5EULL;
    id &= 0x001FFFFFFFFFFFFFULL;
    return id;
}

static u32 rd32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void wr32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

// Returns the creation stage (0 none, 1 generated, 2 registered) and the ID,
// generating one first if the configuration has none.
static s32 kd_user_id(u64* outId, u32* outStage) {
    static u8 cfg[WC24_CONFIG_SIZE];
    u32 t, size;
    if (fs_stat(WC24_CONFIG_PATH, &t, &size) != 0 || size < WC24_CONFIG_SIZE ||
        webjs_fs_read(WC24_CONFIG_PATH, 0, cfg, WC24_CONFIG_SIZE) != WC24_CONFIG_SIZE || rd32(cfg) != 0x57634366) {
        *outId = 0;
        *outStage = 0;
        return WC24_ERR_FAILED;
    }
    u32 stage = rd32(cfg + 0x14);
    s32 result = stage == 2 ? WC24_ERR_ID_REGISTERED : WC24_ERR_ID_GENERATED;
    if (stage == 0) {
        u32 counter = rd32(cfg + 0x10);
        u64 id = wc24_make_user_id(0x0403AC68, (u16)counter, 1 /* RVL */, 1 /* USA */);
        wr32(cfg + 0x08, (u32)(id >> 32));
        wr32(cfg + 0x0C, (u32)id);
        wr32(cfg + 0x10, counter + 1);
        wr32(cfg + 0x14, stage = 1);
        u32 sum = 0;
        for (u32 i = 0; i < WC24_CONFIG_SIZE - 4; i += 4)
            sum += rd32(cfg + i);
        wr32(cfg + WC24_CONFIG_SIZE - 4, sum);
        webjs_fs_write(WC24_CONFIG_PATH, 0, cfg, WC24_CONFIG_SIZE, 1);
        if (!fs_exists(WC24_CONFIG_BACKUP_PATH))
            fs_create(WC24_CONFIG_BACKUP_PATH, 0);
        webjs_fs_write(WC24_CONFIG_BACKUP_PATH, 0, cfg, WC24_CONFIG_SIZE, 1);
        result = WC24_OK;
    }
    *outId = ((u64)rd32(cfg + 0x08) << 32) | rd32(cfg + 0x0C);
    *outStage = stage;
    return result;
}

static s32 kd_ioctl(IosFd* f, s32 cmd, u8* in, u32 inLen, u8* out, u32 outLen) {
    if (out && outLen)
        memset(out, 0, outLen);
    s32 result = WC24_OK;
    if (strcmp(f->path, "/dev/net/kd/time") == 0) {
        // 23 SET_RTC_COUNTER, 24 GET_TIME_DIFFERENCE (zero: the clock is right)
    } else {
        switch (cmd) {
            case 1:   // SUSPEND_SCHEDULER
            case 2:   // TRY_SUSPEND_SCHEDULER
            case 3:   // RESUME_SCHEDULER
            case 8:   // LOCK_SOCKET
            case 9:   // UNLOCK_SOCKET
            case 13:  // SAVE_MAIL_NOW
            case 30:  // GET_SCHEDULER_STAT
            case 34:  // SET_SCRIPT_MODE
            case 40:  // SHUTDOWN
                break;
            case 6:   // STARTUP_SOCKET
            case 7:   // CLEANUP_SOCKET
            case 10:  // CHECK_MAIL_NOW
            case 11:  // SEND_MAIL_NOW
            case 12:  // RECEIVE_MAIL_NOW
            case 14:  // DOWNLOAD_NOW_EX
                result = WC24_ERR_NETWORK;
                break;
            case 15: {  // REQUEST_GENERATED_USER_ID: [result, id (u64), stage]
                u64 id;
                u32 stage;
                result = kd_user_id(&id, &stage);
                if (out && outLen >= 16) {
                    wr32(out + 4, (u32)(id >> 32));
                    wr32(out + 8, (u32)id);
                    wr32(out + 12, stage);
                }
                break;
            }
            case 16:  // REQUEST_REGISTER_USER_ID: needs the server
                result = WC24_ERR_NETWORK;
                break;
            default:
                __web_report("[ios] kd ioctl %d (stubbed)", cmd);
                break;
        }
    }
    if (out && outLen >= 4)
        wr32(out, (u32)result);
    return 0;
}

// ---------------------------------------------------------------------------
// Request dispatch

static int device_kind(const char* path) {
    if (strcmp(path, "/dev/fs") == 0)
        return KIND_FS;
    if (strcmp(path, "/dev/es") == 0)
        return KIND_ES;
    if (strcmp(path, "/dev/di") == 0)
        return KIND_DI;
    if (strcmp(path, "/dev/stm/immediate") == 0)
        return KIND_STM_IMMEDIATE;
    if (strcmp(path, "/dev/stm/eventhook") == 0)
        return KIND_STM_EVENT;
    if (strcmp(path, "/dev/net/ncd/manage") == 0)
        return KIND_NCD;
    if (strcmp(path, "/dev/net/kd/request") == 0 || strcmp(path, "/dev/net/kd/time") == 0)
        return KIND_KD;
    return KIND_STUB;
}

static s32 do_open(const char* path, u32 mode) {
    if (strncmp(path, "/dev/", 5) == 0) {
        int kind = device_kind(path);
        if (kind == KIND_STUB)
            __web_report("[ios] open %s (stubbed)", path);
        s32 fd = alloc_fd(kind, path, mode);
        return fd < 0 ? IPC_RESULT_MAX : fd;
    }
    u32 t, size;
    if (fs_stat(path, &t, &size) != 0) {
        __web_report("[ios] open %s: not found", path);
        return ISFS_NOEXISTS;
    }
    if (t != 1)
        return ISFS_ACCESS;
    s32 fd = alloc_fd(KIND_FILE, path, mode);
    return fd < 0 ? ISFS_MAXFD : fd;
}

static IosFd* get_fd(s32 fd) {
    if (fd < 0 || fd >= MAX_FDS || sFds[fd].kind == KIND_NONE)
        return NULL;
    return &sFds[fd];
}

static s32 do_close(s32 fd) {
    IosFd* f = get_fd(fd);
    if (!f)
        return IPC_RESULT_INVALID;
    f->kind = KIND_NONE;
    return 0;
}

static s32 do_read(s32 fd, void* buf, u32 len) {
    IosFd* f = get_fd(fd);
    if (!f)
        return IPC_RESULT_INVALID;
    if (f->kind != KIND_FILE)
        return IPC_RESULT_INVALID;
    s32 n = webjs_fs_read(f->path, f->pos, buf, len);
    if (n > 0)
        f->pos += n;
    return n;
}

static s32 do_write(s32 fd, const void* buf, u32 len) {
    IosFd* f = get_fd(fd);
    if (!f || f->kind != KIND_FILE)
        return IPC_RESULT_INVALID;
    if (!(f->mode & 2))
        return ISFS_ACCESS;
    s32 n = webjs_fs_write(f->path, f->pos, buf, len, 0);
    if (n > 0)
        f->pos += n;
    return n;
}

static s32 do_seek(s32 fd, s32 offset, u32 whence) {
    IosFd* f = get_fd(fd);
    if (!f || f->kind != KIND_FILE)
        return IPC_RESULT_INVALID;
    u32 t, size;
    fs_stat(f->path, &t, &size);
    s32 base = whence == 0 ? 0 : whence == 1 ? (s32)f->pos : (s32)size;
    s32 pos = base + offset;
    if (pos < 0 || (u32)pos > size)
        return ISFS_INVALID;
    f->pos = pos;
    return pos;
}

static s32 stm_ioctl(IosFd* f, s32 cmd, u8* in, u32 inLen, u8* out, u32 outLen) {
    if (out && outLen)
        memset(out, 0, outLen);
    switch (cmd) {
        case 0x2001:  // HOT_RESET
            webjs_power(WEB_POWER_REBOOT);
            break;
        case 0x2003:  // SHUTDOWN_TO_SBY
        case 0x2004:  // SHUTDOWN_TO_IDL
            webjs_power(WEB_POWER_OFF);
            break;
    }
    return 0;
}

s32 __web_di_ioctl(s32 cmd, u8* in, u32 inLen, u8* out, u32 outLen);
s32 __web_di_ioctlv(s32 cmd, u32 nIn, u32 nOut, IOSIoVector* v);

static s32 do_ioctl(s32 fd, s32 cmd, void* in, u32 inLen, void* out, u32 outLen) {
    IosFd* f = get_fd(fd);
    if (!f)
        return IPC_RESULT_INVALID;
    switch (f->kind) {
        case KIND_FS:
            return fs_ioctl(cmd, (u8*)in, inLen, (u8*)out, outLen);
        case KIND_FILE:
            if (cmd == 11) {  // GET_FILE_STATS
                u32 t, size;
                fs_stat(f->path, &t, &size);
                ((u32*)out)[0] = size;
                ((u32*)out)[1] = f->pos;
                return 0;
            }
            return ISFS_INVALID;
        case KIND_DI:
            return __web_di_ioctl(cmd, (u8*)in, inLen, (u8*)out, outLen);
        case KIND_STM_IMMEDIATE:
            return stm_ioctl(f, cmd, (u8*)in, inLen, (u8*)out, outLen);
        case KIND_KD:
            return kd_ioctl(f, cmd, (u8*)in, inLen, (u8*)out, outLen);
        default:
            __web_report("[ios] ioctl %d on %s (stubbed)", cmd, f->path);
            if (out && outLen)
                memset(out, 0, outLen);
            return IPC_RESULT_INVALID;
    }
}

static s32 do_ioctlv(s32 fd, s32 cmd, u32 nIn, u32 nOut, IOSIoVector* v) {
    IosFd* f = get_fd(fd);
    if (!f)
        return IPC_RESULT_INVALID;
    switch (f->kind) {
        case KIND_FS:
            return fs_ioctlv(cmd, nIn, nOut, v);
        case KIND_ES:
            return es_ioctlv(cmd, nIn, nOut, v);
        case KIND_DI:
            return __web_di_ioctlv(cmd, nIn, nOut, v);
        case KIND_NCD:
            return ncd_ioctlv(cmd, nIn, nOut, v);
        default:
            __web_report("[ios] ioctlv %d on %s (stubbed)", cmd, f->path);
            for (u32 i = nIn; i < nIn + nOut; i++)
                if (v[i].base)
                    memset(v[i].base, 0, v[i].length);
            return IPC_RESULT_INVALID;
    }
}

// ---------------------------------------------------------------------------
// Async completion

typedef struct {
    IOSIpcCb cb;
    void* arg;
    s32 result;
    int used;
} Reply;

#define MAX_REPLIES 128
static Reply sReplies[MAX_REPLIES];

static void reply_irq(void* p) {
    Reply* r = (Reply*)p;
    IOSIpcCb cb = r->cb;
    void* arg = r->arg;
    s32 result = r->result;
    r->used = 0;
    if (cb)
        cb(result, arg);
}

static IOSError complete_async(s32 result, IOSIpcCb cb, void* arg) {
    for (int i = 0; i < MAX_REPLIES; i++) {
        if (!sReplies[i].used) {
            sReplies[i].used = 1;
            sReplies[i].cb = cb;
            sReplies[i].arg = arg;
            sReplies[i].result = result;
            __web_queue_irq(reply_irq, &sReplies[i]);
            return 0;
        }
    }
    return IPC_RESULT_MAX;
}

// The STM event hook completes when a console button is pressed.
static int is_event_hook(IOSFd fd) {
    IosFd* f = get_fd(fd);
    return f && f->kind == KIND_STM_EVENT;
}

static struct {
    IOSIpcCb cb;
    void* arg;
    u32* out;
} sEventHook;

void __web_console_buttons(u32 buttons) {
    if (!sEventHook.cb)
        return;
    sEventHook.out[0] = (buttons & 1) ? 0x800 : 0x20000;
    IOSIpcCb cb = sEventHook.cb;
    sEventHook.cb = NULL;
    complete_async(0, cb, sEventHook.arg);
}

IOSError IOS_Open(const char* path, u32 flags) { return do_open(path, flags); }
IOSError IOS_OpenAsync(const char* path, u32 flags, IOSIpcCb cb, void* arg) {
    return complete_async(do_open(path, flags), cb, arg);
}
IOSError IOS_Close(IOSFd fd) { return do_close(fd); }
IOSError IOS_CloseAsync(IOSFd fd, IOSIpcCb cb, void* arg) { return complete_async(do_close(fd), cb, arg); }
IOSError IOS_Read(IOSFd fd, void* buf, u32 len) { return do_read(fd, buf, len); }
IOSError IOS_ReadAsync(IOSFd fd, void* buf, u32 len, IOSIpcCb cb, void* arg) {
    return complete_async(do_read(fd, buf, len), cb, arg);
}
IOSError IOS_Write(IOSFd fd, void* buf, u32 len) { return do_write(fd, buf, len); }
IOSError IOS_WriteAsync(IOSFd fd, void* buf, u32 len, IOSIpcCb cb, void* arg) {
    return complete_async(do_write(fd, buf, len), cb, arg);
}
IOSError IOS_Seek(IOSFd fd, s32 offset, u32 whence) { return do_seek(fd, offset, whence); }
IOSError IOS_SeekAsync(IOSFd fd, s32 offset, u32 whence, IOSIpcCb cb, void* arg) {
    return complete_async(do_seek(fd, offset, whence), cb, arg);
}
IOSError IOS_Ioctl(IOSFd fd, s32 cmd, void* in, u32 inLen, void* out, u32 outLen) {
    return do_ioctl(fd, cmd, in, inLen, out, outLen);
}
IOSError IOS_IoctlAsync(IOSFd fd, s32 cmd, void* in, u32 inLen, void* out, u32 outLen, IOSIpcCb cb, void* arg) {
    if (is_event_hook(fd)) {
        if (cmd == 0x1000) {  // REG_STM_EVENT
            sEventHook.cb = cb;
            sEventHook.arg = arg;
            sEventHook.out = (u32*)out;
        }
        return 0;
    }
    return complete_async(do_ioctl(fd, cmd, in, inLen, out, outLen), cb, arg);
}
IOSError IOS_Ioctlv(IOSFd fd, s32 cmd, u32 nIn, u32 nOut, IOSIoVector* v) { return do_ioctlv(fd, cmd, nIn, nOut, v); }
IOSError IOS_IoctlvAsync(IOSFd fd, s32 cmd, u32 nIn, u32 nOut, IOSIoVector* v, IOSIpcCb cb, void* arg) {
    return complete_async(do_ioctlv(fd, cmd, nIn, nOut, v), cb, arg);
}
IOSError IOS_IoctlvReboot(IOSFd fd, s32 cmd, u32 nIn, u32 nOut, IOSIoVector* v) {
    return do_ioctlv(fd, cmd, nIn, nOut, v);
}

// ---------------------------------------------------------------------------
// IPC buffer and IOS heaps

static void* sIpcLo;
static void* sIpcHi;

void __OSInitIPCBuffer(void) {
    sIpcLo = (void*)*(u32*)0x3130;
    sIpcHi = (void*)*(u32*)0x3134;
}

void* IPCGetBufferHi(void) { return sIpcHi; }
void* IPCGetBufferLo(void) { return sIpcLo; }
void IPCSetBufferLo(void* lo) { sIpcLo = lo; }
IOSError IPCCltInit(void) { return 0; }
IOSError IPCCltReInit(void) { return 0; }
void IPCReInit(void) {}
void IPCInit(void) {}

// Simple first-fit heaps over caller-supplied memory.
typedef struct HeapBlock {
    u32 size;  // including header; low bit = used
    u32 pad[7];
} HeapBlock;

typedef struct {
    u8* base;
    u32 size;
    int used;
} IosHeap;

#define MAX_HEAPS 16
static IosHeap sHeaps[MAX_HEAPS];

IOSHeapId iosCreateHeap(void* ptr, u32 size) {
    for (int i = 0; i < MAX_HEAPS; i++) {
        if (!sHeaps[i].used) {
            sHeaps[i].used = 1;
            sHeaps[i].base = (u8*)ptr;
            sHeaps[i].size = size & ~31;
            HeapBlock* b = (HeapBlock*)ptr;
            b->size = sHeaps[i].size;
            return i;
        }
    }
    return IPC_RESULT_MAX;
}

IOSError iosDestroyHeap(IOSHeapId id) {
    if (id < 0 || id >= MAX_HEAPS)
        return IPC_RESULT_INVALID;
    sHeaps[id].used = 0;
    return 0;
}

void* iosAllocAligned(IOSHeapId id, u32 size, u32 alignment) {
    if (id < 0 || id >= MAX_HEAPS || !sHeaps[id].used)
        return NULL;
    IosHeap* h = &sHeaps[id];
    u32 need = ((size + 31) & ~31) + sizeof(HeapBlock);
    u8* p = h->base;
    u8* end = h->base + h->size;
    while (p < end) {
        HeapBlock* b = (HeapBlock*)p;
        u32 bsize = b->size & ~1u;
        if (bsize == 0)
            break;
        if (!(b->size & 1)) {
            // merge following free blocks
            while (p + bsize < end && !(((HeapBlock*)(p + bsize))->size & 1) && ((HeapBlock*)(p + bsize))->size)
                bsize += ((HeapBlock*)(p + bsize))->size;
            b->size = bsize;
            if (bsize >= need) {
                if (bsize - need >= sizeof(HeapBlock) + 32) {
                    HeapBlock* rest = (HeapBlock*)(p + need);
                    rest->size = bsize - need;
                    b->size = need | 1;
                } else {
                    b->size = bsize | 1;
                }
                return p + sizeof(HeapBlock);
            }
        }
        p += bsize;
    }
    return NULL;
}

IOSError iosFree(IOSHeapId id, void* ptr) {
    if (!ptr)
        return IPC_RESULT_INVALID;
    HeapBlock* b = (HeapBlock*)((u8*)ptr - sizeof(HeapBlock));
    b->size &= ~1u;
    return 0;
}
