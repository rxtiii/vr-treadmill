// VR Treadmill
// Turns movement from ONE chosen mouse (the treadmill) into walking:
//   - SteamVR (default): blended into your real VR controller's thumbstick by the bundled
//     SteamVR driver, like KAT Gateway does, so games need no setup
//   - a held keyboard key (W), or
//   - a virtual Xbox controller's left stick (needs ViGEmBus)
// The treadmill mouse is read with plain Windows Raw Input (no driver), or optionally with
// the Interception driver, which also hides it from the cursor.

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include "ViGEm/Client.h"
#include "shared.h"

// ---------------- Interception API (loaded at runtime, optional) ----------------
typedef void *ICtx;
typedef int IDev;
typedef unsigned short IFilter;
typedef int (*IPred)(IDev);
typedef struct { unsigned short state, flags; short rolling; int x, y; unsigned int information; } IMouseStroke;
typedef char IStroke[sizeof(IMouseStroke)];
#define I_MOUSE_MOVE_ABSOLUTE 0x001
#define I_FILTER_MOUSE_ALL 0xFFFF

static ICtx (*ic_create)(void);
static void (*ic_destroy)(ICtx);
static void (*ic_set_filter)(ICtx, IPred, IFilter);
static IDev (*ic_wait_timeout)(ICtx, unsigned long);
static int  (*ic_send)(ICtx, IDev, const IStroke *, unsigned int);
static int  (*ic_receive)(ICtx, IDev, IStroke *, unsigned int);
static unsigned int (*ic_hwid)(ICtx, IDev, void *, unsigned int);
static int is_mouse(IDev d) { return d > 10 && d <= 20; }

// ---------------- Settings ----------------
enum { OUT_STEAMVR = 0, OUT_KEYBOARD = 1, OUT_PAD = 2 };
enum { IN_RAW = 0, IN_INTERCEPTION = 1 };

static wchar_t g_ini[MAX_PATH], g_dir[MAX_PATH];
static wchar_t g_hwid[512];          // Interception hardware id of the treadmill mouse
static wchar_t g_rawName[512];       // Raw Input device path of the treadmill mouse
static volatile LONG g_dev = 0;      // Interception device number (11..20), 0 = none
static volatile LONG g_output = OUT_STEAMVR;
static volatile LONG g_hand = 1;     // SteamVR: 1 left controller, 2 right
static volatile LONG g_inputMode = -1; // -1 = auto
static volatile LONG g_axis = 0;     // 0 = horizontal X, 1 = vertical Y, 2 = both
static volatile LONG g_mode = 0;     // 0 = any direction -> forward, 1 = forward / backward
static volatile LONG g_invert = 0;
static volatile LONG g_prop = 1;     // push further the faster you walk
static volatile LONG g_topSpeed = 300;
static volatile LONG g_threshold = 15;
static volatile LONG g_releaseMs = 250;
static volatile LONG g_key1 = L'W', g_key2 = L'S';
static volatile LONG g_block = 1;
static volatile LONG g_enabled = 1;

static volatile LONG g_detecting = 0, g_quit = 0, g_calibrating = 0;
static volatile LONG g_feedX = 0, g_feedY = 0;   // raw-input movement waiting for the worker
static volatile LONG g_rawFound = 0;             // treadmill seen via raw input
static HANDLE g_thread;
static HWND g_hwnd;
static int g_icOk = 0, g_padDriverOk = 0, g_calCount = 0;
static volatile TreadmillShared *g_shm;

#define WM_APP_STATUS   (WM_APP + 1)
#define WM_APP_DETECTED (WM_APP + 2)
#define WM_APP_FOUND    (WM_APP + 3)
#define WM_APP_PAD      (WM_APP + 4)
#define WM_APP_CALIB    (WM_APP + 5)

enum { PAD_OFF = 0, PAD_CONNECTED, PAD_NODRIVER, PAD_ERROR };

enum { ID_INSTALLVR = 100, ID_INSTALL, ID_INSTALLPAD, ID_DETECT, ID_OUTPUT, ID_HAND, ID_INPUT, ID_AXIS, ID_MODE,
       ID_INVERT, ID_PROP, ID_THRESH, ID_RELEASE, ID_KEY1, ID_KEY2, ID_BLOCK, ID_ENABLED, ID_TOPSPEED, ID_CALIB,
       ID_TIMER_DETECT, ID_TIMER_CALIB, ID_TIMER_VR };

static HWND hVrDrv, hInstallVr, hDrv, hInstall, hPadDrv, hInstallPad, hDevLbl, hDetect, hOutput, hHand, hInput,
            hAxis, hMode, hInvert, hProp, hTop, hCalib, hThresh, hRelease, hKey1, hKey2, hBlock, hEnabled, hBar, hLive;

static int UseInterception(void) { return g_inputMode == IN_INTERCEPTION && g_icOk; }

