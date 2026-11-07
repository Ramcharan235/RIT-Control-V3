/*
  RIT Control - 04 SERVO CONTROL      board: NodeMCU 1.0 (ESP-12E Module)
  App card: Servo Control  (range 0-45, 0-90, 0-180 or 0-360 chosen in the app)

  WIRING
    Servo signal (orange/yellow) -> D4
    Servo VCC (red)              -> VIN (5V)   (small servos only; big servos need their own 5V supply)
    Servo GND (brown/black)      -> G          (join the grounds if you use a separate supply)
  A normal SG90 / MG90S turns only 180 degrees. The app sends a pulse width (500-2500 us = the servo's full travel); you can fine-tune it in the app (Servo > Calibrate).
  App request: /servo?us=400..2700   (old app: /servo?a=0..180)
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <Servo.h>

const char* PROJECT_TYPE = "servo";
const char* CAPS_JSON = "[\"servo\"]";
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

const int SERVO_PIN = D4;
Servo servo;
int servoAngle = 0;
int servoUs = 500;                          // current pulse width in microseconds
const int SERVO_MIN_US = 500, SERVO_MAX_US = 2500;   // 0 deg .. full travel (wide range = servo turns its FULL angle)
void servoGo(int us) {
  servoUs = constrain(us, 400, 2700);
  servo.writeMicroseconds(servoUs);
  servoAngle = constrain(map(servoUs, SERVO_MIN_US, SERVO_MAX_US, 0, 180), 0, 180);
}

String statusJson() {
  return "{\"ok\":1,\"type\":\"servo\",\"servo\":" + String(servoAngle) + ",\"us\":" + String(servoUs) + "}";
}

void setup() {
  Serial.begin(115200);
  servo.attach(SERVO_PIN, 400, 2700);       // wide pulse range = full servo travel
  servoGo(SERVO_MIN_US);                    // start at 0 degrees

  connectWiFi();
  addBasicRoutes();

  server.on("/servo", []() {                // /servo?us=400..2700 (new app) or /servo?a=0..180 (old app)
    if (server.hasArg("us")) servoGo(server.arg("us").toInt());
    else servoGo(map(constrain(server.arg("a").toInt(), 0, 180), 0, 180, SERVO_MIN_US, SERVO_MAX_US));
    sendJson(statusJson());
  });
  server.on("/off", []() { sendJson(statusJson()); });   // app Disconnect (nothing to switch off here)
  WiFiClient::setDefaultNoDelay(true);   // send replies immediately
  server.begin();
  discoveryBegin();
}

void loop() { server.handleClient(); discoveryTick(); netTick(); }
