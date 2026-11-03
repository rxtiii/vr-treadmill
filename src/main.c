// VR Treadmill Mouse -> Key / Controller
// Turns movement from ONE chosen mouse (the treadmill) into either holding a key (W)
// or pushing a virtual Xbox controller's left stick forward. That mouse is hidden from
// the cursor; every other mouse/trackpad is left alone.
// Mouse isolation: Interception driver (https://github.com/oblitum/Interception)
// Virtual controller: ViGEmBus + ViGEmClient (https://github.com/nefarius)

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
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include "ViGEm/Client.h"

// ---------------- Interception API (loaded at runtime) ----------------
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
static wchar_t g_ini[MAX_PATH], g_dir[MAX_PATH];
static wchar_t g_hwid[512];          // saved hardware id of treadmill mouse
static volatile LONG g_dev = 0;      // interception device number (11..20), 0 = none
static volatile LONG g_output = 0;   // 0 = keyboard key, 1 = virtual controller left stick
static volatile LONG g_axis = 0;     // 0 = horizontal X, 1 = vertical Y, 2 = both
static volatile LONG g_mode = 0;     // 0 = any direction -> forward, 1 = forward / backward
static volatile LONG g_invert = 0;
static volatile LONG g_prop = 1;     // stick deflection scales with speed
static volatile LONG g_topSpeed = 300; // belt speed (units per 0.1 s) that gives full stick
static volatile LONG g_threshold = 15;
static volatile LONG g_releaseMs = 250;
static volatile LONG g_key1 = L'W', g_key2 = L'S';
static volatile LONG g_block = 1;
static volatile LONG g_enabled = 1;

static volatile LONG g_detecting = 0, g_quit = 0, g_calibrating = 0;
static HANDLE g_thread;
static HWND g_hwnd;
static int g_driverOk = 0, g_padDriverOk = 0, g_calCount = 0;

#define WM_APP_STATUS   (WM_APP + 1)
#define WM_APP_DETECTED (WM_APP + 2)
#define WM_APP_FOUND    (WM_APP + 3)
#define WM_APP_PAD      (WM_APP + 4)
#define WM_APP_CALIB    (WM_APP + 5)

enum { PAD_OFF = 0, PAD_CONNECTED, PAD_NODRIVER, PAD_ERROR };

enum { ID_INSTALL = 100, ID_INSTALLPAD, ID_DETECT, ID_OUTPUT, ID_AXIS, ID_MODE, ID_INVERT, ID_PROP,
       ID_THRESH, ID_RELEASE, ID_KEY1, ID_KEY2, ID_BLOCK, ID_ENABLED, ID_TOPSPEED, ID_CALIB, ID_TIMER_DETECT, ID_TIMER_CALIB };

static HWND hDrv, hInstall, hPadDrv, hInstallPad, hDevLbl, hDetect, hOutput, hAxis, hMode, hInvert,
            hProp, hTop, hCalib, hThresh, hRelease, hKey1, hKey2, hBlock, hEnabled, hBar, hLive;

