// Shared memory between the VRTreadmill app (writer) and the SteamVR driver (reader).
#pragma once
#include <stdint.h>

#define TREADMILL_SHM_NAME  L"Local\\VRTreadmillJoystick"
#define TREADMILL_SHM_MAGIC 0x564D5453u

typedef struct {
    uint32_t magic;
    uint32_t heartbeat;   /* GetTickCount() of the last update; driver ignores data older than 500 ms */
    float x;              /* -1..1, right is positive (unused for now) */
    float y;              /* -1..1, forward is positive */
    uint32_t hand;        /* controller to blend walking into: 1 = left, 2 = right */
    uint32_t inject;      /* 1 = blend into that controller's thumbstick (KAT-style) */
    uint32_t driverSeen;  /* written by the driver: GetTickCount() while SteamVR has it loaded */
    uint32_t hooked;      /* written by the driver: number of controller sticks it found */
} TreadmillShared;
