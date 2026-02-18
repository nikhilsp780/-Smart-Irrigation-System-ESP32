/*
  Final Irrigation + Sensor Check Sketch
  - Combines:
    • Sensor quick-check (ADC smoothing, DHT safe reads, thresholds)
    • Blynk dashboard updates (V1..V13 as used)
    • OpenWeather forecast (city-id mode) with textual weather
    • Auto/manual pump control
    • Optional flow sensor (kept but not required)
  - Serial prints a concise single-line summary each telemetry update for quick verification.
*/

#define BLYNK_TEMPLATE_ID "TMPL37Gcy-DPx"
#define BLYNK_TEMPLATE_NAME "IrrigationSystem"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include "DHT.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ctype.h> // isdigit

/* ---------------- CONFIG ---------------- */
char auth[] = "a6Uc7A2JFi4t6Ebji_qGtkK8AyZh3dos"; // Blynk auth token
const char* ssid = "Airtel_Prakash";
const char* pass = "Nikhil.sp";

/* --------- OpenWeather settings (CITY-ID MODE) --------- */
const char* OPENWEATHER_KEY = "50e6c58d9028442f63df047022ceafde"; // <-- your API key
const char* CITY_ID          = "1253315";                         // <-- Mysore (or change to Bogadi etc.)
/* ------------------------------------------------------ */

/* Pins */
const int PIN_SOIL  = 32;
const int PIN_DHT   = 4;
const int PIN_RELAY = 23;
const int PIN_FLOW  = 14;  // optional; remove interrupt attach if you don't have a flow sensor
const int PIN_RAIN  = 15;

#define DHTTYPE DHT22
DHT dht(PIN_DHT, DHTTYPE);

BlynkTimer timer;

/* Calibration */
int SOIL_ADC_WET = 1500;   // adjust after your calibration
int SOIL_ADC_DRY = 3000;   // adjust after your calibration

/* Thresholds & timings */
float THRESHOLD_LOW = 40.0; // percent
unsigned long MIN_WATER_INTERVAL = 6UL * 60UL * 60UL * 1000UL; // 6 hours
const unsigned long FORECAST_INTERVAL = 10UL * 60UL * 1000UL; // 10 minutes

/* Globals */
volatile unsigned long flowCount = 0;
unsigned long lastFlowCalcMillis = 0;
float flowRateLperMin = 0.0;

bool pumpState = false;
unsigned long lastWaterMillis = 0;
int modeAuto = 1;
int manualPumpDuration = 20;

/* Forecast cache (extended) */
bool forecastWillRain = false;
unsigned long lastForecastCheck = 0;
String forecastWeatherMain = "";   // e.g. "Rain", "Clouds", "Mist", "Clear"
String forecastWeatherDesc = "";   // e.g. "light rain"

/* Virtual pins (match to your Blynk dashboard) */
#define V_SOIL        V1    // soil percent
#define V_TEMP        V2
#define V_HUM         V3
#define V_WEATHER     V5    // forecast boolean (0/1)
#define V_AUTOMODE    V10
#define V_PUMP        V11
#define V_MANUAL_PUMP V11
#define V_PUMP_TIME   V6
#define V_CHART       V7
#define V_FLOW        V9
#define V_WEATHER_TEXT V8   // forecast main as text
#define V_WEATHER_DESC V13  // forecast description text
#define V_RAIN_SENSOR  V12  // raw rain state

/* --- Smoothing / debouncing globals --- */
float soilEMA = -1.0;
const float SOIL_EMA_ALPHA = 0.20; // 0..1 (lower = smoother)
const int SOIL_SAMPLES = 6;
const float SOIL_WRITE_THRESHOLD = 1.0; // percent

float lastSentSoil = -999;
float lastSentTemp = -999;
float lastSentHum  = -999;
const float TEMP_THRESHOLD = 0.5;
const float HUM_THRESHOLD  = 1.0;

unsigned long lastDHTReadMillis = 0;
const unsigned long DHT_MIN_INTERVAL = 2000; // ms

