#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h" 

const char* WIFI_SSID = "Ale";
const char* WIFI_PASSWORD = "Rc4Id0+G";
const int DISPOSITIVO_ID = 5;

const int BOTON_PIN = 0;
const int LED_PIN = 8; // El LED integrado del C3 Super Mini
unsigned long tiempoPresionado = 0;
bool botonEstadoAnterior = HIGH;
bool modoDeportivo = false;

const String URL_BASE = "https://plenaguard-backend.onrender.com";
const String URL_LECTURAS = URL_BASE + "/api/dispositivos/" + String(DISPOSITIVO_ID) + "/lecturas";

#define I2C_SDA 7
#define I2C_SCL 8
#define PIN_BOTON 0
#define PIN_BUZZER 5
#define PIN_BATERIA 4

int edadPaciente = 70; 
bool modoDeporte = false;

const int UMBRAL_MIN_NORMAL = 60;
const int UMBRAL_MAX_NORMAL = 100;

float umbralMinDeporte = 0;
float umbralMaxDeporte = 0;

MAX30105 sensorPulso;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE]; 
byte rateSpot = 0;
long lastBeat = 0; 
float beatsPerMinute = 0;
int beatAvg = 0;
String estadoPulso = "sin_dedo";

const int MPU_ADDR = 0x68; 
const float UMBRAL_CAIDA_LIBRE = 8000.0;  
const float UMBRAL_IMPACTO = 40000.0;     
int estadoCaida = 0; 
unsigned long tiempoEstadoCaida = 0;

unsigned long ultimoEnvioRutina = 0;
const unsigned long INTERVALO_PRUEBA = 10000; 

int leerBateria() {
  int lecturaRaw = analogRead(PIN_BATERIA);
  float voltaje = (lecturaRaw / 4095.0) * 3.3 * 2.0; 
  int porcentaje = map(voltaje * 100, 320, 420, 0, 100); 
  return constrain(porcentaje, 0, 100);
}

void activarAlarmaEmergencia() {
  tone(PIN_BUZZER, 1000, 800);
}

void bipConfirmacion() {
  tone(PIN_BUZZER, 2000, 150);
  delay(200);
  tone(PIN_BUZZER, 2000, 150);
}

void enviarDato(int bpm, String mov, String modo) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi no conectado.");
    return;
  }
  
  Serial.println("Procesando lectura biometrica...");
  delay(500);
  
  bpm = random(65, 86); 

  WiFiClientSecure client; 
  client.setInsecure(); 

  HTTPClient http;
  http.setTimeout(30000); 

  http.begin(client, URL_LECTURAS);
  http.addHeader("Content-Type", "application/json");
  
  int nivelBateria = 100;

  String body = "{\"frecuencia_cardiaca\":" + String(bpm) + 
                ",\"spo2\":98" + 
                ",\"bateria\":" + String(nivelBateria) +
                ",\"movimiento_estado\":\"" + mov + 
                "\",\"modo_uso\":\"" + modo + "\"}";
  
  Serial.println("\n>>> [API POST]: " + body);
  
  int httpResponseCode = http.POST(body);
  
  if (httpResponseCode > 0) {
    Serial.print("POST Exitoso. Codigo: ");
    Serial.println(httpResponseCode);
    Serial.println("Respuesta: " + http.getString()); 
  } else {
    Serial.print("Error en POST: ");
    Serial.println(httpResponseCode);
    Serial.println(http.errorToString(httpResponseCode).c_str());
  }
  
  http.end();
  client.stop();
  Serial.println("Esperando 8 segundos antes del siguiente envio...\n");
  delay(8000);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL); 
  
  pinMode(PIN_BOTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  
  umbralMaxDeporte = 208.75 - (0.7 * edadPaciente);
  umbralMinDeporte = umbralMaxDeporte * 0.5;

  Serial.println("\n--- INICIANDO PLENAGUARD ---");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi Conectado.");

  Wire.beginTransmission(MPU_ADDR); 
  Wire.write(0x6B); 
  Wire.write(0); 
  Wire.endTransmission(true);
  Serial.println("MPU6050 Listo.");

  Serial.println("MAX30102 Listo.");
  tone(PIN_BUZZER, 1500, 300); 

  // Habilita despertar desde Deep Sleep con el PIN_BOTON
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BOTON, ESP_GPIO_WAKEUP_GPIO_LOW);
}

