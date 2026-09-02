#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// --- Pin-Definitionen ---
#define PIN_RAIN_SENSOR   34  // Keyestudio Steam/Drop Sensor Dach (Analog)
#define PIN_DHT           17  // DHT11 Data Pin
#define PIN_DHT_TYPE      DHT11

#define PIN_FAN_PWM       18  // Luefter Speed (PWM)
#define PIN_FAN_DIR       19  // Luefter Direction/Enable
#define PIN_LED_STATUS    12  // Status LED
#define PIN_BUZZER        25  // Passiver Piezo Buzzer
#define PIN_BTN_RESET     16  // Reset Taster
#define PIN_SERVO_WINDOW  13  // Servo Motor Fenster / Klappe

// Dedizierte I2C-Pins des ESP32 Boards
#define I2C_SDA           21
#define I2C_SCL           22

// Servo-Winkel (Je nach Ausrichtung 0° offen / 90° zu)
#define WINDOW_OPEN_DEG   0
#define WINDOW_CLOSE_DEG  90

// --- Hardware-Objekte ---
DHT dht(PIN_DHT, PIN_DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo windowServo;

// --- FSM Phasen ---
enum SystemPhase {
  PHASE_0_NORMAL = 0,
  PHASE_1_VENTILATION = 1,
  PHASE_2_CRITICAL = 2,
  PHASE_3_EMERGENCY_RAIN = 3
};

SystemPhase currentPhase = PHASE_0_NORMAL;

float temperature = 0.0;
float humidity = 0.0;
int rainRaw = 0;
unsigned long lastUpdate = 0;
char lineBuffer[17];

void setFan(int speed) {
  digitalWrite(PIN_FAN_DIR, speed > 0 ? HIGH : LOW);
  analogWrite(PIN_FAN_PWM, speed);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n[RainyGuard] Booting ESP32 Node...");

  pinMode(PIN_RAIN_SENSOR, INPUT);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  pinMode(PIN_FAN_PWM, OUTPUT);
  pinMode(PIN_FAN_DIR, OUTPUT);
  pinMode(PIN_LED_STATUS, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  // Servo Setup
  windowServo.setPeriodHertz(50);
  windowServo.attach(PIN_SERVO_WINDOW, 500, 2400);
  windowServo.write(WINDOW_OPEN_DEG); // Startposition: Offen

  setFan(0);
  digitalWrite(PIN_LED_STATUS, LOW);
  noTone(PIN_BUZZER);

  dht.begin();
  
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(50000);
  Wire.setTimeOut(50);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("RainyGuard Node ");
  lcd.setCursor(0, 1);
  lcd.print("System Ready... ");
  delay(1200);
}

void loop() {
  unsigned long now = millis();

  if (now - lastUpdate >= 1000) {
    lastUpdate = now;

    humidity = dht.readHumidity();
    temperature = dht.readTemperature();
    rainRaw = analogRead(PIN_RAIN_SENSOR);

    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("[WARN] DHT11 nicht lesbar (Pruefe GPIO 17)");
      return;
    }

    // --- Phasen-Eskalation ---
    if (rainRaw > 500 || humidity > 90.0) {
      currentPhase = PHASE_3_EMERGENCY_RAIN;
    } else if (humidity >= 78.0) {
      currentPhase = PHASE_2_CRITICAL;
    } else if (humidity >= 65.0) {
      currentPhase = PHASE_1_VENTILATION;
    } else {
      currentPhase = PHASE_0_NORMAL;
    }

    Serial.printf("[FSM] Phase: %d | Temp: %.1f C | Hum: %.1f %% | Rain ADC: %d\n", 
                  currentPhase, temperature, humidity, rainRaw);

    // --- Aktor- & Display-Steuerung nach Phase ---
    switch (currentPhase) {
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

      case PHASE_1_VENTILATION:
        setFan(130);
        windowServo.write(WINDOW_OPEN_DEG);
        digitalWrite(PIN_LED_STATUS, (now / 500) % 2);
        noTone(PIN_BUZZER);

        snprintf(lineBuffer, sizeof(lineBuffer), "T:%.1fC H:%.0f%%    ", temperature, humidity);
        lcd.setCursor(0, 0);
        lcd.print(lineBuffer);
        lcd.setCursor(0, 1);
        lcd.print("P1: Fan 50%     ");
        break;

      case PHASE_2_CRITICAL:
        setFan(255);
        windowServo.write(WINDOW_OPEN_DEG);
        digitalWrite(PIN_LED_STATUS, (now / 200) % 2);
        
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

      case PHASE_3_EMERGENCY_RAIN:
        setFan(0); // Not-Aus Luefter
        windowServo.write(WINDOW_CLOSE_DEG); // Fenster sofort verriegeln
        digitalWrite(PIN_LED_STATUS, HIGH);
        
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