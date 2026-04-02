# ESP32 Multi-Tool

A combined wireless security/hacking tool for ESP32 with nRF24L01+PA+LNA, OLED display, LEDs, and toggle switches.

## Modes

| Mode | Description |
|------|-------------|
| BLE Scanner | Scans for nearby BLE devices, identifies Apple, Tile, HID devices by manufacturer data |
| WiFi Deauther | Scans APs and sends 802.11 deauth frames to disconnect clients |
| Evil Twin AP | Clones a nearby SSID and serves a captive portal to harvest credentials |
| nRF24 Capture | Promiscuous capture of 2.4GHz ESB packets (doorbells, RC cars, remote sockets) |
| nRF24 Replay | Retransmit captured nRF24 packets |

## Hardware

- ESP32 dev board
- 2x nRF24L01+PA+LNA modules
- 0.96" SSD1306 OLED display (I2C)
- 4x LEDs (red, blue, green, yellow) with 220Ω resistors
- 5x toggle switches

## Wiring

Board label → GPIO number mapping used below.

```
OLED (I2C)
  SDA  → D21  (GPIO 21)
  SCL  → D22  (GPIO 22)

nRF24 (VSPI) — put 100uF cap across VCC/GND on each module
  CE   → D5   (GPIO 5)
  CSN  → TX2  (GPIO 17)  ← labeled TX2 on board
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

Switches (active LOW — connect pin to GND when pressed)
  SW_UP     → D32  (GPIO 32) — internal pullup, no resistor needed
  SW_DOWN   → D33  (GPIO 33) — internal pullup, no resistor needed
  SW_SELECT → D34  (GPIO 34) — input-only, needs 10kΩ from pin to 3V3
  SW_BACK   → D35  (GPIO 35) — input-only, needs 10kΩ from pin to 3V3
  SW_FIRE   → VP   (GPIO 36) — input-only, needs 10kΩ from pin to 3V3
```

## Controls

| Switch | Menu | BLE | Deauth | Evil Twin | nRF Cap | nRF Replay |
|--------|------|-----|--------|-----------|---------|------------|
| UP     | scroll up | prev device | prev AP | prev SSID | chan+ | prev pkt |
| DOWN   | scroll dn | next device | next AP | next SSID | chan- | next pkt |
| SELECT | enter mode | — | pick target | — | — | fire replay |
| BACK   | — | → menu | → menu | stop/menu | stop/menu | → menu |
| FIRE   | — | scan | scan/toggle | launch AP | cap on/off | — |

## LED Indicators

| LED | Meaning |
|-----|---------|
| RED | Deauth active / Apple BLE device detected |
| BLUE | nRF24 capturing / HID BLE device detected |
| GREEN | Credential captured (Evil Twin) |
| YELLOW | BLE device within 50cm / nRF24 packet received |

## Libraries Required

Install via Arduino Library Manager:
- `RF24` by TMRh20
- `Adafruit SSD1306`
- `Adafruit GFX Library`
- ESP32 Arduino core (includes WiFi, BLE, DNSServer, WebServer, SPIFFS)

## Notes

- Run **Deauth** mode first to populate AP list before using **Evil Twin**
- **nRF24 Capture → Replay**: capture packets first, then switch to Replay mode — packets persist between mode switches
- Deauth only works against WPA2 and older networks (WPA3 ignores unauthenticated deauth frames)
- GPIOs 34/35/36 are input-only on ESP32 — they have no internal pullup, so external 10kΩ resistors to 3.3V are required

## Disclaimer

For educational and authorized testing use only. Only use on networks and devices you own or have explicit permission to test.
