// VR Treadmill - SteamVR driver
//
// Two jobs:
//  1. "KAT-style" walking: blend the treadmill's speed into your real controller's
//     thumbstick (or touchpad) inside SteamVR, so every game that supports thumbstick
//     walking works with no per-game setup. It does this by wrapping SteamVR's driver
//     input interface (IVRDriverInput): it notes which input handles belong to controller
//     sticks when they are created, and adds the treadmill value whenever they update.
//     The driver is named "00vrtreadmill" so SteamVR loads it before the controller drivers.
//  2. Register a "treadmill" device (SteamVR's TrackedControllerRole_Treadmill) with its
//     own joystick, so the treadmill shows up in SteamVR and can also be bound manually.
//
// The VRTreadmill desktop app writes the walking speed into shared memory every few ms.
//
// Built with MinGW (clang). OpenVR's interfaces are plain C++ vtables with no virtual
// destructors, which MinGW and MSVC lay out identically. The one exception is
// ITrackedDeviceServerDriver::GetPose(), which returns a struct by value: MSVC passes the
// hidden return pointer AFTER `this`, MinGW before it. So the device class below mirrors
// that interface by hand and declares GetPose as an ordinary pointer-in/pointer-out
// function, which matches MSVC's calling convention exactly.

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include "openvr_driver.h"
#include "../../src/shared.h"

using namespace vr;

static void Log(const char *fmt, ...) {
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    if (VRDriverLog()) VRDriverLog()->Log(buf);
}

// =====================================================================================
// Shared memory with the desktop app
// =====================================================================================
static HANDLE g_map;
static volatile TreadmillShared *g_shm;

static void OpenShared() {
    if (g_shm) return;
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(TreadmillShared), TREADMILL_SHM_NAME);
    if (!g_map) return;
    g_shm = (volatile TreadmillShared *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TreadmillShared));
    if (!g_shm) { CloseHandle(g_map); g_map = NULL; }
}
static void CloseShared() {
    if (g_shm) UnmapViewOfFile((LPCVOID)g_shm);
    if (g_map) CloseHandle(g_map);
    g_shm = NULL; g_map = NULL;
}

struct AppState { float x, y; bool alive; bool inject; int hand; };

static AppState ReadShared() {
    AppState s = { 0, 0, false, false, 1 };
    if (!g_shm) OpenShared();
    if (!g_shm) return s;
    g_shm->driverSeen = GetTickCount();
    if (g_shm->magic != TREADMILL_SHM_MAGIC) return s;
    if (GetTickCount() - g_shm->heartbeat > 500) return s;   // app closed or frozen: stop walking
    s.alive = true;
    s.x = fmaxf(-1.f, fminf(1.f, g_shm->x));
    s.y = fmaxf(-1.f, fminf(1.f, g_shm->y));
    s.inject = g_shm->inject != 0;
    s.hand = g_shm->hand == 2 ? 2 : 1;
    return s;
}

// =====================================================================================
// KAT-style injection: wrap IVRDriverInput
// =====================================================================================
// Slots 0..3 of IVRDriverInput have been the same in every version (_001.._004).
typedef EVRInputError (*CreateBoolFn)(void *self, PropertyContainerHandle_t c, const char *name, VRInputComponentHandle_t *h);
typedef EVRInputError (*UpdateBoolFn)(void *self, VRInputComponentHandle_t h, bool v, double t);
typedef EVRInputError (*CreateScalarFn)(void *self, PropertyContainerHandle_t c, const char *name, VRInputComponentHandle_t *h, EVRScalarType type, EVRScalarUnits units);
typedef EVRInputError (*UpdateScalarFn)(void *self, VRInputComponentHandle_t h, float v, double t);

struct Patched {                 // one per distinct vtable we wrapped
    void **vtable;
    CreateBoolFn createBool;
    UpdateBoolFn updateBool;
    CreateScalarFn createScalar;
    UpdateScalarFn updateScalar;
};
static Patched g_patched[8];
static int g_numPatched;