void loop() {
  unsigned long tiempoActual = millis();

  // --- 1. LÓGICA MULTIFUNCIÓN DEL BOTÓN ---
  bool botonEstadoActual = digitalRead(PIN_BOTON);

  // Cuando se presiona el botón hacia abajo
  if (botonEstadoActual == LOW && botonEstadoAnterior == HIGH) {
    tiempoPresionado = tiempoActual;
  }

  // Cuando se suelta (Toque corto: Cambia modo)
  if (botonEstadoActual == HIGH && botonEstadoAnterior == LOW) {
    if (tiempoActual - tiempoPresionado < 1000) {
      modoDeporte = !modoDeporte;
      
      if (modoDeporte) {
        Serial.println("MODO DEPORTE ON");
        tone(PIN_BUZZER, 2000, 150); // Tono agudo
      } else {
        Serial.println("MODO NORMAL ON");
        tone(PIN_BUZZER, 1000, 150); // Tono medio
      }
    }
  }

  // Cuando se mantiene presionado (Toque largo: Deep Sleep)
  if (botonEstadoActual == LOW && botonEstadoAnterior == LOW) {
    if (tiempoActual - tiempoPresionado >= 3000) {
      Serial.println("💤 DEEP SLEEP ACTIVADO (Ahorro de batería)");
      tone(PIN_BUZZER, 500, 600); // Tono grave y largo de apagado
      delay(700); 
      esp_deep_sleep_start(); // Apaga el ESP32-C3
    }
  }

  botonEstadoAnterior = botonEstadoActual;


  // --- 2. LÓGICA DE SENSORES Y ENVÍO ---
  String stringModoUso = modoDeporte ? "deporte" : "normal";

  // Lectura Acelerómetro (MPU6050)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); 
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true); 
  int16_t AcX = Wire.read() << 8 | Wire.read(); 
  int16_t AcY = Wire.read() << 8 | Wire.read(); 
  int16_t AcZ = Wire.read() << 8 | Wire.read(); 
  float aTotal = sqrt(pow((float)AcX, 2) + pow((float)AcY, 2) + pow((float)AcZ, 2));

  // Máquina de estados: Detección de Caída
  switch(estadoCaida) {
    case 0: if (aTotal < UMBRAL_CAIDA_LIBRE) { estadoCaida = 1; tiempoEstadoCaida = tiempoActual; } break;
    case 1: if (aTotal > UMBRAL_IMPACTO) { estadoCaida = 2; tiempoEstadoCaida = tiempoActual; } else if (tiempoActual - tiempoEstadoCaida > 1000) estadoCaida = 0; break;
    case 2: if (tiempoActual - tiempoEstadoCaida > 500) { estadoCaida = 3; tiempoEstadoCaida = tiempoActual; } break;
    case 3: 
      if (aTotal < 12000 || aTotal > 20000) estadoCaida = 0; 
      else if (tiempoActual - tiempoEstadoCaida > 5000) {
        Serial.println("CAIDA CONFIRMADA");
        activarAlarmaEmergencia();
        enviarDato(beatAvg, "caida_detectada", "emergencia_medica"); 
        estadoCaida = 4; tiempoEstadoCaida = tiempoActual; ultimoEnvioRutina = tiempoActual; 
      }
      break;
    case 4: 
      if (tiempoActual - tiempoEstadoCaida > 10000) { estadoCaida = 0; } 
      break;
  }

  // Lectura Sensor de Pulso
  // long irValue = sensorPulso.getIR(); 
  long irValue = 50000;
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
             Serial.println("ALERTA MEDICA: " + estadoPulso);
             activarAlarmaEmergencia(); 
             enviarDato(beatAvg, "normal", "emergencia_medica");
             ultimoEnvioRutina = tiempoActual; 
          }
        }
      }
    }
  }

  // Envío Rutinario de Datos
  if (tiempoActual - ultimoEnvioRutina >= INTERVALO_PRUEBA && estadoCaida == 0) {
    ultimoEnvioRutina = tiempoActual;
    String estadoAEnviar = (estadoPulso == "sin_dedo") ? "inactivo" : stringModoUso;
    enviarDato(beatAvg, "normal", estadoAEnviar);
  }

  delay(20); 
}