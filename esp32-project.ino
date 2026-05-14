#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// ===================== 配置区 =====================
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* MQTT_SERVER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* MQTT_TOPIC    = "esp32/wokwi/led";
const char* MQTT_CLIENTID = "esp32_wokwi_client_";

#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define LED_PIN  13

// ===================== 对象 =====================
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ===================== 非阻塞 LED 状态机 =====================
enum LedMode {
  LED_OFF,
  LED_ON,
  LED_BLINKING       // 闪烁模式，由 BlinkTask 控制
};

// 闪烁任务（非阻塞，用 millis() 驱动）
struct BlinkTask {
  bool active = false;
  int remaining = 0;
  int onMs = 0;
  int offMs = 0;
  unsigned long nextToggle = 0;
  bool state = false;       // true = LED on, false = LED off

  void start(int count, int onTime, int offTime) {
    active = true;
    remaining = count;
    onMs = onTime;
    offMs = offTime;
    nextToggle = millis();
    state = false;          // 立即进入第一个 on 周期
  }

  // 返回 true 表示闪烁还在进行中
  bool update() {
    if (!active) return false;

    unsigned long now = millis();
    if (now < nextToggle) return true;

    state = !state;
    digitalWrite(LED_PIN, state ? HIGH : LOW);

    if (state) {
      // 刚从 off→on，下一个切换是 on→off
      nextToggle = now + onMs;
    } else {
      // 刚从 on→off，减少剩余次数
      remaining--;
      if (remaining <= 0) {
        active = false;
        digitalWrite(LED_PIN, LOW);
        return false;
      }
      nextToggle = now + offMs;
    }
    return true;
  }

  void stop() {
    active = false;
    digitalWrite(LED_PIN, LOW);
  }
};

BlinkTask blinkTask;

// ===================== 系统状态 =====================
enum SystemState {
  SYS_START,
  SYS_WIFI_CONNECTING,
  SYS_MQTT_CONNECTING,
  SYS_RUNNING
};

SystemState sysState = SYS_START;

// 上次连接尝试的时间（非阻塞重试用）
unsigned long lastConnectAttempt = 0;
const unsigned long CONNECT_RETRY_MS = 3000;  // 重试间隔 3 秒
unsigned long lastHeartbeatMs = 0;
unsigned long lastStatusMs = 0;
unsigned long lastMqttLoopMs = 0;

// ===================== 函数声明 =====================
void initScreen();
void showStatus(const char* status, uint16_t color);
void connectWiFiAsync();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void startMQTTConnection();
bool processMQTT();

// ===================== setup =====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("===========================================");
  Serial.println("ESP32 MQTT + ILI9341 + LED  非阻塞版 v3.0");
  Serial.println("===========================================");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  initScreen();
  showStatus("System Starting...", ILI9341_BLUE);

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(10);  // 10 秒 keepalive，比默认 15 秒更积极

  // LED 自检：快闪 3 次确认 LED 引脚正常（非阻塞）
  Serial.println("[LED] 自检开始 (GPIO 13): 快闪 3 次");
  blinkTask.start(3, 200, 200);
  sysState = SYS_START;
}

// ===================== 非阻塞 LED 控制 =====================
// 在非闪烁状态下持续点亮/关闭 LED
void setLedPermanent(bool on) {
  if (!blinkTask.active) {
    digitalWrite(LED_PIN, on ? HIGH : LOW);
  }
}

