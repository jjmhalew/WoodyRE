/* datasetup_posix.c - datasetup.h outside Windows (Linux, Steam Deck; the Windows build has datasetup.c). Looks for the game
 * files in this order: WOODY_DATA (a Data dir), data/ next to the executable, the CD files straight next to it, extract/ in
 * the current directory (a development checkout), $XDG_DATA_HOME/WoodyRE/data (~/.local/share/WoodyRE/data). Nothing there:
 * looks for the CD (or a mounted ISO image) under /media, /run/media and /mnt, asks, and copies the 232 files of the
 * manifest (src/datafiles.h) into ~/.local/share/WoodyRE/data with their SHA-1 checked. The folder that holds data/ becomes
 * the current directory, so woodyre.cfg, woodyre.sav and mods/ live beside it. Names on the disc are matched ignoring case.
 * Android has its own data_find and folder (below): the app's folder, filled from an ISO image or a folder the user picks.
 * So has the Switch: sdmc:/switch/woodyre on the SD card, with the CD files copied into data/ or an ISO image of the CD
 * that is unpacked into data/ at the first start. */
#ifndef _WIN32
#include "datasetup.h"
#include "datafiles.h"
#include "plat.h"
#include <SDL.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define PMAX 4096

/* ---- SHA-1 (FIPS 180-1) ---------------------------------------------------------------------------------------------- */
typedef struct { uint32_t h[5]; uint64_t n; unsigned char b[64]; unsigned nb; } Sha1;
static uint32_t rol(uint32_t x, int k) { return x << k | x >> (32 - k); }
static void sha1_block(Sha1 *s, const unsigned char *p)
{
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; } else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; } else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) { static const uint32_t h0[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 }; memcpy(s->h, h0, sizeof h0); s->n = 0; s->nb = 0; }
static void sha1_add(Sha1 *s, const unsigned char *p, size_t n)
{
    s->n += n;
    while (n) { size_t k = 64 - s->nb < n ? 64 - s->nb : n; memcpy(s->b + s->nb, p, k); s->nb += (unsigned)k; p += k; n -= k; if (s->nb == 64) { sha1_block(s, s->b); s->nb = 0; } }
}
static void sha1_hex(Sha1 *s, char *hex)
{
    uint64_t bits = s->n * 8; unsigned char pad = 0x80, z = 0, len[8];
    sha1_add(s, &pad, 1); while (s->nb != 56) sha1_add(s, &z, 1);
    for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_add(s, len, 8);
    for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", (unsigned)(s->h[i / 4] >> (24 - 8 * (i % 4)) & 0xff));
}