enum Kind { K_STICK = 0, K_PAD = 1 };
struct Comp {                    // a controller stick/touchpad component we track
    VRInputComponentHandle_t handle;
    PropertyContainerHandle_t container;
    void *self;                  // interface object it was created through
    int patchIdx;
    int kind;                    // K_STICK or K_PAD
    int axis;                    // 0 = x, 1 = y, 2 = touchpad touch (bool)
    float real;                  // last value the real controller reported
    bool realBool;
    float sent;                  // last value we passed on
    bool sentBool;
    int role;                    // cached controller role (1 left, 2 right, 0 unknown)
};
static Comp g_comps[64];
static int g_numComps;
static CRITICAL_SECTION g_lock;
static PropertyContainerHandle_t g_ownContainer;
static thread_local int t_depth;          // stops double-processing if one wrapper forwards to another

// Current injected forward amount (written by RunFrame, read by update hooks)
static volatile float g_injectY;
static volatile int g_injectRole;         // 1 left, 2 right, 0 = off

static bool EndsWith(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && _stricmp(s + a - b, suffix) == 0;
}

static int Classify(const char *name, int *kind, int *axis) {
    // Sticks: Index "/input/thumbstick", Oculus/WMR/Pico "/input/joystick"; Vive wand "/input/trackpad"
    if (EndsWith(name, "/input/thumbstick/y") || EndsWith(name, "/input/joystick/y")) { *kind = K_STICK; *axis = 1; return 1; }
    if (EndsWith(name, "/input/thumbstick/x") || EndsWith(name, "/input/joystick/x")) { *kind = K_STICK; *axis = 0; return 1; }
    if (EndsWith(name, "/input/trackpad/y")) { *kind = K_PAD; *axis = 1; return 1; }
    if (EndsWith(name, "/input/trackpad/x")) { *kind = K_PAD; *axis = 0; return 1; }
    if (EndsWith(name, "/input/trackpad/touch")) { *kind = K_PAD; *axis = 2; return 1; }
    return 0;
}

static int FindPatch(void *self) {
    void **vt = *(void ***)self;
    for (int i = 0; i < g_numPatched; i++) if (g_patched[i].vtable == vt) return i;
    return 0;
}

static void Track(void *self, PropertyContainerHandle_t c, const char *name, VRInputComponentHandle_t h) {
    int kind, axis;
    if (!h || h == k_ulInvalidInputComponentHandle || c == g_ownContainer || !Classify(name, &kind, &axis)) return;
    EnterCriticalSection(&g_lock);
    if (g_numComps < (int)(sizeof(g_comps) / sizeof(g_comps[0]))) {
        Comp &k = g_comps[g_numComps++];
        memset(&k, 0, sizeof(k));
        k.handle = h; k.container = c; k.self = self; k.patchIdx = FindPatch(self);
        k.kind = kind; k.axis = axis;
    }
    LeaveCriticalSection(&g_lock);
    Log("VR Treadmill: found controller input %s\n", name);
}

// Is this component the one we should currently be adding walking to?
// (Only the Y axis / touch of the chosen hand; sticks win over touchpads on the same controller.)
static bool IsTarget(const Comp &k) {
    if (!g_injectRole || k.role != g_injectRole || k.axis == 0) return false;
    if (k.kind == K_PAD) {
        for (int i = 0; i < g_numComps; i++)
            if (g_comps[i].container == k.container && g_comps[i].kind == K_STICK) return false;
    }
    return true;
}

static float Combine(float real, float add) { return fmaxf(-1.f, fminf(1.f, real + add)); }

static EVRInputError HookCreateBool(void *self, PropertyContainerHandle_t c, const char *name, VRInputComponentHandle_t *h) {
    Patched &p = g_patched[FindPatch(self)];
    if (t_depth) return p.createBool(self, c, name, h);
    t_depth++;
    EVRInputError e = p.createBool(self, c, name, h);
    t_depth--;
    if (e == VRInputError_None && h && name) Track(self, c, name, *h);
    return e;
}

