/**
 * @file Rainyguard.ino
 * @brief Autonome Klimasteuerung und Unwetterschutz auf Basis eines ESP32.
 * 
 * Deterministische Finite State Machine (FSM) zur sensorgestuetzten Steuerung
 * von Lueftung, Fenstermechanik und akustischen/visuellen Warnsystemen.
 * Inklusive latenzfreier Hardware-Interrupt-Service-Routine (ISR) im IRAM
 * fuer sofortigen Regenschutz ohne Polling-Verzoegerung.
 * 
 * Hardware: Keyestudio ESP32 Smart Home Shield
 */

#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// ============================================================================
// 1. PIN-ZUWEISUNGEN & HARDWARE-PARAMETER
// ============================================================================

// Sensoren
#define PIN_RAIN_SENSOR   34  // Analog-In: Keyestudio Steam/Water Drop Sensor (Dach)
#define PIN_DHT           17  // Digital I/O: DHT11 Kombisensor (Temp/Feuchte)
#define PIN_DHT_TYPE      DHT11
#define PIN_RAIN_SENSOR       34  // Analog-In: Keyestudio Steam/Water Drop Sensor (Dach, ADC1_CH6)
#define PIN_RAIN_INTERRUPT    32  // Digitaler Hardware-Interrupt: DO-Trigger mit internem Pull-up (Active LOW)
#define PIN_DHT               17  // Digital I/O: DHT11 Kombisensor (Temp/Feuchte)
#define PIN_DHT_TYPE          DHT11

// Aktoren & Signalisierung
#define PIN_FAN_PWM       18  // Luefter: Drehzahlsteuerung via PWM (0-255)
#define PIN_FAN_DIR       19  // Luefter: H-Bruecken Richtungs-/Enable-Pin
#define PIN_LED_STATUS    12  // Status-LED (Taktung signalisiert Eskalationsstufe)
#define PIN_BUZZER        25  // Passiver Piezo-Buzzer (benoetigt Frequenzsignal via tone())
#define PIN_BTN_RESET     16  // Taster mit internem Pullup
#define PIN_SERVO_WINDOW  13  // PWM-Signal fuer Servomotor (Fensterverriegelung)
#define PIN_FAN_PWM           18  // Luefter: Drehzahlsteuerung via PWM (0-255)
#define PIN_FAN_DIR           19  // Luefter: H-Bruecken Richtungs-/Enable-Pin
#define PIN_LED_STATUS        12  // Status-LED (Taktung signalisiert Eskalationsstufe)
#define PIN_BUZZER            25  // Passiver Piezo-Buzzer (benoetigt Frequenzsignal via tone())
#define PIN_BTN_RESET         16  // Taster mit internem Pullup
#define PIN_SERVO_WINDOW      13  // PWM-Signal fuer Servomotor (Fensterverriegelung)

// Hardware-I2C Pins (Standard ESP32 Bus)
#define I2C_SDA           21
#define I2C_SCL           22
#define I2C_SDA               21
#define I2C_SCL               22

// Mechanische Anschlaege des Fensterservos
#define WINDOW_OPEN_DEG    0  // Grundstellung: Lueftung zulaessig
#define WINDOW_CLOSE_DEG  90  // Notfallstellung: Mechanisch verriegelt
#define WINDOW_OPEN_DEG        0  // Grundstellung: Lueftung zulaessig
#define WINDOW_CLOSE_DEG      90  // Notfallstellung: Mechanisch verriegelt

// ============================================================================
// 2. OBJEKT-INSTANZIIERUNG & ZUSTANDSKONFIGURATION
// ============================================================================