/* ---- folders --------------------------------------------------------------------------------------------------------- */
static int readable(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return 0; fclose(f); return 1; }   /* fopen = plat_fopen: any case */
static int cd_layout(const char *root)
{
    static const char *probe[3] = { "Data/W1A/W1A.gel", "Common/Woody.rck", "Music.bf" };   /* Music.bf is copied last */
    char p[PMAX];
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", root, probe[i]); if (!readable(p)) return 0; }
    return 1;
}
#if !defined __ANDROID__ && !defined __SWITCH__
static void exe_dir(char *d)
{
    ssize_t n = readlink("/proc/self/exe", d, PMAX - 1);
    if (n <= 0) { strcpy(d, "."); return; }
    d[n] = 0; char *s = strrchr(d, '/'); if (s) *s = 0;
}
static int home_dir(char *d)                                /* $XDG_DATA_HOME/WoodyRE or ~/.local/share/WoodyRE (not created here) */
{
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x) { snprintf(d, PMAX, "%s/WoodyRE", x); return 1; }
    if (h && *h) { snprintf(d, PMAX, "%s/.local/share/WoodyRE", h); return 1; }
    return 0;
}
#endif
static void make_dirs(char *path)                           /* every parent directory of path */
{
    for (char *p = path + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
}
static const char *enter(const char *home, const char *rel) { return chdir(home) ? NULL : rel; }
#ifndef __SWITCH__
static int ask(const char *text, const char *yes, const char *no)   /* 1 = yes, 0 = no, -1 = cancel */
{
#ifdef __ANDROID__
    return plat_dialog(text, yes, no, "Cancel");
#endif
    const SDL_MessageBoxButtonData b[3] = { { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes }, { 0, 0, no }, { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -1, "Cancel" } };
    const SDL_MessageBoxData m = { SDL_MESSAGEBOX_INFORMATION, NULL, "WoodyRE", text, no ? 3 : 2, no ? b : (const SDL_MessageBoxButtonData[]){ b[0], b[2] }, NULL };
    int r = -1; if (SDL_ShowMessageBox(&m, &r)) { fprintf(stderr, "%s\n", text); return -1; }
    return r;
}
#endif

static int g_fit[DATAFILES_RELEASES];   /* per supported release: how many files of the last copy/check were its copy (datafile_tally) */
/* reads one manifest file under src_root, writes it under dst_root when that is set (as name.part, renamed at the end) and
 * compares its SHA-1: 1 = equal, 2 = missing as in a supported release, 0 = differs, -1 = missing (src) or cannot be written (dst) */
static int file_pass(const char *src_root, const char *dst_root, int i, unsigned char *buf, size_t bufsz, unsigned long long *done)
{
    char sp[PMAX], dp[PMAX], tp[PMAX + 8];
    snprintf(sp, sizeof sp, "%s/%s", src_root, k_datafiles[i].path);
    FILE *s = fopen(sp, "rb"); if (!s) return datafile_absent(i, g_fit) ? 2 : -1;   /* the Russian CD has no Data/Lang (never read) */
    FILE *d = NULL;
    if (dst_root) {
        snprintf(dp, sizeof dp, "%s/%s", dst_root, k_datafiles[i].path); make_dirs(dp); snprintf(tp, sizeof tp, "%s.part", dp);
        if (!(d = fopen(tp, "wb"))) { fclose(s); return -1; }
    }
    Sha1 h; sha1_init(&h); int ok = 1; unsigned long long size = 0; size_t got;
    while ((got = fread(buf, 1, bufsz, s)) > 0) {
        sha1_add(&h, buf, got);
        if (d && fwrite(buf, 1, got, d) != got) { ok = -1; break; }
        size += got; if (done) *done += got;
    }
    fclose(s);
    char hex[41]; sha1_hex(&h, hex);
    if (ok > 0 && !datafile_tally(i, size, hex, g_fit)) ok = 0;
    if (d) { if (fclose(d) || ok < 0 || rename(tp, dp)) { remove(tp); return -1; } }
    return ok;
}
#if !defined __ANDROID__ && !defined __SWITCH__
static int copy_cd(const char *src, const char *home)       /* the number of files that differ from the 1.00 CD, -1 = failed */
{
    char dst[PMAX], m[PMAX + 400]; snprintf(dst, sizeof dst, "%s/data", home); mkdir(dst, 0755);
    struct statvfs vf;
    if (!statvfs(home, &vf) && (unsigned long long)vf.f_bavail * vf.f_frsize < (unsigned long long)DATAFILES_BYTES + (16u << 20)) {
        snprintf(m, sizeof m, "Not enough free disk space for the game files (%u MB) in\n%s", DATAFILES_BYTES >> 20, dst); plat_message(m, 1); return -1;
    }
    size_t bufsz = 4u << 20; unsigned char *buf = malloc(bufsz); if (!buf) return -1;
    unsigned long long done = 0; int bad = 0, first_bad = -1, last = -1; memset(g_fit, 0, sizeof g_fit);
    printf("data: copying the game files from %s to %s\n", src, dst); fflush(stdout);
    for (int i = 0; i < DATAFILES_COUNT; i++) {
        int r = file_pass(src, dst, i, buf, bufsz, &done);
        if (r < 0) {
            snprintf(m, sizeof m, "Could not copy %s\nfrom %s/ to\n%s/\n\nIs the CD complete, and is there room on the disk?", k_datafiles[i].path, src, dst);
            plat_message(m, 1); free(buf); return -1;
        }
        if (!r) { bad++; if (first_bad < 0) first_bad = i; printf("data: %s matches none of the supported CDs\n", k_datafiles[i].path); }
        int pct = (int)(done * 100 / DATAFILES_BYTES); if (pct / 10 != last) { last = pct / 10; printf("data: %d%%\n", pct); fflush(stdout); }
    }
    free(buf);
    printf("data: copied from the %s CD\n", k_releases[datafile_best(g_fit)]);
    if (bad) {
        snprintf(m, sizeof m, "%d of the copied files match none of the supported CDs (the first: %s).\n\n"
                              "WoodyRE supports the English 1.00, Brazilian, Polish, Spanish and Russian CDs; another release or a damaged copy may not work correctly.", bad, k_datafiles[first_bad].path);
        plat_message(m, 1);
    }
    return bad;
}
static int find_cd(char *root)                              /* a mounted CD or ISO image with the CD layout */
{
    const char *user = getenv("USER"); char bases[4][PMAX]; int nb = 0;
    if (user) { snprintf(bases[nb++], PMAX, "/media/%s", user); snprintf(bases[nb++], PMAX, "/run/media/%s", user); }
    snprintf(bases[nb++], PMAX, "/media"); snprintf(bases[nb++], PMAX, "/mnt");
    for (int b = 0; b < nb; b++) {
        DIR *d = opendir(bases[b]); struct dirent *de; if (!d) continue;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            snprintf(root, PMAX, "%s/%s", bases[b], de->d_name);
            if (cd_layout(root)) { closedir(d); return 1; }
        }
        closedir(d);
    }
    return 0;
}

