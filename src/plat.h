/* plat.h - the little the engine takes from the operating system outside the window (render_gl.h). On Windows that is
 * <windows.h> itself; elsewhere (Linux, the SDL2 build: src/plat_sdl.c) the same names, so the engine keeps one spelling:
 * the Windows virtual-key codes (the key bindings, woodyre.cfg and the Controls page all store VK codes), Sleep, the
 * case-insensitive string compares, and fopen made case-insensitive for reading - the CD's names do not always match the
 * original's spelling of them ("Blackbox" on the disc, "BlackBox" in the level table). */
#ifndef WOODY_PLAT_H
#define WOODY_PLAT_H
#ifdef _WIN32
#include <windows.h>
#define plat_gl_proc(name) ((void (*)(void))wglGetProcAddress(name))
#define plat_exists(path) (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
#define plat_vsc_to_vk(sc) ((int)MapVirtualKeyA((UINT)(sc), 1 /* MAPVK_VSC_TO_VK */))
static inline void plat_message(const char *text, int warn) { MessageBoxA(NULL, text, "WoodyRE", warn ? MB_ICONWARNING : MB_ICONINFORMATION); }
#define plat_replace(from, to) (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)   /* 1 = file from now is file to */
#else
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <errno.h>
/* libnx's rename() is the FS service's RenameFile, which refuses a name that exists (2002-0002, EEXIST): a plain rename
 * failed every save but the first (issue #5). So the old file steps aside as to.old, the new one takes its name, then the
 * old one goes; file_read takes to.old back when a crash between the two renames left only that one. */
static inline int plat_replace(const char *from, const char *to)
{
    char old[512]; snprintf(old, sizeof old, "%s.old", to);
    remove(old);
    if (rename(to, old) && errno != ENOENT) return 0;
    if (rename(from, to)) { rename(old, to); return 0; }
    remove(old);
    return 1;
}
#else
#define plat_replace(from, to) (rename(from, to) == 0)
#endif
#define APIENTRY
#define WINAPI
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
static inline void Sleep(unsigned ms) { usleep(ms ? ms * 1000u : 0u); }
#define timeBeginPeriod(x) ((void)0)
#define _fseeki64 fseeko                              /* 64-bit offsets (Music.bf); build.sh sets _FILE_OFFSET_BITS=64 */
#define _ftelli64 ftello
FILE *plat_fopen(const char *path, const char *mode);   /* fopen; for reading, a path that does not exist is looked up again per part ignoring case */
#define fopen plat_fopen
int plat_exists(const char *path);
int plat_vsc_to_vk(int scancode);                      /* a DirectInput (set 1) scan code -> VK on a US layout (Woody.cfg import) */
void (*plat_gl_proc(const char *name))(void);
void plat_message(const char *text, int warn);
#ifdef __ANDROID__
int plat_dialog(const char *text, const char *b1, const char *b2, const char *b3);   /* Android's own dialog: 1 = b1, 0 = b2, -1 = b3 */
#endif
enum {                                                 /* the Windows virtual-key codes (winuser.h) */
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_PAUSE = 0x13,
    VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24,
    VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E,
    VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D, VK_NUMPAD0 = 0x60, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
    VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D,
    VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F4 = 0x73, VK_F5 = 0x74, VK_F11 = 0x7A, VK_F12 = 0x7B, VK_F24 = 0x87,
    VK_NUMLOCK = 0x90, VK_SCROLL = 0x91, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3,
    VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD,
    VK_OEM_PERIOD = 0xBE, VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE,
    VK_OEM_102 = 0xE2
};
#endif
#endif
