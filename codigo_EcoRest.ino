#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <BH1750.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <DFRobotDFPlayerMini.h>

#ifndef ESP32
#error "Este codigo es solo para ESP32. Selecciona ESP32 Dev Module."
#endif

// =====================================================
// SISTEMA INTELIGENTE - LÓGICA DE AUDIO CORREGIDA
// =====================================================

// -------------------- PINES --------------------
#define PIN_DHT11 32 
#define DHT_TIPO DHT11

#define PIN_SDA 21
#define PIN_SCL 22

#define PIN_LD_RX 16
#define PIN_LD_TX 17
#define LD_BAUD 256000

#define PIN_MIC 34

// ¡OJO! Pines puestos igual que en tu script de prueba exitoso
#define PIN_DF_TX 26
#define PIN_DF_RX 27

#define PIN_MAX_DIN 23
#define PIN_MAX_CLK 14
#define PIN_MAX_CS  18

#define PIN_RELE_VENTILADOR 33
#define PIN_RELE_NEBULIZADOR 25

#define RELE_ACTIVO_LOW false

// -------------------- CONFIGURACION --------------------
#define LUX_OSCURIDAD 200.0
#define AJUSTAR_RTC_COMPILACION false

#define LD_DISTANCIA_MIN_CM 0
#define LD_DISTANCIA_MAX_CM 120
#define LD_ENERGIA_MINIMA 15
#define LD_CONFIRMAR_SI 2
#define LD_CONFIRMAR_NO 4

#define UMBRAL_RUIDO_ADC 900

#define TRACK_PRESENCIA 1
#define TRACK_RUIDO_ALTO 2
#define COOLDOWN_AUDIO_RUIDO 10000UL // 10 segundos para ruido alto

// Tiempos optimizados
#define INTERVALO_CLIMA 5000UL  
#define INTERVALO_LCD   3000UL  
#define INTERVALO_DEBUG 1000UL  

#define BRILLO_MATRIZ_ALTO 2    

// -------------------- OBJETOS --------------------
DHT dht(PIN_DHT11, DHT_TIPO);
BH1750 bh1750;
RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2);

HardwareSerial SerialLD(2);
HardwareSerial SerialDF(1);
DFRobotDFPlayerMini dfPlayer;

// -------------------- VARIABLES --------------------
bool dht_ok = false, bh1750_ok = false, rtc_ok = false, lcd_ok = false, dfplayer_ok = false;
float temperatura = 0.0, humedad = 0.0, lux = 0.0;
int ruido_pp = 0;
bool ruido_alto = false;

bool presencia = false, presencia_anterior = false;
uint16_t distancia_cm = 0, dist_mov_cm = 0, dist_est_cm = 0;
uint8_t energia_mov = 0, energia_est = 0, estado_ld = 0;

bool ventilador_activo = false, nebulizador_activo = false, matriz_activa = false;
bool audio_reproduciendo = false; 
int volumenActual = 20; 

unsigned long t_clima = 0, t_lcd = 0, t_debug = 0;
unsigned long t_audio_ruido = 0;
int pantalla = 0;

// =====================================================
// FUNCIONES DE CONTROL BÁSICO (MATRIZ Y RELES)
// =====================================================

void max7219Send(byte reg, byte data) {
  digitalWrite(PIN_MAX_CS, LOW);
  shiftOut(PIN_MAX_DIN, PIN_MAX_CLK, MSBFIRST, reg);
  shiftOut(PIN_MAX_DIN, PIN_MAX_CLK, MSBFIRST, data);
  digitalWrite(PIN_MAX_CS, HIGH);
}
void matrizSetBrillo(byte brillo) {
  if (brillo > 15) brillo = 15;
  max7219Send(0x0A, brillo);
}
void matrizInit() {
  pinMode(PIN_MAX_DIN, OUTPUT); pinMode(PIN_MAX_CLK, OUTPUT); pinMode(PIN_MAX_CS, OUTPUT);
  digitalWrite(PIN_MAX_CS, HIGH);
  max7219Send(0x09, 0x00); max7219Send(0x0A, 2); max7219Send(0x0B, 0x07);
  max7219Send(0x0C, 0x01); max7219Send(0x0F, 0x00);
  matrizApagada();
}
void matrizApagada() {
  if (!matriz_activa) return; 
  for (int i = 1; i <= 8; i++) max7219Send(i, B00000000);
  matriz_activa = false;
}
void matrizBlanca(byte brillo) {
  if (matriz_activa) return; 
  matrizSetBrillo(brillo);
  for (int i = 1; i <= 8; i++) max7219Send(i, B11111111);
  matriz_activa = true;
}