static EVRInputError HookCreateScalar(void *self, PropertyContainerHandle_t c, const char *name, VRInputComponentHandle_t *h, EVRScalarType type, EVRScalarUnits units) {
    Patched &p = g_patched[FindPatch(self)];
    if (t_depth) return p.createScalar(self, c, name, h, type, units);
    t_depth++;
    EVRInputError e = p.createScalar(self, c, name, h, type, units);
    t_depth--;
    if (e == VRInputError_None && h && name) Track(self, c, name, *h);
    return e;
}

static EVRInputError HookUpdateScalar(void *self, VRInputComponentHandle_t h, float v, double t) {
    Patched &p = g_patched[FindPatch(self)];
    if (t_depth) return p.updateScalar(self, h, v, t);
    float out = v;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_numComps; i++) {
        Comp &k = g_comps[i];
        if (k.handle != h || k.axis == 2) continue;
        k.real = v;
        if (IsTarget(k)) out = Combine(v, g_injectY);
        k.sent = out;
        break;
    }
    LeaveCriticalSection(&g_lock);
    t_depth++;
    EVRInputError e = p.updateScalar(self, h, out, t);
    t_depth--;
    return e;
}

static EVRInputError HookUpdateBool(void *self, VRInputComponentHandle_t h, bool v, double t) {
    Patched &p = g_patched[FindPatch(self)];
    if (t_depth) return p.updateBool(self, h, v, t);
    bool out = v;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_numComps; i++) {
        Comp &k = g_comps[i];
        if (k.handle != h || k.axis != 2) continue;
        k.realBool = v;
        if (IsTarget(k) && g_injectY != 0) out = true;   // touchpads only move you while "touched"
        k.sentBool = out;
        break;
    }
    LeaveCriticalSection(&g_lock);
    t_depth++;
    EVRInputError e = p.updateBool(self, h, out, t);
    t_depth--;
    return e;
}

static bool PatchSlot(void **slot, void *fn) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return false;
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return true;
}

static void InstallHooks(IVRDriverContext *ctx) {
    static const char *versions[] = { "IVRDriverInput_004", "IVRDriverInput_003", "IVRDriverInput_002", "IVRDriverInput_001" };
    for (const char *ver : versions) {
        EVRInitError err = VRInitError_None;
        void *obj = ctx->GetGenericInterface(ver, &err);
        if (!obj || err != VRInitError_None) continue;
        void **vt = *(void ***)obj;
        bool dup = false;
        for (int i = 0; i < g_numPatched; i++) if (g_patched[i].vtable == vt) dup = true;
        if (dup || g_numPatched >= 8) continue;
        Patched &p = g_patched[g_numPatched];
        p.vtable = vt;
        p.createBool = (CreateBoolFn)vt[0];
        p.updateBool = (UpdateBoolFn)vt[1];
        p.createScalar = (CreateScalarFn)vt[2];
        p.updateScalar = (UpdateScalarFn)vt[3];
        g_numPatched++;   // register before patching so hooks can find it
        bool ok = PatchSlot(&vt[0], (void *)HookCreateBool) && PatchSlot(&vt[1], (void *)HookUpdateBool) &&
                  PatchSlot(&vt[2], (void *)HookCreateScalar) && PatchSlot(&vt[3], (void *)HookUpdateScalar);
        Log("VR Treadmill: wrapped %s %s\n", ver, ok ? "OK" : "FAILED");
    }
}

static void RemoveHooks() {
    for (int i = 0; i < g_numPatched; i++) {
        Patched &p = g_patched[i];
        PatchSlot(&p.vtable[0], (void *)p.createBool);
        PatchSlot(&p.vtable[1], (void *)p.updateBool);
        PatchSlot(&p.vtable[2], (void *)p.createScalar);
        PatchSlot(&p.vtable[3], (void *)p.updateScalar);
    }
    g_numPatched = 0;
}