/* the game files in dir/rel next to the executable: dir becomes the current directory (woodyre.cfg, .sav, .log, mods/ beside
 * them) unless it cannot be written (/opt, /usr, a read-only mount): then home does, and the files are read by their full path */
static const char *enter_exe(const char *dir, const char *rel, const char *home)
{
    static char out[PMAX + 16]; char h[PMAX + 2];
    if (!home || access(dir, W_OK) == 0) return enter(dir, rel);
    snprintf(out, sizeof out, "%s/%s", dir, rel); snprintf(h, sizeof h, "%s/", home); make_dirs(h);
    return chdir(home) ? enter(dir, rel) : out;
}

const char *data_find(void)
{
    const char *env = getenv("WOODY_DATA"); if (env && *env) return env;
    char exe[PMAX], home[PMAX], p[PMAX + 16];
    exe_dir(exe); int have_home = home_dir(home);
    snprintf(p, sizeof p, "%s/data", exe); if (cd_layout(p)) return enter_exe(exe, "data/Data", have_home ? home : NULL);
    if (cd_layout(exe)) return enter_exe(exe, "Data", have_home ? home : NULL);
    if (cd_layout("extract")) return "extract/Data";
    if (have_home) { snprintf(p, sizeof p, "%s/data", home); if (cd_layout(p)) return enter(home, "data/Data"); }
    if (!have_home) { plat_message("No folder for the game files ($HOME is not set).", 1); return NULL; }

    /* first start: copy the CD into ~/.local/share/WoodyRE/data */
    for (;;) {
        char src[PMAX], m[3 * PMAX];
        if (find_cd(src)) {
            snprintf(m, sizeof m, "Found the Woody Woodpecker CD at %s\n\nCopy its game files (%u MB) to\n%s/data ?\n\n"
                                  "This happens once; afterwards the CD is not needed. It takes a few minutes and the window\n"
                                  "only opens when it is done.", src, DATAFILES_BYTES >> 20, home);
            int r = ask(m, "Copy", NULL);
            if (r != 1) return NULL;
            make_dirs(strcat(strcpy(p, home), "/"));
            if (copy_cd(src, home) < 0) return NULL;
            return enter(home, "data/Data");
        }
        snprintf(m, sizeof m, "WoodyRE needs the files of the original game CD-ROM:\n"
                              "Woody Woodpecker: Escape from Buzz Buzzard Park (PC; the English, Brazilian, Polish, Spanish or Russian CD).\n\n"
                              "Insert the CD or mount your ISO image of it, then press Search.\n"
                              "Or copy Data, Common, Logo, Game and Music.bf from the CD into\n%s/data\nyourself and start WoodyRE again.", home);
        if (ask(m, "Search", NULL) != 1) return NULL;
    }
}
#else
#include <fcntl.h>
#ifdef __ANDROID__
/* ---- Android: the app's own folder on the shared storage (Android/data/<package>/files: a USB cable reaches it, no
 * permission needed), filled once from an ISO image of the CD or a folder with a copy of it that the user picks in the
 * system's file picker (WoodyActivity.java); an ISO is read here, through the file descriptor the picker hands out. */