void releVentilador(bool encender) {
  if (ventilador_activo == encender) return; 
  ventilador_activo = encender;
  digitalWrite(PIN_RELE_VENTILADOR, (encender ^ RELE_ACTIVO_LOW) ? HIGH : LOW);
}
void releNebulizador(bool encender) {
  if (nebulizador_activo == encender) return; 
  nebulizador_activo = encender;
  digitalWrite(PIN_RELE_NEBULIZADOR, (encender ^ RELE_ACTIVO_LOW) ? HIGH : LOW);
}

// =====================================================
// AUDIO CORREGIDO
// =====================================================

void reproducirTrack(uint16_t track) {
  if (!dfplayer_ok) return;
  dfPlayer.stop();
  delay(50); 
  dfPlayer.play(track); // Usando el play simple que sí funciona
  audio_reproduciendo = true;
}

void detenerAudio() {
  if (!dfplayer_ok || !audio_reproduciendo) return;
  dfPlayer.stop();
  audio_reproduciendo = false;
}

void actualizarAudio() {
  unsigned long ahora = millis();
  
  // LOGICA CORREGIDA: Sin cooldown. Si pasas de "despejado" a "presencia", suena de una.
  if (presencia && !presencia_anterior) {
    reproducirTrack(TRACK_PRESENCIA);
  }
  
  // LOGICA RUIDO: Este sí usa Cooldown para no cortarse a cada rato
  if (presencia && ruido_alto) {
    if (t_audio_ruido == 0 || ahora - t_audio_ruido >= COOLDOWN_AUDIO_RUIDO) {
      reproducirTrack(TRACK_RUIDO_ALTO);
      t_audio_ruido = ahora;
    }
  }
  
  presencia_anterior = presencia;
}

// =====================================================
// SENSORES
// =====================================================

void leerClimaLento() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(t) && !isnan(h)) {
    temperatura = t; humedad = h; dht_ok = true;
  } else { dht_ok = false; }

  if (bh1750_ok) {
    float valor = bh1750.readLightLevel();
    if (valor >= 0) lux = valor;
  }
}

void leerMicrofonoAsincrono() {
  static int minL = 4095, maxL = 0;
  static unsigned long t_inicio_mic = 0;

  int lectura = analogRead(PIN_MIC);
  if (lectura < minL) minL = lectura;
  if (lectura > maxL) maxL = lectura;

  if (millis() - t_inicio_mic >= 50) {
    ruido_pp = maxL - minL;
    ruido_alto = ruido_pp >= UMBRAL_RUIDO_ADC;
    minL = 4095; maxL = 0;
    t_inicio_mic = millis();
  }
}