// Called every frame: refresh controller roles and push the walking value even when the
// real stick isn't moving (controllers only send updates when something changes).
static DWORD g_lastRoleCheck;
static void InjectFrame(const AppState &s) {
    float add = (s.alive && s.inject) ? s.y : 0.f;
    g_injectY = add;
    g_injectRole = (s.alive && s.inject) ? s.hand : 0;

    DWORD now = GetTickCount();
    bool refreshRoles = now - g_lastRoleCheck > 1000;
    if (refreshRoles) g_lastRoleCheck = now;

    struct Pending { void *self; int patch; VRInputComponentHandle_t h; bool isBool; float f; bool b; };
    Pending todo[64]; int n = 0;

    if (refreshRoles) {
        // Look roles up outside the lock (it's a call into SteamVR), then store them.
        PropertyContainerHandle_t conts[64]; int roles[64]; int nc;
        EnterCriticalSection(&g_lock);
        nc = g_numComps;
        for (int i = 0; i < nc; i++) conts[i] = g_comps[i].container;
        LeaveCriticalSection(&g_lock);
        for (int i = 0; i < nc; i++) {
            ETrackedPropertyError pe = TrackedProp_Success;
            int32_t r = VRProperties()->GetInt32Property(conts[i], Prop_ControllerRoleHint_Int32, &pe);
            roles[i] = (pe == TrackedProp_Success && (r == 1 || r == 2)) ? r : 0;
        }
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < nc; i++) g_comps[i].role = roles[i];
        LeaveCriticalSection(&g_lock);
    }

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_numComps && n < 64; i++) {
        Comp &k = g_comps[i];
        bool target = IsTarget(k);
        if (k.axis == 2) {
            bool want = (target && add != 0) ? true : k.realBool;
            if (want != k.sentBool) { k.sentBool = want; todo[n++] = { k.self, k.patchIdx, k.handle, true, 0, want }; }
        } else if (k.axis == 1) {
            float want = target ? Combine(k.real, add) : k.real;
            if (fabsf(want - k.sent) > 0.0005f) { k.sent = want; todo[n++] = { k.self, k.patchIdx, k.handle, false, want, false }; }
        }
    }
    LeaveCriticalSection(&g_lock);

    t_depth++;
    for (int i = 0; i < n; i++) {
        Patched &p = g_patched[todo[i].patch];
        if (todo[i].isBool) p.updateBool(todo[i].self, todo[i].h, todo[i].b, 0);
        else p.updateScalar(todo[i].self, todo[i].h, todo[i].f, 0);
    }
    t_depth--;

    if (g_shm) {
        int sticks = 0;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_numComps; i++) if (g_comps[i].axis == 1 && g_comps[i].role) sticks++;
        LeaveCriticalSection(&g_lock);
        g_shm->hooked = sticks;
    }
}

// =====================================================================================
// The treadmill device (shows up in SteamVR, bindable manually)
// =====================================================================================
// Hand-written mirror of vr::ITrackedDeviceServerDriver (same virtual order).
class TreadmillDevice {
public:
    virtual EVRInitError Activate(uint32_t unObjectId);
    virtual void Deactivate() { id = k_unTrackedDeviceIndexInvalid; }
    virtual void EnterStandby() {}
    virtual void *GetComponent(const char *) { return nullptr; }
    virtual void DebugRequest(const char *, char *resp, uint32_t size) { if (size) resp[0] = 0; }
    virtual DriverPose_t *GetPose(DriverPose_t *out);   // MSVC-compatible "returns struct by value"

    void RunFrame(const AppState &s);
    ITrackedDeviceServerDriver *AsDriver() { return reinterpret_cast<ITrackedDeviceServerDriver *>(this); }

    uint32_t id = k_unTrackedDeviceIndexInvalid;
    VRInputComponentHandle_t hX = k_ulInvalidInputComponentHandle, hY = k_ulInvalidInputComponentHandle;
    float lastX = 2, lastY = 2;
};

