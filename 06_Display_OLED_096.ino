/*
  RIT Control - 06 DISPLAY: OLED 0.96" (SSD1306 128x64)   board: NodeMCU 1.0 (ESP-12E Module)
  App card: Display > OLED  (Text, Live Text, Draw Pad, Clear display)

  LIBRARIES to install (Manage Libraries):  "Adafruit SSD1306"  and  "Adafruit GFX Library"
  WIRING (I2C)
    SDA -> D2     SCL -> D1     VCC -> 3V3 (3.3V)     GND -> G
  I2C address is usually 0x3C.
  Text wraps by words at 21 characters and scrolls up when longer than 8 lines.
  App requests: /display?m=oled&t=text , /clear?m=oled , POST /draw (Draw Pad picture, 2048 hex characters)
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const char* PROJECT_TYPE = "oled";
const char* CAPS_JSON = "[\"oled\"]";
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
                 "   IP: " + ipStr() + "   signal: " + String(rssiNow()) + " dBm");
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

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
#define OLED_COLS 21
#define OLED_ROWS 8
#define MAX_LINES 64
String oledLines[MAX_LINES];
int oledCount = 0, oledOffset = 0;
bool oledOK = false;
unsigned long oledT = 0;
uint8_t oledBmp[1024];

void wrapText(const String& t) {
  oledCount = 0;
  String line = "", word = "";
  auto pushLine = [&]() { if (oledCount < MAX_LINES) oledLines[oledCount++] = line; line = ""; };
  auto flushWord = [&]() {
    while ((int)word.length() > OLED_COLS) {
      if (line.length()) pushLine();
      line = word.substring(0, OLED_COLS);
      word = word.substring(OLED_COLS);
      pushLine();
    }
    if (word.length() == 0) return;
    if (line.length() == 0) line = word;
    else if ((int)(line.length() + 1 + word.length()) <= OLED_COLS) line += " " + word;
    else { pushLine(); line = word; }
    word = "";
  };
  for (unsigned int i = 0; i < t.length(); i++) {
    char c = t[i];
    if (c == '\n') { flushWord(); pushLine(); }
    else if (c == ' ') flushWord();
    else if (c != '\r') word += c;
  }
  flushWord();
  if (line.length() || oledCount == 0) pushLine();
}
void oledDraw() {
  if (!oledOK) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  for (int r = 0; r < OLED_ROWS; r++) {
    int idx = (oledCount <= OLED_ROWS) ? r : (oledOffset + r) % (oledCount + 1);
    if (idx < oledCount) { oled.setCursor(0, r * 8); oled.print(oledLines[idx]); }
  }
  oled.display();
}
void oledSet(const String& t) { wrapText(t); oledOffset = 0; oledDraw(); }
void oledTick() {                           // scroll long text upwards
  if (oledCount <= OLED_ROWS || millis() - oledT < 700) return;
  oledT = millis();
  oledOffset = (oledOffset + 1) % (oledCount + 1);
  oledDraw();
}
int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}
bool oledShowHex(const String& h) {         // Draw Pad: 1024 bytes, 1 bit per pixel
  if (!oledOK || h.length() < 2048) return false;
  for (int i = 0; i < 1024; i++) oledBmp[i] = (hexVal(h[2 * i]) << 4) | hexVal(h[2 * i + 1]);
  oledCount = 0; oledOffset = 0;            // stop any text scrolling
  oled.clearDisplay();
  oled.drawBitmap(0, 0, oledBmp, 128, 64, SSD1306_WHITE);
  oled.display();
  return true;
}
void oledClear() {
  oledCount = 0; oledOffset = 0;
  if (!oledOK) return;
  oled.clearDisplay();
  oled.display();
}

String statusJson() { return "{\"ok\":1,\"type\":\"oled\"}"; }

void setup() {
  Serial.begin(115200);
  Wire.begin(D2, D1);                       // SDA = D2, SCL = D1
  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  oledSet("Connecting WiFi...");

  connectWiFi();
  oledSet("NodeMCU Ready\n" + ipStr());      // the IP address is shown on the OLED
  addBasicRoutes();

  server.on("/display", []() {              // /display?m=oled&t=text
    String t = server.arg("t");
    if (t.length() > 300) t = t.substring(0, 300);
    if (server.arg("m") != "oled") sendErr("This board has the OLED sketch only");
    else if (!oledOK) sendErr("OLED not found - check wiring (SDA D2, SCL D1)");
    else { oledSet(t); sendJson("{\"ok\":1}"); }
  });
  server.on("/clear", []() { oledClear(); sendJson("{\"ok\":1}"); });   // /clear?m=oled
  server.on("/draw", HTTP_OPTIONS, []() { cors(); server.send(204); });
  server.on("/draw", HTTP_POST, []() {      // Draw Pad picture
    if (!oledOK) sendErr("OLED not found - check wiring (SDA D2, SCL D1)");
    else if (oledShowHex(server.arg("plain"))) sendJson("{\"ok\":1}");
    else sendErr("Bad drawing data");
  });
  server.on("/off", []() { sendJson(statusJson()); });   // app Disconnect (nothing to switch off here)
  WiFiClient::setDefaultNoDelay(true);   // send replies immediately
  server.begin();
  discoveryBegin();
}

void loop() { server.handleClient(); discoveryTick(); netTick(); oledTick(); }