static void SaveSettings(void) {
    wchar_t b[32];
    WritePrivateProfileStringW(L"Settings", L"Hwid", g_hwid, g_ini);
#define SAVEI(name, v) swprintf(b, 32, L"%ld", (long)(v)); WritePrivateProfileStringW(L"Settings", name, b, g_ini)
    SAVEI(L"Device", g_dev); SAVEI(L"Output", g_output); SAVEI(L"Axis", g_axis); SAVEI(L"Mode", g_mode);
    SAVEI(L"Invert", g_invert); SAVEI(L"Proportional", g_prop); SAVEI(L"TopSpeed", g_topSpeed); SAVEI(L"Threshold", g_threshold);
    SAVEI(L"ReleaseMs", g_releaseMs); SAVEI(L"Key1", g_key1); SAVEI(L"Key2", g_key2);
    SAVEI(L"Block", g_block); SAVEI(L"Enabled", g_enabled);
}
static void LoadSettings(void) {
    GetPrivateProfileStringW(L"Settings", L"Hwid", L"", g_hwid, 512, g_ini);
#define LOADI(name, v) v = GetPrivateProfileIntW(L"Settings", name, v, g_ini)
    LOADI(L"Device", g_dev); LOADI(L"Output", g_output); LOADI(L"Axis", g_axis); LOADI(L"Mode", g_mode);
    LOADI(L"Invert", g_invert); LOADI(L"Proportional", g_prop); LOADI(L"TopSpeed", g_topSpeed); LOADI(L"Threshold", g_threshold);
    LOADI(L"ReleaseMs", g_releaseMs); LOADI(L"Key1", g_key1); LOADI(L"Key2", g_key2);
    LOADI(L"Block", g_block); LOADI(L"Enabled", g_enabled);
    if (g_output < 0 || g_output > 1) g_output = 0;
    if (g_axis < 0 || g_axis > 2) g_axis = 0;
    if (g_mode < 0 || g_mode > 1) g_mode = 0;
    if (g_threshold < 1) g_threshold = 1;
    if (g_releaseMs < 20) g_releaseMs = 20;
    if (g_topSpeed < 10) g_topSpeed = 10;
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

// ---------------- Virtual controller ----------------
static PVIGEM_CLIENT g_vc;
static PVIGEM_TARGET g_pad;

static int PadConnect(void) {           // returns PAD_* status
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

// ---------------- Worker thread ----------------
static DWORD WINAPI Worker(LPVOID p) {
    (void)p;
    ICtx ctx = ic_create();
    if (!ctx) return 1;
    ic_set_filter(ctx, is_mouse, I_FILTER_MOUSE_ALL);

    // Re-find the saved treadmill by hardware id (device numbers can change after replugging)
    if (g_hwid[0]) {
        LONG found = 0;
        for (IDev d = 11; d <= 20; d++) {
            wchar_t h[512] = {0};
            if (ic_hwid(ctx, d, h, sizeof(h) - 2) && wcscmp(h, g_hwid) == 0) {
                if (d == g_dev) { found = d; break; }   // prefer same slot (identical mice)
                if (!found) found = d;
            }
        }
        InterlockedExchange(&g_dev, found);
        PostMessageW(g_hwnd, WM_APP_FOUND, found, 0);
    }

    long detAccum[21] = {0};
    long winAbs = 0, winSigned = 0;
    long tickAbs = 0;                      // movement in the current 50 ms tick
    double ema = 0, calMax = 0;            // smoothed speed (units per 0.1 s)
    DWORD tickStart = GetTickCount(), calEnd = 0;
    int wasCalibrating = 0;
    DWORD winStart = GetTickCount(), lastActive = 0, lastPadTry = 0;
    int active = 0, dir = 1;               // dir: +1 forward, -1 backward
    wchar_t held = 0;                      // keyboard mode: key currently held
    SHORT stick = 0;                       // controller mode: current stick Y sent
    int padStatus = -1;
    LONG curOutput = -1;

    while (!g_quit) {
        // ---- switch outputs cleanly ----
        LONG out = g_output;
        if (out != curOutput) {
            if (held) { SendKey(held, 0); held = 0; }
            if (stick) { PadSetStickY(0); stick = 0; }
            if (out == 0) { PadDisconnect(); padStatus = PAD_OFF; PostMessageW(g_hwnd, WM_APP_PAD, PAD_OFF, 0); }
            curOutput = out;
            lastPadTry = 0;
        }
        if (out == 1 && !g_pad && (lastPadTry == 0 || GetTickCount() - lastPadTry > 3000)) {
            lastPadTry = GetTickCount();
            int st = PadConnect();
            if (st != padStatus) { padStatus = st; PostMessageW(g_hwnd, WM_APP_PAD, st, 0); }
        }

        IDev d = ic_wait_timeout(ctx, 10);
        DWORD now = GetTickCount();

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
                        PostMessageW(g_hwnd, WM_APP_DETECTED, d, 0);
                    }
                    ic_send(ctx, d, &s, 1);
                } else if (d == g_dev) {
                    if (rel) {
                        int v = g_axis == 0 ? m->x : g_axis == 1 ? m->y : (abs(m->x) > abs(m->y) ? m->x : m->y);
                        int a = g_axis == 2 ? abs(m->x) + abs(m->y) : abs(v);
                        winAbs += a;
                        tickAbs += a;
                        winSigned += v;
                        if (g_enabled && winAbs >= g_threshold) {
                            lastActive = now;
                            active = 1;
                            if (g_mode == 1) {
                                long sgn = g_invert ? -winSigned : winSigned;
                                dir = sgn >= 0 ? 1 : -1;
                            } else dir = 1;
                        }
                    }
                    if (!g_block) ic_send(ctx, d, &s, 1);   // otherwise swallowed: no cursor, no clicks
                } else {
                    ic_send(ctx, d, &s, 1);                 // every other mouse: untouched
                }
            }
        }

        if (active && (!g_enabled || now - lastActive > (DWORD)g_releaseMs)) active = 0;

        // ---- smoothed speed, sampled every 50 ms ----
        if (now - tickStart >= 50) {
            double sample = tickAbs * 100.0 / (double)(now - tickStart);
            ema = ema * 0.6 + sample * 0.4;
            tickAbs = 0;
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

        // ---- apply output ----
        if (out == 0) {
            wchar_t want = active ? (wchar_t)(dir > 0 ? g_key1 : g_key2) : 0;
            if (want != held) {
                if (held) SendKey(held, 0);
                if (want) SendKey(want, 1);
                held = want;
            }
        } else {
            SHORT want = 0;
            if (active) {
                double amt = 1.0;
                if (g_prop) {
                    // 25% at walking start (clears typical stick dead zones) -> 100% at calibrated top speed
                    double lo = (double)g_threshold, hi = (double)g_topSpeed;
                    double frac = hi > lo ? (ema - lo) / (hi - lo) : 1.0;
                    if (frac < 0) frac = 0;
                    if (frac > 1) frac = 1;
                    amt = 0.25 + 0.75 * frac;
                }
                want = (SHORT)(dir * (int)(amt * 32767));
            }
            if (want != stick) { PadSetStickY(want); stick = want; }
        }

        if (now - winStart >= 100) {
            winAbs = winSigned = 0;
            winStart = now;
            LPARAM info = out == 0 ? (LPARAM)held : (LPARAM)(0x10000 | (stick & 0xFFFF));
            PostMessageW(g_hwnd, WM_APP_STATUS, (WPARAM)(ema + 0.5), info);
        }
    }
    if (held) SendKey(held, 0);
    if (stick) PadSetStickY(0);
    PadDisconnect();
    ic_destroy(ctx);
    return 0;
}

