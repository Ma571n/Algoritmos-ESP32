#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h" 

// ==========================================
// 1. CONFIGURACIÓN DE RED Y API
// ==========================================
const char* WIFI_SSID = "HASON";
const char* WIFI_PASSWORD = "encUCacdJcea";
const int DISPOSITIVO_ID = 5;

const String URL_BASE = "https://plenaguard-backend.onrender.com";
const String URL_LECTURAS = URL_BASE + "/api/dispositivos/" + String(DISPOSITIVO_ID) + "/lecturas";

// ==========================================
// 2. DEFINICIÓN DE PINES
// ==========================================
#define I2C_SDA 8
#define I2C_SCL 9
#define PIN_BOTON 6     // Alternar Modo Deporte
#define PIN_BUZZER 5    // Zumbador de alertas
#define PIN_BATERIA 4   // Pin analógico para medir batería LiPo

// ==========================================
// 3. VARIABLES MÉDICAS 
// ==========================================
int edadPaciente = 70; 
bool modoDeporte = false;

// Umbrales Normales
const int UMBRAL_MIN_NORMAL = 60;
const int UMBRAL_MAX_NORMAL = 100;

// Umbrales calculados por fórmula
float umbralMinDeporte = 0;
float umbralMaxDeporte = 0;

// Variables MAX30102
MAX30105 sensorPulso;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE]; 
byte rateSpot = 0;
long lastBeat = 0; 
float beatsPerMinute = 0;
int beatAvg = 0;
String estadoPulso = "sin_dedo";

// Variables MPU6050
const int MPU_ADDR = 0x68; 
const float UMBRAL_CAIDA_LIBRE = 8000.0;  
const float UMBRAL_IMPACTO = 40000.0;     
int estadoCaida = 0; 
unsigned long tiempoEstadoCaida = 0;

// Timers generales
unsigned long ultimoEnvioRutina = 0;
const unsigned long INTERVALO_PRUEBA = 10000; 

// ==========================================
// 4. FUNCIONES DE APOYO
// ==========================================

int leerBateria() {
  int lecturaRaw = analogRead(PIN_BATERIA);
  float voltaje = (lecturaRaw / 4095.0) * 3.3 * 2.0; 
  int porcentaje = map(voltaje * 100, 320, 420, 0, 100); 
  return constrain(porcentaje, 0, 100);
}

// Alarma larga y grave para emergencias médicas/caídas
void activarAlarmaEmergencia() {
  tone(PIN_BUZZER, 1000, 800); // 1000 Hz por 800ms
}

// Alarma corta y aguda para cambio de modo
void bipConfirmacion() {
  tone(PIN_BUZZER, 2000, 150);
  delay(200);
  tone(PIN_BUZZER, 2000, 150);
}

void enviarDato(int bpm, String mov, String modo) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  WiFiClientSecure client; 
  client.setInsecure(); 
  HTTPClient http;
  
  http.begin(client, URL_LECTURAS);
  http.addHeader("Content-Type", "application/json");
  
  int nivelBateria = leerBateria();

  String body = "{\"frecuencia_cardiaca\":" + String(bpm) + 
                ",\"spo2\":98" + 
                ",\"bateria\":" + String(nivelBateria) +
                ",\"movimiento_estado\":\"" + mov + 
                "\",\"modo_uso\":\"" + modo + "\"}";
  
  Serial.println("\n>>> [API POST]: " + body);
  http.POST(body);
  http.end();
}

// ==========================================
// 5. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL); 
  
  pinMode(PIN_BOTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  
  // Calcular Tanaka
  umbralMaxDeporte = 208.75 - (0.7 * edadPaciente);
  umbralMinDeporte = umbralMaxDeporte * 0.5;

  Serial.println("\n--- INICIANDO PLENAGUARD ---");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n✅ WiFi Conectado.");

  Wire.beginTransmission(MPU_ADDR); Wire.write(0x6B); Wire.write(0); Wire.endTransmission(true);
  Serial.println("✅ MPU6050 Listo.");

  if (sensorPulso.begin(Wire, I2C_SPEED_FAST)) {
    sensorPulso.setup(); 
    sensorPulso.setPulseAmplitudeRed(0x0A); 
    sensorPulso.setPulseAmplitudeIR(0x1F);  
    Serial.println("✅ MAX30102 Listo.");
  } else {
    Serial.println("❌ ERROR: MAX30102 no encontrado.");
  }
  
  // Un pitido simple indicando que el dispositivo encendió correctamente
  tone(PIN_BUZZER, 1500, 300); 
}