#include <jni.h>

static int home_dir(char *d)
{
    const char *p = SDL_AndroidGetExternalStoragePath(); if (!p) p = SDL_AndroidGetInternalStoragePath();
    if (!p) return 0;
    snprintf(d, PMAX, "%s", p); return 1;
}
/* WoodyActivity.pickGameData(kind, dest): kind 1 = an ISO image, returns its file descriptor; kind 2 = a folder, copied
 * into dest by the Java side, returns 0. -1 = cancelled, -2 = failed (the Java side said why). */
static int java_pick(int kind, const char *dest)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv(); jobject act = (jobject)SDL_AndroidGetActivity(); int r = -2;
    if (!env || !act) return -2;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "pickGameData", "(ILjava/lang/String;)I");
    if (m) { jstring s = (*env)->NewStringUTF(env, dest ? dest : ""); r = (*env)->CallStaticIntMethod(env, c, m, kind, s); (*env)->DeleteLocalRef(env, s); }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -2; }
    (*env)->DeleteLocalRef(env, c); (*env)->DeleteLocalRef(env, act);
    return r;
}
static void show_progress(const char *text)                 /* WoodyActivity.progress: a dialog with this text, NULL closes it */
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv(); jobject act = (jobject)SDL_AndroidGetActivity();
    if (!env || !act) return;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "progress", "(Ljava/lang/String;)V");
    if (m) { jstring s = text ? (*env)->NewStringUTF(env, text) : NULL; (*env)->CallStaticVoidMethod(env, c, m, s); if (s) (*env)->DeleteLocalRef(env, s); }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, c); (*env)->DeleteLocalRef(env, act);
}
#else
/* ---- Switch: sdmc:/switch/woodyre on the SD card (next to woodyre.nro), with the CD files in data/ or an ISO image of the
 * CD that is unpacked into data/ at the first start. The unpacking shows its progress on libnx's text console, which is
 * closed again before the game opens its window. */
#include <switch/types.h>
#include <switch/runtime/devices/console.h>
#define SWITCH_HOME "sdmc:/switch/woodyre"
static int home_dir(char *d) { snprintf(d, PMAX, "%s", SWITCH_HOME); return 1; }
static int g_con;                                           /* the text console is up */
static void show_progress(const char *text)
{
    if (!text) return;
    if (!g_con) { consoleInit(NULL); g_con = 1; }
    printf("\r%s   ", text); fflush(stdout); consoleUpdate(NULL);
}
#endif
static void progress_pct(const char *what, unsigned long long done, int *last)
{
    int pct = (int)(done * 100 / DATAFILES_BYTES); char m[128];
    if (pct == *last) return;
    *last = pct; snprintf(m, sizeof m, "%s %d %%", what, pct); show_progress(m);
}

