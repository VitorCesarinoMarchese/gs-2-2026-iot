/*
 * Chaveiro de Pânico — firmware do ESP32 (parte IoT da CitizenTech)
 *
 * Um clique no botão dispara um alerta silencioso. O evento vai por MQTT
 * para a nuvem; o app e o backend da equipe recebem, bloqueiam o celular
 * e avisam a polícia e os contatos de confiança cadastrados no app.
 *
 * Gestos no botão:
 *   - 1 clique ............ ALERTA (silencioso)
 *   - segurar 3 s (alerta ativo) ... CANCELA o alerta ("estou bem")
 *   - segurar 3 s (sem alerta) ..... TESTE (verifica a cadeia sem acionar ninguém)
 *
 * O chaveiro reenvia cada alerta a cada 5 s até o app confirmar (ack),
 * então um alerta disparado sem rede não se perde: ele sai assim que a
 * conexão voltar, mesmo depois de um reinício (fica salvo na flash).
 *
 * Contrato MQTT completo: docs/contrato-mqtt.md
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ───────────────────────── Configuração ─────────────────────────
static const char *WIFI_SSID    = "Wokwi-GUEST";
static const char *WIFI_PASS    = "";
static const int   WIFI_CHANNEL = 6;  // canal fixo acelera a conexão no Wokwi

static const char    *MQTT_HOST  = "broker.hivemq.com";
static const uint16_t MQTT_PORT  = 1883;
static const char    *TOPIC_ROOT = "citizentech/chaveiro";
static const char    *DEVICE_ID  = "chaveiro-0001";  // impresso no QR code do chaveiro para parear no app
static const char    *FW_VERSION = "1.0.0";

// Pinos
static const uint8_t PIN_BUTTON  = 4;   // botão de pânico (INPUT_PULLUP, aperta = LOW)
static const uint8_t PIN_LED_R   = 27;  // LED RGB (catodo comum)
static const uint8_t PIN_LED_G   = 26;
static const uint8_t PIN_LED_B   = 25;
static const uint8_t PIN_BUZZER  = 23;  // só toca no "achar chaveiro" e no teste, nunca no alerta
static const uint8_t PIN_BATTERY = 34;  // potenciômetro simula a tensão da bateria

// Tempos e limites
static const uint32_t DEBOUNCE_MS           = 30;
static const uint32_t HOLD_HINT_MS          = 800;   // a partir daqui o LED mostra "continue segurando"
static const uint32_t LONG_PRESS_MS         = 3000;
static const uint8_t  CLICKS_TO_TRIGGER     = 1;     // suba para 2 se quiser exigir clique duplo
static const uint32_t MULTI_CLICK_WINDOW_MS = 600;
static const uint32_t RETRY_INTERVAL_MS     = 5000;
static const uint32_t HEARTBEAT_MS          = 30000;
static const uint32_t WIFI_RETRY_MS         = 15000;
static const uint32_t MQTT_RETRY_MIN_MS     = 2000;
static const uint32_t MQTT_RETRY_MAX_MS     = 30000;
static const uint8_t  LOW_BATTERY_PCT       = 15;
static const uint8_t  LOW_BATTERY_CLEAR_PCT = 20;    // histerese: evita alerta piscando perto do limite
static const uint32_t FIND_DURATION_MS      = 6000;

// ───────────────────────── Estado ─────────────────────────
WiFiClient   net;
PubSubClient mqtt(net);
Preferences  prefs;

String topicEvents, topicStatus, topicCmd;

enum Color { OFF, RED, GREEN, BLUE, AMBER, WHITE };

struct OutEvent {
  bool     used;
  bool     needsAck;
  char     eventId[40];
  char     type[16];
  char     alertId[40];
  time_t   createdTs;     // epoch UTC (0 se o relógio ainda não sincronizou)
  uint32_t createdMs;
  uint32_t lastSentMs;
  uint16_t attempts;
};

static const uint8_t OUTBOX_SIZE = 6;
OutEvent outbox[OUTBOX_SIZE];

uint32_t eventSeq = 0;
char     activeAlertId[40] = "";
bool     activeAlertAcked  = false;

uint8_t  batteryPct = 100;
bool     lowBattery = false;

// Botão
bool     btnStable       = HIGH;
bool     btnLastRead     = HIGH;
uint32_t btnLastChangeMs = 0;
uint32_t btnPressedAtMs  = 0;
bool     longPressFired  = false;
uint8_t  clickCount      = 0;
uint32_t lastClickMs     = 0;

// Conectividade
uint32_t lastWifiAttemptMs = 0;
uint32_t lastMqttAttemptMs = 0;
uint32_t mqttRetryMs       = MQTT_RETRY_MIN_MS;
bool     ntpStarted        = false;
uint32_t lastHeartbeatMs   = 0;
bool     statusRequested   = false;

// Feedback visual/sonoro
Color    flashColor   = OFF;
uint32_t flashUntilMs = 0;
uint16_t flashBlinkMs = 0;   // 0 = aceso fixo
uint32_t findUntilMs  = 0;

// ───────────────────────── Utilidades ─────────────────────────
time_t nowEpoch() {
  time_t t = time(nullptr);
  return t > 1700000000 ? t : 0;  // antes do NTP, time() devolve algo perto de 1970
}

void setColor(Color c) {
  digitalWrite(PIN_LED_R, c == RED || c == AMBER || c == WHITE);
  digitalWrite(PIN_LED_G, c == GREEN || c == AMBER || c == WHITE);
  digitalWrite(PIN_LED_B, c == BLUE || c == WHITE);
}

void flash(Color c, uint32_t durationMs, uint16_t blinkMs = 0) {
  flashColor   = c;
  flashUntilMs = millis() + durationMs;
  flashBlinkMs = blinkMs;
}

// Pulso curto a cada "period" ms: discreto e economiza bateria.
bool pulse(uint32_t now, uint32_t period, uint32_t onMs) { return (now % period) < onMs; }
bool blink(uint32_t now, uint32_t halfPeriod) { return (now / halfPeriod) % 2 == 0; }

bool alertActive() { return activeAlertId[0] != '\0'; }

void persistAlert() {
  prefs.putString("alertId", activeAlertId);
  prefs.putBool("alertAcked", activeAlertAcked);
}

uint8_t readBatteryPct() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_BATTERY);
  return (uint8_t)((sum / 8) * 100 / 4095);
}

// ───────────────────────── Fila de saída ─────────────────────────
OutEvent *enqueue(const char *type, bool needsAck, const char *alertId = nullptr,
                  const char *reuseEventId = nullptr) {
  // Procura vaga; se a fila lotar, descarta o evento mais antigo que não seja alerta.
  OutEvent *slot = nullptr;
  for (auto &e : outbox) if (!e.used) { slot = &e; break; }
  if (!slot) {
    for (auto &e : outbox) {
      if (strcmp(e.type, "alert") != 0 && (!slot || e.createdMs < slot->createdMs)) slot = &e;
    }
  }
  if (!slot) slot = &outbox[0];

  memset(slot, 0, sizeof(*slot));
  slot->used     = true;
  slot->needsAck = needsAck;
  if (reuseEventId) {
    strlcpy(slot->eventId, reuseEventId, sizeof(slot->eventId));
  } else {
    eventSeq++;
    prefs.putUInt("seq", eventSeq);
    snprintf(slot->eventId, sizeof(slot->eventId), "%s-%lu", DEVICE_ID, (unsigned long)eventSeq);
  }
  strlcpy(slot->type, type, sizeof(slot->type));
  strlcpy(slot->alertId, alertId ? alertId : "", sizeof(slot->alertId));
  slot->createdTs = nowEpoch();
  slot->createdMs = millis();
  return slot;
}

bool publishEvent(OutEvent &e) {
  JsonDocument doc;
  doc["eventId"]  = e.eventId;
  doc["type"]     = e.type;
  doc["deviceId"] = DEVICE_ID;
  if (e.alertId[0]) doc["alertId"] = e.alertId;
  if (e.createdTs) doc["ts"] = e.createdTs; else doc["ts"] = nullptr;
  doc["ageMs"]    = millis() - e.createdMs;  // quanto tempo atrás o botão foi apertado
  doc["attempt"]  = e.attempts + 1;
  doc["battery"]  = batteryPct;
  doc["fw"]       = FW_VERSION;

  char buf[384];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqtt.publish(topicEvents.c_str(), (const uint8_t *)buf, n, false);
  if (ok) {
    e.attempts++;
    e.lastSentMs = millis();
    Serial.printf("[MQTT] -> %s (tentativa %u): %s\n", e.type, e.attempts, buf);
  }
  return ok;
}

void serviceOutbox() {
  if (!mqtt.connected()) return;
  uint32_t now = millis();
  for (auto &e : outbox) {
    if (!e.used) continue;
    bool due = e.attempts == 0 || (e.needsAck && now - e.lastSentMs >= RETRY_INTERVAL_MS);
    if (!due) continue;
    if (publishEvent(e) && !e.needsAck) e.used = false;  // dispara-e-esquece
  }
}

void dropOutboxForAlert(const char *alertId) {
  for (auto &e : outbox) {
    if (e.used && (strcmp(e.alertId, alertId) == 0 || strcmp(e.eventId, alertId) == 0)) e.used = false;
  }
}

// ───────────────────────── Ações do usuário ─────────────────────────
void triggerAlert() {
  if (alertActive()) {
    // Clique extra com alerta ainda sem confirmação: reenvia já, sem esperar o próximo ciclo.
    for (auto &e : outbox) if (e.used && strcmp(e.eventId, activeAlertId) == 0) e.attempts = 0;
    Serial.println("[ALERTA] Já existe alerta ativo; reforçando envio.");
    return;
  }
  OutEvent *e = enqueue("alert", true);
  strlcpy(e->alertId, e->eventId, sizeof(e->alertId));
  strlcpy(activeAlertId, e->eventId, sizeof(activeAlertId));
  activeAlertAcked = false;
  persistAlert();
  Serial.printf("[ALERTA] Botão de pânico acionado! id=%s\n", activeAlertId);
  serviceOutbox();  // tenta mandar na hora
}

void cancelAlert() {
  char id[40];
  strlcpy(id, activeAlertId, sizeof(id));
  dropOutboxForAlert(id);  // se o alerta nem saiu ainda, não precisa sair
  enqueue("cancel", true, id);
  activeAlertId[0] = '\0';
  activeAlertAcked = false;
  persistAlert();
  flash(GREEN, 1500, 150);
  Serial.printf("[ALERTA] Cancelado pelo usuário (estou bem). id=%s\n", id);
  serviceOutbox();
}

void sendTest() {
  enqueue("test", true);
  flash(WHITE, 300);
  Serial.println("[TESTE] Evento de teste enviado; aguardando confirmação do app.");
  serviceOutbox();
}

// ───────────────────────── Botão ─────────────────────────
void handleButton() {
  uint32_t now = millis();
  bool reading = digitalRead(PIN_BUTTON);

  if (reading != btnLastRead) { btnLastRead = reading; btnLastChangeMs = now; }

  if (now - btnLastChangeMs >= DEBOUNCE_MS && reading != btnStable) {
    btnStable = reading;
    if (btnStable == LOW) {          // apertou
      btnPressedAtMs = now;
      longPressFired = false;
    } else if (!longPressFired) {    // soltou depois de um clique curto
      if (now - lastClickMs > MULTI_CLICK_WINDOW_MS) clickCount = 0;
      clickCount++;
      lastClickMs = now;
      if (clickCount >= CLICKS_TO_TRIGGER) { clickCount = 0; triggerAlert(); }
    }
  }

  // Pressão longa dispara enquanto ainda está segurando, para o usuário sentir a resposta.
  if (btnStable == LOW && !longPressFired && now - btnPressedAtMs >= LONG_PRESS_MS) {
    longPressFired = true;
    clickCount = 0;
    if (alertActive()) cancelAlert(); else sendTest();
  }
}

// ───────────────────────── Comandos vindos do app ─────────────────────────
void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("[CMD] JSON inválido, ignorado.");
    return;
  }
  const char *cmd = doc["cmd"] | "";
  Serial.printf("[CMD] <- %s\n", cmd);

  if (strcmp(cmd, "ack") == 0) {
    const char *eventId = doc["eventId"] | "";
    for (auto &e : outbox) {
      if (!e.used || strcmp(e.eventId, eventId) != 0) continue;
      e.used = false;
      if (strcmp(e.type, "alert") == 0 && strcmp(eventId, activeAlertId) == 0) {
        activeAlertAcked = true;
        persistAlert();
        flash(RED, 3000);  // vermelho fixo 3 s: "o app recebeu, ajuda a caminho"
        Serial.println("[ALERTA] App confirmou o recebimento.");
      } else if (strcmp(e.type, "test") == 0) {
        flash(GREEN, 1500, 150);
        tone(PIN_BUZZER, 2000, 120);
        Serial.println("[TESTE] Cadeia OK: o app respondeu.");
      } else if (strcmp(e.type, "cancel") == 0) {
        Serial.println("[ALERTA] App confirmou o cancelamento.");
      }
    }
  } else if (strcmp(cmd, "resolve") == 0) {
    // Alerta encerrado pelo outro lado (usuário cancelou no app, ou a central fechou a ocorrência).
    const char *alertId = doc["alertId"] | "";
    if (alertActive() && strcmp(alertId, activeAlertId) == 0) {
      dropOutboxForAlert(activeAlertId);
      activeAlertId[0] = '\0';
      activeAlertAcked = false;
      persistAlert();
      flash(GREEN, 1500, 150);
      Serial.println("[ALERTA] Encerrado pelo app.");
    }
  } else if (strcmp(cmd, "find") == 0) {
    findUntilMs = millis() + FIND_DURATION_MS;
    Serial.println("[ACHAR] Tocando para localizar o chaveiro.");
  } else if (strcmp(cmd, "ping") == 0) {
    statusRequested = true;  // responde com o status no próximo loop
  }
}

// ───────────────────────── Conectividade ─────────────────────────
void publishStatus(bool online) {
  JsonDocument doc;
  doc["deviceId"]    = DEVICE_ID;
  doc["online"]      = online;
  doc["battery"]     = batteryPct;
  doc["lowBattery"]  = lowBattery;
  doc["rssi"]        = WiFi.RSSI();
  doc["alertActive"] = alertActive();
  if (alertActive()) doc["alertId"] = activeAlertId; else doc["alertId"] = nullptr;
  doc["uptimeS"]     = millis() / 1000;
  doc["fw"]          = FW_VERSION;
  char buf[320];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topicStatus.c_str(), (const uint8_t *)buf, n, true);  // retido: o app vê o último estado ao abrir
}

void ensureConnectivity() {
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiAttemptMs >= WIFI_RETRY_MS) {
      lastWifiAttemptMs = now;
      Serial.println("[WIFI] Sem conexão, tentando de novo...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
    }
    return;
  }

  if (!ntpStarted) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
    Serial.printf("[WIFI] Conectado, IP %s\n", WiFi.localIP().toString().c_str());
  }

  if (mqtt.connected()) return;
  if (now - lastMqttAttemptMs < mqttRetryMs) return;
  lastMqttAttemptMs = now;

  char clientId[48];
  snprintf(clientId, sizeof(clientId), "%s-%04lx", DEVICE_ID, (unsigned long)(esp_random() & 0xFFFF));
  char will[96];
  snprintf(will, sizeof(will), "{\"deviceId\":\"%s\",\"online\":false}", DEVICE_ID);
  Serial.printf("[MQTT] Conectando em %s...\n", MQTT_HOST);
  if (mqtt.connect(clientId, topicStatus.c_str(), 1, true, will)) {
    mqttRetryMs = MQTT_RETRY_MIN_MS;
    mqtt.subscribe(topicCmd.c_str(), 1);
    Serial.printf("[MQTT] Conectado. Publicando em %s, ouvindo %s\n", topicEvents.c_str(), topicCmd.c_str());
    publishStatus(true);
    lastHeartbeatMs = millis();
    // Reconectou: tudo que ficou na fila sai agora.
    for (auto &e : outbox) if (e.used) e.lastSentMs = 0;
  } else {
    mqttRetryMs = min(mqttRetryMs * 2, MQTT_RETRY_MAX_MS);  // backoff exponencial
    Serial.printf("[MQTT] Falhou (estado %d). Nova tentativa em %lus\n", mqtt.state(),
                  (unsigned long)(mqttRetryMs / 1000));
  }
}

void serviceBattery() {
  static uint32_t lastReadMs = 0;
  uint32_t now = millis();
  if (now - lastReadMs < 1000) return;
  lastReadMs = now;

  batteryPct = readBatteryPct();
  if (!lowBattery && batteryPct <= LOW_BATTERY_PCT) {
    lowBattery = true;
    enqueue("low_battery", false);
    Serial.printf("[BATERIA] Baixa: %u%%\n", batteryPct);
  } else if (lowBattery && batteryPct >= LOW_BATTERY_CLEAR_PCT) {
    lowBattery = false;
    Serial.printf("[BATERIA] Normalizada: %u%%\n", batteryPct);
  }
}

// ───────────────────────── LED e buzzer ─────────────────────────
void serviceIndicators() {
  uint32_t now = millis();
  Color c = OFF;
  bool holding = btnStable == LOW && !longPressFired && now - btnPressedAtMs >= HOLD_HINT_MS;

  if (now < flashUntilMs) {
    c = (flashBlinkMs == 0 || blink(now, flashBlinkMs)) ? flashColor : OFF;
  } else if (holding) {
    c = AMBER;                                           // "continue segurando"
  } else if (now < findUntilMs) {
    c = blink(now, 150) ? BLUE : WHITE;
  } else if (alertActive() && !activeAlertAcked) {
    c = blink(now, 250) ? RED : OFF;                     // enviando / aguardando confirmação
  } else if (alertActive()) {
    c = pulse(now, 5000, 120) ? RED : OFF;               // alerta confirmado, ainda ativo
  } else if (!mqtt.connected()) {
    c = pulse(now, 1000, 100) ? BLUE : OFF;              // procurando rede
  } else if (lowBattery) {
    c = pulse(now, 4000, 150) ? AMBER : OFF;
  } else {
    c = pulse(now, 4000, 60) ? GREEN : OFF;              // tudo certo
  }
  setColor(c);

  // Buzzer só no "achar chaveiro". Alerta é sempre silencioso para não expor a vítima.
  static bool buzzerOn = false;
  bool wantBuzzer = now < findUntilMs && blink(now, 200);
  if (wantBuzzer != buzzerOn) {
    buzzerOn = wantBuzzer;
    if (buzzerOn) tone(PIN_BUZZER, 2700); else noTone(PIN_BUZZER);
  }
}

// ───────────────────────── setup / loop ─────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  analogReadResolution(12);
  setColor(WHITE);

  Serial.printf("\n=== Chaveiro de Pânico %s | id %s ===\n", FW_VERSION, DEVICE_ID);

  topicEvents = String(TOPIC_ROOT) + "/" + DEVICE_ID + "/events";
  topicStatus = String(TOPIC_ROOT) + "/" + DEVICE_ID + "/status";
  topicCmd    = String(TOPIC_ROOT) + "/" + DEVICE_ID + "/cmd";

  // Recupera estado salvo: um alerta disparado antes de um reinício continua valendo.
  prefs.begin("chaveiro", false);
  eventSeq = prefs.getUInt("seq", 0);
  String savedAlert = prefs.getString("alertId", "");
  strlcpy(activeAlertId, savedAlert.c_str(), sizeof(activeAlertId));
  activeAlertAcked = prefs.getBool("alertAcked", false);
  if (alertActive()) {
    Serial.printf("[ALERTA] Alerta %s recuperado da memória (%s).\n", activeAlertId,
                  activeAlertAcked ? "já confirmado" : "reenviando");
    if (!activeAlertAcked) enqueue("alert", true, activeAlertId, activeAlertId);
  }

  batteryPct = readBatteryPct();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  lastWifiAttemptMs = millis();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);  // folga para o simulador, que às vezes roda abaixo de 100%
}

void loop() {
  handleButton();
  ensureConnectivity();
  if (mqtt.connected()) mqtt.loop();
  serviceBattery();
  serviceOutbox();
  serviceIndicators();

  if (mqtt.connected() && (statusRequested || millis() - lastHeartbeatMs >= HEARTBEAT_MS)) {
    statusRequested = false;
    lastHeartbeatMs = millis();
    publishStatus(true);
  }
  delay(5);
}