static void SaveSettings(void) {
    wchar_t b[32];
    WritePrivateProfileStringW(L"Settings", L"Hwid", g_hwid, g_ini);
    WritePrivateProfileStringW(L"Settings", L"RawName", g_rawName, g_ini);
#define SAVEI(name, v) swprintf(b, 32, L"%ld", (long)(v)); WritePrivateProfileStringW(L"Settings", name, b, g_ini)
    SAVEI(L"Device", g_dev); SAVEI(L"OutputMode", g_output); SAVEI(L"Hand", g_hand); SAVEI(L"InputMode", g_inputMode);
    SAVEI(L"Axis", g_axis); SAVEI(L"Mode", g_mode); SAVEI(L"Invert", g_invert); SAVEI(L"Proportional", g_prop);
    SAVEI(L"TopSpeed", g_topSpeed); SAVEI(L"Threshold", g_threshold); SAVEI(L"ReleaseMs", g_releaseMs);
    SAVEI(L"Key1", g_key1); SAVEI(L"Key2", g_key2); SAVEI(L"Block", g_block); SAVEI(L"Enabled", g_enabled);
}
static void LoadSettings(void) {
    GetPrivateProfileStringW(L"Settings", L"Hwid", L"", g_hwid, 512, g_ini);
    GetPrivateProfileStringW(L"Settings", L"RawName", L"", g_rawName, 512, g_ini);
#define LOADI(name, v) v = GetPrivateProfileIntW(L"Settings", name, v, g_ini)
    LOADI(L"Device", g_dev); LOADI(L"Hand", g_hand); LOADI(L"InputMode", g_inputMode);
    LOADI(L"Axis", g_axis); LOADI(L"Mode", g_mode); LOADI(L"Invert", g_invert); LOADI(L"Proportional", g_prop);
    LOADI(L"TopSpeed", g_topSpeed); LOADI(L"Threshold", g_threshold); LOADI(L"ReleaseMs", g_releaseMs);
    LOADI(L"Key1", g_key1); LOADI(L"Key2", g_key2); LOADI(L"Block", g_block); LOADI(L"Enabled", g_enabled);
    // v1.0/1.1 stored "Output" as 0 = keyboard, 1 = controller
    int oldOut = GetPrivateProfileIntW(L"Settings", L"Output", -1, g_ini);
    int newOut = GetPrivateProfileIntW(L"Settings", L"OutputMode", -1, g_ini);
    g_output = newOut >= 0 ? newOut : oldOut == 0 ? OUT_KEYBOARD : oldOut == 1 ? OUT_PAD : OUT_STEAMVR;
    if (g_output < 0 || g_output > 2) g_output = OUT_STEAMVR;
    if (g_hand != 2) g_hand = 1;
    if (g_inputMode < -1 || g_inputMode > 1) g_inputMode = -1;
    if (g_axis < 0 || g_axis > 2) g_axis = 0;
    if (g_mode < 0 || g_mode > 1) g_mode = 0;
    if (g_threshold < 1) g_threshold = 1;
    if (g_releaseMs < 20) g_releaseMs = 20;
    if (g_topSpeed < 10) g_topSpeed = 10;
}

// ---------------- Shared memory for the SteamVR driver ----------------
static void OpenShared(void) {
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(TreadmillShared), TREADMILL_SHM_NAME);
    if (!m) return;
    g_shm = (volatile TreadmillShared *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TreadmillShared));
}
static void WriteShared(float y) {
    if (!g_shm) return;
    g_shm->x = 0;
    g_shm->y = y;
    g_shm->hand = (uint32_t)g_hand;
    g_shm->inject = 1;
    g_shm->magic = TREADMILL_SHM_MAGIC;
    g_shm->heartbeat = GetTickCount();
}