/* ---- ISO 9660 (ECMA-119) with the Joliet names when there are some: just enough to find the manifest's files ---- */
typedef struct { int fd; uint32_t root_lba, root_len; int joliet; } Iso;
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int iso_pread(int fd, void *b, size_t n, unsigned long long off)
{
#ifdef __SWITCH__                                           /* libnx has no pread */
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return -1;
    for (size_t got = 0; got < n; ) { ssize_t k = read(fd, (char *)b + got, n - got); if (k <= 0) return -1; got += (size_t)k; }
#else
    for (size_t got = 0; got < n; ) { ssize_t k = pread(fd, (char *)b + got, n - got, (off_t)(off + got)); if (k <= 0) return -1; got += (size_t)k; }
#endif
    return 0;
}
static int iso_open(Iso *is, int fd)
{
    unsigned char b[2048]; int have = 0; memset(is, 0, sizeof *is); is->fd = fd;
    for (uint32_t s = 16; s < 64; s++) {
        if (iso_pread(fd, b, sizeof b, (unsigned long long)s * 2048) || memcmp(b + 1, "CD001", 5)) break;
        if (b[0] == 255) break;
        int jol = b[0] == 2 && b[88] == '%' && b[89] == '/' && (b[90] == '@' || b[90] == 'C' || b[90] == 'E');
        if ((b[0] == 1 && !have) || jol) { is->root_lba = le32(b + 156 + 2); is->root_len = le32(b + 156 + 10); is->joliet = jol; have = 1; }
        if (jol) break;
    }
    return have ? 0 : -1;
}
static int iso_name_is(const unsigned char *n, int len, int joliet, const char *want, size_t wl)
{
    char a[256]; size_t o = 0;
    if (joliet) { for (int i = 0; i + 1 < len && o < sizeof a - 1; i += 2) a[o++] = n[i] ? '?' : (char)n[i + 1]; }
    else for (int i = 0; i < len && o < sizeof a - 1; i++) a[o++] = (char)n[i];
    a[o] = 0;
    char *semi = strchr(a, ';'); if (semi) *semi = 0;             /* the version ";1" */
    o = strlen(a); if (o && a[o - 1] == '.') a[--o] = 0;          /* "MUSIC." of a name without an extension */
    return o == wl && !strncasecmp(a, want, wl);
}
static int iso_find(const Iso *is, const char *path, uint32_t *lba, uint32_t *len)   /* 0 = found (a file) */
{
    uint32_t cl = is->root_lba, cn = is->root_len; const char *p = path;
    while (*p) {
        const char *e = strchr(p, '/'); size_t wl = e ? (size_t)(e - p) : strlen(p); int found = 0;
        unsigned char *d = malloc(cn ? cn : 1); if (!d || iso_pread(is->fd, d, cn, (unsigned long long)cl * 2048)) { free(d); return -1; }
        for (uint32_t o = 0; o < cn; ) {
            unsigned rl = d[o];
            if (!rl) { o = (o / 2048 + 1) * 2048; continue; }        /* records do not cross sectors */
            if (o + 33 > cn || o + rl > cn) break;
            int nl = d[o + 32];
            if (!(nl == 1 && d[o + 33] <= 1) && iso_name_is(d + o + 33, nl, is->joliet, p, wl)) { cl = le32(d + o + 2); cn = le32(d + o + 10); found = 1; if (!e && (d[o + 25] & 2)) found = 0; break; }
            o += rl;
        }
        free(d);
        if (!found) return -1;
        p = e ? e + 1 : p + wl;
    }
    *lba = cl; *len = cn; return 0;
}
static int iso_copy(int fd, const char *home)               /* the number of files that differ from the 1.00 CD, -1 = failed */
{
    Iso is; char m[PMAX + 400], dst[PMAX];
    if (iso_open(&is, fd)) { plat_message("That file is not an ISO image of a CD.", 1); return -1; }
    uint32_t l0, n0;
    if (iso_find(&is, "Data/W1A/W1A.gel", &l0, &n0) || iso_find(&is, "Music.bf", &l0, &n0)) { plat_message("That ISO image does not hold the Woody Woodpecker game files (Data, Common, Music.bf).", 1); return -1; }
    snprintf(dst, sizeof dst, "%s/data", home); mkdir(dst, 0755);
    struct statvfs vf;
    if (!statvfs(home, &vf) && (unsigned long long)vf.f_bavail * vf.f_frsize < (unsigned long long)DATAFILES_BYTES + (16u << 20)) {
        snprintf(m, sizeof m, "Not enough free space for the game files (%u MB) in\n%s", DATAFILES_BYTES >> 20, dst); plat_message(m, 1); return -1;
    }
    size_t bufsz = 4u << 20; unsigned char *buf = malloc(bufsz); if (!buf) return -1;
    unsigned long long done = 0; int bad = 0, first_bad = -1, last = -1; memset(g_fit, 0, sizeof g_fit);
    printf("data: copying the game files from the ISO image to %s\n", dst);
    for (int i = 0; i < DATAFILES_COUNT; i++) {
        char dp[PMAX], tp[PMAX + 8]; uint32_t lba, len; int ok = 1;
        if (iso_find(&is, k_datafiles[i].path, &lba, &len) && datafile_absent(i, g_fit)) continue;
        if (iso_find(&is, k_datafiles[i].path, &lba, &len)) { snprintf(m, sizeof m, "The ISO image has no %s.", k_datafiles[i].path); show_progress(NULL); plat_message(m, 1); free(buf); return -1; }
        snprintf(dp, sizeof dp, "%s/%s", dst, k_datafiles[i].path); make_dirs(dp); snprintf(tp, sizeof tp, "%s.part", dp);
        FILE *d = fopen(tp, "wb"); if (!d) ok = -1;
        Sha1 h; sha1_init(&h);
        for (uint32_t o = 0; ok > 0 && o < len; ) {
            size_t n = len - o < bufsz ? len - o : bufsz;
            if (iso_pread(fd, buf, n, (unsigned long long)lba * 2048 + o) || fwrite(buf, 1, n, d) != n) { ok = -1; break; }
            sha1_add(&h, buf, n); o += (uint32_t)n; done += n; progress_pct("Copying the game files...", done, &last);
        }
        if (d && fclose(d)) ok = -1;
        if (ok < 0 || rename(tp, dp)) {
            remove(tp); show_progress(NULL);
            snprintf(m, sizeof m, "Could not copy %s from the ISO image to\n%s/\n\nIs there room on the device?", k_datafiles[i].path, dst);
            plat_message(m, 1); free(buf); return -1;
        }
        char hex[41]; sha1_hex(&h, hex);
        if (!datafile_tally(i, len, hex, g_fit)) { bad++; if (first_bad < 0) first_bad = i; printf("data: %s matches none of the supported CDs\n", k_datafiles[i].path); }
    }
    free(buf); show_progress(NULL);
    printf("data: the %s CD\n", k_releases[datafile_best(g_fit)]);
    if (bad) {
        snprintf(m, sizeof m, "%d of the copied files match none of the supported CDs (the first: %s).\n\n"
                              "WoodyRE supports the English 1.00, Brazilian, Polish, Spanish and Russian CDs; another release or a damaged copy may not work correctly.", bad, k_datafiles[first_bad].path);
        plat_message(m, 1);
    }
    return bad;
}
#ifdef __ANDROID__
static void check_copy(const char *home)                     /* after the Java side copied a folder: compare it with the manifest */
{
    char root[PMAX], m[PMAX + 300]; snprintf(root, sizeof root, "%s/data", home);
    size_t bufsz = 4u << 20; unsigned char *buf = malloc(bufsz); if (!buf) return;
    unsigned long long done = 0; int bad = 0, first_bad = -1, last = -1; memset(g_fit, 0, sizeof g_fit);
    for (int i = 0; i < DATAFILES_COUNT; i++) {
        if (file_pass(root, NULL, i, buf, bufsz, &done) <= 0) { bad++; if (first_bad < 0) first_bad = i; printf("data: %s missing or differs\n", k_datafiles[i].path); }
        progress_pct("Checking the game files...", done, &last);
    }
    free(buf); show_progress(NULL);
    printf("data: the %s CD\n", k_releases[datafile_best(g_fit)]);
    if (bad) {
        snprintf(m, sizeof m, "%d game files are missing or match none of the supported CDs (the first: %s).\n\n"
                              "WoodyRE supports the English 1.00, Brazilian, Polish, Spanish and Russian CDs; another release or a damaged copy may not work correctly.", bad, k_datafiles[first_bad].path);
        plat_message(m, 1);
    }
}

