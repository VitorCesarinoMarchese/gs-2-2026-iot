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
  uint32_t order;         // ordem de criação (contador; desempata eventos do mesmo milissegundo)
  uint32_t lastSentMs;
  uint16_t attempts;
};

static const uint8_t OUTBOX_SIZE = 6;
OutEvent outbox[OUTBOX_SIZE];
uint32_t outboxOrder = 0;

uint32_t eventSeq = 0;
char     activeAlertId[40] = "";
bool     activeAlertAcked  = false;
time_t   activeAlertTs     = 0;      // hora do clique do alerta ativo (sobrevive a reinício)
uint32_t bootTag           = 0;      // muda a cada boot: eventId não repete mesmo se a flash for apagada

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
// ISR registra o gesto mesmo com o loop bloqueado (ex.: conectando ao broker)
volatile bool     pressLatched   = false;  // houve descida
volatile bool     pressReleased  = false;  // houve subida depois dela
volatile uint32_t pressLatchMs   = 0;      // primeira descida
volatile uint32_t pressReleaseMs = 0;      // última subida
volatile uint32_t lastRiseMs     = 0;      // primeira subida depois de um aperto aceito pelo debounce
volatile bool     riseArmed      = false;  // aperto aceito, ainda sem soltura registrada

// Conectividade
uint32_t lastWifiAttemptMs = 0;
uint32_t lastMqttAttemptMs = 0;
uint32_t mqttRetryMs       = MQTT_RETRY_MIN_MS;
bool     ntpStarted        = false;
uint32_t lastHeartbeatMs   = 0;
bool     statusRequested   = false;

// Feedback visual/sonoro (início + duração: seguro na virada do millis())
Color    flashColor   = OFF;
bool     flashActive  = false;
uint32_t flashStartMs = 0;
uint32_t flashDurMs   = 0;
uint16_t flashBlinkMs = 0;   // 0 = aceso fixo
bool     findActive   = false;
uint32_t findStartMs  = 0;
bool     beepActive   = false;  // bipe curto de "teste confirmado"
uint32_t beepStartMs  = 0;
bool     buzzerOn     = false;

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
  flashActive  = true;
  flashStartMs = millis();
  flashDurMs   = durationMs;
  flashBlinkMs = blinkMs;
}

void stopSound() {
  findActive = false;
  beepActive = false;
  if (buzzerOn) { noTone(PIN_BUZZER); buzzerOn = false; }
}

// Guarda o primeiro gesto completo (aperto + soltura). Repique: uma descida logo depois de uma
// subida desfaz a "soltura". Toques seguintes, já com o gesto completo, são ignorados.
void IRAM_ATTR onButtonChange() {
  uint32_t t = millis();
  if (digitalRead(PIN_BUTTON) == LOW) {
    if (!pressLatched) {
      if (t - lastRiseMs < DEBOUNCE_MS) return;  // repique da soltura que acabou de ser registrada
      pressLatched = true; pressReleased = false; pressLatchMs = t;
    } else if (pressReleased && t - pressReleaseMs < DEBOUNCE_MS) {
      pressReleased = false;                 // era repique, o botão continua apertado
    }
  } else {
    if (riseArmed) { riseArmed = false; lastRiseMs = t; }
    if (pressLatched && !pressReleased) { pressReleased = true; pressReleaseMs = t; }
  }
}

// Pulso curto a cada "period" ms: discreto e economiza bateria.
bool pulse(uint32_t now, uint32_t period, uint32_t onMs) { return (now % period) < onMs; }
bool blink(uint32_t now, uint32_t halfPeriod) { return (now / halfPeriod) % 2 == 0; }

bool alertActive() { return activeAlertId[0] != '\0'; }

void persistAlert() {
  prefs.putString("alertId", activeAlertId);
  prefs.putBool("alertAcked", activeAlertAcked);
  prefs.putLong64("alertTs", (int64_t)activeAlertTs);
}

uint8_t readBatteryPct() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_BATTERY);
  return (uint8_t)((sum / 8) * 100 / 4095);
}