// ---------------- Key output ----------------
static void SendKey(wchar_t c, int down) {
    SHORT vk = VkKeyScanW(c);
    if (vk == -1) return;
    WORD sc = (WORD)MapVirtualKeyW(vk & 0xFF, MAPVK_VK_TO_VSC);
    if (!sc) return;
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = sc;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

// ---------------- Virtual Xbox controller (optional) ----------------
static PVIGEM_CLIENT g_vc;
static PVIGEM_TARGET g_pad;

static int PadConnect(void) {
    if (g_pad) return PAD_CONNECTED;
    if (!g_vc) {
        g_vc = vigem_alloc();
        if (!g_vc) return PAD_ERROR;
        VIGEM_ERROR e = vigem_connect(g_vc);
        if (!VIGEM_SUCCESS(e)) {
            vigem_free(g_vc); g_vc = NULL;
            return e == VIGEM_ERROR_BUS_NOT_FOUND ? PAD_NODRIVER : PAD_ERROR;
        }
    }
    g_pad = vigem_target_x360_alloc();
    if (!VIGEM_SUCCESS(vigem_target_add(g_vc, g_pad))) {
        vigem_target_free(g_pad); g_pad = NULL;
        return PAD_ERROR;
    }
    return PAD_CONNECTED;
}
static void PadDisconnect(void) {
    if (g_pad) { vigem_target_remove(g_vc, g_pad); vigem_target_free(g_pad); g_pad = NULL; }
    if (g_vc) { vigem_disconnect(g_vc); vigem_free(g_vc); g_vc = NULL; }
}
static void PadSetStickY(SHORT y) {
    if (!g_pad) return;
    XUSB_REPORT r;
    XUSB_REPORT_INIT(&r);
    r.sThumbLY = y;
    vigem_target_x360_update(g_vc, g_pad, r);
}

// ---------------- Walking logic (worker thread only) ----------------
static long w_winAbs, w_winSigned, w_tickAbs;
static DWORD w_lastActive;
static int w_active, w_dir = 1;

static void FeedMovement(int x, int y, DWORD now) {
    int v = g_axis == 0 ? x : g_axis == 1 ? y : (abs(x) > abs(y) ? x : y);
    int a = g_axis == 2 ? abs(x) + abs(y) : abs(v);
    w_winAbs += a;
    w_tickAbs += a;
    w_winSigned += v;
    if (g_enabled && w_winAbs >= g_threshold) {
        w_lastActive = now;
        w_active = 1;
        if (g_mode == 1) {
            long sgn = g_invert ? -w_winSigned : w_winSigned;
            w_dir = sgn >= 0 ? 1 : -1;
        } else w_dir = 1;
    }
}

static ICtx IcOpen(void) {
    ICtx ctx = ic_create();
    if (!ctx) return NULL;
    ic_set_filter(ctx, is_mouse, I_FILTER_MOUSE_ALL);
    if (g_hwid[0]) {         // re-find the saved treadmill (device numbers can change after replugging)
        LONG found = 0;
        for (IDev d = 11; d <= 20; d++) {
            wchar_t h[512] = {0};
            if (ic_hwid(ctx, d, h, sizeof(h) - 2) && wcscmp(h, g_hwid) == 0) {
                if (d == g_dev) { found = d; break; }
                if (!found) found = d;
            }
        }
        InterlockedExchange(&g_dev, found);
    }
    PostMessageW(g_hwnd, WM_APP_FOUND, 0, 0);
    return ctx;
}

static DWORD WINAPI Worker(LPVOID p) {
    (void)p;
    ICtx ctx = NULL;
    long detAccum[21] = {0};
    double ema = 0, calMax = 0;
    DWORD tickStart = GetTickCount(), calEnd = 0, winStart = GetTickCount(), lastPadTry = 0;
    int wasCalibrating = 0, padStatus = -1;
    wchar_t held = 0;          // keyboard: key currently held
    SHORT stick = 0;           // Xbox controller: stick Y sent
    float vrY = 0;             // SteamVR: value sent
    LONG curOutput = -1;

    while (!g_quit) {
        // ---- input source: open/close Interception as the setting changes ----
        if (UseInterception() && !ctx) ctx = IcOpen();
        if (!UseInterception() && ctx) { ic_destroy(ctx); ctx = NULL; PostMessageW(g_hwnd, WM_APP_FOUND, 0, 0); }

        // ---- switch outputs cleanly ----
        LONG out = g_output;
        if (out != curOutput) {
            if (held) { SendKey(held, 0); held = 0; }
            if (stick) { PadSetStickY(0); stick = 0; }
            if (out != OUT_PAD) { PadDisconnect(); padStatus = PAD_OFF; PostMessageW(g_hwnd, WM_APP_PAD, PAD_OFF, 0); }
            vrY = 0;
            curOutput = out;
            lastPadTry = 0;
        }
        if (out == OUT_PAD && !g_pad && (lastPadTry == 0 || GetTickCount() - lastPadTry > 3000)) {
            lastPadTry = GetTickCount();
            int st = PadConnect();
            if (st != padStatus) { padStatus = st; PostMessageW(g_hwnd, WM_APP_PAD, st, 0); }
        }

        // ---- read movement ----
        DWORD now;
        if (ctx) {
            IDev d = ic_wait_timeout(ctx, 5);
            now = GetTickCount();
            if (d > 0) {
                IStroke s;
                if (ic_receive(ctx, d, &s, 1) > 0) {
                    IMouseStroke *m = (IMouseStroke *)&s;
                    int rel = !(m->flags & I_MOUSE_MOVE_ABSOLUTE);
                    if (g_detecting) {
                        if (rel && d <= 20) detAccum[d] += labs(m->x) + labs(m->y);
                        if (d <= 20 && detAccum[d] >= 400) {
                            wchar_t h[512] = {0};
                            ic_hwid(ctx, d, h, sizeof(h) - 2);
                            lstrcpynW(g_hwid, h, 512);
                            InterlockedExchange(&g_dev, d);
                            InterlockedExchange(&g_detecting, 0);
                            memset(detAccum, 0, sizeof(detAccum));
                            PostMessageW(g_hwnd, WM_APP_DETECTED, 0, 0);
                        }
                        ic_send(ctx, d, &s, 1);
                    } else if (d == g_dev) {
                        if (rel) FeedMovement(m->x, m->y, now);
                        if (!g_block) ic_send(ctx, d, &s, 1);   // otherwise swallowed: no cursor, no clicks
                    } else {
                        ic_send(ctx, d, &s, 1);                 // every other mouse: untouched
                    }
                }
            }
        } else {
            Sleep(4);
            now = GetTickCount();
            LONG fx = InterlockedExchange(&g_feedX, 0), fy = InterlockedExchange(&g_feedY, 0);
            if (fx || fy) FeedMovement(fx, fy, now);
        }

        if (w_active && (!g_enabled || now - w_lastActive > (DWORD)g_releaseMs)) w_active = 0;

        // ---- smoothed speed, sampled every 50 ms ----
        if (now - tickStart >= 50) {
            double sample = w_tickAbs * 100.0 / (double)(now - tickStart);
            ema = ema * 0.6 + sample * 0.4;
            w_tickAbs = 0;
            tickStart = now;
            if (g_calibrating) {
                if (!wasCalibrating) { wasCalibrating = 1; calMax = 0; calEnd = now + 5000; }
                if (ema > calMax) calMax = ema;
                if ((LONG)(now - calEnd) >= 0) {
                    wasCalibrating = 0;
                    InterlockedExchange(&g_calibrating, 0);
                    PostMessageW(g_hwnd, WM_APP_CALIB, (WPARAM)(calMax * 0.9), 0);
                }
            } else wasCalibrating = 0;
        }

        // ---- how far to push (SteamVR and controller) ----
        double amt = 0;
        if (w_active) {
            amt = 1.0;
            if (g_prop) {
                // 25% at walking start (clears typical stick dead zones) -> 100% at calibrated top speed
                double lo = (double)g_threshold, hi = (double)g_topSpeed;
                double frac = hi > lo ? (ema - lo) / (hi - lo) : 1.0;
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                amt = 0.25 + 0.75 * frac;
            }
            amt *= w_dir;
        }

        // ---- apply output ----
        if (out == OUT_KEYBOARD) {
            wchar_t want = w_active ? (wchar_t)(w_dir > 0 ? g_key1 : g_key2) : 0;
            if (want != held) {
                if (held) SendKey(held, 0);
                if (want) SendKey(want, 1);
                held = want;
            }
        } else if (out == OUT_PAD) {
            SHORT want = (SHORT)(amt * 32767);
            if (want != stick) { PadSetStickY(want); stick = want; }
        } else {
            vrY = (float)amt;
        }
        WriteShared(out == OUT_STEAMVR ? vrY : 0.f);   // also keeps the driver's heartbeat alive

        if (now - winStart >= 100) {
            w_winAbs = w_winSigned = 0;
            winStart = now;
            LPARAM info;
            if (out == OUT_KEYBOARD) info = (LPARAM)held;
            else info = (LPARAM)(0x10000 | ((SHORT)(amt * 32767) & 0xFFFF));
            PostMessageW(g_hwnd, WM_APP_STATUS, (WPARAM)(ema + 0.5), info);
        }
    }
    if (held) SendKey(held, 0);
    if (stick) PadSetStickY(0);
    if (g_shm) { g_shm->y = 0; g_shm->heartbeat = 0; }
    PadDisconnect();
    if (ctx) ic_destroy(ctx);
    return 0;
}

// ---------------- Raw Input (no driver needed) ----------------
typedef struct { HANDLE h; int isTreadmill; long detAccum; } RawDev;
static RawDev g_rawDevs[32];
static int g_numRawDevs;

static void RawDevName(HANDLE h, wchar_t *out, UINT cap) {
    out[0] = 0;
    UINT n = cap;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, out, &n) == (UINT)-1) out[0] = 0;
}
static RawDev *RawLookup(HANDLE h) {
    for (int i = 0; i < g_numRawDevs; i++) if (g_rawDevs[i].h == h) return &g_rawDevs[i];
    if (g_numRawDevs >= 32) return NULL;
    RawDev *d = &g_rawDevs[g_numRawDevs++];
    d->h = h; d->detAccum = 0;
    wchar_t name[512];
    RawDevName(h, name, 512);
    d->isTreadmill = g_rawName[0] && _wcsicmp(name, g_rawName) == 0;
    return d;
}
static void RawResetCache(void) { g_numRawDevs = 0; }