void procesarRadarLimitado() {
  static uint8_t buffer[80];
  static int len = 0;
  static unsigned long ultimaTramaValida = 0;
  static int contadorSi = 0, contadorNo = 0;
  int bytesLeidos = 0; 

  while (SerialLD.available() && bytesLeidos < 32) {
    uint8_t b = SerialLD.read();
    bytesLeidos++;
    
    if (len < 80) buffer[len++] = b;
    else { memmove(buffer, buffer + 1, 79); buffer[79] = b; len = 80; }

    for (int i = 0; i <= len - 8; i++) {
      if (buffer[i] == 0xF4 && buffer[i + 1] == 0xF3 && buffer[i + 2] == 0xF2 && buffer[i + 3] == 0xF1) {
        for (int j = i + 4; j <= len - 4; j++) {
          if (buffer[j] == 0xF8 && buffer[j + 1] == 0xF7 && buffer[j + 2] == 0xF6 && buffer[j + 3] == 0xF5) {
            if (j + 4 - i >= 17) {
              estado_ld = buffer[i + 8];
              dist_mov_cm = buffer[i + 9] | (buffer[i + 10] << 8);
              energia_mov = buffer[i + 11];
              dist_est_cm = buffer[i + 12] | (buffer[i + 13] << 8);
              energia_est = buffer[i + 14];
              distancia_cm = buffer[i + 15] | (buffer[i + 16] << 8);

              bool estadoDetecta = (estado_ld == 0x01 || estado_ld == 0x02 || estado_ld == 0x03);
              if (estadoDetecta && (distancia_cm >= LD_DISTANCIA_MIN_CM && distancia_cm <= LD_DISTANCIA_MAX_CM) && 
                 (energia_mov >= LD_ENERGIA_MINIMA || energia_est >= LD_ENERGIA_MINIMA)) {
                contadorSi++; contadorNo = 0;
              } else {
                contadorNo++; contadorSi = 0;
              }

              if (contadorSi >= LD_CONFIRMAR_SI) presencia = true;
              if (contadorNo >= LD_CONFIRMAR_NO || estado_ld == 0x00) { presencia = false; distancia_cm = 0; }
              ultimaTramaValida = millis();
            }
            int restantes = len - (j + 4);
            memmove(buffer, buffer + j + 4, restantes);
            len = restantes;
            return;
          }
        }
      }
    }
  }
  if (millis() - ultimaTramaValida > 5000) { presencia = false; distancia_cm = 0; }
}

// =====================================================
// LCD Y PANEL SERIAL
// =====================================================

String horaLCD() {
  if (!rtc_ok) return "--:--:--";
  DateTime now = rtc.now(); char buffer[10];
  sprintf(buffer, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  return String(buffer);
}
String fechaLCD() {
  if (!rtc_ok) return "--/--/----";
  DateTime now = rtc.now(); char buffer[12];
  sprintf(buffer, "%02d/%02d/%04d", now.day(), now.month(), now.year());
  return String(buffer);
}

void actualizarLCD() {
  if (!lcd_ok) return;
  lcd.clear();
  if (!presencia) { lcd.noBacklight(); return; }
  lcd.backlight();
  switch (pantalla) {
    case 0: lcd.setCursor(0, 0); lcd.print("Hora: "); lcd.print(horaLCD()); lcd.setCursor(0, 1); lcd.print(fechaLCD()); break;
    case 1: lcd.setCursor(0, 0); lcd.print("Temp: "); lcd.print(temperatura, 1); lcd.print((char)223); lcd.print("C");
            lcd.setCursor(0, 1); lcd.print("Hum:  "); lcd.print(humedad, 0); lcd.print("%"); break;
    case 2: lcd.setCursor(0, 0); lcd.print("Luz: "); lcd.print((int)lux); lcd.print(" lx");
            lcd.setCursor(0, 1); lcd.print(lux < LUX_OSCURIDAD ? "Matriz ON" : "Matriz OFF"); break;
    case 3: lcd.setCursor(0, 0); lcd.print("Presencia: SI");
            lcd.setCursor(0, 1); lcd.print("Dist: "); lcd.print(distancia_cm); lcd.print("cm"); break;
    case 4: lcd.setCursor(0, 0); lcd.print("Ruido: "); lcd.print(ruido_pp);
            lcd.setCursor(0, 1); lcd.print(ruido_alto ? "RUIDO ALTO" : "NORMAL"); break;
  }
  pantalla = (pantalla + 1) % 5;
}

void mostrarSerial() {
  Serial.println("\n[⚡ SISTEMA RESPONDIENDO A MAX VELOCIDAD ⚡]");
  Serial.printf(" 🚶 PRESENCIA : %s \t| 📏 DISTANCIA : %d cm\n", presencia ? "DETECTADA" : "DESPEJADO", distancia_cm);
  Serial.printf(" 🌡️ TEMPERAT. : %.1f C \t| 💧 HUMEDAD   : %.1f %%\n", temperatura, humedad);
  Serial.printf(" ☀️ LUZ (Lux) : %.1f \t| 🔊 RUIDO (pp): %d %s\n", lux, ruido_pp, ruido_alto ? "[ALTO]" : "");
  Serial.println("--------------------------------------------------");
  Serial.printf(" 🌬️ Vent: [%s] | 💦 Nebul: [%s] | 💡 Matriz: [%s]\n", 
                ventilador_activo?"ON":"OFF", nebulizador_activo?"ON":"OFF", matriz_activa?"ON":"OFF");
  Serial.printf(" 🎵 Audio:[%s] | Vol: %d | CMDS: '+', '-', 'P'\n", 
                audio_reproduciendo?"SONANDO":"SILENCIO", volumenActual);
  Serial.println("==================================================");
}

void revisarComandosTerminal() {
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == '\n' || cmd == '\r') return; 
    if (cmd == '+' && volumenActual < 30) { volumenActual++; dfPlayer.volume(volumenActual); } 
    else if (cmd == '-' && volumenActual > 0) { volumenActual--; dfPlayer.volume(volumenActual); } 
    else if (cmd == 'p' || cmd == 'P') { dfPlayer.play(1); audio_reproduciendo = true; }
  }
}

