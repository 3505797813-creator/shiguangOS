// 拾光安装程序 v0.9.2 —— 独立 UEFI 程序
// 作用：把 ISO 里的 SHIGUANG.IMG 原样写到选中的硬盘，并登记 UEFI 启动项
// 和系统本体同一个视觉，无 libc、无 EDK2

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef long long          i64;

typedef struct { u32 a; u16 b; u16 c; u8 d[8]; } GUID;

static const GUID GOP_GUID  = {0x9042A9DE,0x23DC,0x4A38,{0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};
static const GUID BLOCK_GUID= {0x964E5B21,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const GUID FS_GUID   = {0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const GUID LOADED_GUID={0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const GUID DP_GUID   = {0x09576E91,0x6D3F,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const GUID GLOBALVAR_GUID={0x8BE4DF61,0x93CA,0x11D2,{0xAA,0x0D,0x00,0xE0,0x98,0x03,0x2B,0x8C}};

/* EFI_BOOT_SERVICES 表内偏移（x64） */
#define BS_ALLOC_PAGES     0x28
#define BS_FREE_POOL       0x48
#define BS_WAIT_EVENT      0x60
#define BS_HANDLE_PROTOCOL 0x98
#define BS_CONNECT_CTRL    0x108
#define BS_LOCATE_HANDLE_BUF 0x138
#define BS_LOCATE_PROTOCOL 0x140
#define BS_STALL           0xF8
#define ST_CONOUT          0x40
#define ST_CONIN           0x30
#define ST_BOOT_SERVICES   0x60
#define ST_RUNTIME         0x58
#define RT_GET_VARIABLE    0x48
#define RT_SET_VARIABLE    0x58
#define RT_RESET_SYSTEM    0x70

static void** bs;
static void** rt;
static void** conin;
static void** conout;

static void ser_outb(u16 p, u8 v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static u8  ser_inb(u16 p) { u8 v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void ser_puts(const char* s) { int i; for (i = 0; s[i]; i++) { while (!(ser_inb(0x3FD) & 0x20)) {} ser_outb(0x3F8, (u8)s[i]); } }
static void ser_hex(u64 v) {
    char b[17]; int i; for (i = 15; i >= 0; i--) { int d = (int)((v >> (i * 4)) & 15); b[15 - i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); } b[16] = 0; ser_puts(b);
}
static void stall(u64 us) { ((u64(*)(u64))bs[BS_STALL/8])(us); }

#include "font_data.h"
#include "str_data.h"

/* ---------------------------------------------------------------- 图形 */
static u32* fb;
static int  W, H, PITCH;

static void put(int x, int y, u32 c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    fb[y * PITCH + x] = c;
}
static void rect(int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) {
        u32* p = fb + (y + r) * PITCH + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}
static void round_rect(int x, int y, int w, int h, int r, u32 c) {
    for (int row = 0; row < h; row++) {
        int d = row < h - 1 - row ? row : h - 1 - row;
        if (d >= r) { rect(x, y + row, w, 1, c); continue; }
        int dy = r - d, rr = r * r, dx = 0;
        for (int k = r; k >= 0; k--) if ((dy*dy + k*k) <= rr) { dx = k; break; }
        rect(x + r - dx, y + row, w - 2*(r - dx), 1, c);
    }
}
static void grad(int x, int y, int w, int h, u32 c1, u32 c2) {
    int r1 = (c1>>16)&255, g1 = (c1>>8)&255, b1 = c1&255;
    int r2 = (c2>>16)&255, g2 = (c2>>8)&255, b2 = c2&255;
    for (int row = 0; row < h; row++) {
        int t = h > 1 ? row * 256 / (h - 1) : 0;
        u32 c = ((u32)(r1 + (r2-r1)*t/256) << 16) | ((u32)(g1 + (g2-g1)*t/256) << 8) | (u32)(b1 + (b2-b1)*t/256);
        rect(x, y + row, w, 1, c);
    }
}
static void draw_str(const unsigned char* s, int x, int y, u32 c, int scale) {
    for (; *s; s++) {
        int wch = 8; const u8* g8; const u16* g16;
        if (*s >= 0x80) { wch = 16; g16 = cjk16[*s - 0x80]; g8 = 0; }
        else            { g16 = 0; g8 = font8[*s - 32]; }
        for (int row = 0; row < 16; row++) {
            u32 bits = g16 ? g16[row] : g8[row];
            for (int col = 0; col < wch; col++) {
                int on = g16 ? (bits >> (15 - col)) & 1 : (bits >> (7 - col)) & 1;
                if (on) rect(x + col * scale, y + row * scale, scale, scale, c);
            }
        }
        x += wch * scale;
    }
}
static void draw_str16(const unsigned short* s, int x, int y, u32 c, int scale) {
    for (; *s; s++) {
        int wch = 8; const u8* g8; const u16* g16;
        if (*s >= 0x80) { wch = 16; g16 = cjk16[*s - 0x80]; g8 = 0; }
        else            { g16 = 0; g8 = font8[*s - 32]; }
        for (int row = 0; row < 16; row++) {
            u32 bits = g16 ? g16[row] : g8[row];
            for (int col = 0; col < wch; col++) {
                int on = g16 ? (bits >> (15 - col)) & 1 : (bits >> (7 - col)) & 1;
                if (on) rect(x + col * scale, y + row * scale, scale, scale, c);
            }
        }
        x += wch * scale;
    }
}
static int str_width16(const unsigned short* s, int scale) {
    int w = 0;
    for (; *s; s++) w += ((*s >= 0x80) ? 16 : 8) * scale;
    return w;
}
static void str16_center(const unsigned short* s, int y, u32 c, int scale) {
    draw_str16(s, (W - str_width16(s, scale)) / 2, y, c, scale);
}

/* ---------------------------------------------------------------- 键盘 */
typedef struct { u16 scan; u16 ch; } KEY;

static int key_read(KEY* k) {
    u64* ci = (u64*)conin;
    if (((u64(*)(void*, void*))ci[1])(conin, k) == 0) return 1;
    return 0;
}
static void key_wait(void) {
    /* 不调用 Reset：Reset 会把刚按下的键直接丢掉 */
    u64* ci = (u64*)conin;
    void* ev = (void*)ci[2];
    ((u64(*)(u64, void**, void*))bs[BS_WAIT_EVENT/8])(1, &ev, 0);
}

/* ---------------------------------------------------------------- 块设备 */
typedef struct {
    u32 MediaId;
    u8  RemovableMedia;
    u8  MediaPresent;
    u8  LogicalPartition;
    u8  ReadOnly;
    u8  WriteCaching;
    u32 BlockSize;
    u32 IoAlign;
    u64 LastBlock;
} BLK_MEDIA;

typedef struct {
    void* handle;
    void* bio;
    BLK_MEDIA media;
    u64 bytes;
    char label[40];
    int  label_len;
} DISK;

#define MAXDISK 16
static DISK disks[MAXDISK];
static int  ndisk = 0;

static int scan_disks(void) {
    void* buf = 0; u64 cnt = 0;
    u64 r = ((u64(*)(u32, const GUID*, void*, u64*, void**))bs[BS_LOCATE_HANDLE_BUF/8])
            (2 /*ByProtocol*/, &BLOCK_GUID, 0, &cnt, &buf);
    if (r != 0 || !buf) return 0;
    void** hs = (void**)buf;
    for (u64 i = 0; i < cnt && ndisk < MAXDISK; i++) {
        void* bio = 0;
        if (((u64(*)(void*, const GUID*, void**))bs[BS_HANDLE_PROTOCOL/8])(hs[i], &BLOCK_GUID, &bio) != 0 || !bio) continue;
        void** bt = (void**)bio;
        BLK_MEDIA* m = (BLK_MEDIA*)bt[1];
        if (!m || !m->MediaPresent) continue;
        if (m->LogicalPartition) continue;      /* 只要整盘，不要分区 */
        if (m->ReadOnly) continue;              /* 只读（光盘/锁住的盘）跳过 */
        if (!m->BlockSize) continue;
        u64 bytes = (m->LastBlock) * (u64)m->BlockSize;
        if (bytes < 64ULL * 1024 * 1024) continue;   /* 小于 64MB 装不下 */
        disks[ndisk].handle = hs[i];
        disks[ndisk].bio = bio;
        disks[ndisk].media = *m;
        disks[ndisk].bytes = bytes;
        disks[ndisk].label_len = 0;
        ndisk++;
    }
    return ndisk;
}

static u64 disk_write(void* bio, u64 lba, u64 bytes, void* buf) {
    void** bt = (void**)bio;
    BLK_MEDIA* m = (BLK_MEDIA*)bt[1];
    u64 (*wb)(void*, u32, u64, u64, void*) = (u64(*)(void*,u32,u64,u64,void*))bt[4];
    return wb(bio, m->MediaId, lba, bytes, buf);
}
static u64 disk_flush(void* bio) {
    void** bt = (void**)bio;
    u64 (*fbk)(void*) = (u64(*)(void*))bt[5];
    return fbk ? fbk(bio) : 0;
}

/* ---------------------------------------------------------------- 读镜像 */
static void* img_volume(void* image) {
    void* li = 0;
    if (((u64(*)(void*, const GUID*, void**))bs[BS_HANDLE_PROTOCOL/8])(image, &LOADED_GUID, &li) != 0 || !li) return 0;
    void* dev = *(void**)((u8*)li + 24);      /* LoadedImage->DeviceHandle */
    void* sfs = 0;
    if (((u64(*)(void*, const GUID*, void**))bs[BS_HANDLE_PROTOCOL/8])(dev, &FS_GUID, &sfs) != 0 || !sfs) return 0;
    void* root = 0;
    void** sv = (void**)sfs;
    if (((u64(*)(void*, void**))sv[1])(sfs, &root) != 0 || !root) return 0;
    return root;
}

static u16 img_name[20];
static void make_img_name(void) {
    const char* n = "\\SHIGUANG.IMG";
    int i;
    for (i = 0; n[i]; i++) img_name[i] = (u16)n[i];
    img_name[i] = 0;
}

static void* img_open(void* root) {
    void* f = 0;
    void** rv = (void**)root;
    u64 st = ((u64(*)(void*, void**, u16*, u64, u64))rv[1])(root, &f, img_name, 1 /*READ*/, 0);  /* EFI_FILE_PROTOCOL: Revision 占 0，Open 在 1 */
    if (st != 0) return 0;
    return f;
}
static u64 img_read(void* f, u64* size, void* buf) {
    void** fv = (void**)f;
    return ((u64(*)(void*, u64*, void*))fv[4])(f, size, buf);   /* Read 在 4 */
}

/* ---------------------------------------------------------------- UEFI 启动项 */
static int make_boot_entry(void* image, void* targeth) {
    void* dp = 0;
    if (((u64(*)(void*, const GUID*, void**))bs[BS_HANDLE_PROTOCOL/8])(targeth, &DP_GUID, &dp) != 0 || !dp) return -1;
    int dsz = 0;
    u8* p = (u8*)dp;
    while (1) {
        int ln = *(u16*)(p + 2);
        if (ln <= 0) break;
        dsz += ln;
        if (p[0] == 0x7F) break;     /* end of device path */
        p += ln;
    }
    if (dsz <= 0) return -1;
    u32 attr = 1;
    u16 fpl = (u16)dsz;
    static u16 desc[12];
    const char* ds = "Shiguang OS";
    int i; for (i = 0; ds[i]; i++) desc[i] = (u16)ds[i];
    for (; i < 12; i++) desc[i] = 0;

    int total = 4 + 2 + 24 + dsz;
    void* lo = 0;
    u64 rr = ((u64(*)(u64, u64, u64, void**))bs[BS_ALLOC_PAGES/8])(0, 2, 1, (void**)&lo);
    if (rr != 0 || !lo) return -1;
    u8* b = (u8*)lo;
    *(u32*)b = attr;
    *(u16*)(b + 4) = fpl;
    for (i = 0; i < 12; i++) ((u16*)(b + 6))[i] = desc[i];
    for (i = 0; i < dsz; i++) b[6 + 24 + i] = ((u8*)dp)[i];

    /* 找一个没被占用的 Boot#### */
    int idx = -1;
    for (int k = 0x1000; k < 0x1100 && idx < 0; k++) {
        u16 nm[9]; nm[0]='B';nm[1]='o';nm[2]='o';nm[3]='t';
        for (int j = 0; j < 4; j++) nm[4+j] = (u16)("0123456789ABCDEF"[(k >> ((3-j)*4)) & 15]);
        nm[8] = 0;
        u32 a = 0; u64 sz = 0;
        u64 g = ((u64(*)(u16*, const GUID*, u32*, u64*, void*))rt[RT_GET_VARIABLE/8])(nm, &GLOBALVAR_GUID, &a, &sz, 0);
        if (g != 0) idx = k;    /* 不存在 → 可占用 */
    }
    if (idx < 0) return -1;

    u16 name[9]; name[0]='B';name[1]='o';name[2]='o';name[3]='t';
    for (int j = 0; j < 4; j++) name[4+j] = (u16)("0123456789ABCDEF"[(idx >> ((3-j)*4)) & 15]);
    name[8] = 0;

    u64 sr = ((u64(*)(u16*, const GUID*, u32, u64, void*))rt[RT_SET_VARIABLE/8])
             (name, &GLOBALVAR_GUID, 0x7 /*NV|BS|RT*/, (u64)total, b);
    if (sr != 0) return -1;

    /* 追加到 BootOrder */
    u16 bo[10]; bo[0]='B';bo[1]='o';bo[2]='o';bo[3]='t';bo[4]='O';bo[5]='r';bo[6]='d';bo[7]='e';bo[8]='r';bo[9]=0;
    u32 a2 = 0; u64 sz2 = 256;
    static u16 boval[128];
    u64 g2 = ((u64(*)(u16*, const GUID*, u32*, u64*, void*))rt[RT_GET_VARIABLE/8])(bo, &GLOBALVAR_GUID, &a2, &sz2, boval);
    int n = (g2 == 0) ? (int)(sz2 / 2) : 0;
    if (n > 120) n = 120;
    for (int j = 0; j < n; j++) if (boval[j] == (u16)idx) return 0;   /* 已经在里面 */
    boval[n] = (u16)idx;
    u64 s3 = ((u64(*)(u16*, const GUID*, u32, u64, void*))rt[RT_SET_VARIABLE/8])(bo, &GLOBALVAR_GUID, 0x7, (u64)((n + 1) * 2), boval);
    return (int)s3;
}

/* ---------------------------------------------------------------- 界面 */
#define C_BG     0x00101A2E
#define C_CARD   0x001A2740
#define C_CARD2  0x00223455
#define C_TXT    0x00FFFFFF
#define C_SUB    0x0099A8C0
#define C_ACC    0x003B82F6
#define C_ACC2   0x00256BD6
#define C_RED    0x00E0533D
#define C_OK     0x0022C55E

static int page = 0;          /* 0 选盘 1 确认 2 写入 3 完成 4 无盘 */
static int sel = 0;
static int prog = 0;          /* 0-1000 */
static const unsigned short* errmsg = 0;
static int last_key = 0;

static void draw_button(int x, int y, int w, int h, const unsigned short* t, u32 fill, int focused) {
    round_rect(x, y, w, h, 8, fill);
    if (focused) round_rect(x - 2, y - 2, w + 4, h + 4, 9, C_ACC);
    int tw = str_width16(t, 1);
    draw_str16(t, x + (w - tw) / 2, y + (h - 16) / 2, C_TXT, 1);
}

static void fmt_mb(char* out, u64 bytes) {
    /* 变成 "12345 MB" 或 "X.Y GB" */
    u64 mb = bytes / (1024 * 1024);
    int i = 0;
    char tmp[24]; int p = 0;
    if (mb >= 1024) {
        u64 gb = mb / 1024, frac = (mb % 1024) * 10 / 1024;
        tmp[p++] = (char)('0' + (gb > 9 ? 9 : gb)); /* 只做 1 位，够用 */
        if (gb >= 10) { /* 两位以上重新写 */
            p = 0;
            char s2[24]; int q = 0; u64 g2 = gb;
            while (g2) { s2[q++] = (char)('0' + (g2 % 10)); g2 /= 10; }
            while (q > 0) tmp[p++] = s2[--q];
        }
        tmp[p++] = '.'; tmp[p++] = (char)('0' + frac);
        tmp[p++] = ' '; tmp[p++] = 'G'; tmp[p++] = 'B';
    } else {
        char s2[24]; int q = 0; u64 m2 = mb;
        if (!m2) { s2[q++] = '0'; }
        while (m2) { s2[q++] = (char)('0' + (m2 % 10)); m2 /= 10; }
        while (q > 0) tmp[p++] = s2[--q];
        tmp[p++] = ' '; tmp[p++] = 'M'; tmp[p++] = 'B';
    }
    tmp[p] = 0;
    for (i = 0; i < p; i++) out[i] = tmp[i];
    out[p] = 0;
}

static void scene(void) {
    grad(0, 0, W, H, 0x00162440, C_BG);
    rect(0, 0, W, 3, C_ACC);
    draw_str16(s_ver, W - str_width16(s_ver, 1) - 24, H - 26, 0x00677A94, 1);
    draw_str16(i_title, 48, 40, C_TXT, 2);
    { int tw = str_width16(s_ver, 1); (void)tw; }
    draw_str16(i_sub, 48, 92, C_SUB, 1);

    if (page == 0 || page == 1) {
        draw_str16(i_sel, 48, 140, C_SUB, 1);
    }

    if (page == 0) {
        int y = 184;
        for (int i = 0; i < ndisk; i++) {
            int h = 64;
            u32 fill = (i == sel) ? C_CARD2 : C_CARD;
            round_rect(64, y, W - 128, h, 8, fill);
            if (i == sel) rect(64, y, 4, h, C_ACC);
            /* 磁盘 N */
            {
                char nm[16]; int p = 0;
                nm[p++] = 'D'; nm[p++] = 'I'; nm[p++] = 'S'; nm[p++] = 'K';
                nm[p++] = ' ';
                nm[p++] = (char)('0' + (i % 10));
                nm[p] = 0;
                draw_str((const unsigned char*)nm, 88, y + 14, C_TXT, 2);
            }
            {
                char sz[24]; fmt_mb(sz, disks[i].bytes);
                int l = 0; while (sz[l]) l++;
                draw_str((const unsigned char*)sz, W - 88 - l * 8, y + 22, C_SUB, 1);
            }
            draw_str16(i_cap, 88, y + 46, 0x008FA3BE, 1);
            y += h + 12;
        }
        if (!ndisk) {
            str16_center(i_none, 300, C_SUB, 1);
        }
        draw_button(W - 460, H - 88, 200, 56, i_cancel, C_CARD2, 0);
        draw_button(W - 240, H - 88, 200, 56, i_go, C_ACC, ndisk > 0);
        draw_str((const unsigned char*)"UP DOWN = SELECT   ENTER = NEXT", 64, H - 70, 0x00677A94, 1);
    } else if (page == 1) {
        int y = 190;
        round_rect(64, y, W - 128, 230, 10, C_CARD);
        rect(64, y, 4, 230, C_RED);
        draw_str16(i_warn, 96, y + 26, C_TXT, 2);
        draw_str16(i_warn2, 96, y + 78, C_SUB, 1);
        {
            char nm[16]; int p = 0;
            nm[p++] = 'D';nm[p++] = 'I';nm[p++] = 'S';nm[p++] = 'K';nm[p++] = ' ';
            nm[p++] = (char)('0' + (sel % 10)); nm[p] = 0;
            draw_str((const unsigned char*)nm, 96, y + 120, C_TXT, 1);
            char sz[24]; fmt_mb(sz, disks[sel].bytes);
            draw_str((const unsigned char*)sz, 200, y + 120, C_SUB, 1);
        }
        draw_str((const unsigned char*)"THIS WILL ERASE THE WHOLE DISK", 96, y + 170, C_RED, 1);
        draw_button(W - 460, H - 88, 200, 56, i_cancel, C_CARD2, 0);
        draw_button(W - 240, H - 88, 200, 56, i_go, C_RED, 1);
    } else if (page == 2) {
        draw_str16(i_writing, 64, 240, C_TXT, 2);
        round_rect(64, 300, W - 128, 26, 6, C_CARD);
        {
            int pw = (W - 128) * prog / 1000;
            if (pw > 0) round_rect(64, 300, pw, 26, 6, C_ACC);
        }
        {
            char pc[8]; int p = 0; int v = prog / 10;
            pc[p++] = (char)('0' + (v / 100) % 10);
            pc[p++] = (char)('0' + (v / 10) % 10);
            pc[p++] = (char)('0' + v % 10);
            pc[p++] = '%'; pc[p] = 0;
            draw_str((const unsigned char*)pc, 64, 342, C_SUB, 1);
        }
    } else if (page == 3) {
        draw_str16(i_done, 64, 240, C_OK, 2);
        draw_str16(i_done2, 64, 296, C_SUB, 1);
        draw_str((const unsigned char*)"PRESS ENTER TO RESTART", 64, 360, C_TXT, 1);
    } else if (page == 4) {
        draw_str16(i_err, 64, 240, C_RED, 2);
        draw_str((const unsigned char*)"WRITE FAILED", 64, 300, C_SUB, 1);
    }
}

/* ---------------------------------------------------------------- 主流程 */
u64 efi_main(void* image, void* st) {
    u64* stq = (u64*)st;
    conout = (void**)stq[ST_CONOUT/8];
    conin  = (void**)stq[ST_CONIN/8];
    bs     = (void**)stq[ST_BOOT_SERVICES/8];
    rt     = (void**)stq[ST_RUNTIME/8];

    void* gop = 0;
    u64 r = ((u64(*)(const GUID*, void*, void**))bs[BS_LOCATE_PROTOCOL/8])(&GOP_GUID, 0, &gop);
    if (r != 0 || !gop) { while (1) stall(1000000); }
    void** g = (void**)gop;
    u64 (*query)(void*, u32, u64*, void**) = (u64(*)(void*,u32,u64*,void**))g[0];
    u64 (*setmode)(void*, u32) = (u64(*)(void*,u32))g[1];
    u64* mode = (u64*)g[3];
    int best = -1;
    u32 maxmode = *(u32*)mode;
    for (u32 i = 0; i < maxmode; i++) {
        void* info = 0; u64 sz = 0;
        if (query(gop, i, &sz, &info) != 0 || !info) continue;
        u32* p = (u32*)info;
        if (p[1] == 1024 && p[2] == 768) { best = (int)i; break; }
        if (best < 0 && p[1] >= 800 && p[2] >= 600) best = (int)i;
    }
    if (best < 0) { while (1) stall(1000000); }
    setmode(gop, (u32)best);
    void* inf = (void*)mode[1];
    u32* pi = (u32*)inf;
    W = pi[1]; H = pi[2]; PITCH = pi[8];
    fb = (u32*)mode[3];

    ser_puts("SHIGUANG INSTALLER v0.9.2\r\n");
    make_img_name();
    ndisk = scan_disks();
    ser_puts("disks="); ser_hex((u64)ndisk); ser_puts("\r\n");
    if (!ndisk) page = 0;
    scene();

    while (1) {
        KEY k; k.scan = 0; k.ch = 0;
        key_wait();
        if (!key_read(&k)) continue;
        if (k.ch == 0x0D || k.scan == 0x0D) {
            if (page == 0 && ndisk > 0) { page = 1; scene(); }
            else if (page == 1) {
                void* root = img_volume(image);
                void* f = root ? img_open(root) : 0;
                if (!f) { page = 4; scene(); continue; }
                void* buf = 0;
                u64 ar = ((u64(*)(u64, u64, u64, void**))bs[BS_ALLOC_PAGES/8])(0, 2, 256 /*1MB*/, (void**)&buf);
                if (ar != 0 || !buf) { page = 4; scene(); continue; }
                u64 bs_ = disks[sel].media.BlockSize;
                if (!bs_) bs_ = 512;
                u64 chunk = 256ULL * 1024;
                u64 total_mb = disks[sel].bytes / chunk;
                (void)total_mb;
                page = 2; prog = 0; scene();
                u64 lba = 0, written = 0;
                int fail = 0;
                while (1) {
                    u64 sz = chunk;
                    u64 rr = img_read(f, &sz, buf);
                    if (rr != 0) { ser_puts("read fail st="); ser_hex(rr); ser_puts("\r\n"); fail = 1; break; }
                    if (sz == 0) break;
                    u64 nb = (sz + bs_ - 1) / bs_;
                    u64 nbytes = nb * bs_;
                    u64 wr = disk_write(disks[sel].bio, lba, nbytes, buf);
                    if (wr != 0) { ser_puts("write fail at lba "); ser_hex(lba); ser_puts(" st "); ser_hex(wr); ser_puts("\r\n"); fail = 1; break; }
                    lba += nb;
                    written += sz;
                    u64 pm = written / (64ULL * 1024);      /* 64MB → 1000 */
                    if (written > 64ULL*1024*1024) pm = 1000;
                    if ((int)pm != prog) { prog = (int)pm; scene(); }
                }
                disk_flush(disks[sel].bio);
                if (fail) { page = 4; scene(); }
                else {
                    ser_puts("written bytes="); ser_hex(written); ser_puts("\r\n");
                    make_boot_entry(image, disks[sel].handle);
                    page = 3; prog = 1000; scene();
                }
            } else if (page == 3) {
                ((u64(*)(u32, u64, u64, void*))rt[RT_RESET_SYSTEM/8])(1 /*warm*/, 0, 0, 0);
            } else if (page == 4) {
                page = 0; scene();
            }
            continue;
        }
        if (k.scan == 0x01) { if (sel > 0 && page == 0) { sel--; scene(); } }
        else if (k.scan == 0x02) { if (sel < ndisk - 1 && page == 0) { sel++; scene(); } }
        else if (k.ch == 0x1B) { if (page == 1) { page = 0; scene(); } }
    }
    return 0;
}