// ───────────────────────── Fila de saída ─────────────────────────
// Prioridade na fila cheia: teste e bateria saem primeiro, cancelamento depois, alerta nunca.
int eventRank(const char *type) {
  if (strcmp(type, "alert") == 0) return 2;
  if (strcmp(type, "cancel") == 0) return 1;
  return 0;
}

OutEvent *enqueue(const char *type, bool needsAck, const char *alertId = nullptr,
                  const char *reuseEventId = nullptr) {
  OutEvent *slot = nullptr;
  for (auto &e : outbox) if (!e.used) { slot = &e; break; }
  if (!slot) {
    for (auto &e : outbox) {
      int re = eventRank(e.type), rs = slot ? eventRank(slot->type) : 99;
      if (re < rs || (re == rs && e.order < slot->order)) slot = &e;
    }
    // Fila cheia de eventos mais importantes que este: o novo é que fica de fora.
    if (eventRank(slot->type) > eventRank(type)) {
      Serial.printf("[FILA] Cheia; evento %s descartado.\n", type);
      return nullptr;
    }
  }

  memset(slot, 0, sizeof(*slot));
  slot->used     = true;
  slot->needsAck = needsAck;
  if (reuseEventId) {
    strlcpy(slot->eventId, reuseEventId, sizeof(slot->eventId));
  } else {
    // Etiqueta do boot: a hora do relógio (se já sincronizou) ou um número aleatório.
    if (!bootTag) bootTag = nowEpoch() ? (uint32_t)nowEpoch() : ((esp_random() ^ micros()) | 1);
    eventSeq++;
    prefs.putUInt("seq", eventSeq);
    snprintf(slot->eventId, sizeof(slot->eventId), "%s-%08lx-%lu", DEVICE_ID, (unsigned long)bootTag,
             (unsigned long)eventSeq);
  }
  strlcpy(slot->type, type, sizeof(slot->type));
  strlcpy(slot->alertId, alertId ? alertId : "", sizeof(slot->alertId));
  slot->createdTs = nowEpoch();
  slot->createdMs = millis();
  slot->order     = ++outboxOrder;
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

// Regra de ordem: um evento nunca sai antes dos eventos mais antigos ainda sem confirmação.
// Sempre que algo sai, os pendentes mais antigos saem junto, antes, em ordem de criação.
// O app processa na ordem de chegada, então um "cancel" antigo nunca chega depois de um
// alerta mais novo (e não fecha a emergência nova).
void serviceOutbox() {
  if (!mqtt.connected()) return;
  uint32_t now = millis();
  bool due[OUTBOX_SIZE] = {};
  int newestDue = -1;
  for (int i = 0; i < OUTBOX_SIZE; i++) {
    OutEvent &e = outbox[i];
    if (!e.used) continue;
    due[i] = e.attempts == 0 || (e.needsAck && now - e.lastSentMs >= RETRY_INTERVAL_MS);
    if (due[i] && (newestDue < 0 || e.order > outbox[newestDue].order)) newestDue = i;
  }
  if (newestDue < 0) return;
  for (int i = 0; i < OUTBOX_SIZE; i++) {
    OutEvent &e = outbox[i];
    if (e.used && e.needsAck && e.order < outbox[newestDue].order) due[i] = true;
  }
  bool done[OUTBOX_SIZE] = {};
  for (;;) {
    int pick = -1;
    for (int i = 0; i < OUTBOX_SIZE; i++) {
      OutEvent &e = outbox[i];
      if (done[i] || !e.used || !due[i]) continue;
      if (pick < 0 || e.order < outbox[pick].order) pick = i;
    }
    if (pick < 0) break;
    done[pick] = true;
    // Falhou: para aqui, para nenhum evento mais novo passar na frente; tenta de novo depois.
    if (!publishEvent(outbox[pick])) break;
    if (!outbox[pick].needsAck) outbox[pick].used = false;  // dispara-e-esquece
  }
}

void dropOutboxForAlert(const char *alertId) {
  for (auto &e : outbox) {
    if (e.used && (strcmp(e.alertId, alertId) == 0 || strcmp(e.eventId, alertId) == 0)) e.used = false;
  }
}

// ───────────────────────── Ações do usuário ─────────────────────────
void triggerAlert() {
  stopSound();  // alerta é silencioso: corta qualquer som em andamento

  if (alertActive() && !activeAlertAcked) {
    // Clique extra com alerta ainda sem confirmação: reenvia já, sem esperar o próximo ciclo.
    for (auto &e : outbox) {
      if (e.used && strcmp(e.eventId, activeAlertId) == 0) e.lastSentMs = millis() - RETRY_INTERVAL_MS;
    }
    Serial.println("[ALERTA] Já existe alerta aguardando confirmação; reforçando envio.");
    serviceOutbox();
    return;
  }
  // Alerta anterior já confirmado e ainda aberto: um novo aperto é uma nova emergência.
  if (alertActive()) Serial.printf("[ALERTA] Alerta %s já confirmado; abrindo um novo.\n", activeAlertId);

  OutEvent *e = enqueue("alert", true);
  strlcpy(e->alertId, e->eventId, sizeof(e->alertId));
  strlcpy(activeAlertId, e->eventId, sizeof(activeAlertId));
  activeAlertAcked = false;
  activeAlertTs    = e->createdTs;
  persistAlert();
  statusRequested = true;
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
  activeAlertTs    = 0;
  persistAlert();
  statusRequested = true;
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
void registerClick(uint32_t now) {
  if (now - lastClickMs > MULTI_CLICK_WINDOW_MS) clickCount = 0;
  clickCount++;
  lastClickMs = now;
  if (clickCount >= CLICKS_TO_TRIGGER) { clickCount = 0; triggerAlert(); }
}

// Gesto inteiro (aperto + soltura) que o polling não viu porque o loop estava parado.
void handleRecoveredGesture(uint32_t held, uint32_t now) {
  if (held >= LONG_PRESS_MS) {
    Serial.println("[BOTAO] Pressão longa registrada enquanto o chaveiro estava ocupado.");
    clickCount = 0;
    if (alertActive()) cancelAlert(); else sendTest();
  } else if (held >= DEBOUNCE_MS) {  // mais curto que isso é ruído, como no debounce normal
    Serial.println("[BOTAO] Clique registrado enquanto o chaveiro estava ocupado.");
    registerClick(now);
  }
}

void handleButton() {
  uint32_t now = millis();
  bool reading = digitalRead(PIN_BUTTON);

  if (reading != btnLastRead) { btnLastRead = reading; btnLastChangeMs = now; }

  if (now - btnLastChangeMs >= DEBOUNCE_MS && reading != btnStable) {
    btnStable = reading;
    bool     hadLatch   = pressLatched;
    bool     hadRelease = pressReleased;
    uint32_t latchMs    = pressLatchMs;
    uint32_t releaseMs  = pressReleaseMs;
    pressLatched  = false;           // o polling viu a mudança; a trava da ISR não é mais necessária
    pressReleased = false;
    if (btnStable == LOW) {          // apertou
      // Um toque completo ficou guardado na ISR antes deste aperto: trata ele primeiro.
      if (hadLatch && hadRelease) handleRecoveredGesture(releaseMs - latchMs, now);
      // Se o aperto começou com o loop parado, a ISR sabe a hora real.
      // (Uma trava que já tem soltura é de um toque anterior: não serve como início deste aperto.)
      btnPressedAtMs = (hadLatch && !hadRelease) ? latchMs : now;
      longPressFired = false;
      riseArmed      = true;
    } else {                         // soltou
      bool rose = !riseArmed;        // a ISR registrou a hora da soltura deste aperto
      riseArmed = false;
      if (longPressFired) {
        // pressão longa já foi tratada enquanto segurava
      } else if (rose && (int32_t)(lastRiseMs - btnPressedAtMs) >= (int32_t)LONG_PRESS_MS) {
        // O loop travou com o botão seguro por 3 s: pela hora real da soltura, é pressão longa.
        clickCount = 0;
        if (alertActive()) cancelAlert(); else sendTest();
      } else {
        registerClick(now);          // soltou antes de 3 s: conta como clique
      }
      // Um toque inteiro guardado pela ISR depois desta soltura (loop parado): trata também.
      if (hadLatch && hadRelease) handleRecoveredGesture(releaseMs - latchMs, now);
    }
  }

  // Aperto e soltura inteiros aconteceram com o loop parado (ex.: conectando ao broker):
  // o polling nunca viu o botão em LOW, mas a ISR guardou quando apertou e quando soltou.
  // millis() novo e diferença com sinal: a ISR pode ter rodado depois de "now" ser lido.
  if (pressLatched && pressReleased && btnStable == HIGH &&
      (int32_t)(millis() - pressReleaseMs) >= (int32_t)DEBOUNCE_MS && digitalRead(PIN_BUTTON) == HIGH) {
    uint32_t held = pressReleaseMs - pressLatchMs;
    pressLatched  = false;
    pressReleased = false;
    handleRecoveredGesture(held, now);
  }

  // Pressão longa dispara enquanto ainda está segurando, para o usuário sentir a resposta.
  // "reading == LOW" e "riseArmed": se o botão já foi solto (soltura ainda no debounce, ou durante
  // um travamento do loop), quem decide é a soltura, medida pela hora real.
  if (btnStable == LOW && reading == LOW && riseArmed && !longPressFired && now - btnPressedAtMs >= LONG_PRESS_MS) {
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
        if (!alertActive()) { beepActive = true; beepStartMs = millis(); }  // nunca faz som com alerta ativo
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
      activeAlertTs    = 0;
      persistAlert();
      statusRequested = true;
      flash(GREEN, 1500, 150);
      Serial.println("[ALERTA] Encerrado pelo app.");
    }
  } else if (strcmp(cmd, "find") == 0) {
    if (alertActive()) {
      Serial.println("[ACHAR] Ignorado: com alerta ativo o chaveiro fica em silêncio.");
    } else {
      findActive  = true;
      findStartMs = millis();
      Serial.println("[ACHAR] Tocando para localizar o chaveiro.");
    }
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

  // clientId fixo: ao reconectar, o broker substitui a sessão antiga em vez de, minutos depois,
  // publicar o "offline" dela por cima do status atual.
  const char *clientId = DEVICE_ID;
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
  if (flashActive && now - flashStartMs >= flashDurMs) flashActive = false;
  if (findActive && now - findStartMs >= FIND_DURATION_MS) findActive = false;
  if (beepActive && now - beepStartMs >= 120) beepActive = false;

  if (flashActive) {
    c = (flashBlinkMs == 0 || blink(now, flashBlinkMs)) ? flashColor : OFF;
  } else if (holding) {
    c = AMBER;                                           // "continue segurando"
  } else if (findActive) {
    c = blink(now, 150) ? BLUE : WHITE;
  } else if (alertActive() && !activeAlertAcked) {
    c = pulse(now, 1000, 80) ? RED : OFF;                // enviando / aguardando confirmação (discreto)
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
  bool wantBuzzer = !alertActive() && (beepActive || (findActive && blink(now, 200)));
  if (wantBuzzer != buzzerOn) {
    buzzerOn = wantBuzzer;
    if (buzzerOn) tone(PIN_BUZZER, 2700); else noTone(PIN_BUZZER);
  }
}

// ───────────────────────── setup / loop ─────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButtonChange, CHANGE);
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
  activeAlertTs    = (time_t)prefs.getLong64("alertTs", 0);
  if (alertActive()) {
    Serial.printf("[ALERTA] Alerta %s recuperado da memória (%s).\n", activeAlertId,
                  activeAlertAcked ? "já confirmado" : "reenviando");
    if (!activeAlertAcked) {
      OutEvent *e = enqueue("alert", true, activeAlertId, activeAlertId);
      e->createdTs = activeAlertTs;  // mantém a hora original do clique (ageMs passa a contar do reinício)
    }
  }

  batteryPct = readBatteryPct();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  lastWifiAttemptMs = millis();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(5);  // espera no máximo 5 s pelo CONNACK (padrão 15 s)
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
