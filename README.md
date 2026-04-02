# ESP32 Multi-Tool v2.7

A combined wireless security tool for ESP32 with nRF24L01+PA+LNA, OLED display, LEDs, and toggle switches.

---

## Modes

| Mode | Description |
|------|-------------|
| WiFi Deauther | Scans APs and sends 802.11 deauth frames to kick clients off WiFi |
| Evil Twin | Clones a nearby AP (SSID + BSSID + channel), serves captive portal to harvest credentials |
| Jammer | nRF24 continuous carrier wave with selectable target: WiFi, Bluetooth, BLE, RC/Drone, or custom channel |

---

## Hardware

- ESP32 dev board
- 1x nRF24L01+PA+LNA module (100uF cap across VCC/GND)
- 0.96" SSD1306 OLED display (I2C)
- 4x LEDs: red, blue, green, yellow (220Ω resistors to GND)
- 5x SPDT slide switches

---

## Wiring

```
OLED (I2C)
  SDA  → D21  (GPIO 21)
  SCL  → D22  (GPIO 22)

nRF24 (VSPI) — solder 100uF cap across VCC/GND on each module
  CE   → D5   (GPIO 5)
  CSN  → TX2  (GPIO 17)   ← labeled TX2 on your board
  SCK  → D18  (GPIO 18)
  MOSI → D23  (GPIO 23)
  MISO → D19  (GPIO 19)
  VCC  → 3V3
  GND  → GND

LEDs (220Ω resistor in series to GND)
  RED    → D2   (GPIO 2)
  BLUE   → D25  (GPIO 25)
  GREEN  → D26  (GPIO 26)
  YELLOW → D27  (GPIO 27)

SPDT Slide Switches (only 2 used)
  Middle pin → GND
  Left pin   → GPIO (active LOW = mode ON)

  SW_JAMMER    → D32  (GPIO 32) — internal pullup, right pin optional
  SW_EVIL_TWIN → D33  (GPIO 33) — internal pullup, right pin optional
```

---

## How to Enter a Mode

### Method 1 — Switches (fastest)
Slide the corresponding switch to the active position. The OLED switches immediately.
Slide it back to exit and return to idle.

| Switch | Mode activated |
|--------|---------------|
| D32 | Jammer |
| D33 | Evil Twin |

> Only one switch should be ON at a time. If multiple are ON, the first one detected wins (order above).

### Method 2 — Hold Button (for other modes or when no cables free)
1. Make sure **all switches are OFF** (idle screen showing)
2. **Hold the BOOT button** (built into ESP32 board, ~700ms)
3. Mode name appears in a box on screen, **cycling every 1000ms**
4. **Release** when you see the mode you want → enters it
5. Triple click to exit back to idle

> Evil Twin, NRF Cap, and NRF Replay are only accessible this way (no dedicated switch).

---

## Button Reference

The BOOT button (built into the ESP32 board, labeled BOOT or IO0) handles all in-mode navigation.

| Press type | How to do it |
|------------|-------------|
| Single click | Press and release quickly once |
| Double click | Two quick presses |
| Triple click | Three quick presses |
| Hold | Press and hold ~700ms until screen reacts |

### Button actions per mode

#### WiFi Deauther
| Button | Action |
|--------|--------|
| Single | Select next AP in list |
| Double | Toggle deauth ON/OFF on selected AP |
| Triple | Rescan for APs (or exit if using button nav) |

> Run this mode before Evil Twin to populate the AP list.

#### Evil Twin
| Button | Action |
|--------|--------|
| Single (not live) | Cycle through scanned SSIDs to clone |
| Double (not live) | Launch the twin (clones SSID + BSSID + channel) |
| Double (live) | Stop the twin |
| Triple (live) | Open OLED credentials viewer |
| Triple (not live) | Exit mode |

**OLED Credentials Viewer** (triple click while live):
| Button | Action |
|--------|--------|
| Single | Next captured credential |
| Double | Back to status screen |
| Triple | Back to status screen |

#### Jammer
**Jammer OFF (target selection):**
| Button | Action |
|--------|--------|
| Single | Cycle to next jam target (WIFI → BLUETOOTH → BLE → RC/DRONE → CUSTOM → ...) |
| Single (CUSTOM target) | Increment channel (+1) |
| Double | Start jamming selected target |
| Triple | Exit to idle |

**Jammer ON:**
| Button | Action |
|--------|--------|
| Double | Stop jammer |
| Single (CUSTOM only) | Increment channel (+1, updates live) |
| Triple | Stop and exit to idle |

**Jam targets:**

