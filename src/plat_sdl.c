/* plat_sdl.c - the window, keyboard, mouse and OS bits of render_gl.h / plat.h on SDL2, for the builds outside Windows
 * (Linux, Steam Deck: build.sh; Android: android/, on OpenGL ES 2.0 / 3.0 through src/gles). The Windows build keeps its own
 * Win32 + WGL window in render_gl.c. Keys arrive as SDL
 * scancodes and are stored as the Windows virtual-key codes the rest of the engine uses: letters by the layout (as VK
 * letters are on Windows), everything else by position; Ctrl / Shift / Alt set both their side and the plain code. */
#ifndef _WIN32
#include "plat.h"
#undef fopen
#include "render_gl.h"
#include "touch.h"
#include <SDL.h>
#include <GL/gl.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---- files ----------------------------------------------------------------------------------------------------------- */
int plat_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

/* a name on disk that stands for want: equal ignoring case, or with one dot more at the end - an ISO 9660 name without an
 * extension is stored as "CODE.;1", and some tools that unpack the image keep that dot (Windows drops it, so the engine asks
 * for Data/House/code) */
static int name_is(const char *disk, const char *want)
{
    size_t n = strlen(disk), w = strlen(want);
    return !strcasecmp(disk, want) || (n == w + 1 && disk[w] == '.' && !strncasecmp(disk, want, w));
}

/* path with every part matched ignoring case (and an ISO trailing dot) against what is on disk; 0 = some part is not there */
static int resolve(const char *in, char *out, size_t cap)
{
    char buf[1024]; size_t n = strlen(in); if (n >= sizeof buf || n + 1 > cap) return 0;
    for (size_t i = 0; i <= n; i++) buf[i] = in[i] == '\\' ? '/' : in[i];
    size_t o = 0; out[0] = 0; char *p = buf;
    if (*p == '/') { out[o++] = '/'; out[o] = 0; while (*p == '/') p++; }
    while (*p) {
        char *e = strchr(p, '/'); if (e) *e = 0;
        char test[1024];
        snprintf(test, sizeof test, "%s%s", out, p);
        if (!strcmp(p, ".") || !strcmp(p, "..") || plat_exists(test)) { o += (size_t)snprintf(out + o, cap - o, "%s", p); }
        else {
            DIR *d = opendir(o ? out : "."); struct dirent *de; int found = 0;
            if (d) { while ((de = readdir(d))) if (name_is(de->d_name, p)) { o += (size_t)snprintf(out + o, cap - o, "%s", de->d_name); found = 1; break; } closedir(d); }
            if (!found) return 0;
        }
        if (o >= cap - 1) return 0;
        if (!e) break;
        out[o++] = '/'; out[o] = 0; p = e + 1; while (*p == '/') p++;
    }
    return 1;
}
FILE *plat_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (f || mode[0] != 'r' || errno != ENOENT) return f;
    char real[1024]; if (!resolve(path, real, sizeof real)) return NULL;
    return fopen(real, mode);
}

int plat_vsc_to_vk(int sc)            /* MapVirtualKey(sc, MAPVK_VSC_TO_VK) of a US keyboard, set 1 scan codes 0x01..0x58 */
{
    static const unsigned char T[0x59] = {
        0, VK_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', VK_OEM_MINUS, VK_OEM_PLUS, VK_BACK, VK_TAB,
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', VK_OEM_4, VK_OEM_6, VK_RETURN, VK_LCONTROL, 'A', 'S',
        'D', 'F', 'G', 'H', 'J', 'K', 'L', VK_OEM_1, VK_OEM_7, VK_OEM_3, VK_LSHIFT, VK_OEM_5, 'Z', 'X', 'C', 'V',
        'B', 'N', 'M', VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_2, VK_RSHIFT, VK_MULTIPLY, VK_LMENU, VK_SPACE, VK_CAPITAL, VK_F1, VK_F1 + 1, VK_F1 + 2, VK_F1 + 3, VK_F1 + 4,
        VK_F1 + 5, VK_F1 + 6, VK_F1 + 7, VK_F1 + 8, VK_F1 + 9, VK_NUMLOCK, VK_SCROLL, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_SUBTRACT, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_ADD, VK_NUMPAD1,
        VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL, 0, 0, VK_OEM_102, VK_F11, VK_F12 };
    return sc > 0 && sc < 0x59 ? T[sc] : 0;
}
#ifdef __ANDROID__
void (*plat_gl_proc(const char *name))(void) { return gles_proc(name); }   /* eglGetProcAddress may hand out stubs for any name */
#else
void (*plat_gl_proc(const char *name))(void) { return (void (*)(void))SDL_GL_GetProcAddress(name); }
#endif
#ifdef __ANDROID__
#include <jni.h>
/* WoodyActivity.dialog: a scrolling message with up to three buttons that stay on the screen (SDL's message box does
 * not scroll, and in landscape its buttons end up below a phone's screen). Blocks until one is pressed. */
