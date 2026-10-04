// ESP32-WROOM で DC モーターを制御する
// モータードライバ: TB6612FNG / L298N など (IN1, IN2, PWM の 3 本で制御するタイプ)
//
// シリアルコマンド (115200bps, 改行区切り):
//   f <0-255>  正転 (例: f 200)
//   b <0-255>  逆転 (例: b 120)
//   s          停止 (惰性で止まる)
//   x          ブレーキ (短絡制動)
//   a <0-180>  サーボの角度 (例: a 90)
//
// Wi-Fi: アクセスポイント "uilab-motor" (パスワード 12345678) に接続し、
//        http://192.168.4.1/ をブラウザで開くと操作画面が出る

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "page.h"

constexpr const char* WIFI_SSID = "uilab-motor";
constexpr const char* WIFI_PASS = "12345678";  // 8 文字以上必要

// ---- ピン設定 ----
constexpr int PIN_IN1 = 25;
constexpr int PIN_IN2 = 26;
constexpr int PIN_PWM = 27;   // TB6612 の PWMA / L298N の ENA
constexpr int PIN_STBY = 33;  // TB6612 の STBY。L298N なら -1 にする
constexpr int PIN_LED = 2;    // DevKit のオンボード LED
constexpr int PIN_SERVO = 18;

// ---- PWM 設定 ----
constexpr int PWM_CHANNEL = 0;
constexpr int PWM_FREQ = 20000;  // 20kHz (可聴域外にしてモーター鳴きを防ぐ)
constexpr int PWM_RESOLUTION = 8;  // 0-255

// ---- サーボ設定 (SG90 などの一般的なホビーサーボ) ----
// ESP32 Arduino 2.x では ch0/ch1 がタイマーを共有するので、周波数の違うサーボは ch2 を使う
constexpr int SERVO_CHANNEL = 2;
constexpr int SERVO_FREQ = 50;  // 20ms 周期
constexpr int SERVO_RESOLUTION = 16;
constexpr int SERVO_MIN_US = 500;   // 0 度のパルス幅
constexpr int SERVO_MAX_US = 2400;  // 180 度のパルス幅

// 速度変化をなめらかにする (1 ステップあたりの変化量と間隔)
constexpr int RAMP_STEP = 5;
constexpr uint32_t RAMP_INTERVAL_MS = 10;

// サーボは目標角度に向かって一定速度で動かす
enum class ServoMode { Manual, Sweep, Wiggle, Random };
constexpr uint32_t SERVO_UPDATE_MS = 20;

ServoMode servoMode = ServoMode::Manual;
float servoAngle = 90;
float servoTarget = 90;
float servoSpeed = 180;  // 度/秒
uint32_t lastServoAt = 0;
uint32_t servoPauseUntil = 0;

WebServer server(80);

int currentSpeed = 0;  // -255 (逆転) 〜 255 (正転)
int targetSpeed = 0;
uint32_t lastRampAt = 0;

void pwmInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_PWM, PWM_FREQ, PWM_RESOLUTION);
#else
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PIN_PWM, PWM_CHANNEL);
#endif
}

void pwmWrite(uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_PWM, duty);
#else
  ledcWrite(PWM_CHANNEL, duty);
#endif
}

void servoInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_SERVO, SERVO_FREQ, SERVO_RESOLUTION);
#else
  ledcSetup(SERVO_CHANNEL, SERVO_FREQ, SERVO_RESOLUTION);
  ledcAttachPin(PIN_SERVO, SERVO_CHANNEL);
#endif
}

void servoWrite(int angle) {
  int us = map(angle, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  uint32_t duty = (uint32_t)us * ((1 << SERVO_RESOLUTION) - 1) / (1000000 / SERVO_FREQ);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_SERVO, duty);
#else
  ledcWrite(SERVO_CHANNEL, duty);
#endif
}

const char* modeName(ServoMode mode) {
  switch (mode) {
    case ServoMode::Sweep: return "sweep";
    case ServoMode::Wiggle: return "wiggle";
    case ServoMode::Random: return "random";
    default: return "manual";
  }
}

void setServoMode(ServoMode mode) {
  servoMode = mode;
  servoPauseUntil = 0;
  if (mode == ServoMode::Sweep) servoTarget = servoAngle < 90 ? 180 : 0;
  if (mode == ServoMode::Wiggle) servoTarget = servoAngle < 90 ? 120 : 60;
}

// 目標角度に着いたとき、モードに応じて次の目標を決める
void pickNextTarget() {
  switch (servoMode) {
    case ServoMode::Sweep:
      servoTarget = servoTarget < 90 ? 180 : 0;
      break;
    case ServoMode::Wiggle:
      servoTarget = servoTarget < 90 ? 120 : 60;
      break;
    case ServoMode::Random:
      if (servoPauseUntil == 0) {
        servoPauseUntil = millis() + random(100, 1200);
      } else if ((int32_t)(millis() - servoPauseUntil) >= 0) {
        servoPauseUntil = 0;
        servoTarget = random(0, 181);
      }
      break;
    case ServoMode::Manual:
      break;
  }
}