static DriverPose_t MakePose() {
    DriverPose_t p;
    memset(&p, 0, sizeof(p));
    p.qWorldFromDriverRotation.w = 1;
    p.qDriverFromHeadRotation.w = 1;
    p.qRotation.w = 1;
    p.poseIsValid = false;                       // a treadmill has no position in the room
    p.deviceIsConnected = true;
    p.result = TrackingResult_Running_OK;
    return p;
}

DriverPose_t *TreadmillDevice::GetPose(DriverPose_t *out) { *out = MakePose(); return out; }

EVRInitError TreadmillDevice::Activate(uint32_t unObjectId) {
    id = unObjectId;
    PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer(id);
    g_ownContainer = c;
    VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, "DIY VR Treadmill");
    VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "vr-treadmill");
    VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "vrtreadmill");
    VRProperties()->SetStringProperty(c, Prop_ControllerType_String, "vrtreadmill");
    VRProperties()->SetInt32Property(c, Prop_ControllerRoleHint_Int32, TrackedControllerRole_Treadmill);
    VRProperties()->SetInt32Property(c, Prop_DeviceClass_Int32, TrackedDeviceClass_Controller);
    VRProperties()->SetBoolProperty(c, Prop_NeverTracked_Bool, true);
    VRProperties()->SetBoolProperty(c, Prop_DeviceIsWireless_Bool, false);
    VRProperties()->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, false);
    VRProperties()->SetBoolProperty(c, Prop_Identifiable_Bool, false);
    VRProperties()->SetStringProperty(c, Prop_InputProfilePath_String, "{00vrtreadmill}/input/vrtreadmill_profile.json");

    VRDriverInput()->CreateScalarComponent(c, "/input/joystick/x", &hX, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
    VRDriverInput()->CreateScalarComponent(c, "/input/joystick/y", &hY, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);

    DriverPose_t p = MakePose();
    VRServerDriverHost()->TrackedDevicePoseUpdated(id, p, sizeof(p));
    Log("VR Treadmill: treadmill device active (id %u)\n", id);
    return VRInitError_None;
}

void TreadmillDevice::RunFrame(const AppState &s) {
    if (id == k_unTrackedDeviceIndexInvalid) return;
    // When walking is already being blended into a controller, keep this device's own
    // joystick at zero so a manual binding can't double your speed.
    float x = (s.alive && !s.inject) ? s.x : 0.f;
    float y = (s.alive && !s.inject) ? s.y : 0.f;
    if (x != lastX) { VRDriverInput()->UpdateScalarComponent(hX, x, 0); lastX = x; }
    if (y != lastY) { VRDriverInput()->UpdateScalarComponent(hY, y, 0); lastY = y; }
}

// =====================================================================================
// Provider
// =====================================================================================
static TreadmillDevice g_device;
static bool g_lastAlive;

class Provider : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext *ctx) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        InitializeCriticalSection(&g_lock);
        OpenShared();
        InstallHooks(ctx);
        if (!VRServerDriverHost()->TrackedDeviceAdded("VRTREADMILL-001", TrackedDeviceClass_Controller, g_device.AsDriver()))
            Log("VR Treadmill: couldn't add the treadmill device (thumbstick blending still works)\n");
        return VRInitError_None;
    }
    void Cleanup() override {
        RemoveHooks();
        CloseShared();
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }
    const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
    void RunFrame() override {
        AppState s = ReadShared();
        if (s.alive != g_lastAlive) {
            g_lastAlive = s.alive;
            Log(s.alive ? "VR Treadmill: app connected\n" : "VR Treadmill: app not running\n");
        }
        InjectFrame(s);
        g_device.RunFrame(s);
        VREvent_t ev;
        while (VRServerDriverHost()->PollNextEvent(&ev, sizeof(ev))) {}
    }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
};

static Provider g_provider;

extern "C" __declspec(dllexport) void *HmdDriverFactory(const char *iface, int *ret) {
    if (strcmp(iface, IServerTrackedDeviceProvider_Version) == 0) return &g_provider;
    if (ret) *ret = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
