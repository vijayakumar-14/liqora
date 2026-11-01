#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

#define SUMP_TRIG 5
#define SUMP_ECHO 18
#define OHT_TRIG 4
#define OHT_ECHO 19
#define SUMP_FLOW_PIN 23
#define BORE_FLOW_PIN 32
#define RELAY_IN1 25
#define RELAY_IN2 26
#define RELAY_IN3 27
#define RELAY_IN4 33

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* FIREBASE_DATABASE_URL = "https://test-base-2fb81-default-rtdb.firebaseio.com";
const unsigned long FIREBASE_UPLOAD_INTERVAL_MS = 5000;

#define RELAY_ON LOW
#define RELAY_OFF HIGH

const float SUMP_EMPTY_DISTANCE = 9.25;
const float SUMP_FULL_DISTANCE = 2.50;
const float OHT_EMPTY_DISTANCE = 9.20;
const float OHT_FULL_DISTANCE = 2.50;

const float OHT_LOW_LEVEL = 30.0;
const float OHT_FULL_LEVEL = 95.0;
const float SUMP_PUMP_SAFE_START_LEVEL = 15.0;
const float SUMP_AVAILABLE_LEVEL = 40.0;
const float SUMP_MUNICIPAL_START_LEVEL = 15.0;
const float SUMP_MUNICIPAL_FULL_LEVEL = 98.0;

const float SUMP_PULSES_PER_LITRE = 450.0;
const float BORE_PULSES_PER_LITRE = 450.0;
const float FLOW_DETECTED_THRESHOLD = 0.10;

volatile unsigned long sumpPulseCount = 0;
volatile unsigned long borePulseCount = 0;
float sumpFlowRate = 0.0;
float boreFlowRate = 0.0;
unsigned long lastFlowCalculation = 0;

enum WaterSource {
  SOURCE_NONE,
  SOURCE_SUMP,
  SOURCE_BORE
};

WaterSource currentSource = SOURCE_NONE;

bool municipalFillingActive = false;
bool ohtFillingActive = false;
bool sumpAvailable = false;
bool sumpLockedOut = false;
bool boreLockedOut = false;

struct TelemetrySnapshot {
  float sumpLevel;
  float ohtLevel;
  float sumpFlowRate;
  float boreFlowRate;
  WaterSource currentSource;
  bool sumpAvailable;
  bool sumpLockedOut;
  bool boreLockedOut;
  bool municipalFillingActive;
  bool ohtFillingActive;
  bool sumpPumpOn;
  bool borePumpOn;
  bool municipalValveOn;
  bool relayIn4On;
};

TelemetrySnapshot firebaseSnapshot = {};
portMUX_TYPE firebaseSnapshotMux = portMUX_INITIALIZER_UNLOCKED;

unsigned long sumpPumpStartTime = 0;
unsigned long borePumpStartTime = 0;
const unsigned long SUMP_FLOW_CHECK_DELAY = 5000;
const unsigned long BORE_FLOW_CHECK_DELAY = 5000;
const unsigned long MOTOR_SWITCH_DELAY = 1500;
unsigned long lastStatusPrint = 0;
const unsigned long STATUS_PRINT_INTERVAL = 2000;

void IRAM_ATTR sumpFlowPulse() {
  sumpPulseCount++;
}

void IRAM_ATTR boreFlowPulse() {
  borePulseCount++;
}

float getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(5);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) {
    return -1;
  }

  return (duration * 0.0343) / 2.0;
}

float getAverageDistance(int trigPin, int echoPin) {
  const int NUM_READINGS = 10;
  float total = 0;
  int validReadings = 0;

  for (int i = 0; i < NUM_READINGS; i++) {
    float distance = getDistance(trigPin, echoPin);
    if (distance > 0) {
      total += distance;
      validReadings++;
    }
    delay(40);
  }

  if (validReadings == 0) {
    return -1;
  }

  return total / validReadings;
}

float calculateLevel(float distance, float emptyDistance, float fullDistance) {
  if (distance < 0) {
    return -1;
  }

  float level =
    ((emptyDistance - distance) / (emptyDistance - fullDistance)) * 100.0;

  if (level < 0) {
    level = 0;
  }
  if (level > 100) {
    level = 100;
  }

  return level;
}