/* Flow ISR (if you have flow sensor) */
void IRAM_ATTR flowPulseISR() {
  flowCount++;
}

/* ---------- Helpers ---------- */

float readSoilRawAverage() {
  #ifdef analogSetPinAttenuation
    analogSetPinAttenuation(PIN_SOIL, ADC_11db);
  #endif
  long sum = 0;
  for (int i = 0; i < SOIL_SAMPLES; ++i) {
    sum += analogRead(PIN_SOIL);
    delay(5);
  }
  return (float)sum / (float)SOIL_SAMPLES;
}

float readSoilPercent_smoothed() {
  float raw = readSoilRawAverage();
  float pct = (raw - (float)SOIL_ADC_WET) / (float)(SOIL_ADC_DRY - SOIL_ADC_WET) * 100.0;
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  if (soilEMA < 0.0f) soilEMA = pct;
  soilEMA = SOIL_EMA_ALPHA * pct + (1.0f - SOIL_EMA_ALPHA) * soilEMA;
  return soilEMA;
}

float safeReadTemp() {
  unsigned long now = millis();
  if (now - lastDHTReadMillis < DHT_MIN_INTERVAL) return lastSentTemp;
  lastDHTReadMillis = now;
  float t = dht.readTemperature();
  if (isnan(t)) {
    Serial.println("DHT temp NaN — keeping last value");
    return lastSentTemp;
  }
  return t;
}

float safeReadHum() {
  unsigned long now = millis();
  // don't update lastDHTReadMillis here — safeReadTemp manages timing
  float h = dht.readHumidity();
  if (isnan(h)) {
    Serial.println("DHT hum NaN — keeping last value");
    return lastSentHum;
  }
  return h;
}

/* Build the correct forecast URL using id= */
String buildForecastUrlById() {
  if (!CITY_ID || CITY_ID[0] == '\0' || !OPENWEATHER_KEY || OPENWEATHER_KEY[0] == '\0') {
    return String("");
  }
  String url = String("http://api.openweathermap.org/data/2.5/forecast?id=") +
               CITY_ID +
               "&appid=" + OPENWEATHER_KEY +
               "&units=metric";
  return url;
}

/* ---------- OpenWeather forecast (city-id mode) ----------
   returns true if rain expected in the checked slots and ALSO
   sets forecastWeatherMain / forecastWeatherDesc for textual display */
bool checkForecastWillRainNow() {
  // clear previous
  forecastWeatherMain = "";
  forecastWeatherDesc = "";

  String url = buildForecastUrlById();
  if (url.length() == 0) {
    Serial.println("OpenWeather config empty: set OPENWEATHER_KEY and CITY_ID.");
    return false;
  }

  Serial.print("HTTP GET: ");
  Serial.println(url);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  Serial.print("HTTP response code: ");
  Serial.println(code);

  String body = http.getString(); // read body regardless
  if (code != 200) {
    Serial.println("OpenWeather returned non-200. Response body:");
    Serial.println(body);
    http.end();
    return false;
  }

  // print preview for debugging
  Serial.println("OpenWeather response (preview):");
  if (body.length() > 600) {
    Serial.println(body.substring(0, 600));
  } else {
    Serial.println(body);
  }

  http.end();

  // Parse JSON
  StaticJsonDocument<12000> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  bool willRain = false;

  // Find first valid forecast slot that has weather
  for (int i = 0; i < 3; ++i) { // check first ~3 slots (next ~9 hours)
    JsonObject item = doc["list"][i];
    if (item.isNull()) continue;

    // rain field (explicit)
    if (!item["rain"].isNull()) {
      JsonVariant rv = item["rain"];
      if (!rv.isNull()) {
        if (!rv["3h"].isNull()) {
          float r = rv["3h"].as<float>();
          if (r > 0.0) {
            willRain = true;
          }
        } else {
          // presence indicates some rain data
          willRain = true;
        }
      }
    }

    // weather main / description
    const char* wmain = nullptr;
    const char* wdesc = nullptr;
    if (!item["weather"].isNull() && !item["weather"][0].isNull()) {
      wmain = item["weather"][0]["main"].as<const char*>();
      wdesc = item["weather"][0]["description"].as<const char*>();
    }

    if (wmain) {
      forecastWeatherMain = String(wmain);
      if (wdesc) forecastWeatherDesc = String(wdesc);
      Serial.print("Slot ");
      Serial.print(i);
      Serial.print(" weather.main = ");
      Serial.print(forecastWeatherMain);
      Serial.print(" / desc = ");
      Serial.println(forecastWeatherDesc);
      // check main category for rain-like types
      if (forecastWeatherMain == "Rain" ||
          forecastWeatherMain == "Thunderstorm" ||
          forecastWeatherMain == "Drizzle") {
        willRain = true;
      }
      // Use first available meaningful weather slot
      break;
    }
  }

  if (forecastWeatherMain.length() == 0) {
    Serial.println("Forecast check: no weather.main found in checked slots.");
  } else {
    Serial.print("Forecast weather main: ");
    Serial.println(forecastWeatherMain);
  }

  Serial.print("Forecast check: willRain = ");
  Serial.println(willRain ? "true" : "false");

  // set global and return
  forecastWillRain = willRain;
  return willRain;
}

