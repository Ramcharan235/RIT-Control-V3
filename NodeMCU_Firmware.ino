/*
  RIT Control - NodeMCU firmware v5   (ESP8266, board: "NodeMCU 1.0 (ESP-12E Module)")

  Works with the "RIT Control" Android app:
    Car (L298N) + light + horn | 4 relays | servo | LCD 16x2 | OLED 0.96"

  A NodeMCU has too few pins for the car AND the 4 relays together, so choose ONE project below.

  PINS            HOME project        CAR project
  D5              Relay 1             L298N IN1
  D6              Relay 2             L298N IN2
  D7              Relay 3             L298N IN3
  D0              Relay 4             L298N IN4
  D4              Servo signal        Servo signal
  D8              -                   Headlight (LED + 220 ohm to GND)
  D3              -                   Horn (active buzzer)
  D1 / D2         Display SCL / SDA   Display SCL / SDA   (LCD + OLED can share these 2 pins)

  ARDUINO LIBRARIES (Sketch > Include Library > Manage Libraries):
    - LiquidCrystal I2C   (by Frank de Brabander)   -> only if USE_LCD
    - Adafruit SSD1306  and  Adafruit GFX Library   -> only if USE_OLED
    Servo, WiFi and WebServer come with the ESP8266 board package.

  NEW in v5:  instant device discovery (UDP) | /off on Disconnect | full-travel servo (500-2500 us)
  v4:  - Wi-Fi power-saving is OFF  -> fast, lag-free response in the app
              - the board reports its signal strength (the Wi-Fi icon in the app)
              - the board tells the app what it can do (Car / Relays / Servo / LCD / OLED)
              - the IP address is printed in the Serial Monitor every 5 seconds
              - if your Wi-Fi is not found, the board ALSO makes its own hotspot
                  "NodeMCU-Control" (password 12345678, address 192.168.4.1) and keeps
                  trying to join your Wi-Fi in the background
              - /clear?m=lcd|oled|all  and  /draw (OLED Draw Pad) from v3
  In the app you do NOT need to type the IP: Connect > the board appears under "Available devices".
*/

// ===== 1) CHOOSE ONE PROJECT (keep exactly one line) =====
#define PROJECT_HOME
//#define PROJECT_CAR

// ===== 2) DISPLAYS (put // in front of a line if you don't have that display) =====
#define USE_LCD       // LCD 16x2 with I2C backpack
#define USE_OLED      // OLED 0.96" SSD1306 128x64, I2C

// ===== 3) YOUR WI-FI =====
const char* WIFI_SSID   = "Ram";
const char* WIFI_PASS   = "12345678";
const char* DEVICE_NAME = "NodeMCU-1";
const bool  RELAY_ACTIVE_LOW = false;  // false = relay ON with HIGH (your module) ; true = ON with LOW
// ==========================================================

#if defined(PROJECT_HOME) && defined(PROJECT_CAR)
  #error "Choose only ONE project: PROJECT_HOME or PROJECT_CAR"
#endif
#if !defined(PROJECT_HOME) && !defined(PROJECT_CAR)
  #error "Choose a project: PROJECT_HOME or PROJECT_CAR"
#endif

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <Servo.h>
#include <Wire.h>
#ifdef USE_LCD
  #include <LiquidCrystal_I2C.h>
  LiquidCrystal_I2C lcd(0x27, 16, 2);          // use 0x3F if your LCD stays blank
#endif
#ifdef USE_OLED
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 oled(128, 64, &Wire, -1);
#endif

// ---------- pins ----------
const int PIN_A = D5, PIN_B = D6, PIN_C = D7, PIN_D = D0;   // relays 1-4  /  motor IN1-IN4
const int SERVO_PIN = D4;
const int LIGHT_PIN = D8;
const int HORN_PIN  = D3;

#ifdef PROJECT_HOME
  const char* PROJECT_NAME = "home";
#else
  const char* PROJECT_NAME = "car";
#endif

ESP8266WebServer server(80);
Servo servo;
int  servoAngle = 0;
bool relayState[4] = {false, false, false, false};
bool lightOn = false, hornOn = false, carMoving = false;
unsigned long lastCarCmd = 0, lastHorn = 0;
int servoUs = 500;                          // current pulse width in microseconds
const int SERVO_MIN_US = 500, SERVO_MAX_US = 2500;   // 0 deg .. full travel (wide range = servo turns its FULL angle)
void servoGo(int us) {
  servoUs = constrain(us, 400, 2700);
  servo.writeMicroseconds(servoUs);
  servoAngle = constrain(map(servoUs, SERVO_MIN_US, SERVO_MAX_US, 0, 180), 0, 180);
}