// =====================================================
// SETUP Y LOOP PRINCIPAL
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n🚀 Iniciando Paradero...");

  pinMode(PIN_RELE_VENTILADOR, OUTPUT); pinMode(PIN_RELE_NEBULIZADOR, OUTPUT);
  releVentilador(false); releNebulizador(false);
  
  Wire.begin(PIN_SDA, PIN_SCL);
  dht.begin();
  
  bh1750_ok = bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  rtc_ok = rtc.begin();
  if (rtc_ok && AJUSTAR_RTC_COMPILACION) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  lcd.init(); lcd.backlight(); lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Sistema Ok"); 
  lcd_ok = true;

  matrizInit(); matrizBlanca(BRILLO_MATRIZ_ALTO); delay(500); matrizApagada();

  SerialLD.begin(LD_BAUD, SERIAL_8N1, PIN_LD_RX, PIN_LD_TX);
  SerialDF.begin(9600, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);

  if (dfPlayer.begin(SerialDF, false, true)) {
    dfplayer_ok = true; dfPlayer.volume(volumenActual); dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
  } else { Serial.println("[ERROR] DFPlayer"); }

  leerClimaLento(); 
  lcd.clear(); lcd.print("Sistema Listo!");
}

void loop() {
  unsigned long ahora = millis();

  procesarRadarLimitado();
  leerMicrofonoAsincrono();
  revisarComandosTerminal();

  if (!presencia) {
    matrizApagada(); releVentilador(false); releNebulizador(false); detenerAudio();
    presencia_anterior = false; 
  } else {
    releVentilador(true); releNebulizador(true);
    if (lux < LUX_OSCURIDAD) matrizBlanca(BRILLO_MATRIZ_ALTO); else matrizApagada();
    actualizarAudio(); // <-- Aquí sucede la magia ahora sin lag
  }

  if (ahora - t_clima >= INTERVALO_CLIMA) {
    leerClimaLento();
    t_clima = ahora;
  }

  if (ahora - t_lcd >= INTERVALO_LCD) {
    actualizarLCD();
    t_lcd = ahora;
  }

  if (ahora - t_debug >= INTERVALO_DEBUG) {
    mostrarSerial();
    t_debug = ahora;
  }
}