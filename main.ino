#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ---------- CONFIG ----------
const char* WIFI_SSID   = "";
const char* WIFI_PASS   = "";

const char* MQTT_HOST   = "";  // host de tu deployment
const uint16_t MQTT_PORT = 0000;                   // TLS
const char* MQTT_USER   = "";
const char* MQTT_PASS   = "";
const char* MQTT_ID     = "";

const char* T_STATUS = "server/status";
const char* T_STATE  = "server/state";
const char* T_CMD    = "server/cmd";

// CA del broker: descargala desde la consola de EMQX (sección TLS/SSL).
const char* CA_CERT = "";

const int PIN_RELAY = 26;
const int PIN_SENSE = 27;                 // INPUT_PULLUP, LOW = PC encendida
#define RELAY_ACTIVE_LOW false             // poné false si tu módulo es activo en HIGH

const uint32_t PULSE_SHORT_MS   = 300;
const uint32_t PULSE_LONG_MS    = 5000;
const uint32_t DEBOUNCE_MS      = 1000;
const uint32_t BOOT_TIMEOUT_MS  = 90000;   // si no enciende, vuelve a "off"
const uint32_t SHUT_TIMEOUT_MS  = 180000;  // si no apaga, vuelve a "on"
// -----------------------------

enum PcState { ST_OFF, ST_BOOTING, ST_ON, ST_SHUTTING_DOWN };
const char* stateName(PcState s) {
  switch (s) {
    case ST_OFF:           return "off";
    case ST_BOOTING:       return "booting";
    case ST_ON:            return "on";
    case ST_SHUTTING_DOWN: return "shutting_down";
  }
  return "unknown";
}

WiFiClientSecure net;
PubSubClient mqtt(net);

PcState state = ST_OFF;
uint32_t stateSince = 0;

// lectura con antirrebote
bool rawOn = false, stableOn = false;
uint32_t rawChangedAt = 0;

// pulso no bloqueante
bool pulsing = false;
uint32_t pulseEnd = 0;

uint32_t lastMqttAttempt = 0;

void relayWrite(bool active) {
  digitalWrite(PIN_RELAY, (active ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

bool startPulse(uint32_t ms) {
  if (pulsing) return false;
  relayWrite(true);
  pulsing = true;
  pulseEnd = millis() + ms;
  return true;
}

void publishState() {
  if (mqtt.connected()) mqtt.publish(T_STATE, stateName(state), true); // retained
}

void setState(PcState s) {
  if (s == state) return;
  state = s;
  stateSince = millis();
  Serial.printf("Estado -> %s\n", stateName(s));
  publishState();
}

void readSensor() {
  bool r = (digitalRead(PIN_SENSE) == LOW);
  if (r != rawOn) { rawOn = r; rawChangedAt = millis(); }
  if (millis() - rawChangedAt >= DEBOUNCE_MS) stableOn = rawOn;
}

void updateState() {
  uint32_t elapsed = millis() - stateSince;
  switch (state) {
    case ST_OFF:
      if (stableOn) setState(ST_ON);                 // encendida a mano
      break;
    case ST_BOOTING:
      if (stableOn) setState(ST_ON);
      else if (elapsed > BOOT_TIMEOUT_MS) setState(ST_OFF);
      break;
    case ST_ON:
      if (!stableOn) setState(ST_OFF);               // apagada a mano o por script
      break;
    case ST_SHUTTING_DOWN:
      if (!stableOn) setState(ST_OFF);
      else if (elapsed > SHUT_TIMEOUT_MS) setState(ST_ON);
      break;
  }
}

void handleCommand(const String& cmd) {
  Serial.printf("CMD: %s (estado %s)\n", cmd.c_str(), stateName(state));

  if (cmd == "on") {
    if (state == ST_OFF && startPulse(PULSE_SHORT_MS)) setState(ST_BOOTING);
  } else if (cmd == "off") {
    if (state == ST_ON && startPulse(PULSE_SHORT_MS)) setState(ST_SHUTTING_DOWN);
  } else if (cmd == "force_off") {
    if (state != ST_OFF && startPulse(PULSE_LONG_MS)) setState(ST_SHUTTING_DOWN);
  }
  // cualquier otra cosa o comando fuera de contexto se ignora
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  msg.toLowerCase();
  if (String(topic) == T_CMD) handleCommand(msg);
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250); Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " fallo");
}

void connectMqtt() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttAttempt < 5000) return;     // reintento cada 5 s
  lastMqttAttempt = millis();
  if (WiFi.status() != WL_CONNECTED) { connectWifi(); return; }

  Serial.print("MQTT...");
  // LWT: offline, retained, QoS 1. cleanSession = true
  if (mqtt.connect(MQTT_ID, MQTT_USER, MQTT_PASS,
                   T_STATUS, 1, true, "offline", true)) {
    Serial.println(" OK");
    mqtt.publish(T_STATUS, "online", true);
    publishState();
    mqtt.subscribe(T_CMD, 1);
  } else {
    Serial.printf(" fallo rc=%d\n", mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);

  // dejar el relé inactivo ANTES de configurarlo como salida (evita pulsos al arrancar)
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(PIN_RELAY, OUTPUT);
  relayWrite(false);

  pinMode(PIN_SENSE, INPUT_PULLUP);
  rawOn = stableOn = (digitalRead(PIN_SENSE) == LOW);
  rawChangedAt = millis();
  state = stableOn ? ST_ON : ST_OFF;
  stateSince = millis();

  net.setCACert(CA_CERT);
  // Solo para pruebas, si todavía no pegaste el certificado:
  // net.setInsecure();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(60);

  connectWifi();
}

void loop() {
  connectMqtt();
  mqtt.loop();

  readSensor();
  updateState();

  if (pulsing && (int32_t)(millis() - pulseEnd) >= 0) {
    relayWrite(false);
    pulsing = false;
  }
}