void updateFlow() {
  unsigned long now = millis();

  if (now - lastFlowCalculation >= 1000) {
    noInterrupts();
    unsigned long sumpPulses = sumpPulseCount;
    unsigned long borePulses = borePulseCount;
    sumpPulseCount = 0;
    borePulseCount = 0;
    interrupts();

    sumpFlowRate = (sumpPulses / SUMP_PULSES_PER_LITRE) * 60.0;
    boreFlowRate = (borePulses / BORE_PULSES_PER_LITRE) * 60.0;
    lastFlowCalculation = now;
  }
}

void resetFlowMeasurement() {
  noInterrupts();
  sumpPulseCount = 0;
  borePulseCount = 0;
  interrupts();

  sumpFlowRate = 0.0;
  boreFlowRate = 0.0;
  lastFlowCalculation = millis();
}

void stopPumps() {
  digitalWrite(RELAY_IN1, RELAY_OFF);
  digitalWrite(RELAY_IN2, RELAY_OFF);
  currentSource = SOURCE_NONE;
  sumpPumpStartTime = 0;
  borePumpStartTime = 0;
}

void useSump() {
  if (currentSource == SOURCE_SUMP) {
    return;
  }

  if (digitalRead(RELAY_IN2) == RELAY_ON) {
    digitalWrite(RELAY_IN2, RELAY_OFF);
    Serial.println("BOREWELL PUMP OFF");
    delay(MOTOR_SWITCH_DELAY);
  }

  digitalWrite(RELAY_IN1, RELAY_ON);
  currentSource = SOURCE_SUMP;
  sumpPumpStartTime = millis();
  sumpFlowRate = 0.0;

  noInterrupts();
  sumpPulseCount = 0;
  interrupts();

  Serial.println();
  Serial.println(">>> SOURCE SELECTED: SUMP (PRIMARY) <<<");
}

void useBore() {
  if (currentSource == SOURCE_BORE) {
    return;
  }

  if (digitalRead(RELAY_IN1) == RELAY_ON) {
    digitalWrite(RELAY_IN1, RELAY_OFF);
    Serial.println("SUMP PUMP OFF");
    delay(MOTOR_SWITCH_DELAY);
  }

  digitalWrite(RELAY_IN2, RELAY_ON);
  currentSource = SOURCE_BORE;
  borePumpStartTime = millis();
  boreFlowRate = 0.0;

  noInterrupts();
  borePulseCount = 0;
  interrupts();

  Serial.println();
  Serial.println(">>> SOURCE SELECTED: BOREWELL (SECONDARY) <<<");
}

bool checkSumpDryRun() {
  if (currentSource != SOURCE_SUMP) {
    return false;
  }
  if (sumpPumpStartTime == 0) {
    return false;
  }
  if (millis() - sumpPumpStartTime < SUMP_FLOW_CHECK_DELAY) {
    return false;
  }
  if (sumpFlowRate <= FLOW_DETECTED_THRESHOLD) {
    return true;
  }
  return false;
}

bool checkBoreDryRun() {
  if (currentSource != SOURCE_BORE) {
    return false;
  }
  if (borePumpStartTime == 0) {
    return false;
  }
  if (millis() - borePumpStartTime < BORE_FLOW_CHECK_DELAY) {
    return false;
  }
  if (boreFlowRate <= FLOW_DETECTED_THRESHOLD) {
    return true;
  }
  return false;
}

const char* sourceName(WaterSource source) {
  switch (source) {
    case SOURCE_SUMP: return "SUMP";
    case SOURCE_BORE: return "BORE";
    default: return "NONE";
  }
}