/* Pump control */
void setPump(bool on) {
  digitalWrite(PIN_RELAY, on ? LOW : HIGH); // active LOW assumed
  pumpState = on;
  if (Blynk.connected()) {
    Blynk.virtualWrite(V_PUMP, on ? 1 : 0);
  }
}

/* Blynk handlers */
BLYNK_WRITE(V_MANUAL_PUMP) {
  int v = param.asInt();
  if (modeAuto == 0) {
    if (v == 1) {
      setPump(true);
      timer.setTimeout((unsigned long)manualPumpDuration * 1000UL, []() { setPump(false); });
    } else {
      setPump(false);
    }
  } else {
    if (Blynk.connected()) Blynk.virtualWrite(V_MANUAL_PUMP, pumpState ? 1 : 0);
  }
}

BLYNK_WRITE(V_PUMP_TIME) {
  manualPumpDuration = param.asInt();
  if (manualPumpDuration < 1) manualPumpDuration = 1;
}

BLYNK_WRITE(V_AUTOMODE) {
  modeAuto = param.asInt();
}

/* Flow calculation (only if you use flow sensor) */
void computeFlowRateIfNeeded() {
  unsigned long now = millis();
  if (now - lastFlowCalcMillis >= 5000) {
    const float pulsesPerLiter = 450.0;
    noInterrupts();
    unsigned long pulses = flowCount;
    flowCount = 0;
    interrupts();
    float liters = pulses / pulsesPerLiter;
    float minutes = (float)(now - lastFlowCalcMillis) / 60000.0;
    flowRateLperMin = (minutes > 0) ? (liters / minutes) : 0;
    lastFlowCalcMillis = now;

    if (Blynk.connected()) Blynk.virtualWrite(V_FLOW, flowRateLperMin);
  }
}

