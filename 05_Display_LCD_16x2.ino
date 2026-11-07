/*
  RIT Control - 05 DISPLAY: LCD 16x2 (I2C)   board: NodeMCU 1.0 (ESP-12E Module)
  App card: Display > LCD  (Send, Live, Clear display)

  FIXED VERSION - cures the "row of strange symbols" problem:
    1) waits for the LCD to finish powering up before the first command
    2) finds the I2C address by itself (0x27, 0x3F ... no need to edit it)
    3) starts the LCD again after Wi-Fi is connected (a bad first start can never stay)
    4) keeps the same app routes:  /display?m=lcd&t=text   /clear?m=lcd   /ping   /status
    5) extra:  http://<board-ip>/reinit  restarts the LCD without re-uploading

  LIBRARY to install (Sketch > Include Library > Manage Libraries):
    "LiquidCrystal I2C" by Frank de Brabander

  WIRING (I2C backpack on the LCD)
    SDA -> D2     SCL -> D1     VCC -> VIN (5V, NOT 3V3)     GND -> G

  Still strange symbols or blocks?  1) turn the blue contrast screw slowly,
  2) make sure VCC really goes to VIN (5V) and the 4 wires are not loose,
  3) open Serial Monitor (115200) - it prints which I2C address was found.
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

const char* PROJECT_TYPE = "lcd";
const char* CAPS_JSON = "[\"lcd\"]";
String statusJson();   // defined below

// ---------- YOUR WI-FI (change these two lines) ----------
const char* WIFI_SSID   = "Ram";
const char* WIFI_PASS   = "12345678";
const char* DEVICE_NAME = "NodeMCU-1";
// If your Wi-Fi is not found in 20 seconds the board ALSO makes its own hotspot and keeps trying your Wi-Fi:
//   name "NodeMCU-Control", password "12345678", address 192.168.4.1

ESP8266WebServer server(80);

void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
void sendJson(const String& s) { cors(); server.send(200, "application/json", s); }
void sendErr(const String& m)  { sendJson("{\"ok\":0,\"err\":\"" + m + "\"}"); }

int rssiNow() { return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0; }
String ipStr() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return WiFi.softAPIP().toString();                 // hotspot fallback: 192.168.4.1
}

// ======================= LCD =======================
LiquidCrystal_I2C* lcd = nullptr;
uint8_t lcdAddr = 0x27;
bool lcdFound = false;
String lcdText[2] = {"", ""};
int lcdOff[2] = {0, 0};
unsigned long lcdT = 0;

// look for the LCD backpack on the I2C bus (usually 0x27 or 0x3F)
uint8_t findLcdAddress() {
  const uint8_t first[] = {0x27, 0x3F};
  for (uint8_t i = 0; i < 2; i++) {
    Wire.beginTransmission(first[i]);
    if (Wire.endTransmission() == 0) { lcdFound = true; return first[i]; }
  }
  for (uint8_t a = 0x20; a <= 0x3F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { lcdFound = true; return a; }
  }
  lcdFound = false;
  return 0x27;
}

// (re)start the LCD cleanly
void lcdStart() {
  if (lcd) { delete lcd; lcd = nullptr; }
  lcd = new LiquidCrystal_I2C(lcdAddr, 16, 2);
  delay(250);                  // LCD controller needs time after power-up
  lcd->init();                 // full reset + 4-bit mode setup
  delay(20);
  lcd->backlight();
  lcd->clear();
  delay(5);
}

String lcdWindow(int row) {
  String s = lcdText[row];
  if (s.length() <= 16) { while (s.length() < 16) s += ' '; return s; }
  s += "    ";
  String w = "";
  for (int i = 0; i < 16; i++) w += s[(lcdOff[row] + i) % s.length()];
  return w;
}
void lcdDraw() {
  if (!lcd) return;
  for (int r = 0; r < 2; r++) { lcd->setCursor(0, r); lcd->print(lcdWindow(r)); }
}
// keep only plain characters the LCD can show (everything else becomes '?')
String lcdClean(const String& in) {
  String o = "";
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\r') continue;
    if (c == '\n' || (c >= 32 && c <= 126)) o += c; else o += '?';
  }
  return o;
}
void lcdSet(String t) {
  t = lcdClean(t);
  int nl = t.indexOf('\n');
  lcdText[0] = (nl < 0) ? t : t.substring(0, nl);
  lcdText[1] = (nl < 0) ? "" : t.substring(nl + 1);
  int nl2 = lcdText[1].indexOf('\n');
  if (nl2 >= 0) lcdText[1] = lcdText[1].substring(0, nl2);
  lcdOff[0] = lcdOff[1] = 0;
  lcdDraw();
}
void lcdTick() {                            // scroll long lines
  if (millis() - lcdT < 350) return;
  lcdT = millis();
  bool any = false;
  for (int r = 0; r < 2; r++)
    if (lcdText[r].length() > 16) { lcdOff[r] = (lcdOff[r] + 1) % (lcdText[r].length() + 4); any = true; }
  if (any) lcdDraw();
}

// ======================= Wi-Fi =======================
void connectWiFi() {
  WiFi.persistent(false);                            // do not wear out the flash memory
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);                // NO power-saving = no lag
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("\nConnecting to Wi-Fi \"" + String(WIFI_SSID) + "\" ");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) { delay(250); Serial.print("."); }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi not found (check name / password / 2.4 GHz). Starting hotspot NodeMCU-Control ...");
    WiFi.mode(WIFI_AP_STA);                          // hotspot ON + keep trying your Wi-Fi
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.softAP("NodeMCU-Control", "12345678");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  Serial.println("\n>>> Board IP address: " + ipStr());
}
unsigned long ipT = 0;
void netTick() {                                     // repeats the IP every 5 s so you never miss it
  if (millis() - ipT < 5000) return;
  ipT = millis();
  Serial.println(String("[Wi-Fi] ") + (WiFi.status() == WL_CONNECTED ? "connected" : "hotspot only") +
                 "   IP: " + ipStr() + "   signal: " + String(rssiNow()) + " dBm" +
                 "   LCD: " + (lcdFound ? "0x" + String(lcdAddr, HEX) : String("NOT FOUND")));
}

// the app uses /ping to find the board (name, signal, what it can do) and /status to read its state
String pingJson() {
  return "{\"device\":\"nodemcu\",\"name\":\"" + String(DEVICE_NAME) + "\",\"mac\":\"" + WiFi.macAddress() +
             "\",\"ip\":\"" + ipStr() + "\",\"type\":\"" + String(PROJECT_TYPE) + "\",\"fw\":\"v5\",\"rssi\":" +
             String(rssiNow()) + ",\"caps\":" + String(CAPS_JSON) + "}";
}
// ---- instant discovery: the app shouts "RIT_DISCOVER" on UDP port 4210, every board answers at once
WiFiUDP discUdp;
void discoveryBegin() { discUdp.begin(4210); }
void discoveryTick() {
  if (discUdp.parsePacket() <= 0) return;
  char b[20]; int n = discUdp.read(b, 19); b[n > 0 ? n : 0] = 0;
  if (strncmp(b, "RIT_DISCOVER", 12) != 0) return;
  String s = pingJson();
  discUdp.beginPacket(discUdp.remoteIP(), discUdp.remotePort());
  discUdp.write((const uint8_t*)s.c_str(), s.length());
  discUdp.endPacket();
}

void addBasicRoutes() {
  server.on("/ping", []() { sendJson(pingJson()); });
  server.on("/status", []() { sendJson(statusJson()); });
}

String statusJson() {
  return "{\"ok\":1,\"type\":\"lcd\",\"lcd\":" + String(lcdFound ? "1" : "0") + "}";
}

// ======================= setup / loop =======================
void setup() {
  Serial.begin(115200);
  delay(300);
  Wire.begin(D2, D1);                       // SDA = D2, SCL = D1
  Wire.setClock(100000);                    // standard speed = most reliable for the backpack
  Wire.setClockStretchLimit(2000);
  delay(200);

  lcdAddr = findLcdAddress();
  if (lcdFound) Serial.println("\nLCD found at I2C address 0x" + String(lcdAddr, HEX));
  else Serial.println("\nLCD NOT found on I2C - check SDA->D2, SCL->D1, VCC->VIN (5V), GND->G");
  lcdStart();
  lcdSet(lcdFound ? "LCD OK  0x" + String(lcdAddr, HEX) + "\nConnecting WiFi" : "LCD not found!\nCheck wiring");

  connectWiFi();

  lcdStart();                               // start the LCD again, now that power is stable
  lcdSet("NodeMCU Ready\n" + ipStr());      // the IP address is shown on the LCD
  addBasicRoutes();

  server.on("/display", []() {              // /display?m=lcd&t=text
    String t = server.arg("t");
    if (t.length() > 300) t = t.substring(0, 300);
    if (server.arg("m") == "lcd") { lcdSet(t); sendJson("{\"ok\":1}"); }
    else sendErr("This board has the LCD sketch only");
  });
  server.on("/clear", []() { lcdSet(""); sendJson("{\"ok\":1}"); });   // /clear?m=lcd
  server.on("/reinit", []() {               // restart the LCD if it ever shows strange symbols
    lcdAddr = findLcdAddress();
    lcdStart();
    lcdSet("NodeMCU Ready\n" + ipStr());
    sendJson("{\"ok\":1}");
  });
  server.on("/off", []() { sendJson(statusJson()); });   // app Disconnect (nothing to switch off here)
  WiFiClient::setDefaultNoDelay(true);      // send replies immediately
  server.begin();
  discoveryBegin();
}

void loop() { server.handleClient(); discoveryTick(); netTick(); lcdTick(); }
