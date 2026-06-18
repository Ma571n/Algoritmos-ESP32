#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <FastLED.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// ==========================================
// 1. CONFIGURACIÓN DE RED Y API
// ==========================================
const char* WIFI_SSID = "VTR-7297333";
const char* WIFI_PASSWORD = "Mt3knjhgpWqr";
const int DISPOSITIVO_ID = 5;

const String URL_BASE = "https://plenaguard-backend.onrender.com";
const String URL_LECTURAS = URL_BASE + "/api/dispositivos/" + String(DISPOSITIVO_ID) + "/lecturas";

// ==========================================
// 2. DEFINICIÓN DE PINES Y OBJETOS
// ==========================================
#define I2C_SDA 8
#define I2C_SCL 9
#define PIN_BOTON 6
#define PIN_LED 7

#define NUM_LEDS 1
CRGB leds[NUM_LEDS];
MAX30105 sensorPulso;
Adafruit_MPU6050 mpu;

// ==========================================
// 3. VARIABLES PARA LA LÓGICA MÉDICA
// ==========================================
// --- Caídas (MPU6050) ---
const float UMBRAL_CAIDA_LIBRE = 8000.0;  
const float UMBRAL_IMPACTO = 40000.0;     
int estadoCaida = 0; 
unsigned long tiempoEstadoCaida = 0;

// --- Pulso (MAX30102) ---
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE]; 
byte rateSpot = 0;
long lastBeat = 0; 
float beatsPerMinute = 0;
int beatAvg = 0;

// --- Control General ---
unsigned long ultimoEnvio = 0;
bool modoEmergencia = false;

// ==========================================
// 4. FUNCIÓN PARA ENVIAR DATOS A LA API
// ==========================================
void enviarDato(int bpm, String mov, String modo) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(URL_LECTURAS);
    http.addHeader("Content-Type", "application/json");

    // Armamos el JSON con los datos
    String body = "{\"frecuencia_cardiaca\":" + String(bpm) + 
                  ", \"nivel_oxigeno\": 98" + 
                  ", \"movimiento_estado\":\"" + mov + "\"" + 
                  ", \"modo_uso\":\"" + modo + "\"}";

    int httpResponseCode = http.POST(body);
    
    if (httpResponseCode > 0) {
      Serial.println("✅ Datos enviados a la API. Código: " + String(httpResponseCode));
    } else {
      Serial.println("❌ Error enviando a la API: " + String(httpResponseCode));
    }
    http.end();
  } else {
    Serial.println("❌ Error: WiFi Desconectado");
  }
}

// ==========================================
// 5. INICIO DEL DISPOSITIVO (SETUP)
// ==========================================
void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL); 

  pinMode(PIN_BOTON, INPUT_PULLUP);
  FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(40); 
  leds[0] = CRGB::Blue; // Azul = Iniciando
  FastLED.show();

  Serial.println("\n--- INICIANDO PLENAGUARD ---");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n✅ WiFi Conectado.");

  if (!sensorPulso.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("❌ ERROR: MAX30102 no encontrado.");
  } else {
    sensorPulso.setup(); 
    sensorPulso.setPulseAmplitudeRed(0x0A); 
    sensorPulso.setPulseAmplitudeIR(0x1F);  
  }

  if (!mpu.begin()) {
    Serial.println("❌ ERROR: MPU6050 no encontrado.");
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  }

  leds[0] = CRGB::Green; // Verde = Listo
  FastLED.show();
}

// ==========================================
// 6. LÓGICA PRINCIPAL (LOOP)
// ==========================================
void loop() {
  unsigned long tiempoActual = millis();

  // --- A. LECTURA DEL BOTÓN DE PÁNICO ---
  if (digitalRead(PIN_BOTON) == LOW) {
    modoEmergencia = true;
    leds[0] = CRGB::Red; // LED Rojo para emergencia
    FastLED.show();
    Serial.println("🚨 BOTÓN DE PÁNICO PRESIONADO 🚨");
    enviarDato(beatAvg, "boton_panico", "emergencia_medica");
    delay(2000); // Pausa para no saturar la API
  }

  // --- B. LECTURA DE LATIDOS ---
  long irValue = sensorPulso.getIR();
  if (checkForBeat(irValue) == true) {
    long delta = tiempoActual - lastBeat;
    lastBeat = tiempoActual;
    beatsPerMinute = 60 / (delta / 1000.0);

    if (beatsPerMinute < 255 && beatsPerMinute > 20) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;
      beatAvg = 0;
      for (byte x = 0 ; x < RATE_SIZE ; x++) beatAvg += rates[x];
      beatAvg /= RATE_SIZE;

      // Evaluar bradicardia o taquicardia
      if (beatAvg > 0 && beatAvg < 40) {
        modoEmergencia = true;
        enviarDato(beatAvg, "normal", "emergencia_medica_bradicardia");
      } else if (beatAvg > 150) {
        modoEmergencia = true;
        enviarDato(beatAvg, "normal", "emergencia_medica_taquicardia");
      }
    }
  }

  // --- C. ENVÍO DE RUTINA ---
  // Si no hay emergencia, enviamos datos normales cada 30 segundos
  if (tiempoActual - ultimoEnvio >= 30000 && !modoEmergencia) {
    ultimoEnvio = tiempoActual;
    Serial.println("Enviando lectura de rutina...");
    enviarDato(beatAvg, "normal", "normal");
  }
}