// ===================== loop =====================
void loop() {
  unsigned long now = millis();

  // ========== 第1步：更新 LED 闪烁状态机（非阻塞） ==========
  blinkTask.update();

  // ========== 第2步：处理 MQTT 网络数据（最关键！） ==========
  // 每次 loop() 多次调用 loop() 以排空缓冲区
  if (mqttClient.connected()) {
    for (int i = 0; i < 5; i++) {  // 每次最多处理 5 个数据包
      if (!mqttClient.loop()) break;
    }
    lastMqttLoopMs = now;
  }

  // ========== 第3步：系统状态机 ==========
  switch (sysState) {

    case SYS_START:
      if (!blinkTask.active) {
        // LED 自检完成，开始连接 WiFi
        Serial.println("[WiFi] 自检完成，开始连接 WiFi...");
        showStatus("Connecting WiFi...", ILI9341_ORANGE);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        lastConnectAttempt = now;
        sysState = SYS_WIFI_CONNECTING;
      }
      break;

    case SYS_WIFI_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        Serial.println("[WiFi] ✅ 连接成功！");
        Serial.print("[WiFi] IP地址: ");
        Serial.println(WiFi.localIP());
        showStatus("WiFi Connected", ILI9341_GREEN);

        // 开始连接 MQTT
        sysState = SYS_MQTT_CONNECTING;
        lastConnectAttempt = 0;  // 立即尝试
      } else if (now - lastConnectAttempt > 10000) {
        Serial.println();
        Serial.print("[WiFi] ❌ 连接超时，状态码: ");
        Serial.println(WiFi.status());
        showStatus("WiFi Failed!", ILI9341_RED);
        // 10 秒后重试
        delay(10);
        if (now - lastConnectAttempt > 10000) {
          lastConnectAttempt = now;
          WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
      } else {
        // 每 500ms 打印一个点
        static unsigned long lastDot = 0;
        if (now - lastDot > 500) {
          lastDot = now;
          Serial.print(".");
        }
      }
      break;

    case SYS_MQTT_CONNECTING:
      if (now - lastConnectAttempt >= (lastConnectAttempt == 0 ? 0 : CONNECT_RETRY_MS)) {
        lastConnectAttempt = now;
        startMQTTConnection();
      }
      break;

    case SYS_RUNNING:
      // ========== 心跳 LED（每 5 秒微闪 50ms，非阻塞） ==========
      if (!blinkTask.active && now - lastHeartbeatMs > 5000) {
        lastHeartbeatMs = now;
        digitalWrite(LED_PIN, HIGH);
        // 用非阻塞方式：50ms 后关闭
        // 这里使用一个超短闪烁任务
        blinkTask.start(1, 50, 0);  // 闪一次，50ms 亮
      }

      // ========== 每 10 秒打印状态 ==========
      if (now - lastStatusMs > 10000) {
        lastStatusMs = now;
        Serial.print("[状态] 运行中 | WiFi: ");
        Serial.print(WiFi.status() == WL_CONNECTED ? "已连接" : "未连接");
        Serial.print(" | MQTT: ");
        Serial.print(mqttClient.connected() ? "已连接" : "未连接");
        Serial.print(" | 运行时长: ");
        Serial.print(now / 1000);
        Serial.print("s | 堆栈: ");
        Serial.print(ESP.getFreeHeap());
        Serial.println(" bytes");
      }

      // ========== 检查 MQTT 连接 ==========
      if (!mqttClient.connected()) {
        Serial.println("[MQTT] 连接断开，准备重连...");
        showStatus("MQTT Reconnecting...", ILI9341_ORANGE);
        sysState = SYS_MQTT_CONNECTING;
        lastConnectAttempt = 0;  // 立即尝试
      }
      break;
  }
}

// ===================== 屏幕初始化 =====================
void initScreen() {
  tft.begin();
  tft.setRotation(3);
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
}

// ===================== 屏幕显示 =====================
void showStatus(const char* status, uint16_t color) {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(10, 30);
  tft.println("=== MQTT Monitor ===");

  tft.setTextSize(3);
  tft.setTextColor(color);
  tft.setCursor(10, 80);
  tft.println(status);

  tft.setTextSize(1);
  tft.setTextColor(ILI9341_GREEN);
  tft.setCursor(10, 150);
  tft.print("IP: ");
  tft.println(WiFi.localIP().toString());
}

