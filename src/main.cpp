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
// 1. CONFIGURACIÓN DE RED Y API (Datos de Javi)
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
// 3. INICIO DEL DISPOSITIVO (SETUP)
// ==========================================
void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL); 

  // --- Iniciar Hardware y LED ---
  pinMode(PIN_BOTON, INPUT_PULLUP);
  FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(40); 
  leds[0] = CRGB::Blue;  // LED Azul: Cargando...
  FastLED.show();

  Serial.println("\n--- INICIANDO PLENAGUARD ---");

  // --- Conectar WiFi ---
  Serial.print("Conectando a WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Conectado.");

  // --- Iniciar Sensor de Pulso ---
  if (!sensorPulso.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("❌ ERROR: MAX30102 no encontrado.");
  } else {
    sensorPulso.setup(); 
    sensorPulso.setPulseAmplitudeRed(0x0A); 
    sensorPulso.setPulseAmplitudeIR(0x1F);  
    Serial.println("✅ MAX30102 Listo.");
  }

  // --- Iniciar Acelerómetro ---
  if (!mpu.begin()) {
    Serial.println("❌ ERROR: MPU6050 no encontrado.");
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    Serial.println("✅ MPU6050 Listo.");
  }

  // --- Fin del Inicio ---
  leds[0] = CRGB::Green; // LED Verde: Todo OK
  FastLED.show();
}

// ==========================================
// 4. LÓGICA PRINCIPAL (LOOP)
// ==========================================
void loop() {
  // Aquí pondremos la lógica de caídas, pulso y botón
}

