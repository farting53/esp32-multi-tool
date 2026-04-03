# ESP32 Evil Twin v1.0

Dedicated Evil Twin / captive portal credential harvester for ESP32.
Clones a nearby AP (SSID + BSSID + channel) and serves a realistic Free WiFi login page to collect email/password credentials from connecting devices.

---

## What it does

1. Scans for nearby WiFi networks on boot
2. You pick a target AP from the list
3. Launches a fake AP with the exact same SSID, BSSID, and channel
4. Victims connecting see a convincing "Free Public WiFi" login portal
5. Login attempts → "No account found" error (credential captured)
6. Signup flow → "Account created" message (credential captured)
7. All captures shown on OLED or via web admin page

---

## Hardware

- ESP32 dev board
- 0.96" SSD1306 OLED display (I2C)
- 4x LEDs: red, blue, green, yellow (220Ω resistors to GND)
- 2x SPDT slide switches

---

## Wiring

```
OLED (I2C)
  SDA → D21  (GPIO 21)
  SCL → D22  (GPIO 22)

LEDs (220Ω resistor in series to GND)
  RED    → D2   (GPIO 2)   — blinks while twin is live
  BLUE   → D25  (GPIO 25)
  GREEN  → D26  (GPIO 26)  — blinks on each credential captured
  YELLOW → D27  (GPIO 27)

SPDT Slide Switches
  Middle pin → GND
  Left pin   → GPIO (active LOW)

  SW_START → D32  (GPIO 32)  internal pullup
  SW_SCAN  → D33  (GPIO 33)  internal pullup
```

---

## Button Reference

The BOOT button (GPIO 0, built-in on ESP32 board) handles all navigation.

| State | Single click | Double click | Triple click |
|-------|-------------|-------------|-------------|
| **SCANNING** | — | — | — |
| **SELECT** | Next AP in list | Launch twin on selected AP | Rescan |
| **LIVE** | Cycle info page | Stop twin | Open OLED creds viewer |
| **CREDS** | Next credential | Back to previous screen | Back to previous screen |

---

## Switch Reference

| Switch | Action |
|--------|--------|
| D32 ON  | Launch twin on currently selected AP |
| D32 OFF | Stop twin (if running) |
| D33 ON  | Trigger a fresh WiFi rescan |

---

## Workflow

```
1. Boot → auto-scan starts
2. Scan completes → AP list (sorted strongest first)
3. Single click to scroll, pick the target SSID
4. Double click (or flip D32) → twin launches
5. RED LED blinks while live
6. Victims connect and try to log in or sign up
7. GREEN LED flashes on each capture
8. Triple click → OLED shows captured creds one at a time
9. Single click to scroll, double click to go back
10. Double click (or flip D32 off) → stop twin → back to AP list
```

---

## OLED States

### SCANNING
Shows a scanning animation with elapsed time. Automatically advances to SELECT when done.

### SELECT
Lists up to 3 APs at a time with signal strength bars and channel number.
Selected AP is highlighted. Bottom row shows full BSSID and RSSI.

### LIVE — Page 1 (stats)
```
LIVE  [N creds]
SSID: <target SSID>
ch:X  AA:BB:CC
IP:   192.168.4.1
Up: Xm Xs
```

### LIVE — Page 2 (last capture)
Shows the most recently captured credential (email/password or signup details).

### CREDS viewer
Shows each capture one at a time. Displays type (LOGIN or SIGNUP), name (signups), email, and password.

---

## Getting credentials

### Option A — OLED
1. Triple click while live → OLED creds viewer
2. Single click scrolls through captures
3. Double click to go back

### Option B — Web page
While twin is live, connect a device to `192.168.4.1`:
1. Visit `http://192.168.4.1/creds`
2. Username: `admin`
3. Password: whatever `CREDS_PASSWORD` is set to in the sketch (default: `changeme123`)
4. Shows all captures with LOGIN/SIGNUP labels and timestamps
5. Click `[Clear all]` to wipe the file

---

## Portal flow (what victims see)

```
Victim connects → captive portal opens automatically
  ↓
"Free Public WiFi" login page
  ↓ (enter email + password, submit)
"No account found with that email" error  ← LOGIN captured
  ↓ (click "Create an Account")
Signup page: first name, last name, email, password, confirm
  ↓ (fill in and submit)
"Account created! You can now sign in"    ← SIGNUP captured
  ↓ (go back and try logging in again)
"No account found..."                     ← another LOGIN captured
```

Client-side validation:
- Invalid email format → error before submitting
- Password under 8 characters → error before submitting
- Mismatched confirm password (signup) → error before submitting

---

## Arduino Setup

### Partition Scheme
In Arduino IDE:
```
Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)
```

### Libraries (Arduino Library Manager)
- `Adafruit SSD1306`
- `Adafruit GFX Library`
- ESP32 Arduino core v3.x (includes WiFi, DNSServer, WebServer, SPIFFS)

### Customisation
Change the credentials page password at the top of the sketch:
```cpp
#define CREDS_PASSWORD "changeme123"
```

---

## Disclaimer

For educational and authorized testing only. Only use on networks and devices you own or have explicit permission to test.