int plat_dialog(const char *text, const char *b1, const char *b2, const char *b3)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv(); jobject act = (jobject)SDL_AndroidGetActivity(); int r = -1;
    if (!env || !act) { fprintf(stderr, "%s\n", text); return -1; }
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "dialog", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");
    if (m) {
        jstring s[4] = { (*env)->NewStringUTF(env, text), b1 ? (*env)->NewStringUTF(env, b1) : NULL, b2 ? (*env)->NewStringUTF(env, b2) : NULL, b3 ? (*env)->NewStringUTF(env, b3) : NULL };
        r = (*env)->CallStaticIntMethod(env, c, m, s[0], s[1], s[2], s[3]);
        for (int i = 0; i < 4; i++) if (s[i]) (*env)->DeleteLocalRef(env, s[i]);
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -1; }
    (*env)->DeleteLocalRef(env, c); (*env)->DeleteLocalRef(env, act);
    return r;
}
void plat_message(const char *text, int warn) { (void)warn; printf("%s\n", text); plat_dialog(text, "OK", NULL, NULL); }
#else
void plat_message(const char *text, int warn)
{
    if (SDL_ShowSimpleMessageBox(warn ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, "WoodyRE", text, NULL)) fprintf(stderr, "%s\n", text);
}
#endif

#ifdef __ANDROID__
/* stdout (woodyre.log, WOODY_GUI) also to logcat: adb logcat -s WoodyRE */
#include <android/log.h>
#include <pthread.h>
static int g_logfd = -1, g_logpipe[2];
static void *log_pump(void *arg)
{
    (void)arg; char b[1024]; size_t n = 0; ssize_t k;
    while ((k = read(g_logpipe[0], b + n, sizeof b - 1 - n)) > 0) {
        if (g_logfd >= 0) (void)!write(g_logfd, b + n, (size_t)k);
        n += (size_t)k; b[n] = 0;
        char *s = b, *e;
        while ((e = strchr(s, '\n'))) { *e = 0; __android_log_write(ANDROID_LOG_INFO, "WoodyRE", s); s = e + 1; }
        n = strlen(s); memmove(b, s, n + 1);
        if (n == sizeof b - 1) { __android_log_write(ANDROID_LOG_INFO, "WoodyRE", b); n = 0; }
    }
    return NULL;
}
static void log_tee(void)
{
    pthread_t t; fflush(stdout);
    if (pipe(g_logpipe)) return;
    g_logfd = dup(fileno(stdout)); dup2(g_logpipe[1], fileno(stdout));
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (pthread_create(&t, NULL, log_pump, NULL) == 0) pthread_detach(t);
}
#endif