static void OnRawInput(HRAWINPUT hri) {
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof(buf);
    if (GetRawInputData(hri, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    RAWINPUT *ri = (RAWINPUT *)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE || !ri->header.hDevice) return;   // hDevice 0 = injected input
    if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return;
    int dx = ri->data.mouse.lLastX, dy = ri->data.mouse.lLastY;
    RawDev *d = RawLookup(ri->header.hDevice);
    if (!d) return;
    if (g_detecting && !UseInterception()) {
        d->detAccum += labs(dx) + labs(dy);
        if (d->detAccum >= 400) {
            RawDevName(d->h, g_rawName, 512);
            for (int i = 0; i < g_numRawDevs; i++) { g_rawDevs[i].isTreadmill = 0; g_rawDevs[i].detAccum = 0; }
            d->isTreadmill = 1;
            InterlockedExchange(&g_rawFound, 1);
            InterlockedExchange(&g_detecting, 0);
            PostMessageW(g_hwnd, WM_APP_DETECTED, 0, 0);
        }
        return;
    }
    if (d->isTreadmill) {
        if (!g_rawFound) { InterlockedExchange(&g_rawFound, 1); PostMessageW(g_hwnd, WM_APP_FOUND, 0, 0); }
        if (!UseInterception()) {
            InterlockedExchangeAdd(&g_feedX, dx);
            InterlockedExchangeAdd(&g_feedY, dy);
        }
    }
}

// Is the saved treadmill mouse plugged in right now? (Raw Input mode)
static int RawTreadmillPresent(void) {
    if (!g_rawName[0]) return 0;
    UINT n = 0;
    GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    if (!n) return 0;
    RAWINPUTDEVICELIST *list = (RAWINPUTDEVICELIST *)calloc(n, sizeof(*list));
    int found = 0;
    if (GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST)) != (UINT)-1) {
        for (UINT i = 0; i < n && !found; i++) {
            if (list[i].dwType != RIM_TYPEMOUSE) continue;
            wchar_t name[512];
            RawDevName(list[i].hDevice, name, 512);
            if (_wcsicmp(name, g_rawName) == 0) found = 1;
        }
    }
    free(list);
    return found;
}

// ---------------- SteamVR driver registration (vrpathreg, no admin needed) ----------------
static int ReadVrPathFile(char *buf, DWORD cap) {
    wchar_t path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) return 0;
    wcscat(path, L"\\openvr\\openvrpaths.vrpath");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    ReadFile(f, buf, cap - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    return got > 0;
}

static int FindVrPathReg(wchar_t *out) {
    static char buf[65536];
    if (!ReadVrPathFile(buf, sizeof(buf))) return 0;
    char *p = strstr(buf, "\"runtime\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;
    char rt[MAX_PATH * 2]; int n = 0;
    while (*p && *p != '"' && n < (int)sizeof(rt) - 1) {
        if (*p == '\\' && p[1]) p++;
        rt[n++] = *p++;
    }
    rt[n] = 0;
    wchar_t wrt[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, rt, -1, wrt, MAX_PATH)) return 0;
    swprintf(out, MAX_PATH, L"%ls\\bin\\win64\\vrpathreg.exe", wrt);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

static void DriverDir(wchar_t *out) { swprintf(out, MAX_PATH, L"%ls\\steamvr_driver\\00vrtreadmill", g_dir); }

static int VrDriverRegistered(void) {
    static char buf[65536];
    if (!ReadVrPathFile(buf, sizeof(buf))) return 0;
    char *ext = strstr(buf, "\"external_drivers\"");
    return ext && strstr(ext, "00vrtreadmill") != NULL;
}

static int RunVrPathReg(const wchar_t *verb) {
    wchar_t exe[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH * 3];
    if (!FindVrPathReg(exe)) return -1;
    DriverDir(dir);
    swprintf(cmd, MAX_PATH * 3, L"\"%ls\" %ls \"%ls\"", exe, verb, dir);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -2;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

// ---------------- GUI ----------------
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static HFONT g_font, g_bold;
static int g_vrState = -1, g_vrHooked = -1;   // for repainting the status only when it changes

static HWND Mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void UpdateDevLabel(void) {
    wchar_t t[700];
    if (g_detecting)
        lstrcpyW(t, L"Move ONLY the treadmill mouse now...\n(keep your hands off your other mouse)");
    else if (UseInterception()) {
        if (g_dev && g_hwid[0]) swprintf(t, 700, L"Selected (driver): device #%ld\n%ls", (long)g_dev - 10, g_hwid);
        else if (g_hwid[0]) lstrcpyW(t, L"Saved treadmill mouse not plugged in.\nPlug it in, or click Detect.");
        else lstrcpyW(t, L"No treadmill mouse selected.\nClick Detect, then move the treadmill.");
    } else {
        if (g_rawName[0] && (g_rawFound || RawTreadmillPresent())) {
            const wchar_t *n = g_rawName;
            if (!wcsncmp(n, L"\\\\?\\", 4)) n += 4;
            swprintf(t, 700, L"Selected:\n%.70ls", n);
        }
        else if (g_rawName[0]) lstrcpyW(t, L"Saved treadmill mouse not plugged in.\nPlug it in, or click Detect.");
        else lstrcpyW(t, L"No treadmill mouse selected.\nClick Detect, then move the treadmill.");
    }
    SetWindowTextW(hDevLbl, t);
}

static void SetPadStatus(int st) {
    const wchar_t *t;
    switch (st) {
    case PAD_CONNECTED: t = L"Virtual Xbox controller connected \x2714"; g_padDriverOk = 1; break;
    case PAD_NODRIVER:  t = L"Not installed"; g_padDriverOk = 0; break;
    case PAD_ERROR:     t = L"Couldn't create controller (retrying)"; break;
    default:            t = g_padDriverOk ? L"Installed" : L"Optional - only for Xbox output"; break;
    }
    SetWindowTextW(hPadDrv, t);
    EnableWindow(hInstallPad, !g_padDriverOk);
}

// 0 not installed, 1 installed (SteamVR not running), 2 running in SteamVR
static int VrState(void) {
    if (g_shm && g_shm->driverSeen && GetTickCount() - g_shm->driverSeen < 2000) return 2;
    return VrDriverRegistered() ? 1 : 0;
}
static void UpdateVrStatus(int force) {
    int st = VrState();
    int hooked = (st == 2 && g_shm) ? (int)g_shm->hooked : 0;
    if (!force && st == g_vrState && hooked == g_vrHooked) return;
    g_vrState = st; g_vrHooked = hooked;
    wchar_t t[128];
    if (st == 2) {
        if (hooked > 0) swprintf(t, 128, L"Running in SteamVR \x2714 (%d controller%ls)", hooked, hooked == 1 ? L"" : L"s");
        else lstrcpyW(t, L"Running - turn controllers off/on");
    }
    else if (st == 1) lstrcpyW(t, L"Installed - start/restart SteamVR");
    else lstrcpyW(t, L"Not installed");
    SetWindowTextW(hVrDrv, t);
    SetWindowTextW(hInstallVr, st ? L"Remove" : L"Install");
    InvalidateRect(hVrDrv, NULL, TRUE);
}

static int GetInt(HWND e, int def) {
    wchar_t b[32]; GetWindowTextW(e, b, 32);
    return b[0] ? _wtoi(b) : def;
}
static void SetInt(HWND e, int v) { wchar_t b[32]; swprintf(b, 32, L"%d", v); SetWindowTextW(e, b); }
static void SetChar(HWND e, wchar_t c) { wchar_t b[2] = { c, 0 }; SetWindowTextW(e, b); }

static void RunInstaller(const wchar_t *file, const wchar_t *args, const wchar_t *after) {
    wchar_t exe[MAX_PATH];
    swprintf(exe, MAX_PATH, L"%ls\\driver\\%ls", g_dir, file);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwnd, L"Couldn't find the installer in the \"driver\" folder next to this program.\n"
                            L"Make sure you extracted the whole zip folder.", L"Install driver", MB_ICONERROR);
        return;
    }
    if ((INT_PTR)ShellExecuteW(g_hwnd, L"runas", exe, args, g_dir, SW_SHOWNORMAL) > 32)
        MessageBoxW(g_hwnd, after, L"Install driver", MB_ICONINFORMATION);
}