void updateFirebaseSnapshot(float sumpLevel, float ohtLevel) {
  portENTER_CRITICAL(&firebaseSnapshotMux);

  firebaseSnapshot.sumpLevel = sumpLevel;
  firebaseSnapshot.ohtLevel = ohtLevel;
  firebaseSnapshot.sumpFlowRate = sumpFlowRate;
  firebaseSnapshot.boreFlowRate = boreFlowRate;
  firebaseSnapshot.currentSource = currentSource;
  firebaseSnapshot.sumpAvailable = sumpAvailable;
  firebaseSnapshot.sumpLockedOut = sumpLockedOut;
  firebaseSnapshot.boreLockedOut = boreLockedOut;
  firebaseSnapshot.municipalFillingActive = municipalFillingActive;
  firebaseSnapshot.ohtFillingActive = ohtFillingActive;
  firebaseSnapshot.sumpPumpOn = (digitalRead(RELAY_IN1) == RELAY_ON);
  firebaseSnapshot.borePumpOn = (digitalRead(RELAY_IN2) == RELAY_ON);
  firebaseSnapshot.municipalValveOn = (digitalRead(RELAY_IN3) == RELAY_ON);
  firebaseSnapshot.relayIn4On = (digitalRead(RELAY_IN4) == RELAY_ON);

  portEXIT_CRITICAL(&firebaseSnapshotMux);
}

String buildFirebasePayload(const TelemetrySnapshot& data) {
  String json;
  json.reserve(520);

  json += "{";
  json += "\"sumpLevel\":";
  json += (data.sumpLevel >= 0) ? String(data.sumpLevel, 1) : "null";
  json += ",\"ohtLevel\":";
  json += (data.ohtLevel >= 0) ? String(data.ohtLevel, 1) : "null";
  json += ",\"sumpFlowRate\":";
  json += String(data.sumpFlowRate, 2);
  json += ",\"boreFlowRate\":";
  json += String(data.boreFlowRate, 2);
  json += ",\"currentSource\":\"";
  json += sourceName(data.currentSource);
  json += "\",\"sumpAvailable\":";
  json += data.sumpAvailable ? "true" : "false";
  json += ",\"sumpLockedOut\":";
  json += data.sumpLockedOut ? "true" : "false";
  json += ",\"boreLockedOut\":";
  json += data.boreLockedOut ? "true" : "false";
  json += ",\"municipalFillingActive\":";
  json += data.municipalFillingActive ? "true" : "false";
  json += ",\"ohtFillingActive\":";
  json += data.ohtFillingActive ? "true" : "false";
  json += ",\"relays\":{";
  json += "\"sumpPump\":";
  json += data.sumpPumpOn ? "true" : "false";
  json += ",\"borePump\":";
  json += data.borePumpOn ? "true" : "false";
  json += ",\"municipalValve\":";
  json += data.municipalValveOn ? "true" : "false";
  json += ",\"in4\":";
  json += data.relayIn4On ? "true" : "false";
  json += "},\"updatedAt\":{\".sv\":\"timestamp\"}}";

  return json;
}

