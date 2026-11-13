# DIY VR Treadmill (manual treadmill + taped mouse)

Walk in PC VR games by walking on a cheap manual treadmill. A mouse taped over the belt
reads its movement, and this app turns it into walking in SteamVR, much like a KAT Walk:
the bundled **SteamVR driver** blends your walking speed into your real VR controller's
thumbstick, so any game with thumbstick walking works with no per-game setup.

- Picks out **one specific mouse** (the treadmill one) and leaves every other mouse alone
- **SteamVR driver**: shows up in SteamVR as a treadmill device and drives your controller's
  thumbstick (or Vive wand touchpad). No admin rights or reboot needed to install
- Walk faster = move faster (calibrate once for your treadmill)
- No extra Windows drivers needed. The old Interception (mouse) and ViGEmBus (Xbox
  controller) drivers are now **optional** extras
- Also outputs a **W key** or a **virtual Xbox controller** for non-VR or keyboard-only games
- Optional forward/backward mode, settings save automatically

Windows 10/11, 64-bit, SteamVR (works with Index, Vive, Quest via Steam Link / Virtual
Desktop / Air Link, WMR, Pico).

---

## 1. Build the treadmill

### You need
- A **manual (non-motorised) treadmill**. Flat-belt ones are cheap second hand. Curved
  "self-powered" ones are even better but cost more.
- A **wired optical or laser mouse**. Cheap is fine. Wired avoids a wireless mouse going to
  sleep mid-game. Laser mice track glossy belts better.
- Strong tape (gaffer/duct) or a small bracket, and optionally cable ties.

### Mount the mouse
1. Find a spot where the **belt is visible but your feet never land**: the front edge of the
   belt just behind the front roller/hood, or a side strip of belt outside where you walk.
2. Place the mouse **sensor face-down, resting lightly on the belt**. It should sit flat, as
   if on a mouse mat, with the belt sliding underneath it.
3. **Tape the mouse to the treadmill frame, never to the belt.** The belt must move freely
   under it. A bit of foam or a bent strip of card above the mouse helps keep gentle, even
   pressure so it doesn't lift when the belt bounces.
4. Note which way the belt moves relative to the mouse. If the mouse is **sideways** to the
   belt, use **Horizontal** in the app; if it points **along** the belt, use **Vertical**.
5. Run the cable away from the belt and rollers and tie it down.

**Tracking tips:** if movement is jumpy or missing, check the mouse sits flat (no gap), try a
laser mouse on shiny belts, and clean dust off the sensor. Textured matte belts track best.

### Safety (please read)
You can't see the treadmill in VR.
- Use a treadmill with **handrails** and keep a hand on them, or build a waist rail / harness.
- Start slow. Manual treadmills can speed up quickly once the belt gets going.
- Clear the space around it and keep the headset cable away from the belt.
- Have someone nearby the first few times.

---

## 2. Install the app

1. Download **`VRTreadmill.zip`** from the [Releases](../../releases) page and extract the
   **whole folder** somewhere permanent (e.g. `Documents\VRTreadmill`). Don't run it from
   inside the zip, and don't move the folder afterwards (SteamVR loads the driver from it).
2. Open `VRTreadmill.exe`.
3. Click **Install** next to **SteamVR driver** (no admin prompt, no reboot).
4. Start (or restart) SteamVR. The app should now say *Running in SteamVR ✔ (2 controllers)*.

> Windows SmartScreen may warn about an unsigned app. Click *More info → Run anyway*.
> If SteamVR says an add-on was blocked, enable **00vrtreadmill** in SteamVR Settings →
> Startup / Shutdown → Manage Add-ons.

## 3. Use it

1. Open `VRTreadmill.exe`.
2. Click **Detect**, then move **only** the treadmill belt (hands off your other mouse).
   It remembers this mouse from now on.
3. Leave **Output** on **SteamVR (controller thumbstick)** and **Walk with** on **Left
   controller stick** (most games walk with the left stick).
4. Click **Calibrate**, walk at your fastest pace for 5 seconds, and the app learns your
   treadmill's top speed. Walking slower then moves you slower in game.
