# DIY VR Treadmill (manual treadmill + taped mouse)

Walk in PC VR games by walking on a cheap manual treadmill. A mouse taped over the belt
reads its movement, and this app turns that into **holding W** or **pushing an Xbox
controller's left stick forward**, while your normal mouse/trackpad keeps working as usual.

- Picks out **one specific mouse** (the treadmill one) and leaves every other mouse alone
- Blocks the treadmill mouse from moving your cursor or clicking
- Output: keyboard key (W by default) **or** virtual Xbox 360 left stick
- Optional forward/backward mode (W/S, or stick forward/back)
- Simple window, settings save automatically, remembers the treadmill mouse between sessions

Windows 10/11, 64-bit.

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
   **whole folder** somewhere (e.g. `Documents\VRTreadmill`). Don't run it from inside the zip.
2. Open `VRTreadmill.exe`.
3. Click **Install** next to **Mouse driver** and accept the admin prompt.
4. Want controller mode? Also click **Install** next to **Controller driver**.
5. **Restart your PC.**

> Windows SmartScreen may warn about an unsigned app. Click *More info → Run anyway*.

## 3. Use it

1. Open `VRTreadmill.exe`. "Mouse driver" should say *Installed and running*.
2. Click **Detect**, then move **only** the treadmill belt (hands off your other mouse).
   It remembers this mouse from now on.
3. Pick an **Output**:
   - **Keyboard key (W)**: holds W while you walk.
   - **Controller left stick forward**: virtual Xbox controller. The game must support an
     Xbox controller.
4. Walk. The **Live** bar fills and shows *Holding W* or *Stick fwd 70%*.
5. Minimise the window (don't close it) and start your game.

### Settings

| Setting | What it does |
|---|---|
| Output | Keyboard key, or virtual Xbox controller left stick |
| Movement axis | Which direction of mouse movement counts (match how you mounted it) |
| Mode | Any direction = forward, or one way forward / other way backward |
| Swap forward/backward | Flip it if backward mode is the wrong way round |
| Push stick further the faster you walk | Controller mode only, experimental: may stay at full stick on most treadmills |
| Sensitivity | Movement needed to start walking. Raise it if you "walk" by yourself |
| Stop delay (ms) | How long to keep walking after the belt stops. Raise it if walking stutters |
| Forward / Backward key | Keys used in keyboard mode |
| Stop treadmill mouse moving the cursor | Leave on unless you want it to act as a mouse too |

### Recommended preset (tested on a real build)

These settings have been tested on a manual treadmill with the mouse mounted sideways to the belt.
They're a good starting point:

| Setting | Value |
|---|---|
| Output | Controller left stick forward |
| Movement axis | Horizontal (left/right) |
| Mode | Any direction = forward |
| Push stick further the faster you walk | On |
| Sensitivity | 15 |
| Stop delay (ms) | 250 |
| Stop treadmill mouse moving the cursor / clicking | On |
| Enabled | On |

If your mouse points along the belt instead of sideways, switch **Movement axis** to
**Vertical**. If walking stutters, raise **Stop delay**, and if you start "walking" while
standing still, raise **Sensitivity**.

### Troubleshooting
- **Game ignores W:** if the game runs as administrator, run `VRTreadmill.exe` as administrator too.
- **Game ignores controller mode:** some SteamVR-native games only read VR motion controllers.
  Use keyboard mode, or check the game's controller settings.
- **"Saved treadmill mouse not plugged in":** plug it in and reopen the app, or click Detect again.
- **Anti-cheat:** some online games with anti-cheat dislike the Interception driver.

### Uninstalling the drivers
- Mouse driver: open Command Prompt **as administrator** in the `driver` folder, run
  `install-interception.exe /uninstall`, then restart.
- Controller driver: Windows Settings → Apps → *ViGEm Bus Driver* → Uninstall.

---

## How it works

The [Interception](https://github.com/oblitum/Interception) driver lets the app see each mouse
separately and choose what to pass on. Strokes from the treadmill mouse are swallowed and
turned into a key press (`SendInput` with scan codes, so games see it) or a virtual Xbox 360
controller via [ViGEmBus](https://github.com/nefarius/ViGEmBus). Every other mouse is passed
straight through untouched.

## Building from source

Plain C, cross-compiled from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw):

```sh
git clone https://github.com/nefarius/ViGEmClient
./build.sh /path/to/llvm-mingw/bin ./ViGEmClient
```

Output: `VRTreadmill.exe`. Put it next to `interception.dll` (from the
[Interception release](https://github.com/oblitum/Interception/releases)) and a `driver` folder
holding `install-interception.exe` and the
[ViGEmBus installer](https://github.com/nefarius/ViGEmBus/releases) renamed to `ViGEmBus_setup.exe`.

## Credits & licences

- This app: MIT licence (see `LICENSE`).
- [Interception](https://github.com/oblitum/Interception) by Francisco Lopes: LGPL-3.0 for non-commercial use.
- [ViGEmBus / ViGEmClient](https://github.com/nefarius) by Benjamin Höglinger-Stelzer: BSD-3 / MIT.