void firebaseTelemetryTask(void* parameter) {
  vTaskDelay(pdMS_TO_TICKS(5000));

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    TelemetrySnapshot snapshot;
    portENTER_CRITICAL(&firebaseSnapshotMux);
    snapshot = firebaseSnapshot;
    portEXIT_CRITICAL(&firebaseSnapshotMux);

    WiFiClientSecure tlsClient;
    tlsClient.setInsecure();

    HTTPClient http;
    String url = String(FIREBASE_DATABASE_URL) + "/liquora/telemetry.json";
    http.setConnectTimeout(1500);
    http.setTimeout(1500);

    if (http.begin(tlsClient, url)) {
      http.addHeader("Content-Type", "application/json");
      int responseCode = http.PUT(buildFirebasePayload(snapshot));

      if (responseCode < 200 || responseCode >= 300) {
        Serial.printf("Firebase telemetry write failed: %d\n", responseCode);
      }

      http.end();
    } else {
      Serial.println("Could not start Firebase HTTPS request");
    }

    vTaskDelay(pdMS_TO_TICKS(FIREBASE_UPLOAD_INTERVAL_MS));
  }
}

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  xTaskCreatePinnedToCore(
    firebaseTelemetryTask,
    "FirebaseTelemetry",
    8192,
    nullptr,
    1,
    nullptr,
    0
  );

  pinMode(SUMP_TRIG, OUTPUT);
  pinMode(SUMP_ECHO, INPUT);
  pinMode(OHT_TRIG, OUTPUT);
  pinMode(OHT_ECHO, INPUT);
  digitalWrite(SUMP_TRIG, LOW);
  digitalWrite(OHT_TRIG, LOW);

  pinMode(SUMP_FLOW_PIN, INPUT_PULLUP);
  pinMode(BORE_FLOW_PIN, INPUT_PULLUP);

  attachInterrupt(
    digitalPinToInterrupt(SUMP_FLOW_PIN),
    sumpFlowPulse,
    FALLING
  );
  attachInterrupt(
    digitalPinToInterrupt(BORE_FLOW_PIN),
    boreFlowPulse,
    FALLING
  );

  pinMode(RELAY_IN1, OUTPUT);
  pinMode(RELAY_IN2, OUTPUT);
  pinMode(RELAY_IN3, OUTPUT);
  pinMode(RELAY_IN4, OUTPUT);

  digitalWrite(RELAY_IN1, RELAY_OFF);
  digitalWrite(RELAY_IN2, RELAY_OFF);
  digitalWrite(RELAY_IN3, RELAY_OFF);
  digitalWrite(RELAY_IN4, RELAY_ON);

  sumpFlowRate = 0.0;
  boreFlowRate = 0.0;
  lastFlowCalculation = millis();

  Serial.println();
  Serial.println("============================================");
  Serial.println("       LIQORA WATER MANAGEMENT SYSTEM");
  Serial.println("============================================");
  Serial.println("PRIORITY        : SUMP -> BOREWELL");
  Serial.println("SUMP            : PRIMARY SOURCE");
  Serial.println("BOREWELL        : SECONDARY SOURCE");
  Serial.println("OHT LOW         : 30%");
  Serial.println("OHT FULL        : 95%");
  Serial.println("SUMP SAFE       : 15%");
  Serial.println("SUMP AVAILABLE  : 40%");
  Serial.println("MUNICIPAL START : <15%");
  Serial.println("MUNICIPAL STOP  : 98%");
  Serial.println("SUMP FULL DIST  : 2.50 cm");
  Serial.println("OHT FULL DIST   : 2.50 cm");
  Serial.println("INTERLOCK       : MUTUALLY EXCLUSIVE");
  Serial.println("FLOW MONITORING : SUMP + BORE");
  Serial.println("RELAY IN4       : ALWAYS ON");
  Serial.println("SYSTEM READY");
  Serial.println("============================================");
}