// ===================== 启动 MQTT 连接（非阻塞入口） =====================
void startMQTTConnection() {
  String clientId = MQTT_CLIENTID + String(random(0xffff), HEX);

  Serial.print("[MQTT] 正在连接 ");
  Serial.print(MQTT_SERVER);
  Serial.print(":");
  Serial.print(MQTT_PORT);
  Serial.print("  客户端ID: ");
  Serial.println(clientId);

  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("[MQTT] ✅ 连接成功！");

    // 关键：连接成功后先调用 loop() 处理 CONNACK，
    // 再订阅，确保连接完全建立
    mqttClient.loop();

    Serial.print("[MQTT] 订阅主题: ");
    Serial.println(MQTT_TOPIC);

    // 先 QoS 0（更快建立订阅），再尝试 QoS 1
    if (mqttClient.subscribe(MQTT_TOPIC, 0)) {
      Serial.println("[MQTT] ✅ 订阅成功 (QoS 0)");
      // 调用 loop() 处理 SUBACK
      mqttClient.loop();
    } else {
      Serial.println("[MQTT] ❌ 订阅失败");
    }

    // 快闪 5 次 → MQTT 已连接（非阻塞方式）
    blinkTask.start(5, 80, 80);
    showStatus("MQTT Connected!", ILI9341_GREEN);
    Serial.println("[MQTT] 等待消息中...");
    Serial.println();

    sysState = SYS_RUNNING;
    lastHeartbeatMs = millis();

  } else {
    int state = mqttClient.state();
    Serial.print("[MQTT] ❌ 连接失败，错误码: ");
    Serial.print(state);
    Serial.print(" (");
    switch (state) {
      case -4: Serial.print("连接超时"); break;
      case -3: Serial.print("连接已断开"); break;
      case -2: Serial.print("连接被拒-协议版本"); break;
      case -1: Serial.print("连接被拒-标识符被拒"); break;
      case 1:  Serial.print("连接被拒-服务不可用"); break;
      case 2:  Serial.print("连接被拒-用户名或密码错误"); break;
      case 3:  Serial.print("连接被拒-未授权"); break;
      default: Serial.print("未知错误"); break;
    }
    Serial.println(")");

    // 慢闪 3 次 → MQTT 连接失败（非阻塞方式）
    blinkTask.start(3, 300, 300);
    showStatus("MQTT Failed", ILI9341_RED);
    // 状态会保持在 SYS_MQTT_CONNECTING，定时重试
  }
}

// ===================== MQTT 回调 =====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msgBuffer[256];
  for (unsigned int i = 0; i < length && i < 255; i++) {
    msgBuffer[i] = (char)payload[i];
  }
  msgBuffer[length] = '\0';

  Serial.println();
  Serial.println("==============================");
  Serial.print("[MQTT] 📩 收到消息! 主题: ");
  Serial.print(topic);
  Serial.print("  内容: \"");
  Serial.print(msgBuffer);
  Serial.println("\"");

  // 显示 HEX 调试（检查不可见字符）
  Serial.print("[MQTT] HEX: ");
  for (unsigned int i = 0; i < length; i++) {
    if (payload[i] < 0x10) Serial.print("0");
    Serial.print(payload[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
  Serial.println("==============================");

  // 收到任何消息 → 快闪 2 次确认（非阻塞）
  blinkTask.start(2, 60, 60);

  // 显示到屏幕
  showStatus(msgBuffer, ILI9341_YELLOW);

  // 控制 LED
  if (strcmp(msgBuffer, "on") == 0) {
    // 停止闪烁，常亮
    blinkTask.stop();
    digitalWrite(LED_PIN, HIGH);
    Serial.println("[LED] 🔴 打开 (常亮)");
  } else if (strcmp(msgBuffer, "off") == 0) {
    blinkTask.stop();
    digitalWrite(LED_PIN, LOW);
    Serial.println("[LED] ⚫ 关闭");
  } else {
    Serial.print("[LED] ⚠️ 未知指令: \"");
    Serial.print(msgBuffer);
    Serial.println("\"");
    Serial.print("[LED]  字符码: ");
    for (unsigned int i = 0; i < length; i++) {
      Serial.print((int)payload[i]);
      Serial.print(" ");
    }
    Serial.println();
  }
}