// ==========================================
// 6. LOOP
// ==========================================
void loop() {
  unsigned long tiempoActual = millis();

  // --- A. BOTÓN: MODO DEPORTE ---
  if (digitalRead(PIN_BOTON) == LOW) {
    modoDeporte = !modoDeporte;
    Serial.println(modoDeporte ? "🏃‍♂️ MODO DEPORTE ON" : "🛋️ MODO NORMAL ON");
    bipConfirmacion(); // Avisamos sonoramente al usuario
    delay(500); // Anti-rebote
  }

  String stringModoUso = modoDeporte ? "deporte" : "normal";

  // --- B. CAÍDAS (MPU6050) ---
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); 
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true); 
  int16_t AcX = Wire.read() << 8 | Wire.read(); 
  int16_t AcY = Wire.read() << 8 | Wire.read(); 
  int16_t AcZ = Wire.read() << 8 | Wire.read(); 
  float aTotal = sqrt(pow((float)AcX, 2) + pow((float)AcY, 2) + pow((float)AcZ, 2));

  switch(estadoCaida) {
    case 0: if (aTotal < UMBRAL_CAIDA_LIBRE) { estadoCaida = 1; tiempoEstadoCaida = tiempoActual; } break;
    case 1: if (aTotal > UMBRAL_IMPACTO) { estadoCaida = 2; tiempoEstadoCaida = tiempoActual; } else if (tiempoActual - tiempoEstadoCaida > 1000) estadoCaida = 0; break;
    case 2: if (tiempoActual - tiempoEstadoCaida > 500) { estadoCaida = 3; tiempoEstadoCaida = tiempoActual; } break;
    case 3: 
      if (aTotal < 12000 || aTotal > 20000) estadoCaida = 0; 
      else if (tiempoActual - tiempoEstadoCaida > 5000) {
        Serial.println("🚨 CAÍDA CONFIRMADA 🚨");
        activarAlarmaEmergencia(); // Alarma local
        enviarDato(beatAvg, "caida_detectada", "emergencia_medica"); 
        estadoCaida = 4; tiempoEstadoCaida = tiempoActual; ultimoEnvioRutina = tiempoActual; 
      }
      break;
    case 4: 
      if (tiempoActual - tiempoEstadoCaida > 10000) { estadoCaida = 0; } 
      break;
  }

  // --- C. PULSO Y LÍMITES DINÁMICOS ---
  long irValue = sensorPulso.getIR(); 

  if (irValue < 50000) {
    beatAvg = 0; estadoPulso = "sin_dedo";
  } else {
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

        if (beatAvg > 0 && estadoCaida == 0) {
          int limiteMax = modoDeporte ? umbralMaxDeporte : UMBRAL_MAX_NORMAL;
          int limiteMin = modoDeporte ? umbralMinDeporte : UMBRAL_MIN_NORMAL;

          if (beatAvg < limiteMin) estadoPulso = "bradicardia_critica";
          else if (beatAvg > limiteMax) estadoPulso = "taquicardia_critica";
          else estadoPulso = "normal";

          if (estadoPulso != "normal") {
             Serial.println("⚠️ ALERTA MÉDICA: " + estadoPulso);
             activarAlarmaEmergencia(); 
             enviarDato(beatAvg, "normal", "emergencia_medica");
             ultimoEnvioRutina = tiempoActual; 
          }
        }
      }
    }
  }

  // --- D. ENVÍO DE RUTINA (CADA 10s) ---
  if (tiempoActual - ultimoEnvioRutina >= INTERVALO_PRUEBA && estadoCaida == 0) {
    ultimoEnvioRutina = tiempoActual;
    String estadoAEnviar = (estadoPulso == "sin_dedo") ? "inactivo" : stringModoUso;
    enviarDato(beatAvg, "normal", estadoAEnviar);
  }

  delay(20); 
}