// ---------- small helpers ----------
void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
void sendJson(const String& s) { cors(); server.send(200, "application/json", s); }
void sendErr(const String& m)  { sendJson("{\"ok\":0,\"err\":\"" + m + "\"}"); }

int rssiNow() { return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0; }

String statusJson() {
  String s = "{\"ok\":1,\"type\":\"" + String(PROJECT_NAME) + "\",\"rssi\":" + String(rssiNow()) + ",\"relays\":[";
  for (int i = 0; i < 4; i++) { s += relayState[i] ? "1" : "0"; if (i < 3) s += ","; }
  s += "],\"servo\":" + String(servoAngle) + ",\"us\":" + String(servoUs) + ",\"light\":" + String(lightOn ? 1 : 0) + "}";
  return s;
}
String ipStr() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return WiFi.softAPIP().toString();               // hotspot fallback: 192.168.4.1
}

// ---------- HOME: relays ----------
#ifdef PROJECT_HOME
const int relayPins[4] = {PIN_A, PIN_B, PIN_C, PIN_D};
void writeRelay(int i, bool on) {
  relayState[i] = on;
  digitalWrite(relayPins[i], (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}
#endif

// ---------- CAR: motors, light, horn ----------
#ifdef PROJECT_CAR
// d = "S" stop, or a mix of F/B (forward/back) and L/R (left/right)
void drive(const String& d) {
  bool f = d.indexOf('F') >= 0, b = d.indexOf('B') >= 0;
  bool l = d.indexOf('L') >= 0, r = d.indexOf('R') >= 0;
  int in1 = LOW, in2 = LOW, in3 = LOW, in4 = LOW;      // IN1/IN2 = left motor, IN3/IN4 = right motor
  if      (f && !l && !r) { in1 = HIGH; in3 = HIGH; }  // forward
  else if (b && !l && !r) { in2 = HIGH; in4 = HIGH; }  // backward
  else if (f && l)        { in3 = HIGH; }              // forward + left  (arc)
  else if (f && r)        { in1 = HIGH; }              // forward + right (arc)
  else if (b && l)        { in4 = HIGH; }              // back + left
  else if (b && r)        { in2 = HIGH; }              // back + right
  else if (l)             { in2 = HIGH; in3 = HIGH; }  // spin left
  else if (r)             { in1 = HIGH; in4 = HIGH; }  // spin right
  digitalWrite(PIN_A, in1); digitalWrite(PIN_B, in2);
  digitalWrite(PIN_C, in3); digitalWrite(PIN_D, in4);
  carMoving = (in1 || in2 || in3 || in4);
  lastCarCmd = millis();
}
void setHorn(bool on) { hornOn = on; digitalWrite(HORN_PIN, on ? HIGH : LOW); lastHorn = millis(); }
#endif

// ---------- LCD 16x2 (long lines scroll sideways) ----------
#ifdef USE_LCD
String lcdText[2] = {"", ""};
int lcdOff[2] = {0, 0};
unsigned long lcdT = 0;
String lcdWindow(int row) {
  String s = lcdText[row];
  if (s.length() <= 16) { while (s.length() < 16) s += ' '; return s; }
  s += "    ";
  String w = "";
  for (int i = 0; i < 16; i++) w += s[(lcdOff[row] + i) % s.length()];
  return w;
}
void lcdDraw() {
  for (int r = 0; r < 2; r++) { lcd.setCursor(0, r); lcd.print(lcdWindow(r)); }
}
void lcdSet(String t) {
  t.replace("\r", "");
  int nl = t.indexOf('\n');
  lcdText[0] = (nl < 0) ? t : t.substring(0, nl);
  lcdText[1] = (nl < 0) ? "" : t.substring(nl + 1);
  int nl2 = lcdText[1].indexOf('\n');
  if (nl2 >= 0) lcdText[1] = lcdText[1].substring(0, nl2);
  lcdOff[0] = lcdOff[1] = 0;
  lcdDraw();
}
void lcdTick() {
  if (millis() - lcdT < 350) return;
  lcdT = millis();
  bool any = false;
  for (int r = 0; r < 2; r++)
    if (lcdText[r].length() > 16) { lcdOff[r] = (lcdOff[r] + 1) % (lcdText[r].length() + 4); any = true; }
  if (any) lcdDraw();
}
#endif

// ---------- OLED 0.96" (wraps by words, long text scrolls up) ----------
#ifdef USE_OLED
#define OLED_COLS 21
#define OLED_ROWS 8
#define MAX_LINES 64
String oledLines[MAX_LINES];
int oledCount = 0, oledOffset = 0;
bool oledOK = false;
unsigned long oledT = 0;

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
void oledTick() {
  if (oledCount <= OLED_ROWS || millis() - oledT < 700) return;
  oledT = millis();
  oledOffset = (oledOffset + 1) % (oledCount + 1);
  oledDraw();
}
// ---- Draw Pad: 128x64 picture sent as 2048 hex characters (1024 bytes, 1 bit per pixel)
uint8_t oledBmp[1024];
int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}
bool oledShowHex(const String& h) {
  if (!oledOK || h.length() < 2048) return false;
  for (int i = 0; i < 1024; i++) oledBmp[i] = (hexVal(h[2 * i]) << 4) | hexVal(h[2 * i + 1]);
  oledCount = 0; oledOffset = 0;                 // stop any text scrolling
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
#endif

// ---------- what this board can do (the app reads this) ----------
String capsJson() {
  String c = "[";
#ifdef PROJECT_HOME
  c += "\"relay\"";
#else
  c += "\"car\",\"light\",\"horn\"";
#endif
  c += ",\"servo\"";
#ifdef USE_LCD
  c += ",\"lcd\"";
#endif
#ifdef USE_OLED
  if (oledOK) c += ",\"oled\"";
#endif
  return c + "]";
}

String pingJson() {
  String s = "{\"device\":\"nodemcu\",\"name\":\"" + String(DEVICE_NAME) + "\",\"mac\":\"" + WiFi.macAddress() +
               "\",\"ip\":\"" + ipStr() + "\",\"type\":\"" + String(PROJECT_NAME) + "\",\"fw\":\"v5\",\"rssi\":" +
               String(rssiNow()) + ",\"caps\":" + capsJson() + "}";
  return s;
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

// ---------- Wi-Fi ----------
void connectWiFi() {
  WiFi.persistent(false);                          // do not wear out the flash memory
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);              // NO power-saving = no lag
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("\nConnecting to Wi-Fi \"" + String(WIFI_SSID) + "\" ");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) { delay(250); Serial.print("."); }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi not found (check name / password / 2.4 GHz). Starting hotspot NodeMCU-Control ...");
    WiFi.mode(WIFI_AP_STA);                        // hotspot ON + keep trying your Wi-Fi
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.softAP("NodeMCU-Control", "12345678");    // hotspot IP: 192.168.4.1
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  Serial.println("\n>>> Board IP address: " + ipStr());
}
unsigned long ipT = 0;
void netTick() {                                   // repeat the IP so you never miss it
  if (millis() - ipT < 5000) return;
  ipT = millis();
  Serial.println(String("[Wi-Fi] ") + (WiFi.status() == WL_CONNECTED ? "connected" : "hotspot only") +
                 "   IP: " + ipStr() + "   signal: " + String(rssiNow()) + " dBm");
}

// ======================= setup =======================
void setup() {
  Serial.begin(115200);
  Wire.begin(D2, D1);                              // SDA = D2, SCL = D1

#ifdef PROJECT_HOME
  for (int i = 0; i < 4; i++) { pinMode(relayPins[i], OUTPUT); writeRelay(i, false); }
#else
  pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT); pinMode(PIN_D, OUTPUT);
  pinMode(LIGHT_PIN, OUTPUT); pinMode(HORN_PIN, OUTPUT);
  drive("S"); setHorn(false); digitalWrite(LIGHT_PIN, LOW);
#endif

  servo.attach(SERVO_PIN, 400, 2700);        // wide pulse range = full servo travel
  servoGo(SERVO_MIN_US);

#ifdef USE_LCD
  lcd.init(); lcd.backlight(); lcdSet("Connecting WiFi");
#endif
#ifdef USE_OLED
  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  oledSet("Connecting WiFi...");
#endif

  connectWiFi();

  String ip = ipStr();
#ifdef USE_LCD
  lcdSet("NodeMCU Ready\n" + ip);
#endif
#ifdef USE_OLED
  oledSet("NodeMCU Ready\n" + ip);
#endif

  server.on("/ping", []() { sendJson(pingJson()); });
  server.on("/status", []() { sendJson(statusJson()); });

  server.on("/relay", []() {                       // /relay?n=1..4&s=0|1
#ifdef PROJECT_HOME
    int n = server.arg("n").toInt(), s = server.arg("s").toInt();
    if (n >= 1 && n <= 4) writeRelay(n - 1, s == 1);
    sendJson(statusJson());
#else
    sendErr("This board has the CAR firmware - relays need PROJECT_HOME");
#endif
  });

  server.on("/car", []() {                         // /car?d=F|B|L|R|S (or FL, FR, BL, BR)
#ifdef PROJECT_CAR
    drive(server.arg("d"));
    sendJson(statusJson());
#else
    sendErr("This board has the HOME firmware - car needs PROJECT_CAR");
#endif
  });
  server.on("/light", []() {                       // /light?s=0|1
#ifdef PROJECT_CAR
    lightOn = (server.arg("s").toInt() == 1);
    digitalWrite(LIGHT_PIN, lightOn ? HIGH : LOW);
    sendJson(statusJson());
#else
    sendErr("Lights need the CAR firmware (PROJECT_CAR)");
#endif
  });
  server.on("/horn", []() {                        // /horn?s=0|1
#ifdef PROJECT_CAR
    setHorn(server.arg("s").toInt() == 1);
    sendJson(statusJson());
#else
    sendErr("Horn needs the CAR firmware (PROJECT_CAR)");
#endif
  });

  server.on("/servo", []() {                // /servo?us=400..2700 (new app) or /servo?a=0..180 (old app)
    if (server.hasArg("us")) servoGo(server.arg("us").toInt());
    else servoGo(map(constrain(server.arg("a").toInt(), 0, 180), 0, 180, SERVO_MIN_US, SERVO_MAX_US));
    sendJson(statusJson());
  });

  server.on("/display", []() {                     // /display?m=lcd|oled&t=text
    String m = server.arg("m"), t = server.arg("t");
    if (t.length() > 300) t = t.substring(0, 300);
    if (m == "lcd") {
#ifdef USE_LCD
      lcdSet(t); sendJson("{\"ok\":1}");
#else
      sendErr("LCD is turned off in the firmware (USE_LCD)");
#endif
    } else if (m == "oled") {
#ifdef USE_OLED
      if (!oledOK) sendErr("OLED not found - check wiring (SDA D2, SCL D1)");
      else { oledSet(t); sendJson("{\"ok\":1}"); }
#else
      sendErr("OLED is turned off in the firmware (USE_OLED)");
#endif
    } else sendErr("Unknown display");
  });

  server.on("/clear", []() {                       // /clear?m=lcd|oled|all
    String m = server.arg("m");
    bool done = false;
#ifdef USE_LCD
    if (m == "lcd" || m == "all") { lcdSet(""); done = true; }
#endif
#ifdef USE_OLED
    if (m == "oled" || m == "all") { oledClear(); done = true; }
#endif
    if (done) sendJson("{\"ok\":1}"); else sendErr("That display is turned off or unknown");
  });

  server.on("/draw", HTTP_OPTIONS, []() { cors(); server.send(204); });   // browser pre-check
  server.on("/draw", HTTP_POST, []() {             // POST body = 2048 hex chars (Draw Pad)
#ifdef USE_OLED
    if (!oledOK) sendErr("OLED not found - check wiring (SDA D2, SCL D1)");
    else if (oledShowHex(server.arg("plain"))) sendJson("{\"ok\":1}");
    else sendErr("Bad drawing data");
#else
    sendErr("OLED is turned off in the firmware (USE_OLED)");
#endif
  });

  server.on("/off", []() {                         // app Disconnect: everything OFF
#ifdef PROJECT_HOME
    for (int i = 0; i < 4; i++) writeRelay(i, false);
#else
    drive("S"); lightOn = false; digitalWrite(LIGHT_PIN, LOW); setHorn(false);
#endif
    sendJson(statusJson());
  });

  WiFiClient::setDefaultNoDelay(true);              // send replies immediately
  server.begin();
  discoveryBegin();
}

// ======================= loop =======================
void loop() {
  server.handleClient();
  discoveryTick();
  netTick();

#ifdef PROJECT_CAR
  // Safety: stop if the app stops sending (finger lifted / Wi-Fi lost)
  if (carMoving && millis() - lastCarCmd > 1200) drive("S");
  if (hornOn && millis() - lastHorn > 1200) setHorn(false);
#endif
#ifdef USE_LCD
  lcdTick();
#endif
#ifdef USE_OLED
  oledTick();
#endif
}