void loop() {
  float sumpDistance = getAverageDistance(SUMP_TRIG, SUMP_ECHO);
  float sumpLevel = calculateLevel(
    sumpDistance,
    SUMP_EMPTY_DISTANCE,
    SUMP_FULL_DISTANCE
  );

  float ohtDistance = getAverageDistance(OHT_TRIG, OHT_ECHO);
  float ohtLevel = calculateLevel(
    ohtDistance,
    OHT_EMPTY_DISTANCE,
    OHT_FULL_DISTANCE
  );

  updateFlow();
  digitalWrite(RELAY_IN4, RELAY_ON);

  if (sumpLevel >= 0 &&
      sumpLevel < SUMP_MUNICIPAL_START_LEVEL &&
      !municipalFillingActive) {
    municipalFillingActive = true;
    digitalWrite(RELAY_IN3, RELAY_ON);
    Serial.println();
    Serial.println("MUNICIPAL: SUMP BELOW 15% -> VALVE ON");
  }

  if (municipalFillingActive) {
    digitalWrite(RELAY_IN3, RELAY_ON);

    if (sumpLevel >= SUMP_MUNICIPAL_FULL_LEVEL) {
      municipalFillingActive = false;
      digitalWrite(RELAY_IN3, RELAY_OFF);
      sumpLockedOut = false;
      boreLockedOut = false;
      Serial.println();
      Serial.println("MUNICIPAL: SUMP REACHED 98% -> VALVE OFF");
      Serial.println("SOURCE LOCKOUTS RESET");
    }
  }

  if (sumpLevel < 0) {
    sumpAvailable = false;
  } else if (sumpLevel < SUMP_PUMP_SAFE_START_LEVEL) {
    sumpAvailable = false;
  } else if (sumpLevel >= SUMP_AVAILABLE_LEVEL) {
    sumpAvailable = true;
  }

  if (sumpLevel < 0 || ohtLevel < 0) {
    stopPumps();
    ohtFillingActive = false;
    Serial.println();
    Serial.println("!!! SENSOR ERROR -> ALL PUMPS OFF !!!");
  } else if (ohtLevel >= OHT_FULL_LEVEL) {
    stopPumps();
    ohtFillingActive = false;
    Serial.println();
    Serial.println("OHT >= 95% -> ALL PUMPS OFF");
  } else if (ohtLevel <= OHT_LOW_LEVEL) {
    ohtFillingActive = true;
  }

  if (ohtFillingActive) {
    if (sumpAvailable && !sumpLockedOut) {
      if (currentSource != SOURCE_SUMP) {
        useSump();
      }
    } else if (!boreLockedOut) {
      if (currentSource != SOURCE_BORE) {
        useBore();
      }
    } else {
      stopPumps();
      Serial.println();
      Serial.println("!!! NO WATER SOURCE AVAILABLE !!!");
    }
  }

  if (currentSource == SOURCE_SUMP) {
    if (checkSumpDryRun()) {
      Serial.println();
      Serial.println("!!! SUMP PUMP DRY RUN DETECTED !!!");
      stopPumps();
      sumpLockedOut = true;
      Serial.println("!!! SUMP PUMP LOCKED OUT !!!");

      if (!boreLockedOut && ohtFillingActive) {
        Serial.println(">>> SWITCHING TO BOREWELL <<<");
        useBore();
      }
    }
  }

  if (currentSource == SOURCE_BORE) {
    if (checkBoreDryRun()) {
      Serial.println();
      Serial.println("!!! BOREWELL DRY RUN DETECTED !!!");
      stopPumps();
      boreLockedOut = true;
      Serial.println("!!! BOREWELL PUMP LOCKED OUT !!!");

      if (sumpAvailable && !sumpLockedOut && ohtFillingActive) {
        Serial.println(">>> SWITCHING BACK TO SUMP <<<");
        useSump();
      } else {
        Serial.println("!!! BOTH WATER SOURCES UNAVAILABLE !!!");
      }
    }
  }

  updateFirebaseSnapshot(sumpLevel, ohtLevel);

  if (millis() - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
    lastStatusPrint = millis();
    Serial.println();
    Serial.println("---------------- LIQORA STATUS ----------------");

    Serial.print("Sump Level       : ");
    Serial.print(sumpLevel, 1);
    Serial.println("%");

    Serial.print("OHT Level        : ");
    Serial.print(ohtLevel, 1);
    Serial.println("%");

    Serial.print("Sump Flow        : ");
    Serial.print(sumpFlowRate, 2);
    Serial.println(" L/min");

    Serial.print("Bore Flow        : ");
    Serial.print(boreFlowRate, 2);
    Serial.println(" L/min");

    Serial.print("Municipal Valve  : ");
    if (digitalRead(RELAY_IN3) == RELAY_ON) {
      Serial.println("ON");
    } else {
      Serial.println("OFF");
    }

    Serial.print("Active Source    : ");
    switch (currentSource) {
      case SOURCE_SUMP:
        Serial.println("SUMP -> OHT (PRIMARY)");
        break;
      case SOURCE_BORE:
        Serial.println("BOREWELL -> OHT (SECONDARY)");
        break;
      default:
        Serial.println("NONE");
        break;
    }

    Serial.print("Sump Available   : ");
    if (sumpAvailable) {
      Serial.println("YES");
    } else {
      Serial.println("NO");
    }

    Serial.print("Sump Lockout     : ");
    if (sumpLockedOut) {
      Serial.println("YES");
    } else {
      Serial.println("NO");
    }

    Serial.print("Bore Lockout     : ");
    if (boreLockedOut) {
      Serial.println("YES");
    } else {
      Serial.println("NO");
    }

    Serial.print("OHT Filling      : ");
    if (ohtFillingActive) {
      Serial.println("ACTIVE");
    } else {
      Serial.println("INACTIVE");
    }

    Serial.println("Relay IN4        : ALWAYS ON");
    Serial.println("------------------------------------------------");
  }
}