| Target | Channels | Behaviour |
|--------|----------|-----------|
| WIFI | nRF24 ch 1–83 (2401–2483 MHz) | Sweeps all 2.4 GHz WiFi channels, 2 ms/hop |
| BLUETOOTH | nRF24 ch 2–80 (2402–2480 MHz) | Sweeps BT classic range, 1 ms/hop |
| BLE | ch 2, 26, 80 | Rotates BLE advertising channels, 1 ms/hop |
| RC/DRONE | nRF24 ch 0–125 (full 2.4 GHz) | Sweeps entire band, 2 ms/hop |
| CUSTOM | Single channel, you pick | Fixed carrier on chosen channel |

---

## LED Indicators

| LED | Meaning |
|-----|---------|
| RED | Deauth firing / Jammer active (blinks) |
| BLUE | nRF24 capturing |
| GREEN | Credential captured by Evil Twin |
| YELLOW | nRF24 packet received / Jammer sweep tick |

---

## Evil Twin — Getting the Credentials

### Option A — OLED (works on any phone including iOS)
1. Flip D33 switch (or hold BOOT → cycle to EVIL TWIN → release)
2. Wait for scan to finish, single click to pick the target SSID
3. Double click to launch the twin
4. Victims connecting see a realistic Free WiFi portal with email/password fields
5. They try to log in → "User not found" error → they try signing up → both attempts are captured
6. GREEN LED blinks for each capture
7. Triple click → OLED shows captured credentials one at a time
8. Single click to scroll through them, double click to go back

### Option B — Web page (Android/laptop, not always iOS)
1. While Evil Twin is live, connect a device to `192.168.4.1`
2. Visit `http://192.168.4.1/creds`
3. Enter credentials when prompted:
   - Username: `admin`
   - Password: whatever `CREDS_PASSWORD` is set to in the sketch (default: `changeme123`)
4. Page shows all captured credentials
5. Click `[Clear all]` to wipe the file

> iOS may refuse to browse to the AP because it detects no internet. Use the OLED viewer instead.

---

## Jammer — What Each Target Hits

| Target | What gets hit | Notes |
|--------|--------------|-------|
| WIFI | All 2.4 GHz WiFi (ch 1–13) | Partial interference per channel — use Deauther for clean disconnects |
| BLUETOOTH | Bluetooth Classic (BR/EDR) | BT hops 79 channels — sweep disrupts but not guaranteed kill |
| BLE | BLE device discovery / pairing | Hits advertising channels only; established BLE connections survive |
| RC/DRONE | nRF24 remotes, ESB toys, 2.4G drones | Very effective on fixed-frequency toys |
| CUSTOM | Single chosen channel | Use for Zigbee (ch 11/15/20/25 → nRF24 ch 5/15/25/30), specific WiFi ch, etc. |

> The nRF24 cannot do true wideband jamming simultaneously — it transmits on one channel at a time. Sweep mode is effective because most protocols dwell on each channel long enough to be hit.

WiFi channel to nRF24 channel reference (CUSTOM mode):

| WiFi ch | nRF24 ch |
|---------|----------|
| 1 | 12 |
| 6 | 37 |
| 11 | 62 |
| 13 | 72 |

---

## Recommended Workflow

```
1. Boot up → idle screen (switch states shown)

2. To attack a WiFi network:
   a. Flip D33 (Deauth) → scans APs automatically
   b. Single click to pick target AP
   c. Double click to start firing deauth frames
   d. Flip D33 off, flip D34 (Evil Twin)
   e. Single click to find the same SSID
   f. Double click to launch (clones BSSID + channel)
   g. Combine with deauth to push clients onto your fake AP
   h. Triple click to view captured creds on OLED

3. To jam 2.4GHz RC/drone:
   a. Flip D32 (JAMMER switch) OR hold BOOT → cycle to JAMMER → release
   b. Single click to cycle target → select RC/DRONE
   c. Double click → starts sweep jam across full 2.4 GHz band
   d. Double click again → stop  /  Triple click → exit
```

---

## Arduino Setup

### Partition Scheme
The sketch is large. In Arduino IDE:
```
Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)
```

### Libraries (Arduino Library Manager)
- `RF24` by TMRh20
- `Adafruit SSD1306`
- `Adafruit GFX Library`
- ESP32 Arduino core v3.x (includes WiFi, BLE, DNSServer, WebServer, SPIFFS)

### Customisation
Change the credentials page password at the top of the sketch:
```cpp
#define CREDS_PASSWORD "changeme123"
```

---

## Disclaimer

For educational and authorized testing only. Only use on networks and devices you own or have explicit permission to test.
