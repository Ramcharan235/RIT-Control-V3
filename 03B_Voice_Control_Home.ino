/*
  RIT Control - 03B VOICE CONTROL (HOME)   board: NodeMCU 1.0 (ESP-12E Module)
  App card: Voice Control > Home Control

  The PHONE turns your voice into text (needs internet on the phone) and the app sends
  the same web requests as the Home Control card. So this sketch is the same as
  02_Home_Control. Say: "light on", "fan off", "relay 3 on", "all on", "all off".
  Voice  "light on" -> /relay?n=1&s=1     "fan off" -> /relay?n=2&s=0

  WIRING (4-channel relay module)
    Relay IN1 -> D5   IN2 -> D6   IN3 -> D7   IN4 -> D0
    Relay VCC -> VIN (5V)         Relay GND -> G
  SAFETY: mains (230V) can kill. Keep wiring enclosed.
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>

const char* PROJECT_TYPE = "home";
const char* CAPS_JSON = "[\"relay\"]";
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

// false = relay switches ON with a HIGH signal (your module) ; true = ON with LOW (most modules)
const bool RELAY_ACTIVE_LOW = false;
const int relayPins[4] = {D5, D6, D7, D0};
bool relayState[4] = {false, false, false, false};

void writeRelay(int i, bool on) {
  relayState[i] = on;
  digitalWrite(relayPins[i], (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

String statusJson() {
  String s = "{\"ok\":1,\"type\":\"home\",\"relays\":[";
  for (int i = 0; i < 4; i++) { s += relayState[i] ? "1" : "0"; if (i < 3) s += ","; }
  return s + "]}";
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) { pinMode(relayPins[i], OUTPUT); writeRelay(i, false); }

  connectWiFi();
  addBasicRoutes();

  server.on("/relay", []() {                // /relay?n=1..4&s=0|1
    int n = server.arg("n").toInt(), s = server.arg("s").toInt();
    if (n >= 1 && n <= 4) writeRelay(n - 1, s == 1);
    sendJson(statusJson());
  });
  server.on("/off", []() {                  // app Disconnect: all relays OFF
    for (int i = 0; i < 4; i++) writeRelay(i, false);
    sendJson(statusJson());
  });
  WiFiClient::setDefaultNoDelay(true);   // send replies immediately
  server.begin();
  discoveryBegin();
}

void loop() { server.handleClient(); discoveryTick(); netTick(); }