void updateServo() {
  uint32_t now = millis();
  if (now - lastServoAt < SERVO_UPDATE_MS) return;
  float dt = (now - lastServoAt) / 1000.0f;
  lastServoAt = now;

  if (servoAngle == servoTarget) {
    pickNextTarget();
    return;
  }
  float step = servoSpeed * dt;
  if (fabsf(servoTarget - servoAngle) <= step) {
    servoAngle = servoTarget;
  } else {
    servoAngle += servoTarget > servoAngle ? step : -step;
  }
  servoWrite(lroundf(servoAngle));
}

// speed: -255〜255 をそのままドライバに出力する
void applySpeed(int speed) {
  if (speed > 0) {
    digitalWrite(PIN_IN1, HIGH);
    digitalWrite(PIN_IN2, LOW);
  } else if (speed < 0) {
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, HIGH);
  } else {
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, LOW);
  }
  pwmWrite(abs(speed));
  digitalWrite(PIN_LED, speed != 0 ? HIGH : LOW);
}

void brake() {
  targetSpeed = 0;
  currentSpeed = 0;
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN2, HIGH);
  pwmWrite(255);
  digitalWrite(PIN_LED, LOW);
}

void updateRamp() {
  if (currentSpeed == targetSpeed) return;
  uint32_t now = millis();
  if (now - lastRampAt < RAMP_INTERVAL_MS) return;
  lastRampAt = now;

  if (currentSpeed < targetSpeed) {
    currentSpeed = min(currentSpeed + RAMP_STEP, targetSpeed);
  } else {
    currentSpeed = max(currentSpeed - RAMP_STEP, targetSpeed);
  }
  applySpeed(currentSpeed);
}

void handleCommand(String line) {
  line.trim();
  if (line.isEmpty()) return;

  char cmd = line.charAt(0);
  int arg = line.substring(1).toInt();
  int value = constrain(arg, 0, 255);

  switch (cmd) {
    case 'f':
      targetSpeed = value;
      Serial.printf("forward %d\n", value);
      break;
    case 'b':
      targetSpeed = -value;
      Serial.printf("backward %d\n", value);
      break;
    case 's':
      targetSpeed = 0;
      Serial.println("stop");
      break;
    case 'x':
      brake();
      Serial.println("brake");
      break;
    case 'a': {
      int angle = constrain(arg, 0, 180);
      servoMode = ServoMode::Manual;
      servoTarget = angle;
      Serial.printf("servo %d\n", angle);
      break;
    }
    default:
      Serial.println("unknown command. use: f <0-255> | b <0-255> | s | x | a <0-180>");
      break;
  }
}

void setupWeb() {
  WiFi.softAP(WIFI_SSID, WIFI_PASS);

  server.on("/", []() { server.send_P(200, "text/html", PAGE_HTML); });
  server.on("/servo", []() {
    servoMode = ServoMode::Manual;
    servoTarget = constrain(server.arg("a").toInt(), 0, 180);
    server.send(200, "text/plain", "ok");
  });
  server.on("/speed", []() {
    servoSpeed = constrain(server.arg("v").toInt(), 20, 600);
    server.send(200, "text/plain", "ok");
  });
  server.on("/mode", []() {
    String m = server.arg("m");
    if (m == "sweep") setServoMode(ServoMode::Sweep);
    else if (m == "wiggle") setServoMode(ServoMode::Wiggle);
    else if (m == "random") setServoMode(ServoMode::Random);
    else setServoMode(ServoMode::Manual);
    server.send(200, "text/plain", "ok");
  });
  server.on("/motor", []() {
    targetSpeed = constrain(server.arg("v").toInt(), -255, 255);
    server.send(200, "text/plain", "ok");
  });
  server.on("/state", []() {
    char json[64];
    snprintf(json, sizeof(json), "{\"angle\":%.1f,\"mode\":\"%s\"}", servoAngle, modeName(servoMode));
    server.send(200, "application/json", json);
  });
  server.begin();

  Serial.printf("wifi: connect to \"%s\" (pass %s), open http://%s/\n", WIFI_SSID, WIFI_PASS,
                WiFi.softAPIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  if (PIN_STBY >= 0) {
    pinMode(PIN_STBY, OUTPUT);
    digitalWrite(PIN_STBY, HIGH);
  }
  pwmInit();
  applySpeed(0);
  servoInit();
  servoWrite(90);
  randomSeed(esp_random());

  Serial.println("motor ready. commands: f <0-255> | b <0-255> | s | x | a <0-180>");
  setupWeb();
}

void loop() {
  if (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }
  server.handleClient();
  updateRamp();
  updateServo();
}