DHT dht(PIN_DHT, PIN_DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Adresse 0x27 via I2C-Scanner verifiziert
Servo windowServo;

// Zustandsdefinitionen der Finite State Machine (FSM)
enum SystemPhase {
  PHASE_0_NORMAL = 0,          // Normale Raumluft, Sensorik trocken
  PHASE_1_VENTILATION = 1,     // Erhoehte Feuchte -> Gedaempfte Lueftung
  PHASE_2_CRITICAL = 2,        // Kritische Feuchte -> Maximale Entlueftung
  PHASE_3_EMERGENCY_RAIN = 3   // Nasse Sensorplatte / Extremfeuchte -> Verriegelung
};

SystemPhase currentPhase = PHASE_0_NORMAL;

// Globale Messwerte & Puffer
float temperature = 0.0;
float humidity = 0.0;
int rainRaw = 0;
unsigned long lastUpdate = 0;
char lineBuffer[17];           // Formatierungspuffer (16 Zeichen + Nullterminator)

// Latenzfreie Interrupt-Steuerung (Hardware-ISR im IRAM)
volatile bool rainInterruptTriggered = false;

/**
 * @brief Hardware-Interrupt-Service-Routine (ISR) fuer sofortige Regenerkennung.
 * Liegt im IRAM (Instruction RAM), um Ausfuehrungsverzoegerungen durch SPI-Flash-Zugriffe
 * zu eliminieren (hartes Echtzeitverhalten).
 */
void IRAM_ATTR isr_rain_trigger() {
  rainInterruptTriggered = true;
}

/**
 * @brief Kapselt die Richtungs- und PWM-Ansteuerung des Lueftermotors.
 * @param speed PWM-Tastverhaeltnis von 0 (Stillstand) bis 255 (Volllast).
 */
void setFan(int speed) {
  digitalWrite(PIN_FAN_DIR, speed > 0 ? HIGH : LOW);
  analogWrite(PIN_FAN_PWM, speed);
}

// ============================================================================
// 3. SYSTEM-INITIALISIERUNG
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n[RainyGuard] Booting ESP32 Node...");

  // Pin-Konfigurationen
  pinMode(PIN_RAIN_SENSOR, INPUT);
  pinMode(PIN_RAIN_INTERRUPT, INPUT_PULLUP);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  pinMode(PIN_FAN_PWM, OUTPUT);
  pinMode(PIN_FAN_DIR, OUTPUT);
  pinMode(PIN_LED_STATUS, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  // Hardware-Interrupt fuer latenzfreien Regenschutz aktivieren (FALLING-Flanke)
  attachInterrupt(digitalPinToInterrupt(PIN_RAIN_INTERRUPT), isr_rain_trigger, FALLING);

  // Servo-Setup (50 Hz Standard-PWM fuer Modellbauservos)
  windowServo.setPeriodHertz(50);
  windowServo.attach(PIN_SERVO_WINDOW, 500, 2400);
  windowServo.write(WINDOW_OPEN_DEG);

  // Sicheren Grundzustand der Aktoren etablieren
  setFan(0);
  digitalWrite(PIN_LED_STATUS, LOW);
  noTone(PIN_BUZZER);

  dht.begin();
  
  // I2C-Bus defensiv konfigurieren: 50 kHz Takt & 50ms Timeout verhindern ESP32-Freezes
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(50000);
  Wire.setTimeOut(50);

  // LCD-Startsequenz
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("RainyGuard Node ");
  lcd.setCursor(0, 1);
  lcd.print("System Ready... ");
  delay(1200);
}

// ============================================================================
// 4. HAUPTSCHLEIFE (NON-BLOCKING POLLING & FSM)
// ============================================================================

void loop() {
  unsigned long now = millis();

  // Nicht-blockierender Abtastzyklus (1000 ms) ohne delay()
  // ==========================================================================
  // Latenzfreie Interrupt-Auswertung (Sofortige Notverriegelung ohne 1000ms Delay)
  // ==========================================================================
  if (rainInterruptTriggered) {
    rainInterruptTriggered = false;
    currentPhase = PHASE_3_EMERGENCY_RAIN;

    // Sofortige Notfallmassnahmen direkt einleiten (Zero-Latency Hardware-Schutz)
    setFan(0);
    windowServo.write(WINDOW_CLOSE_DEG);
    digitalWrite(PIN_LED_STATUS, HIGH);
    tone(PIN_BUZZER, 2200);

    // Sofortige Displaymeldung ausgeben
    lcd.setCursor(0, 0);
    lcd.print("! ALARM: REGEN !");
    lcd.setCursor(0, 1);
    lcd.print("LOCKDOWN / NOT  ");

    Serial.println("\n[EMERGENCY] Hardware-ISR ausgeloest: Sofortige Notverriegelung aktiv!");
  }

  // ==========================================================================
  // Nicht-blockierender Abtastzyklus (1000 ms) & FSM-Regelkreis
  // ==========================================================================
  if (now - lastUpdate >= 1000) {
    lastUpdate = now;

    humidity = dht.readHumidity();
    temperature = dht.readTemperature();
    rainRaw = analogRead(PIN_RAIN_SENSOR);

    // Sensorausfall abfangen
    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("[WARN] DHT11 nicht lesbar (Leitung an GPIO 17 pruefen)");
      return;
    }

    // --- FSM Zustandsuebergaenge ---
    // Keyestudio Steam-Sensor Verhalten: Trocken ~0-50 ADC, leitend bei Tropfen >500 ADC
    if (rainRaw > 500 || humidity > 90.0) {
    // Zusaetzlich wird der digitale Pegel des Interrupt-Pins beruecksichtigt (Active LOW)
    if (rainRaw > 500 || humidity > 90.0 || digitalRead(PIN_RAIN_INTERRUPT) == LOW) {
      currentPhase = PHASE_3_EMERGENCY_RAIN;
    } else if (humidity >= 78.0) {
      currentPhase = PHASE_2_CRITICAL;
    } else if (humidity >= 65.0) {
      currentPhase = PHASE_1_VENTILATION;
    } else {
      currentPhase = PHASE_0_NORMAL;
    }

    // Diagnosedaten fuer Serial Monitor / Logging
    Serial.printf("[FSM] Phase: %d | Temp: %.1f C | Hum: %.1f %% | Rain ADC: %d\n", 
                  currentPhase, temperature, humidity, rainRaw);
    Serial.printf("[FSM] Phase: %d | Temp: %.1f C | Hum: %.1f %% | Rain ADC: %d | Rain INT: %d\n", 
                  currentPhase, temperature, humidity, rainRaw, digitalRead(PIN_RAIN_INTERRUPT));

    // --- Aktoren & LCD synchron zum aktuellen Zustand setzen ---
    switch (currentPhase) {
      
      // Zustand 0: Raumluft im Sollbereich, Fenster offen, alle Systeme passiv
      case PHASE_0_NORMAL:
        setFan(0);
        windowServo.write(WINDOW_OPEN_DEG);
        digitalWrite(PIN_LED_STATUS, LOW);
        noTone(PIN_BUZZER);

        snprintf(lineBuffer, sizeof(lineBuffer), "T:%.1fC H:%.0f%%    ", temperature, humidity);
        lcd.setCursor(0, 0);
        lcd.print(lineBuffer);
        lcd.setCursor(0, 1);
        lcd.print("Status: Normal  ");
        break;

      // Zustand 1: Leichte Ueberfeuchtung -> Luefter auf Teillast (50 % PWM)
      case PHASE_1_VENTILATION:
        setFan(130);
        windowServo.write(WINDOW_OPEN_DEG);
        digitalWrite(PIN_LED_STATUS, (now / 500) % 2); // 1 Hz optisches Blinksignal
        noTone(PIN_BUZZER);

        snprintf(lineBuffer, sizeof(lineBuffer), "T:%.1fC H:%.0f%%    ", temperature, humidity);
        lcd.setCursor(0, 0);
        lcd.print(lineBuffer);
        lcd.setCursor(0, 1);
        lcd.print("P1: Fan 50%     ");
        break;

      // Zustand 2: Hohe Feuchte -> Volllast-Lueftung & akustischer Vorwarnton
      case PHASE_2_CRITICAL:
        setFan(255);
        windowServo.write(WINDOW_OPEN_DEG);
        digitalWrite(PIN_LED_STATUS, (now / 200) % 2); // Schnelles Warnblinken (2.5 Hz)
        
        // Diskreter akustischer Intervall-Piepton (1500 Hz, 150ms Puls / 850ms Pause)
        if (now % 1000 < 150) {
          tone(PIN_BUZZER, 1500);
        } else {
          noTone(PIN_BUZZER);
        }

        snprintf(lineBuffer, sizeof(lineBuffer), "WARN: Hum %.0f%%!  ", humidity);
        lcd.setCursor(0, 0);
        lcd.print(lineBuffer);
        lcd.setCursor(0, 1);
        lcd.print("P2: Max Fan 100%");
        break;

      // Zustand 3: Nasse Dachplatte oder Extremfeuchte
      // Massnahme: Sofortiger Not-Halt des Luefters gegen Wassereintrag, Fenster verriegeln
      case PHASE_3_EMERGENCY_RAIN:
        setFan(0);
        windowServo.write(WINDOW_CLOSE_DEG);
        digitalWrite(PIN_LED_STATUS, HIGH);
        
        // Modulierter Alarmton (Wechsel zwischen 2200 Hz und 1600 Hz alle 200ms)
        if ((now / 200) % 2) {
          tone(PIN_BUZZER, 2200);
        } else {
          tone(PIN_BUZZER, 1600);
        }

        lcd.setCursor(0, 0);
        lcd.print("! ALARM: REGEN !");
        lcd.setCursor(0, 1);
        lcd.print("LOCKDOWN / NOT  ");
        break;
    }
  }
}