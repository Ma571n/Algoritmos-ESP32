#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <FastLED.h>
#include "MAX30105.h"
#include "heartRate.h" 

// ==========================================
// 1. CONFIGURACIÓN DE RED Y API (Datos de Javi)
// ==========================================
const char* WIFI_SSID = "VTR-7297333";
const char* WIFI_PASSWORD = "Mt3knjhgpWqr";
const int DISPOSITIVO_ID = 5;

const String URL_BASE = "https://plenaguard-backend.onrender.com";
const String URL_LECTURAS = URL_BASE + "/api/dispositivos/" + String(DISPOSITIVO_ID) + "/lecturas";

// ==========================================
// 2. DEFINICIÓN DE PINES Y CONFIGURACIONES
// ==========================================
#define I2C_SDA 8
#define I2C_SCL 9
#define PIN_BOTON 6
#define PIN_LED 7

#define NUM_LEDS 1
CRGB leds[NUM_LEDS];

// --- Configuración MPU6050 (Algoritmo de Caídas) ---
const int MPU_ADDR = 0x68; 
const float UMBRAL_CAIDA_LIBRE = 8000.0;  
const float UMBRAL_IMPACTO = 40000.0;     
int estadoCaida = 0; 
unsigned long tiempoEstadoCaida = 0;

// --- Configuración MAX30102 (Pulso) ---
MAX30105 sensorPulso;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE]; 
byte rateSpot = 0;
long lastBeat = 0; 
float beatsPerMinute = 0;
int beatAvg = 0;
String estadoPulso = "sin_dedo";

// --- Timers de Envío ---
const unsigned long INTERVALO_PRUEBA = 10000; 
unsigned long ultimoEnvioRutina = 0;

// ==========================================
// 3. FUNCIÓN PARA ENVIAR DATOS A LA API
// ==========================================
void enviarDato(int bpm, String mov, String modo) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Error: WiFi desconectado.");
    return;
  }
  
  WiFiClientSecure client; 
  client.setInsecure(); 
  HTTPClient http;
  
  http.begin(client, URL_LECTURAS);
  http.addHeader("Content-Type", "application/json");
  
  // CORREGIDO: "spo2" en vez de "nivel_oxigeno" para la APP de Javi
  String body = "{\"frecuencia_cardiaca\":" + String(bpm) + 
                ",\"spo2\":0,\"movimiento_estado\":\"" + mov + 
                "\",\"modo_uso\":\"" + modo + "\"}";
  
  Serial.println("\n>>> [API POST]: " + body);
  int code = http.POST(body);
  Serial.println("Status API: " + String(code));
  http.end();
}

// ==========================================
// 4. CONFIGURACIÓN INICIAL (SETUP)
// ==========================================
void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL); 
  
  // Iniciar Botón y LED
  pinMode(PIN_BOTON, INPUT_PULLUP);
  FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(40); 
  leds[0] = CRGB::Blue; // Azul = Cargando sistema
  FastLED.show();

  // Iniciar Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n✅ WiFi Conectado.");

  // Iniciar MPU6050 (Acelerómetro vía Wire directo)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); 
  Wire.write(0);    
  Wire.endTransmission(true);
  Serial.println("✅ MPU6050 Listo.");

  // Iniciar MAX30102 (Pulso)
  if (!sensorPulso.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("❌ ERROR: MAX30102 no encontrado.");
    leds[0] = CRGB::Red;
    FastLED.show();
    while(1); 
  }
  sensorPulso.setup(); 
  sensorPulso.setPulseAmplitudeRed(0x0A); 
  sensorPulso.setPulseAmplitudeIR(0x1F);  
  Serial.println("✅ MAX30102 Listo.");

  leds[0] = CRGB::Green; // Verde = Todo en orden
  FastLED.show();
}