5. Walk. The **Live** bar fills and shows *Walking fwd 70%*.
6. Minimise the window (don't close it) and play. In game, choose **smooth / thumbstick
   locomotion** (not teleport). Walking goes in the direction the game uses for your
   stick, usually where your head or left controller points.

You can still use the real thumbstick at the same time; the treadmill is added on top.

### Settings

| Setting | What it does |
|---|---|
| Output | **SteamVR** (blended into your controller's thumbstick), **Keyboard key** (holds W), or **Xbox controller** (needs the optional controller driver) |
| Walk with | Which controller's stick the treadmill drives in SteamVR mode |
| Read mouse with | **Windows**: no driver needed, but the treadmill mouse also nudges the desktop cursor (harmless in VR). **Mouse driver**: uses Interception to fully hide it |
| Movement axis | Which direction of mouse movement counts (match how you mounted it) |
| Mode | Any direction = forward, or one way forward / other way backward |
| Swap forward/backward | Flip it if backward mode is the wrong way round |
| Walk faster = move faster | Off = always full speed while walking |
| Top speed / Calibrate | The belt speed that gives full speed. Click Calibrate and walk at full pace to set it |
| Sensitivity | Movement needed to start walking. Raise it if you "walk" by yourself |
| Stop delay (ms) | How long to keep walking after the belt stops. Raise it if walking stutters |
| Keys | Forward / backward keys for keyboard mode |
| Hide treadmill mouse from the cursor | Mouse driver mode only |

### Recommended preset (tested on a real build)

Tested on a manual treadmill with the mouse mounted sideways to the belt:

| Setting | Value |
|---|---|
| Output | SteamVR (controller thumbstick) |
| Walk with | Left controller stick |
| Read mouse with | Windows (no driver needed) |
| Movement axis | Horizontal (left/right) |
| Mode | Any direction = forward |
| Walk faster = move faster | On |
| Top speed | Click **Calibrate** (depends on your treadmill) |
| Sensitivity | 15 |
| Stop delay (ms) | 250 |

If your mouse points along the belt instead of sideways, switch **Movement axis** to
**Vertical**. If walking stutters, raise **Stop delay**, and if you start "walking" while
standing still, raise **Sensitivity**.

### Troubleshooting
- **SteamVR driver says "Running - turn controllers off/on":** the driver loaded after your
  controllers connected, so it hasn't found their sticks yet. Turn the controllers off and
  on again (or restart SteamVR with them off, then switch them on).
- **Driver shows "Installed - start/restart SteamVR" while SteamVR is running:** check it's
  enabled in SteamVR Settings → Startup / Shutdown → Manage Add-ons, then restart SteamVR.
- **A game doesn't move you:** make sure it's set to smooth/thumbstick locomotion, and try
  **Walk with: Right controller stick** for games that walk with the right hand.
- **Teleport-only games:** they have no smooth walking, so a treadmill can't drive them.
- **The treadmill device in SteamVR:** it also appears as its own treadmill device you can
  bind manually in SteamVR's controller bindings. You normally don't need to: in SteamVR mode
  its own joystick stays at zero so it can't double your speed.
- **Game ignores W:** if the game runs as administrator, run `VRTreadmill.exe` as administrator too.
- **Old mouse driver won't install / a game refuses it:** you don't need it. Set **Read mouse
  with** to **Windows**. Some anti-cheat systems refuse the Interception driver, so uninstall it
  (below) if a game complains.
- **"Saved treadmill mouse not plugged in":** plug it in, or click Detect again.

### Uninstalling
- SteamVR driver: click **Remove** next to SteamVR driver, then restart SteamVR.
- Mouse driver (if you installed it): open Command Prompt **as administrator** in the `driver`
  folder, run `install-interception.exe /uninstall`, then restart.
- Controller driver (if you installed it): Windows Settings → Apps → *ViGEm Bus Driver* → Uninstall.

---

## How it works

- **Reading the treadmill:** the app uses Windows Raw Input, which reports which physical mouse
  each movement came from, so only the treadmill mouse counts. Optionally the
  [Interception](https://github.com/oblitum/Interception) driver can be used instead, which also
  stops the treadmill moving the cursor.
- **SteamVR driver (`00vrtreadmill`):** a small SteamVR driver, registered with SteamVR's own
  `vrpathreg` tool. Like KAT's gateway driver, it is named so it loads before the controller
  drivers. It wraps SteamVR's driver input interface, notes which inputs are controller
  thumbsticks/touchpads, and adds the treadmill's speed to the chosen controller's stick. To
  games it is simply the stick being pushed forward. It also registers a device with SteamVR's
  built-in *treadmill* role. The app and driver talk through a small block of shared memory;
  if the app closes, walking stops within half a second.
- **Other outputs:** a held key via `SendInput` scan codes, or a virtual Xbox 360 controller via
  [ViGEmBus](https://github.com/nefarius/ViGEmBus).

## Building from source

Plain C (app) and C++ (driver), cross-compiled from Linux with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw):

```sh
git clone https://github.com/nefarius/ViGEmClient
./build.sh /path/to/llvm-mingw/bin ./ViGEmClient
```

Output: `VRTreadmill.exe` and `driver/00vrtreadmill/bin/win64/driver_00vrtreadmill.dll`.
Release folder layout:

```
VRTreadmill/
  VRTreadmill.exe
  interception.dll                       (optional, from the Interception release)
  steamvr_driver/00vrtreadmill/          (copy of driver/00vrtreadmill)
  driver/install-interception.exe        (optional)
  driver/ViGEmBus_setup.exe              (optional, ViGEmBus installer)
```

## Credits & licences

- This app and driver: MIT licence (see `LICENSE`).
- [OpenVR](https://github.com/ValveSoftware/openvr) driver header by Valve: BSD-3 (see `driver/include/OPENVR_LICENSE`).
- [Interception](https://github.com/oblitum/Interception) by Francisco Lopes: LGPL-3.0 for non-commercial use.
- [ViGEmBus / ViGEmClient](https://github.com/nefarius) by Benjamin Höglinger-Stelzer: BSD-3 / MIT.