/* Telemetry + auto watering + Blynk writes */
void sendTelemetry() {
  float soilPercent = readSoilPercent_smoothed();
  float rawAvg = readSoilRawAverage(); // raw average for debug/quick-check
  float hum  = safeReadHum();
  float temp = safeReadTemp();
  int rainRaw = digitalRead(PIN_RAIN); // 0 or 1 depending on wiring

  if (isnan(temp)) temp = -999;
  if (isnan(hum))  hum  = -999;

  // Serial single-line summary (easy to read)
  Serial.print("Temp: ");
  Serial.print(temp, 2);
  Serial.print(" °C, Hum: ");
  Serial.print(hum, 2);
  Serial.print(" %, Soil Raw: ");
  Serial.print((int)rawAvg);
  Serial.print(", Soil %: ");
  Serial.print(soilPercent, 1);
  Serial.print(", Rain: ");
  Serial.print(rainRaw);
  Serial.print(", ForecastRain: ");
  Serial.println(forecastWillRain ? 1 : 0);

  // Blynk updates — only send when change exceeds thresholds to reduce jitter
  if (Blynk.connected()) {
    // soil percent
    if (fabs(soilPercent - lastSentSoil) >= SOIL_WRITE_THRESHOLD || lastSentSoil < -100.0f) {
      Blynk.virtualWrite(V_SOIL, soilPercent);
      lastSentSoil = soilPercent;
    }
    // also send raw soil for debugging / quick-check
    Blynk.virtualWrite(V_CHART, soilPercent); // keep your chart
    Blynk.virtualWrite(V_RAIN_SENSOR, rainRaw);

    // temperature
    if ((temp > -500 && fabs(temp - lastSentTemp) >= TEMP_THRESHOLD) || lastSentTemp < -100.0f) {
      Blynk.virtualWrite(V_TEMP, temp);
      lastSentTemp = temp;
    }
    // humidity
    if ((hum > -500 && fabs(hum - lastSentHum) >= HUM_THRESHOLD) || lastSentHum < -100.0f) {
      Blynk.virtualWrite(V_HUM, hum);
      lastSentHum = hum;
    }
  }

  // update forecast on interval
  unsigned long now = millis();
  if (now - lastForecastCheck > FORECAST_INTERVAL) {
    Serial.println("Checking OpenWeather forecast...");
    bool wf = checkForecastWillRainNow();
    lastForecastCheck = now;

    if (Blynk.connected()) {
      Blynk.virtualWrite(V_WEATHER, wf ? 1 : 0);           // numeric forecast
      Blynk.virtualWrite(V_WEATHER_TEXT, forecastWeatherMain); // textual main
      Blynk.virtualWrite(V_WEATHER_DESC, forecastWeatherDesc); // textual description
      Serial.print("Wrote V5 forecast value: ");
      Serial.println(wf ? 1 : 0);
    }
  }

  // Auto watering decision
  if (modeAuto) {
    bool canWater = (now - lastWaterMillis) > MIN_WATER_INTERVAL;
    // interpret rain sensor depending on wiring; change if your sensor is inverted
    bool rainSensorWet = (rainRaw == 1);

    if (soilPercent < THRESHOLD_LOW &&
        !rainSensorWet &&
        !forecastWillRain &&
        canWater) {

      Serial.println("Auto watering: starting pump.");
      setPump(true);
      timer.setTimeout((unsigned long)manualPumpDuration * 1000UL, []() { setPump(false); });
      lastWaterMillis = now;
    }
  }
}

/* WiFi + Blynk setup */
void setupWiFiAndBlynk() {
  Serial.println("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi NOT connected.");
  }

  Blynk.config(auth);
  Serial.println("Connecting to Blynk...");

  unsigned long t0 = millis();
  while (!Blynk.connected() && millis() - t0 < 20000) {
    Blynk.connect(5000);
    Serial.print("+");
  }
  Serial.println();

  if (Blynk.connected()) Serial.println("Blynk connected.");
  else Serial.println("Blynk NOT connected.");
}

/* Setup */
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Irrigation System (merged final) ===");

  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, HIGH); // default off (active LOW assumed)
  pinMode(PIN_RAIN, INPUT_PULLUP);
  pinMode(PIN_FLOW, INPUT_PULLUP);

  dht.begin();
  // attach flow interrupt only if you have a flow sensor connected to PIN_FLOW
  // If you don't have one, this is harmless but you can comment the line below
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), flowPulseISR, RISING);

  setupWiFiAndBlynk();

  // schedule tasks
  timer.setInterval(30000L, sendTelemetry);          // send telemetry every 30s
  timer.setInterval(5000L, computeFlowRateIfNeeded); // update flow every 5s (if used)

  // Optional: do an immediate telemetry on startup
  sendTelemetry();
}

/* Loop */
void loop() {
  Blynk.run();
  timer.run();

  static unsigned long lastTry = 0;
  if (!Blynk.connected() && millis() - lastTry > 10000) {
    Blynk.connect(2000);
    lastTry = millis();
  }
}