// ==========================================
// 5. CICLO PRINCIPAL (LOOP UNIFICADO)
// ==========================================
void loop() {
  unsigned long tiempoActual = millis();

  // --- A. BOTÓN DE EMERGENCIA MANUAL ---
  if (digitalRead(PIN_BOTON) == LOW) {
    leds[0] = CRGB::Red;
    FastLED.show();
    Serial.println("🚨 BOTÓN DE PÁNICO ACTIVADO 🚨");
    enviarDato(beatAvg, "normal", "emergencia_medica");
    delay(400); // Anti-rebote
  }

  // --- B. ALGORITMO DE DETECCIÓN DE CAÍDAS (MPU6050) ---
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); 
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true); 
  int16_t AcX = Wire.read() << 8 | Wire.read(); 
  int16_t AcY = Wire.read() << 8 | Wire.read(); 
  int16_t AcZ = Wire.read() << 8 | Wire.read(); 
  float aTotal = sqrt(pow((float)AcX, 2) + pow((float)AcY, 2) + pow((float)AcZ, 2));

  switch(estadoCaida) {
    case 0: // Monitoreo Normal
      if (aTotal < UMBRAL_CAIDA_LIBRE) { estadoCaida = 1; tiempoEstadoCaida = tiempoActual; }
      break;
    case 1: // Posible Caída Libre detectada, esperando impacto
      if (aTotal > UMBRAL_IMPACTO) { estadoCaida = 2; tiempoEstadoCaida = tiempoActual; } 
      else if (tiempoActual - tiempoEstadoCaida > 1000) { estadoCaida = 0; }
      break;
    case 2: // Impacto detectado, esperando que el cuerpo se estabilice
      if (tiempoActual - tiempoEstadoCaida > 500) { estadoCaida = 3; tiempoEstadoCaida = tiempoActual; }
      break;
    case 3: // Verificando inmovilidad post-caída (5 segundos)
      if (aTotal < 12000 || aTotal > 20000) { estadoCaida = 0; } // Falsa alarma: se movió
      else if (tiempoActual - tiempoEstadoCaida > 5000) {
        Serial.println("🚨 CAÍDA CONFIRMADA 🚨");
        leds[0] = CRGB::Red; // LED Alerta Visual
        FastLED.show();
        enviarDato(beatAvg, "caida_detectada", "emergencia_medica"); 
        estadoCaida = 4; 
        tiempoEstadoCaida = tiempoActual;
        ultimoEnvioRutina = tiempoActual; 
      }
      break;
    case 4: // Bloqueo temporal post-caída (10s)
      if (tiempoActual - tiempoEstadoCaida > 10000) { 
        estadoCaida = 0; 
        leds[0] = CRGB::Green;
        FastLED.show();
      }
      break;
  }

  // --- C. LÓGICA DE PULSO CON BLOQUEO SEGÚN INFRA-ROJO (MAX30102) ---
  long irValue = sensorPulso.getIR(); 

  if (irValue < 50000) {
    // BLOQUEO: No hay dedo puesto
    beatsPerMinute = 0;
    beatAvg = 0; 
    estadoPulso = "sin_dedo";
    for(byte i=0; i<RATE_SIZE; i++) rates[i] = 0; 
  } 
  else {
    // Dedo detectado: Procesar ritmo cardíaco
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

        // Evaluar alertas médicas de ritmo cardíaco (Solo si no está en proceso una caída)
        if (beatAvg > 0 && estadoCaida == 0) {
          if (beatAvg < 40) estadoPulso = "bradicardia_critica";
          else if (beatAvg > 150) estadoPulso = "taquicardia_critica";
          else estadoPulso = "normal";

          if (estadoPulso == "bradicardia_critica" || estadoPulso == "taquicardia_critica") {
             Serial.println("⚠️ ALERTA MÉDICA POR PULSO: " + estadoPulso);
             leds[0] = CRGB::Red;
             FastLED.show();
             enviarDato(beatAvg, "normal", "emergencia_medica");
             ultimoEnvioRutina = tiempoActual; 
          } else {
             leds[0] = CRGB::Green;
             FastLED.show();
          }
        }
      }
    }
  }

  // --- D. ENVÍO DE RUTINA (CADA 10 SEGUNDOS) ---
  if (tiempoActual - ultimoEnvioRutina >= INTERVALO_PRUEBA && estadoCaida == 0) {
    ultimoEnvioRutina = tiempoActual;
    String modoAEnviar = (estadoPulso == "sin_dedo") ? "inactivo" : "rutina";
    enviarDato(beatAvg, "normal", modoAEnviar);
  }

  delay(20); 
}