const char *data_find(void)
{
    const char *env = getenv("WOODY_DATA"); if (env && *env) return env;
    char home[PMAX], p[PMAX + 16], m[3 * PMAX];
    if (!home_dir(home)) { plat_message("No storage for the game files.", 1); return NULL; }
    snprintf(p, sizeof p, "%s/data", home); if (cd_layout(p)) return enter(home, "data/Data");
    for (;;) {
        snprintf(m, sizeof m, "WoodyRE needs the files of the original game CD-ROM:\n"
                              "Woody Woodpecker: Escape from Buzz Buzzard Park (PC; the English, Brazilian, Polish, Spanish or Russian CD).\n\n"
                              "Choose an ISO image of the CD, or a folder with a copy of it (Data, Common, Logo, Game and Music.bf). "
                              "The game files (%u MB) are copied once.\n\n"
                              "Or copy those files with a USB cable into\n%s/data\nand start WoodyRE again.", DATAFILES_BYTES >> 20, home);
        int r = ask(m, "ISO image", "Folder");
        if (r < 0) return NULL;
        mkdir(p, 0755);
        if (r == 1) {
            int fd = java_pick(1, NULL);
            if (fd == -2) plat_message("Could not open that file.", 1);
            if (fd < 0) continue;
            int bad = iso_copy(fd, home); close(fd);
            if (bad >= 0 && cd_layout(p)) return enter(home, "data/Data");
        } else {
            int k = java_pick(2, p);
            if (k == -2) plat_message("Could not copy that folder. Is there room on the device?", 1);
            if (k < 0) continue;
            if (!cd_layout(p)) { plat_message("That folder does not hold the game files: it needs Data, Common, Logo, Game and Music.bf of the CD.", 1); continue; }
            check_copy(home);
            return enter(home, "data/Data");
        }
    }
}
#else
const char *data_find(void)
{
    const char *env = getenv("WOODY_DATA"); if (env && *env) return env;
    char home[PMAX], p[PMAX + 16], iso[PMAX + 260] = "", m[3 * PMAX];
    home_dir(home); snprintf(p, sizeof p, "%s/", home); make_dirs(p);
    snprintf(p, sizeof p, "%s/data", home); if (cd_layout(p)) return enter(home, "data/Data");
    if (cd_layout(home)) return enter(home, "Data");
    DIR *d = opendir(home); struct dirent *de;                  /* an ISO image of the CD beside woodyre.nro */
    if (d) {
        while ((de = readdir(d))) { size_t n = strlen(de->d_name); if (n > 4 && !strcasecmp(de->d_name + n - 4, ".iso")) { snprintf(iso, sizeof iso, "%s/%s", home, de->d_name); break; } }
        closedir(d);
    }
    if (*iso) {
        int fd = open(iso, O_RDONLY), bad = -1;
        if (fd < 0) { snprintf(m, sizeof m, "Could not open %s", iso); plat_message(m, 1); return NULL; }
        mkdir(p, 0755); show_progress("WoodyRE: unpacking the game files from the ISO image, this happens once ...\n");
        bad = iso_copy(fd, home); close(fd);
        if (g_con) { consoleExit(NULL); g_con = 0; }
        if (bad >= 0 && cd_layout(p)) {
            printf("data: unpacked %s; the ISO image is no longer needed\n", iso);
            return enter(home, "data/Data");
        }
        return NULL;
    }
    snprintf(m, sizeof m, "WoodyRE needs the files of the original game CD-ROM:\n"
                          "Woody Woodpecker: Escape from Buzz Buzzard Park (PC; the English, Brazilian, Polish, Spanish or Russian CD).\n\n"
                          "Put an ISO image of the CD into %s/ (it is unpacked once, then it can be deleted), "
                          "or copy Data, Common, Logo, Game and Music.bf from the CD into\n%s/data/\nand start WoodyRE again.", home, home);
    plat_message(m, 1);
    return NULL;
}
#endif
#endif

int data_verify(const char *data_dir)
{
    char root[PMAX]; snprintf(root, sizeof root, "%s/..", data_dir);
    size_t bufsz = 4u << 20; unsigned char *buf = malloc(bufsz); if (!buf) return -1;
    int bad = 0; memset(g_fit, 0, sizeof g_fit);
    for (int i = 0; i < DATAFILES_COUNT; i++) {
        int k = file_pass(root, NULL, i, buf, bufsz, NULL);
        if (k <= 0) { bad++; printf("verify: %s %s\n", k_datafiles[i].path, k < 0 ? "MISSING" : "matches none of the supported CDs"); }
    }
    free(buf);
    int best = datafile_best(g_fit);
    printf("verify: %d of %d files belong to a supported CD; the copy is the %s CD (%d of its files)%s\n", DATAFILES_COUNT - bad, DATAFILES_COUNT,
           k_releases[best], g_fit[best], bad ? "" : " - all good");
    return bad;
}
#endif
