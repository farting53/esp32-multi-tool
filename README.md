# ESP32 Multi-Tool v2.3

A combined wireless security tool for ESP32 with nRF24L01+PA+LNA, OLED display, LEDs, and toggle switches.

---

## Modes

| Mode | Description |
|------|-------------|
| BLE Scanner | Scans nearby BLE devices, identifies Apple, Tile, HID by manufacturer data |
| WiFi Deauther | Scans APs and sends 802.11 deauth frames to kick clients off WiFi |
| Evil Twin | Clones a nearby AP (SSID + BSSID + channel), serves captive portal to harvest credentials |
| nRF24 Capture | Promiscuous capture of 2.4GHz ESB packets (doorbells, RC cars, remote sockets) |
| nRF24 Replay | Retransmit captured nRF24 packets |
| Jammer | nRF24 continuous carrier wave — fixed channel or sweep. Jams nRF24 devices, RC toys, Zigbee, partial WiFi |

---

## Hardware

- ESP32 dev board
- 2x nRF24L01+PA+LNA modules (100uF cap across each VCC/GND)
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

SPDT Slide Switches
  Middle pin → GND
  Left pin   → GPIO (active LOW = mode ON)
  Right pin  → 3.3V (required for input-only pins D34/D35/VP)

  SW_BLE     → D32  (GPIO 32) — has internal pullup, right pin optional
  SW_DEAUTH  → D33  (GPIO 33) — has internal pullup, right pin optional
  SW_TWIN    → D34  (GPIO 34) — input-only, right pin MUST go to 3V3
  SW_NRF_CAP → D35  (GPIO 35) — input-only, right pin MUST go to 3V3
  SW_NRF_REP → VP   (GPIO 36) — input-only, right pin MUST go to 3V3
```

---

## How to Enter a Mode

### Method 1 — Switches (fastest)
Slide the corresponding switch to the active position. The OLED switches immediately.
Slide it back to exit and return to idle.

| Switch | Mode activated |
|--------|---------------|
| D32 | BLE Scanner |
| D33 | WiFi Deauther |
| D34 | Evil Twin |
| D35 | nRF24 Capture |
| VP | nRF24 Replay |

> Only one switch should be ON at a time. If multiple are ON, the first one detected wins (order above).

### Method 2 — Hold Button (for Jammer or when no cables free)
1. Make sure **all switches are OFF** (idle screen showing)
2. **Hold the BOOT button** (built into ESP32 board, ~700ms)
3. Mode name appears in a box on screen, **cycling every 600ms**
4. **Release** when you see the mode you want → enters it
5. Triple click to exit back to idle

> Jammer mode is only accessible this way (no dedicated switch).

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

#### BLE Scanner
| Button | Action |
|--------|--------|
| Single | Scroll to next device |
| Double | Start a fresh rescan |
| Triple | Scroll to previous device (or exit if using button nav) |

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
| Triple (button nav) | Exit mode |

**OLED Credentials Viewer** (after triple click while live):
| Button | Action |
|--------|--------|
| Single | Next captured credential |
| Double or Triple | Back to status screen |

#### nRF24 Capture
| Button | Action |
|--------|--------|
| Single | Channel +1 (0–125) |
| Double | Channel -1 |
| Triple | Clear captured packet buffer (or exit if button nav) |

#### nRF24 Replay
| Button | Action |
|--------|--------|
| Single | Next captured packet |
| Double | Replay selected packet (fires 5x) |
| Triple | Clear packet buffer (or exit if button nav) |

#### Jammer
| Button | Action |
|--------|--------|
| Single | Channel +1 |
| Triple | Channel -1 |
| Double (1st press) | Start jamming — fixed on selected channel |
| Double (2nd press) | Switch to sweep mode (hops all 126 channels, 2ms each) |
| Double (3rd press) | Stop jammer completely |

---

## LED Indicators

| LED | Meaning |
|-----|---------|
| RED | Deauth firing / Apple device found / Jammer active (blinks) |
| BLUE | nRF24 capturing |
| GREEN | Credential captured by Evil Twin |
| YELLOW | BLE device very close (<50cm) / nRF24 packet received / Jammer sweep tick |

---

## Evil Twin — Getting the Credentials

### Option A — OLED (works on any phone including iOS)
1. Launch Evil Twin (double click after selecting SSID)
2. Wait for victims to submit the login form (GREEN LED blinks each time)
3. Triple click → OLED shows captured credentials one at a time
4. Single click to scroll through them

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

## Jammer — What it Actually Hits

| Target | Result |
|--------|--------|
| nRF24 devices (doorbells, remote sockets) | Effectively jammed |
| 2.4GHz RC toys / drones | Effectively jammed |
| Zigbee / Thread devices | Jammed on overlapping channels |
| WiFi (specific channel) | Partial interference — use deauther for WiFi disruption |
| BLE | Not jammed — BLE hops 40 channels at ~1600/sec |
| Full wideband 2.4GHz jam | Not possible with this hardware |

WiFi channel to nRF24 channel reference:

| WiFi ch | nRF24 ch to target |
|---------|--------------------|
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

3. To capture and replay an nRF24 device (doorbell etc):
   a. Flip D35 (NRF Cap) → starts listening
   b. Single/Double click to tune channel
   c. Trigger the device (press doorbell etc) → packet captured
   d. Flip D35 off, flip VP (NRF Replay)
   e. Double click to replay the packet

4. To jam 2.4GHz RC/drone:
   a. All switches off → hold BOOT → cycle to JAMMER → release
   b. Single/Triple click to tune channel
   c. Double click once → fixed jam
   d. Double click again → sweep mode (broader)
   e. Triple click → exit
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