// ---------------- GUI ----------------
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static HFONT g_font, g_bold;

static HWND Mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void UpdateDevLabel(void) {
    wchar_t t[700];
    if (g_detecting)
        lstrcpyW(t, L"Move ONLY the treadmill mouse now...\n(keep your hands off the trackpad)");
    else if (g_dev && g_hwid[0])
        swprintf(t, 700, L"Selected: device #%ld\n%ls", (long)g_dev - 10, g_hwid);
    else if (g_hwid[0])
        lstrcpyW(t, L"Saved treadmill mouse not plugged in.\nPlug it in and restart, or click Detect.");
    else
        lstrcpyW(t, L"No treadmill mouse selected.\nClick Detect, then move the treadmill.");
    SetWindowTextW(hDevLbl, t);
}

static void SetPadStatus(int st) {
    const wchar_t *t;
    switch (st) {
    case PAD_CONNECTED: t = L"Virtual Xbox controller connected \x2714"; g_padDriverOk = 1; break;
    case PAD_NODRIVER:  t = L"NOT installed - click Install"; g_padDriverOk = 0; break;
    case PAD_ERROR:     t = L"Couldn't create controller (retrying)"; break;
    default:            t = g_padDriverOk ? L"Installed (used in controller mode)" : L"Only needed for controller mode"; break;
    }
    SetWindowTextW(hPadDrv, t);
    EnableWindow(hInstallPad, !g_padDriverOk);
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

static void UpdateEnables(void) {
    EnableWindow(hInvert, g_mode == 1);
    EnableWindow(hKey1, g_output == 0);
    EnableWindow(hKey2, g_output == 0 && g_mode == 1);
    EnableWindow(hProp, g_output == 1);
    EnableWindow(hTop, g_output == 1 && g_prop);
    EnableWindow(hCalib, g_output == 1 && g_prop && g_dev);
}

static void BuildUI(void) {
    Mk(L"STATIC", L"Mouse driver:", SS_LEFT, 14, 15, 110, 20, -1);
    hDrv = Mk(L"STATIC", L"", SS_LEFT, 126, 15, 196, 20, -1);
    hInstall = Mk(L"BUTTON", L"Install", BS_PUSHBUTTON, 332, 10, 100, 28, ID_INSTALL);
    Mk(L"STATIC", L"Controller driver:", SS_LEFT, 14, 47, 110, 20, -1);
    hPadDrv = Mk(L"STATIC", L"", SS_LEFT, 126, 47, 204, 20, -1);
    hInstallPad = Mk(L"BUTTON", L"Install", BS_PUSHBUTTON, 332, 42, 100, 28, ID_INSTALLPAD);

    Mk(L"BUTTON", L"1. Treadmill mouse", BS_GROUPBOX, 8, 78, 428, 86, -1);
    hDevLbl = Mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 20, 100, 292, 56, -1);
    hDetect = Mk(L"BUTTON", L"Detect", BS_PUSHBUTTON, 322, 104, 100, 32, ID_DETECT);
    SendMessageW(hDetect, WM_SETFONT, (WPARAM)g_bold, TRUE);

    Mk(L"BUTTON", L"2. Settings", BS_GROUPBOX, 8, 172, 428, 308, -1);
    Mk(L"STATIC", L"Output:", SS_LEFT, 20, 197, 120, 20, -1);
    hOutput = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 150, 193, 272, 120, ID_OUTPUT);
    SendMessageW(hOutput, CB_ADDSTRING, 0, (LPARAM)L"Keyboard key (W)");
    SendMessageW(hOutput, CB_ADDSTRING, 0, (LPARAM)L"Controller left stick forward");

    Mk(L"STATIC", L"Movement axis:", SS_LEFT, 20, 228, 120, 20, -1);
    hAxis = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 150, 224, 272, 120, ID_AXIS);
    SendMessageW(hAxis, CB_ADDSTRING, 0, (LPARAM)L"Horizontal (left/right)");
    SendMessageW(hAxis, CB_ADDSTRING, 0, (LPARAM)L"Vertical (up/down)");
    SendMessageW(hAxis, CB_ADDSTRING, 0, (LPARAM)L"Both");

    Mk(L"STATIC", L"Mode:", SS_LEFT, 20, 259, 120, 20, -1);
    hMode = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 150, 255, 272, 120, ID_MODE);
    SendMessageW(hMode, CB_ADDSTRING, 0, (LPARAM)L"Any direction = forward");
    SendMessageW(hMode, CB_ADDSTRING, 0, (LPARAM)L"One way = forward, other way = backward");

    hInvert = Mk(L"BUTTON", L"Swap forward/backward direction", BS_AUTOCHECKBOX | WS_TABSTOP, 150, 284, 272, 22, ID_INVERT);
    hProp = Mk(L"BUTTON", L"Push stick further the faster you walk", BS_AUTOCHECKBOX | WS_TABSTOP, 150, 308, 272, 22, ID_PROP);

    Mk(L"STATIC", L"Top speed:", SS_LEFT, 20, 343, 120, 20, -1);
    hTop = Mk(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, 150, 340, 56, 24, ID_TOPSPEED);
    hCalib = Mk(L"BUTTON", L"Calibrate", BS_PUSHBUTTON, 214, 338, 90, 28, ID_CALIB);
    Mk(L"STATIC", L"walk at full speed", SS_LEFT, 312, 343, 112, 20, -1);

    Mk(L"STATIC", L"Sensitivity:", SS_LEFT, 20, 378, 120, 20, -1);
    hThresh = Mk(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, 150, 375, 56, 24, ID_THRESH);
    Mk(L"STATIC", L"movement needed to start (lower = more sensitive)", SS_LEFT, 214, 371, 210, 34, -1);

    Mk(L"STATIC", L"Stop delay (ms):", SS_LEFT, 20, 412, 120, 20, -1);
    hRelease = Mk(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, 150, 409, 56, 24, ID_RELEASE);
    Mk(L"STATIC", L"keep walking this long after belt stops", SS_LEFT, 214, 405, 210, 34, -1);

    Mk(L"STATIC", L"Forward key:", SS_LEFT, 20, 446, 120, 20, -1);
    hKey1 = Mk(L"EDIT", L"", ES_UPPERCASE | WS_BORDER | WS_TABSTOP | ES_CENTER, 150, 443, 34, 24, ID_KEY1);
    Mk(L"STATIC", L"Backward key:", SS_LEFT, 214, 446, 100, 20, -1);
    hKey2 = Mk(L"EDIT", L"", ES_UPPERCASE | WS_BORDER | WS_TABSTOP | ES_CENTER, 316, 443, 34, 24, ID_KEY2);
    SendMessageW(hKey1, EM_SETLIMITTEXT, 1, 0);
    SendMessageW(hKey2, EM_SETLIMITTEXT, 1, 0);

    hBlock = Mk(L"BUTTON", L"Stop treadmill mouse moving the cursor / clicking (recommended)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 14, 488, 420, 22, ID_BLOCK);
    hEnabled = Mk(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, 14, 512, 200, 22, ID_ENABLED);
    SendMessageW(hEnabled, WM_SETFONT, (WPARAM)g_bold, TRUE);

    Mk(L"BUTTON", L"Live", BS_GROUPBOX, 8, 540, 428, 62, -1);
    hBar = Mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, 20, 566, 260, 22, -1);
    SendMessageW(hBar, PBM_SETRANGE32, 0, 100);
    hLive = Mk(L"STATIC", L"Idle", SS_LEFT, 290, 567, 140, 22, -1);
    SendMessageW(hLive, WM_SETFONT, (WPARAM)g_bold, TRUE);

    SendMessageW(hOutput, CB_SETCURSEL, g_output, 0);
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
    UpdateEnables();
}