/* ---- window ---------------------------------------------------------------------------------------------------------- */
static int vk_of(const SDL_Keysym *k)
{
    if (k->sym >= 'a' && k->sym <= 'z') return k->sym - 'a' + 'A';   /* letters follow the layout, like VK_A..VK_Z */
    int s = k->scancode;
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + s - SDL_SCANCODE_A;
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + s - SDL_SCANCODE_1;
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return VK_F1 + s - SDL_SCANCODE_F1;
    if (s >= SDL_SCANCODE_F13 && s <= SDL_SCANCODE_F24) return VK_F1 + 12 + s - SDL_SCANCODE_F13;
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return VK_NUMPAD1 + s - SDL_SCANCODE_KP_1;
    switch (s) {
    case SDL_SCANCODE_0: return '0';                 case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;      case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;            case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;    case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
    case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;  case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
    case SDL_SCANCODE_BACKSLASH: case SDL_SCANCODE_NONUSHASH: return VK_OEM_5;
    case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;    case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
    case SDL_SCANCODE_GRAVE: return VK_OEM_3;        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
    case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;  case SDL_SCANCODE_SLASH: return VK_OEM_2;
    case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;   case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
    case SDL_SCANCODE_SCROLLLOCK: return VK_SCROLL;  case SDL_SCANCODE_PAUSE: return VK_PAUSE;
    case SDL_SCANCODE_INSERT: return VK_INSERT;      case SDL_SCANCODE_HOME: return VK_HOME;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;       case SDL_SCANCODE_DELETE: return VK_DELETE;
    case SDL_SCANCODE_END: return VK_END;            case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;        case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_DOWN: return VK_DOWN;          case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_NUMLOCKCLEAR: return VK_NUMLOCK; case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
    case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY; case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
    case SDL_SCANCODE_KP_PLUS: return VK_ADD;        case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
    case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;  case SDL_SCANCODE_NONUSBACKSLASH: return VK_OEM_102;
    case SDL_SCANCODE_APPLICATION: return VK_APPS;
    case SDL_SCANCODE_AC_BACK: return VK_ESCAPE;     /* Android's back button / gesture: the pause menu and back */
    case SDL_SCANCODE_LCTRL: return VK_LCONTROL;     case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
    case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;      case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
    case SDL_SCANCODE_LALT: return VK_LMENU;         case SDL_SCANCODE_RALT: return VK_RMENU;
    case SDL_SCANCODE_LGUI: return VK_LWIN;          case SDL_SCANCODE_RGUI: return VK_RWIN;
    default: return 0;
    }
}
static void drawable(Window *w) { int dw, dh; SDL_GL_GetDrawableSize((SDL_Window *)w->hwnd, &dw, &dh); if (dw > 0 && dh > 0) { w->width = dw; w->height = dh; } }

int win_open(Window *w, const char *title, int width, int height)
{
    memset(w, 0, sizeof *w);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight"); SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");                     /* the fingers are the on-screen pad (touch.c), not a mouse */
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    flags |= SDL_WINDOW_FULLSCREEN;
    log_tee();
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return -1; }
#ifdef __ANDROID__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_Window *sw = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (!sw) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return -1; }
    SDL_GLContext gc = SDL_GL_CreateContext(sw);
#ifdef __ANDROID__
    if (!gc) { SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2); gc = SDL_GL_CreateContext(sw); }   /* no ES 3.0: 2.0 does too */