static void InstallVrDriver(void) {
    int st = VrState();
    if (st) {
        if (MessageBoxW(g_hwnd, L"Remove the VR Treadmill driver from SteamVR?", L"SteamVR driver", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        int r = RunVrPathReg(L"removedriver");
        MessageBoxW(g_hwnd, r == 0 ? L"Removed. Restart SteamVR to finish." : L"Couldn't run SteamVR's vrpathreg tool.",
                    L"SteamVR driver", r == 0 ? MB_ICONINFORMATION : MB_ICONERROR);
        UpdateVrStatus(1);
        return;
    }
    wchar_t dir[MAX_PATH], manifest[MAX_PATH];
    DriverDir(dir);
    swprintf(manifest, MAX_PATH, L"%ls\\driver.vrdrivermanifest", dir);
    if (GetFileAttributesW(manifest) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwnd, L"Couldn't find the \"steamvr_driver\" folder next to this program.\n"
                            L"Make sure you extracted the whole zip folder.", L"SteamVR driver", MB_ICONERROR);
        return;
    }
    if (wcsstr(g_dir, L"\\Temp\\") || wcsstr(g_dir, L"\\AppData\\Local\\Temp")) {
        MessageBoxW(g_hwnd, L"It looks like this is running from inside the zip.\n"
                            L"Extract the whole folder somewhere permanent first (e.g. Documents), then install.",
                    L"SteamVR driver", MB_ICONWARNING);
        return;
    }
    if (MessageBoxW(g_hwnd, L"This adds the VR Treadmill driver to SteamVR.\n\n"
                            L"SteamVR will load it from this folder, so keep the folder where it is. "
                            L"(If you move it later, click Remove first, then install again.)",
                    L"SteamVR driver", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;
    int r = RunVrPathReg(L"adddriver");
    if (r == -1)
        MessageBoxW(g_hwnd, L"Couldn't find SteamVR. Install SteamVR and run it once, then try again.", L"SteamVR driver", MB_ICONERROR);
    else if (r != 0)
        MessageBoxW(g_hwnd, L"SteamVR's vrpathreg tool reported an error. Close SteamVR and try again.", L"SteamVR driver", MB_ICONERROR);
    else
        MessageBoxW(g_hwnd, L"Installed! Restart SteamVR.\n\n"
                            L"If SteamVR says an add-on was blocked, enable \"00vrtreadmill\" in "
                            L"SteamVR Settings > Startup / Shutdown > Manage Add-ons.", L"SteamVR driver", MB_ICONINFORMATION);
    UpdateVrStatus(1);
}

static void UpdateEnables(void) {
    int vr = g_output == OUT_STEAMVR, pad = g_output == OUT_PAD, kb = g_output == OUT_KEYBOARD;
    EnableWindow(hHand, vr);
    EnableWindow(hInvert, g_mode == 1);
    EnableWindow(hKey1, kb);
    EnableWindow(hKey2, kb && g_mode == 1);
    EnableWindow(hProp, !kb);
    EnableWindow(hTop, !kb && g_prop);
    int haveMouse = UseInterception() ? (g_dev != 0) : (g_rawName[0] != 0);
    EnableWindow(hCalib, !kb && g_prop && haveMouse);
    EnableWindow(hBlock, UseInterception());
    (void)pad;
}

#define RX 452   /* right column x */
static void BuildUI(void) {
    // ---------- left column ----------
    Mk(L"BUTTON", L"Drivers", BS_GROUPBOX, 8, 6, 436, 116, -1);
    Mk(L"STATIC", L"SteamVR driver:", SS_LEFT, 18, 30, 108, 20, -1);
    hVrDrv = Mk(L"STATIC", L"", SS_LEFT, 128, 30, 214, 20, -1);
    hInstallVr = Mk(L"BUTTON", L"Install", BS_PUSHBUTTON, 346, 25, 88, 28, ID_INSTALLVR);
    SendMessageW(hInstallVr, WM_SETFONT, (WPARAM)g_bold, TRUE);
    Mk(L"STATIC", L"Mouse driver:", SS_LEFT, 18, 62, 108, 20, -1);
    hDrv = Mk(L"STATIC", L"", SS_LEFT, 128, 62, 214, 20, -1);
    hInstall = Mk(L"BUTTON", L"Install", BS_PUSHBUTTON, 346, 57, 88, 28, ID_INSTALL);
    Mk(L"STATIC", L"Controller driver:", SS_LEFT, 18, 94, 108, 20, -1);
    hPadDrv = Mk(L"STATIC", L"", SS_LEFT, 128, 94, 214, 20, -1);
    hInstallPad = Mk(L"BUTTON", L"Install", BS_PUSHBUTTON, 346, 89, 88, 28, ID_INSTALLPAD);

    Mk(L"BUTTON", L"1. Treadmill mouse", BS_GROUPBOX, 8, 128, 436, 86, -1);
    hDevLbl = Mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 18, 150, 314, 56, -1);
    hDetect = Mk(L"BUTTON", L"Detect", BS_PUSHBUTTON, 340, 156, 94, 32, ID_DETECT);
    SendMessageW(hDetect, WM_SETFONT, (WPARAM)g_bold, TRUE);

    hBlock = Mk(L"BUTTON", L"Hide treadmill mouse from the cursor (mouse driver only)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 14, 222, 428, 22, ID_BLOCK);
    hEnabled = Mk(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, 14, 246, 200, 22, ID_ENABLED);
    SendMessageW(hEnabled, WM_SETFONT, (WPARAM)g_bold, TRUE);

    Mk(L"BUTTON", L"Live", BS_GROUPBOX, 8, 274, 436, 62, -1);
    hBar = Mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, 18, 300, 250, 22, -1);
    SendMessageW(hBar, PBM_SETRANGE32, 0, 100);
    hLive = Mk(L"STATIC", L"Idle", SS_LEFT, 278, 301, 160, 22, -1);
    SendMessageW(hLive, WM_SETFONT, (WPARAM)g_bold, TRUE);

    // ---------- right column ----------
    Mk(L"BUTTON", L"2. Settings", BS_GROUPBOX, RX, 6, 436, 330, -1);
    int lx = RX + 10, cx = RX + 140, cw = 286;
    Mk(L"STATIC", L"Output:", SS_LEFT, lx, 30, 128, 20, -1);
    hOutput = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, 26, cw, 120, ID_OUTPUT);
    SendMessageW(hOutput, CB_ADDSTRING, 0, (LPARAM)L"SteamVR (controller thumbstick)");
    SendMessageW(hOutput, CB_ADDSTRING, 0, (LPARAM)L"Keyboard key (W)");
    SendMessageW(hOutput, CB_ADDSTRING, 0, (LPARAM)L"Xbox controller left stick");

    Mk(L"STATIC", L"Walk with:", SS_LEFT, lx, 60, 128, 20, -1);
    hHand = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, 56, cw, 120, ID_HAND);
    SendMessageW(hHand, CB_ADDSTRING, 0, (LPARAM)L"Left controller stick (most games)");
    SendMessageW(hHand, CB_ADDSTRING, 0, (LPARAM)L"Right controller stick");

    Mk(L"STATIC", L"Read mouse with:", SS_LEFT, lx, 90, 128, 20, -1);
    hInput = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, 86, cw, 120, ID_INPUT);
    SendMessageW(hInput, CB_ADDSTRING, 0, (LPARAM)L"Windows (no driver needed)");
    SendMessageW(hInput, CB_ADDSTRING, 0, (LPARAM)L"Mouse driver (also hides cursor)");

    Mk(L"STATIC", L"Movement axis:", SS_LEFT, lx, 120, 128, 20, -1);
    hAxis = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, 116, cw, 120, ID_AXIS);
    SendMessageW(hAxis, CB_ADDSTRING, 0, (LPARAM)L"Horizontal (left/right)");
    SendMessageW(hAxis, CB_ADDSTRING, 0, (LPARAM)L"Vertical (up/down)");
    SendMessageW(hAxis, CB_ADDSTRING, 0, (LPARAM)L"Both");

    Mk(L"STATIC", L"Mode:", SS_LEFT, lx, 150, 128, 20, -1);
    hMode = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, 146, cw, 120, ID_MODE);
    SendMessageW(hMode, CB_ADDSTRING, 0, (LPARAM)L"Any direction = forward");
    SendMessageW(hMode, CB_ADDSTRING, 0, (LPARAM)L"One way = forward, other way = backward");

    hInvert = Mk(L"BUTTON", L"Swap forward/backward direction", BS_AUTOCHECKBOX | WS_TABSTOP, cx, 174, cw, 22, ID_INVERT);
    hProp = Mk(L"BUTTON", L"Walk faster = move faster", BS_AUTOCHECKBOX | WS_TABSTOP, cx, 196, cw, 22, ID_PROP);

    Mk(L"STATIC", L"Top speed:", SS_LEFT, lx, 227, 128, 20, -1);
    hTop = Mk(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, cx, 224, 56, 24, ID_TOPSPEED);
    hCalib = Mk(L"BUTTON", L"Calibrate", BS_PUSHBUTTON, cx + 64, 222, 90, 28, ID_CALIB);
    Mk(L"STATIC", L"walk at full speed", SS_LEFT, cx + 162, 227, 124, 20, -1);

    Mk(L"STATIC", L"Sensitivity:", SS_LEFT, lx, 261, 128, 20, -1);
    hThresh = Mk(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, cx, 258, 56, 24, ID_THRESH);
    Mk(L"STATIC", L"lower = starts walking sooner", SS_LEFT, cx + 64, 261, 222, 20, -1);

    Mk(L"STATIC", L"Stop delay (ms):", SS_LEFT, lx, 293, 128, 20, -1);
    hRelease = Mk(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, cx, 290, 56, 24, ID_RELEASE);
    Mk(L"STATIC", L"Keys:", SS_LEFT, cx + 70, 293, 40, 20, -1);
    hKey1 = Mk(L"EDIT", L"", ES_UPPERCASE | WS_BORDER | WS_TABSTOP | ES_CENTER, cx + 112, 290, 30, 24, ID_KEY1);
    Mk(L"STATIC", L"fwd", SS_LEFT, cx + 146, 293, 30, 20, -1);
    hKey2 = Mk(L"EDIT", L"", ES_UPPERCASE | WS_BORDER | WS_TABSTOP | ES_CENTER, cx + 180, 290, 30, 24, ID_KEY2);
    Mk(L"STATIC", L"back", SS_LEFT, cx + 214, 293, 40, 20, -1);
    SendMessageW(hKey1, EM_SETLIMITTEXT, 1, 0);
    SendMessageW(hKey2, EM_SETLIMITTEXT, 1, 0);

    SendMessageW(hOutput, CB_SETCURSEL, g_output, 0);
    SendMessageW(hHand, CB_SETCURSEL, g_hand == 2 ? 1 : 0, 0);
    SendMessageW(hInput, CB_SETCURSEL, g_inputMode == IN_INTERCEPTION ? 1 : 0, 0);
    SendMessageW(hAxis, CB_SETCURSEL, g_axis, 0);
    SendMessageW(hMode, CB_SETCURSEL, g_mode, 0);
    SendMessageW(hInvert, BM_SETCHECK, g_invert ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hProp, BM_SETCHECK, g_prop ? BST_CHECKED : BST_UNCHECKED, 0);
    SetInt(hTop, g_topSpeed);
    SetInt(hThresh, g_threshold);
    SetInt(hRelease, g_releaseMs);
    SetChar(hKey1, (wchar_t)g_key1);
    SetChar(hKey2, (wchar_t)g_key2);
    SendMessageW(hBlock, BM_SETCHECK, g_block ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hEnabled, BM_SETCHECK, g_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void ReadControls(void) {
    g_output = (LONG)SendMessageW(hOutput, CB_GETCURSEL, 0, 0);
    g_hand = SendMessageW(hHand, CB_GETCURSEL, 0, 0) == 1 ? 2 : 1;
    g_inputMode = SendMessageW(hInput, CB_GETCURSEL, 0, 0) == 1 ? IN_INTERCEPTION : IN_RAW;
    g_axis = (LONG)SendMessageW(hAxis, CB_GETCURSEL, 0, 0);
    g_mode = (LONG)SendMessageW(hMode, CB_GETCURSEL, 0, 0);
    g_invert = SendMessageW(hInvert, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_prop = SendMessageW(hProp, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int t = GetInt(hThresh, 15); g_threshold = t < 1 ? 1 : t;
    int ts = GetInt(hTop, 300); g_topSpeed = ts < 10 ? 10 : ts;
    int r = GetInt(hRelease, 250); g_releaseMs = r < 20 ? 20 : r;
    wchar_t b[4];
    GetWindowTextW(hKey1, b, 4); if (b[0]) g_key1 = towupper(b[0]);
    GetWindowTextW(hKey2, b, 4); if (b[0]) g_key2 = towupper(b[0]);
    g_block = SendMessageW(hBlock, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_enabled = SendMessageW(hEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    UpdateEnables();
    SaveSettings();
}

static int LoadInterception(void) {
    HMODULE h = LoadLibraryW(L"interception.dll");
    if (!h) return 0;
    ic_create = (void *)GetProcAddress(h, "interception_create_context");
    ic_destroy = (void *)GetProcAddress(h, "interception_destroy_context");
    ic_set_filter = (void *)GetProcAddress(h, "interception_set_filter");
    ic_wait_timeout = (void *)GetProcAddress(h, "interception_wait_with_timeout");
    ic_send = (void *)GetProcAddress(h, "interception_send");
    ic_receive = (void *)GetProcAddress(h, "interception_receive");
    ic_hwid = (void *)GetProcAddress(h, "interception_get_hardware_id");
    if (!ic_create || !ic_destroy || !ic_set_filter || !ic_wait_timeout || !ic_send || !ic_receive || !ic_hwid) return 0;
    ICtx c = ic_create();      // fails if the driver isn't installed / PC not restarted yet
    if (!c) return -1;
    ic_destroy(c);
    return 1;
}

static int ProbePadDriver(void) {
    PVIGEM_CLIENT c = vigem_alloc();
    if (!c) return 0;
    int ok = VIGEM_SUCCESS(vigem_connect(c));
    if (ok) vigem_disconnect(c);
    vigem_free(c);
    return ok;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = h;
        int r = LoadInterception();
        g_icOk = r == 1;
        if (g_inputMode < 0) g_inputMode = g_icOk && g_hwid[0] ? IN_INTERCEPTION : IN_RAW;   // first run after update
        BuildUI();
        SetWindowTextW(hDrv, r == 1 ? L"Installed \x2714 (optional)" : L"Optional - hides cursor movement");
        EnableWindow(hInstall, r != 1);
        g_padDriverOk = ProbePadDriver();
        SetPadStatus(PAD_OFF);

        RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, h };
        RegisterRawInputDevices(&rid, 1, sizeof(rid));

        UpdateEnables();
        UpdateVrStatus(1);
        SetTimer(h, ID_TIMER_VR, 1000, NULL);
        g_thread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
        UpdateDevLabel();
        return 0;
    }
    case WM_INPUT:
        OnRawInput((HRAWINPUT)lp);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_INPUT_DEVICE_CHANGE:
        RawResetCache();
        if (wp == GIDC_REMOVAL) InterlockedExchange(&g_rawFound, 0);
        UpdateDevLabel();
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        COLORREF grey = RGB(110, 110, 110), green = RGB(0, 128, 0), red = RGB(200, 0, 0);
        if ((HWND)lp == hVrDrv) SetTextColor(dc, g_vrState == 2 ? green : g_output == OUT_STEAMVR ? red : grey);
        if ((HWND)lp == hDrv) SetTextColor(dc, g_icOk ? green : grey);
        if ((HWND)lp == hPadDrv) SetTextColor(dc, g_padDriverOk ? green : g_output == OUT_PAD ? red : grey);
        if ((HWND)lp == hLive) SetTextColor(dc, RGB(0, 90, 200));
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_INSTALLVR) InstallVrDriver();
        else if (id == ID_INSTALL)
            RunInstaller(L"install-interception.exe", L"/install",
                         L"If the installer said it succeeded, RESTART your PC, then open this program again.\n\n"
                         L"This driver is optional. If it won't install, just use \"Read mouse with: Windows\".");
        else if (id == ID_INSTALLPAD)
            RunInstaller(L"ViGEmBus_setup.exe", NULL,
                         L"Finish the controller driver installer, then close and reopen this program.\n\n"
                         L"This is only needed for \"Xbox controller\" output.");
        else if (id == ID_CALIB) {
            if (MessageBoxW(h, L"Get on the treadmill and walk at the FASTEST pace you'd actually use.\n\n"
                               L"Click OK once you're up to speed, then keep walking for 5 seconds.",
                            L"Calibrate top speed", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return 0;
            InterlockedExchange(&g_calibrating, 1);
            EnableWindow(hCalib, FALSE);
            SetTimer(h, ID_TIMER_CALIB, 1000, NULL);
            g_calCount = 5;
        }
        else if (id == ID_DETECT) {
            InterlockedExchange(&g_detecting, 1);
            UpdateDevLabel();
            SetTimer(h, ID_TIMER_DETECT, 15000, NULL);
        } else if ((id == ID_OUTPUT || id == ID_HAND || id == ID_INPUT || id == ID_AXIS || id == ID_MODE) && code == CBN_SELCHANGE) {
            ReadControls();
            if (id == ID_INPUT) {
                if (g_inputMode == IN_INTERCEPTION && !g_icOk)
                    MessageBoxW(h, L"The mouse driver (Interception) isn't installed or isn't working, so the "
                                   L"treadmill will be read through Windows instead.", L"Mouse driver", MB_ICONINFORMATION);
                else if (g_inputMode == IN_INTERCEPTION && !g_hwid[0])
                    MessageBoxW(h, L"Click Detect once more so the mouse driver can find the treadmill.", L"Mouse driver", MB_ICONINFORMATION);
                UpdateDevLabel();
            }
            if (id == ID_OUTPUT) {
                if (g_output == OUT_PAD && !g_padDriverOk && !ProbePadDriver())
                    MessageBoxW(h, L"Xbox controller output needs the controller driver (ViGEmBus).\n"
                                   L"Click Install next to \"Controller driver\".", L"Controller", MB_ICONINFORMATION);
                if (g_output == OUT_STEAMVR && VrState() == 0)
                    MessageBoxW(h, L"SteamVR output needs the SteamVR driver.\nClick Install next to \"SteamVR driver\".",
                                L"SteamVR", MB_ICONINFORMATION);
                InvalidateRect(hPadDrv, NULL, TRUE);
                InvalidateRect(hVrDrv, NULL, TRUE);
            }
        }
        else if ((id == ID_INVERT || id == ID_PROP || id == ID_BLOCK || id == ID_ENABLED) && code == BN_CLICKED) ReadControls();
        else if ((id == ID_TOPSPEED || id == ID_THRESH || id == ID_RELEASE || id == ID_KEY1 || id == ID_KEY2) && code == EN_CHANGE) ReadControls();
        return 0;
    }
    case WM_TIMER:
        if (wp == ID_TIMER_VR) UpdateVrStatus(0);
        if (wp == ID_TIMER_CALIB) {
            if (g_calCount > 0) g_calCount--;
            if (g_calCount <= 0) KillTimer(h, ID_TIMER_CALIB);
        }
        if (wp == ID_TIMER_DETECT) {
            KillTimer(h, ID_TIMER_DETECT);
            if (g_detecting) {
                InterlockedExchange(&g_detecting, 0);
                UpdateDevLabel();
                MessageBoxW(h, L"No mouse movement detected. Make sure the treadmill mouse is plugged in, "
                               L"then click Detect and move the belt.", L"Detect", MB_ICONWARNING);
            }
        }
        return 0;
    case WM_APP_DETECTED:
        KillTimer(h, ID_TIMER_DETECT);
        SaveSettings();
        UpdateEnables();
        UpdateDevLabel();
        return 0;
    case WM_APP_CALIB: {
        KillTimer(h, ID_TIMER_CALIB);
        g_calCount = 0;
        int top = (int)wp;
        if (top < g_threshold * 2) {
            MessageBoxW(h, L"Not enough belt movement was detected. Make sure you're walking on the treadmill "
                           L"during the 5 seconds, then try again.", L"Calibrate", MB_ICONWARNING);
        } else {
            SetInt(hTop, top);       // EN_CHANGE -> ReadControls -> saved
            wchar_t t[160];
            swprintf(t, 160, L"Done! Top speed set to %d.\nWalking at that pace = full speed, slower = slower.", top);
            MessageBoxW(h, t, L"Calibrate", MB_ICONINFORMATION);
        }
        UpdateEnables();
        return 0;
    }
    case WM_APP_FOUND:
        UpdateEnables();
        UpdateDevLabel();
        return 0;
    case WM_APP_PAD:
        SetPadStatus((int)wp);
        return 0;
    case WM_APP_STATUS: {
        int speed = (int)wp;
        int pct = (g_output != OUT_KEYBOARD && g_prop)
                ? speed * 100 / (g_topSpeed > 0 ? g_topSpeed : 1)       // bar = share of top speed
                : (g_threshold > 0 ? speed * 50 / g_threshold : 0);     // threshold = half the bar
        SendMessageW(hBar, PBM_SETPOS, pct > 100 ? 100 : pct, 0);
        wchar_t t[64];
        if (g_calCount > 0) swprintf(t, 64, L"Calibrating... %d", g_calCount);
        else if (!g_enabled) lstrcpyW(t, L"Disabled");
        else if (lp & 0x10000) {
            int st = (SHORT)(lp & 0xFFFF);
            if (st == 0) lstrcpyW(t, L"Standing still");
            else swprintf(t, 64, L"Walking %ls %d%%", st > 0 ? L"fwd" : L"back", abs(st) * 100 / 32767);
        }
        else if (lp) swprintf(t, 64, L"Holding %lc", (wchar_t)lp);
        else lstrcpyW(t, L"Standing still");
        SetWindowTextW(hLive, t);
        return 0;
    }
    case WM_CLOSE:
        InterlockedExchange(&g_quit, 1);
        if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show) {
    (void)hp; (void)cmd;
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"VRTreadmillMouseToKey_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(L"VRTreadmillWnd", NULL);
        if (w) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
        return 0;
    }

    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_dir, L'\\'); if (slash) *slash = 0;
    swprintf(g_ini, MAX_PATH, L"%ls\\VRTreadmill.ini", g_dir);
    SetDllDirectoryW(g_dir);
    LoadSettings();
    OpenShared();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    HDC sdc = GetDC(NULL); g_dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"VRTreadmillWnd";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, S(896), S(344) };
    AdjustWindowRect(&r, style, FALSE);
    HWND w = CreateWindowW(L"VRTreadmillWnd", L"VR Treadmill", style,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    ReleaseMutex(mtx);
    return 0;
}