static void ReadControls(void) {
    g_output = (LONG)SendMessageW(hOutput, CB_GETCURSEL, 0, 0);
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

static int LoadDriver(void) {
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
        BuildUI();
        int r = LoadDriver();
        g_driverOk = r == 1;
        SetWindowTextW(hDrv, r == 1 ? L"Installed and running \x2714"
                           : r == 0 ? L"interception.dll missing"
                                    : L"NOT installed - click Install");
        EnableWindow(hInstall, r != 1);
        EnableWindow(hDetect, g_driverOk);
        g_padDriverOk = ProbePadDriver();
        SetPadStatus(PAD_OFF);
        if (g_driverOk) g_thread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
        else g_dev = 0;
        UpdateDevLabel();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        if ((HWND)lp == hDrv) SetTextColor(dc, g_driverOk ? RGB(0, 128, 0) : RGB(200, 0, 0));
        if ((HWND)lp == hPadDrv) SetTextColor(dc, g_padDriverOk ? RGB(0, 128, 0) : g_output == 1 ? RGB(200, 0, 0) : RGB(110, 110, 110));
        if ((HWND)lp == hLive) SetTextColor(dc, RGB(0, 90, 200));
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_INSTALL)
            RunInstaller(L"install-interception.exe", L"/install",
                         L"If the installer said it succeeded, RESTART your PC, then open this program again.");
        else if (id == ID_INSTALLPAD)
            RunInstaller(L"ViGEmBus_setup.exe", NULL,
                         L"Finish the controller driver installer, then close and reopen this program.");
        else if (id == ID_CALIB) {
            if (!g_dev) { MessageBoxW(h, L"Pick the treadmill mouse first (click Detect).", L"Calibrate", MB_ICONINFORMATION); return 0; }
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
        } else if ((id == ID_OUTPUT || id == ID_AXIS || id == ID_MODE) && code == CBN_SELCHANGE) {
            ReadControls();
            if (id == ID_OUTPUT) {
                if (g_output == 1 && !g_padDriverOk && !ProbePadDriver())
                    MessageBoxW(h, L"Controller mode needs the controller driver (ViGEmBus).\n"
                                   L"Click the Install button next to \"Controller driver\".", L"Controller", MB_ICONINFORMATION);
                InvalidateRect(hPadDrv, NULL, TRUE);
            }
        }
        else if ((id == ID_INVERT || id == ID_PROP || id == ID_BLOCK || id == ID_ENABLED) && code == BN_CLICKED) ReadControls();
        else if ((id == ID_TOPSPEED || id == ID_THRESH || id == ID_RELEASE || id == ID_KEY1 || id == ID_KEY2) && code == EN_CHANGE) ReadControls();
        return 0;
    }
    case WM_TIMER:
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
        UpdateEnables();
        SaveSettings();
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
            swprintf(t, 160, L"Done! Top speed set to %d.\nWalking at that pace = full stick, slower = less.", top);
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
        int pct = (g_output == 1 && g_prop)
                ? speed * 100 / (g_topSpeed > 0 ? g_topSpeed : 1)       // bar = share of top speed
                : (g_threshold > 0 ? speed * 50 / g_threshold : 0);     // threshold = half the bar
        SendMessageW(hBar, PBM_SETPOS, pct > 100 ? 100 : pct, 0);
        wchar_t t[64];
        if (g_calCount > 0) swprintf(t, 64, L"Calibrating... %d", g_calCount);
        else if (!g_enabled) lstrcpyW(t, L"Disabled");
        else if (lp & 0x10000) {
            int st = (SHORT)(lp & 0xFFFF);
            if (st == 0) lstrcpyW(t, L"Stick centred");
            else swprintf(t, 64, L"Stick %ls %d%%", st > 0 ? L"fwd" : L"back", abs(st) * 100 / 32767);
        }
        else if (lp) swprintf(t, 64, L"Holding %lc", (wchar_t)lp);
        else lstrcpyW(t, L"Idle");
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
    RECT r = { 0, 0, S(444), S(612) };
    AdjustWindowRect(&r, style, FALSE);
    HWND w = CreateWindowW(L"VRTreadmillWnd", L"VR Treadmill - Mouse to Key / Controller", style,
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