#endif
    if (!gc) { fprintf(stderr, "SDL OpenGL: %s\n", SDL_GetError()); SDL_DestroyWindow(sw); return -1; }
    w->hwnd = sw; w->hglrc = gc; w->width = width; w->height = height; w->focused = 1; drawable(w);
    printf("OpenGL: %s / %s (SDL %d.%d.%d, %s)\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION), SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL, SDL_GetCurrentVideoDriver());
    return 0;
}
static unsigned char g_down_now[256], g_up_late[256];   /* a key pressed and released within one poll stays down for one frame */
void win_poll(Window *w)
{
    w->mouse_dx = w->mouse_dy = 0; w->raw_dx = w->raw_dy = 0;
    for (int k = 0; k < 256; k++) { if (g_up_late[k]) { w->keys[k] = 0; g_up_late[k] = 0; } g_down_now[k] = 0; }
    w->keys[VK_CONTROL] = w->keys[VK_LCONTROL] || w->keys[VK_RCONTROL]; w->keys[VK_SHIFT] = w->keys[VK_LSHIFT] || w->keys[VK_RSHIFT]; w->keys[VK_MENU] = w->keys[VK_LMENU] || w->keys[VK_RMENU];
    SDL_Event e;
    while (SDL_PollEvent(&e)) switch (touch_event(&e, w->width, w->height), e.type) {
    case SDL_QUIT: w->quit = 1; break;
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) drawable(w);
        else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) w->focused = 1;
        else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { w->focused = 0; memset(w->keys, 0, sizeof w->keys); w->mouse_right = 0; }
        else if (e.window.event == SDL_WINDOWEVENT_CLOSE) w->quit = 1;
        break;
    case SDL_KEYDOWN: case SDL_KEYUP: {
        int vk = vk_of(&e.key.keysym), down = e.type == SDL_KEYDOWN;
        { static int log = -1; if (log < 0) log = getenv("WOODY_KEYLOG") != NULL; if (log) printf("key %s scancode %d sym 0x%x -> vk 0x%02x%s\n", down ? "down" : "up", e.key.keysym.scancode, (unsigned)e.key.keysym.sym, vk, e.key.repeat ? " (repeat)" : ""); }
        if (!vk || e.key.repeat) break;
        if (!down && g_down_now[vk]) { g_up_late[vk] = 1; break; }        /* the game polls once per frame: let it see the tap */
        if (down) { g_down_now[vk] = 1; g_up_late[vk] = 0; }
        w->keys[vk] = down;
        if (vk == VK_LCONTROL || vk == VK_RCONTROL) w->keys[VK_CONTROL] = w->keys[VK_LCONTROL] || w->keys[VK_RCONTROL];
        if (vk == VK_LSHIFT || vk == VK_RSHIFT) w->keys[VK_SHIFT] = w->keys[VK_LSHIFT] || w->keys[VK_RSHIFT];
        if (vk == VK_LMENU || vk == VK_RMENU) w->keys[VK_MENU] = w->keys[VK_LMENU] || w->keys[VK_RMENU];
        break; }
    case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_RIGHT) w->mouse_right = 1; break;
    case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_RIGHT) w->mouse_right = 0; break;
    case SDL_MOUSEMOTION:
        if (w->mouse_right) { w->mouse_dx += e.motion.xrel; w->mouse_dy += e.motion.yrel; }
        if (w->focused) { w->raw_dx += e.motion.xrel; w->raw_dy += e.motion.yrel; }
        break;
    case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED: case SDL_JOYDEVICEADDED: case SDL_JOYDEVICEREMOVED: w->dev_changes++; break;
    }
}
void win_swap(Window *w)
{
    touch_draw(w->width, w->height); SDL_GL_SwapWindow((SDL_Window *)w->hwnd);
#ifdef __ANDROID__
    fflush(stdout);                                                    /* Android's stdio holds even a line-buffered stdout back */
#endif
}
void win_mode(Window *w, int width, int height, int full)
{
    SDL_Window *sw = (SDL_Window *)w->hwnd;
#ifdef __ANDROID__
    (void)width; (void)height; (void)full; (void)sw; drawable(w); return;   /* always the whole screen */
#endif
    if (full) { SDL_SetWindowFullscreen(sw, SDL_WINDOW_FULLSCREEN_DESKTOP); drawable(w); return; }
    SDL_SetWindowFullscreen(sw, 0);
    SDL_Rect r; int d = SDL_GetWindowDisplayIndex(sw);
    if (!SDL_GetDisplayUsableBounds(d < 0 ? 0 : d, &r) && (width > r.w || height > r.h)) {
        float s = (float)r.w / width < (float)r.h / height ? (float)r.w / width : (float)r.h / height;
        printf("window %dx%d does not fit the screen: %dx%d\n", width, height, (int)(width * s), (int)(height * s)); width = (int)(width * s); height = (int)(height * s);
    }
    SDL_SetWindowSize(sw, width, height); SDL_SetWindowPosition(sw, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    w->width = width; w->height = height; drawable(w);
}
int win_vsync(int interval) { return SDL_GL_SetSwapInterval(interval) == 0 ? 0 : -1; }
void win_title(Window *w, const char *title) { SDL_SetWindowTitle((SDL_Window *)w->hwnd, title); }
void win_close(Window *w) { SDL_GL_DeleteContext((SDL_GLContext)w->hglrc); SDL_DestroyWindow((SDL_Window *)w->hwnd); SDL_QuitSubSystem(SDL_INIT_VIDEO); }
double win_time(void) { static double f; if (!f) f = (double)SDL_GetPerformanceFrequency(); return (double)SDL_GetPerformanceCounter() / f; }
#endif
