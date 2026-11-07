/*
  RIT Control - 01 CAR CONTROL        board: NodeMCU 1.0 (ESP-12E Module)
  App card: Car Control  (also used by Voice Control > Car)

  WIRING (L298N motor driver)
    L298N IN1 -> D5      L298N IN2 -> D6      (left motor)
    L298N IN3 -> D7      L298N IN4 -> D0      (right motor)
    ENA / ENB -> leave the jumpers ON (full speed)
    Headlight LED : D8 -> 220 ohm -> LED(+), LED(-) -> G
    Horn (active buzzer) : D3 -> buzzer(+), buzzer(-) -> G
    Battery 7-12V -> L298N 12V terminal ; L298N GND <-> NodeMCU G (common ground!)
  If the board will not upload, unplug the horn from D3 while uploading.
  App requests: /car?d=F|B|L|R|S (or FL FR BL BR) , /light?s=0|1 , /horn?s=0|1
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>

const char* PROJECT_TYPE = "car";
const char* CAPS_JSON = "[\"car\",\"light\",\"horn\"]";
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

const int IN1 = D5, IN2 = D6, IN3 = D7, IN4 = D0;
const int LIGHT_PIN = D8, HORN_PIN = D3;
bool lightOn = false, hornOn = false, carMoving = false;
unsigned long lastCarCmd = 0, lastHorn = 0;

void drive(const String& d) {          // d = "S" stop, or a mix of F/B and L/R
  bool f = d.indexOf('F') >= 0, b = d.indexOf('B') >= 0;
  bool l = d.indexOf('L') >= 0, r = d.indexOf('R') >= 0;
  int a = LOW, bb = LOW, c = LOW, e = LOW;
  if      (f && !l && !r) { a = HIGH; c = HIGH; }      // forward
  else if (b && !l && !r) { bb = HIGH; e = HIGH; }     // backward
  else if (f && l)        { c = HIGH; }                // forward + left (arc)
  else if (f && r)        { a = HIGH; }                // forward + right (arc)
  else if (b && l)        { e = HIGH; }                // back + left
  else if (b && r)        { bb = HIGH; }               // back + right
  else if (l)             { bb = HIGH; c = HIGH; }     // spin left
  else if (r)             { a = HIGH; e = HIGH; }      // spin right
  digitalWrite(IN1, a); digitalWrite(IN2, bb);
  digitalWrite(IN3, c); digitalWrite(IN4, e);
  carMoving = (a || bb || c || e);
  lastCarCmd = millis();
}
void setHorn(bool on) { hornOn = on; digitalWrite(HORN_PIN, on ? HIGH : LOW); lastHorn = millis(); }

String statusJson() {
  return "{\"ok\":1,\"type\":\"car\",\"light\":" + String(lightOn ? 1 : 0) + "}";
}

void setup() {
  Serial.begin(115200);
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(LIGHT_PIN, OUTPUT); pinMode(HORN_PIN, OUTPUT);
  drive("S"); setHorn(false); digitalWrite(LIGHT_PIN, LOW);

  connectWiFi();
  addBasicRoutes();

  server.on("/car", []() { drive(server.arg("d")); sendJson(statusJson()); });
  server.on("/light", []() {
    lightOn = (server.arg("s").toInt() == 1);
    digitalWrite(LIGHT_PIN, lightOn ? HIGH : LOW);
    sendJson(statusJson());
  });
  server.on("/horn", []() { setHorn(server.arg("s").toInt() == 1); sendJson(statusJson()); });
  server.on("/off", []() {                  // app Disconnect: everything OFF
    drive("S"); lightOn = false; digitalWrite(LIGHT_PIN, LOW); setHorn(false);
    sendJson(statusJson());
  });
  WiFiClient::setDefaultNoDelay(true);   // send replies immediately
  server.begin();
  discoveryBegin();
}

void loop() {
  server.handleClient(); discoveryTick(); netTick();
  // safety: stop if the app stops sending (finger lifted / Wi-Fi lost)
  if (carMoving && millis() - lastCarCmd > 1200) drive("S");
  if (hornOn && millis() - lastHorn > 1200) setHorn(false);
}
