#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// ===================== 配置区 =====================
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* MQTT_SERVER      = "broker.hivemq.com";
const int   MQTT_PORT        = 1883;
const char* MQTT_TOPIC            = "esp32/wokwi/led";
const char* MQTT_STATUS_TOPIC     = "esp32/wokwi/led/status";
const char* MQTT_STATUS_REQ_TOPIC = "esp32/wokwi/led/status/req";
const char* MQTT_CLIENTID         = "esp32_wokwi_client_";

#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4

// ===================== 双 LED 定义 =====================
#define STATUS_LED_PIN  12   // 绿色 LED — 状态灯（连接状态、心跳）
#define SWITCH_LED_PIN  13   // 红色 LED — 开关灯（on/off 控制）

// ===================== 对象 =====================
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ===================== 非阻塞 LED 闪烁状态机 =====================
// 仅用于 STATUS_LED_PIN（状态灯）
struct BlinkTask {
  bool active = false;
  int remaining = 0;
  int onMs = 0;
  int offMs = 0;
  unsigned long nextToggle = 0;
  bool state = false;

  void start(int count, int onTime, int offTime) {
    active = true;
    remaining = count;
    onMs = onTime;
    offMs = offTime;
    nextToggle = millis();
    state = false;
  }

  bool update() {
    if (!active) return false;

    unsigned long now = millis();
    if (now < nextToggle) return true;

    state = !state;
    digitalWrite(STATUS_LED_PIN, state ? HIGH : LOW);

    if (state) {
      // off → on: 点亮 onMs 毫秒
      nextToggle = now + onMs;
    } else {
      // on → off: 减少剩余次数
      remaining--;
      if (remaining <= 0) {
        active = false;
        digitalWrite(STATUS_LED_PIN, LOW);
        return false;
      }
      nextToggle = now + offMs;
    }
    return true;
  }

  void stop() {
    active = false;
    digitalWrite(STATUS_LED_PIN, LOW);
  }
};

BlinkTask blinkTask;

// ===================== 系统状态机 =====================
enum SystemState {
  SYS_START,
  SYS_WIFI_CONNECTING,
  SYS_MQTT_CONNECTING,
  SYS_RUNNING
};

SystemState sysState = SYS_START;
unsigned long lastConnectAttempt = 0;
const unsigned long CONNECT_RETRY_MS = 3000;

// 状态灯交替心跳：亮 1000ms  +  灭 1000ms（50%占空比，更明显）
enum HeartbeatPhase { HB_PHASE_OFF, HB_PHASE_ON };
HeartbeatPhase hbPhase = HB_PHASE_OFF;
unsigned long lastHbToggleMs = 0;
const unsigned long HB_ON_MS = 1000;    // 亮 1 秒
const unsigned long HB_OFF_MS = 1000;   // 灭 1 秒（50%占空比）

unsigned long lastSwitchHeartMs = 0;  // 开关灯自检
unsigned long lastStatusMs = 0;

// ===================== 函数声明 =====================
void initScreen();
void showStatus(const char* status, uint16_t color);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void startMQTTConnection();
void publishLEDState(const char* state);

// ===================== setup =====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("===========================================");
  Serial.println("ESP32 MQTT + 双LED  非阻塞版 v3.0");
  Serial.println("===========================================");
  Serial.println("  状态灯(GPIO12) = MQTT连接/心跳指示");
  Serial.println("  开关灯(GPIO13) = on/off 控制");

  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(SWITCH_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  digitalWrite(SWITCH_LED_PIN, LOW);

  initScreen();
  showStatus("System Starting...", ILI9341_BLUE);

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(10);

  // LED 自检：状态灯快闪 3 次，开关灯常亮 0.5 秒
  Serial.println("[TEST] 双LED自检开始:");
  Serial.println("  状态灯(GPIO12): 快闪 3 次");
  blinkTask.start(3, 300, 300);
  Serial.println("  开关灯(GPIO13): 常亮 0.5 秒");
  digitalWrite(SWITCH_LED_PIN, HIGH);
  delay(500);   // 仅此一次，后续全部非阻塞
  digitalWrite(SWITCH_LED_PIN, LOW);
  Serial.println("[TEST] 自检完成");

  sysState = SYS_START;
}

// ===================== loop =====================
void loop() {
  unsigned long now = millis();

  // ========== 第1步：更新状态灯闪烁（非阻塞） ==========
  blinkTask.update();

  // ========== 第2步：处理 MQTT 网络数据 ==========
  if (mqttClient.connected()) {
    for (int i = 0; i < 5; i++) {
      if (!mqttClient.loop()) break;
    }
  }

  // ========== 第3步：系统状态机 ==========
  switch (sysState) {

    case SYS_START:
      if (!blinkTask.active) {
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
        sysState = SYS_MQTT_CONNECTING;
        lastConnectAttempt = 0;
      } else if (now - lastConnectAttempt > 10000) {
        Serial.println();
        Serial.print("[WiFi] ❌ 超时，状态码: ");
        Serial.println(WiFi.status());
        showStatus("WiFi Failed!", ILI9341_RED);
        delay(10);
        if (now - lastConnectAttempt > 10000) {
          lastConnectAttempt = now;
          WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
      } else {
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
      // ========== 状态灯交替心跳（500ms亮 / 2000ms灭 = 2.5秒周期） ==========
      if (!blinkTask.active) {
        switch (hbPhase) {
          case HB_PHASE_ON:
            // 亮满 500ms → 切换为灭
            if (now - lastHbToggleMs >= HB_ON_MS) {
              digitalWrite(STATUS_LED_PIN, LOW);
              hbPhase = HB_PHASE_OFF;
            }
            break;
          case HB_PHASE_OFF:
            // 灭满 2000ms → 切换为亮
            if (now - lastHbToggleMs >= HB_OFF_MS) {
              digitalWrite(STATUS_LED_PIN, HIGH);
              lastHbToggleMs = now;
              hbPhase = HB_PHASE_ON;
            }
            break;
        }
      } else {
        // BlinkTask 运行时（闪烁模式），重置心跳状态
        hbPhase = HB_PHASE_OFF;
        lastHbToggleMs = now;
      }

      // ========== 开关灯自检（每 20 秒闪 100ms，证明GPIO正常） ==========
      if (now - lastSwitchHeartMs > 20000) {
        lastSwitchHeartMs = now;
        digitalWrite(SWITCH_LED_PIN, HIGH);
        delay(100);
        digitalWrite(SWITCH_LED_PIN, LOW);
      }

      // ========== 每 10 秒打印状态 ==========
      if (now - lastStatusMs > 10000) {
        lastStatusMs = now;
        Serial.print("[状态] WiFi: ");
        Serial.print(WiFi.status() == WL_CONNECTED ? "已连接" : "未连接");
        Serial.print(" | MQTT: ");
        Serial.print(mqttClient.connected() ? "已连接" : "未连接");
        Serial.print(" | 运行: ");
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
        lastConnectAttempt = 0;
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

// ===================== 启动 MQTT 连接 =====================
void startMQTTConnection() {
  String clientId = MQTT_CLIENTID + String(random(0xffff), HEX);

  Serial.print("[MQTT] 连接 ");
  Serial.print(MQTT_SERVER);
  Serial.print(":");
  Serial.print(MQTT_PORT);
  Serial.print("  客户端ID: ");
  Serial.println(clientId);

  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("[MQTT] ✅ 连接成功！");

    // 立即处理 CONNACK
    mqttClient.loop();

    Serial.print("[MQTT] 订阅控制: ");
    Serial.println(MQTT_TOPIC);

    if (mqttClient.subscribe(MQTT_TOPIC, 0)) {
      Serial.println("[MQTT] ✅ 订阅控制成功 (QoS 0)");
      mqttClient.loop();  // 处理 SUBACK
    } else {
      Serial.println("[MQTT] ❌ 订阅控制失败");
    }

    // 订阅状态请求主题
    Serial.print("[MQTT] 订阅状态请求: ");
    Serial.println(MQTT_STATUS_REQ_TOPIC);
    if (mqttClient.subscribe(MQTT_STATUS_REQ_TOPIC, 0)) {
      Serial.println("[MQTT] ✅ 订阅状态请求成功 (QoS 0)");
      mqttClient.loop();
    } else {
      Serial.println("[MQTT] ❌ 订阅状态请求失败");
    }

    // 状态灯快闪 5 次 → MQTT 已连接
    blinkTask.start(5, 200, 200);
    showStatus("MQTT Connected!", ILI9341_GREEN);
    Serial.println("[MQTT] 等待消息...");
    Serial.println();

    // 发布初始状态（开关灯默认关闭）
    publishLEDState("off");
    Serial.println();

    sysState = SYS_RUNNING;
    // 初始化交替心跳为"亮"状态
    hbPhase = HB_PHASE_ON;
    lastHbToggleMs = millis();
    digitalWrite(STATUS_LED_PIN, HIGH);

  } else {
    int state = mqttClient.state();
    Serial.print("[MQTT] ❌ 连接失败，错误码: ");
    Serial.print(state);
    Serial.print(" (");
    switch (state) {
      case -4: Serial.print("超时"); break;
      case -3: Serial.print("断开"); break;
      case -2: Serial.print("协议版本"); break;
      case -1: Serial.print("标识符被拒"); break;
      case 1:  Serial.print("服务不可用"); break;
      case 2:  Serial.print("用户名/密码"); break;
      case 3:  Serial.print("未授权"); break;
      default: Serial.print("未知"); break;
    }
    Serial.println(")");

    // 状态灯慢闪 3 次 → MQTT 连接失败
    blinkTask.start(3, 500, 500);
    showStatus("MQTT Failed", ILI9341_RED);
  }
}

// ===================== 发布 LED 状态 =====================
void publishLEDState(const char* state) {
  if (!mqttClient.connected()) {
    Serial.println("[状态发布] ❌ MQTT 未连接，无法发布");
    return;
  }

  bool ok = mqttClient.publish(MQTT_STATUS_TOPIC, state);
  if (ok) {
    Serial.print("[状态发布] 🔄 ");
    Serial.print(MQTT_STATUS_TOPIC);
    Serial.print(": ");
    Serial.println(state);
  } else {
    Serial.print("[状态发布] ❌ 发布失败 ");
    Serial.println(state);
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
  Serial.print("[MQTT] 📩 收到消息 主题: ");
  Serial.print(topic);
  Serial.print("  内容: \"");
  Serial.print(msgBuffer);
  Serial.println("\"");

  // HEX 调试
  Serial.print("[MQTT] HEX: ");
  for (unsigned int i = 0; i < length; i++) {
    if (payload[i] < 0x10) Serial.print("0");
    Serial.print(payload[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
  Serial.println("==============================");

  // 状态灯快闪 2 次 → 确认收到消息
  blinkTask.start(2, 200, 200);

  // 显示到屏幕
  showStatus(msgBuffer, ILI9341_YELLOW);

  // ========== 处理状态请求 ==========
  // 网页重连后发送 "request" 到 MQTT_STATUS_REQ_TOPIC
  // ESP32 收到后发布当前状态 3 次（200ms 间隔），确保网页能同步
  if (strcmp(topic, MQTT_STATUS_REQ_TOPIC) == 0) {
    Serial.println("[状态请求] 收到状态请求，将发送当前状态 3 次");
    const char* currentState = digitalRead(SWITCH_LED_PIN) == HIGH ? "on" : "off";
    Serial.print("[状态请求] 当前开关状态: ");
    Serial.println(currentState);
    for (int i = 0; i < 3; i++) {
      publishLEDState(currentState);
      delay(200);  // 短暂间隔，非阻塞架构中仅此一处小 delay，不影响大局
    }
    Serial.println("[状态请求] ✅ 已发送 3 次状态响应");
    return;  // 不继续执行下面的开关控制逻辑
  }

  // ========== 控制开关灯（GPIO 13） ==========
  // 使用长度+字节比较（比 strcmp 更可靠，避免隐藏字符问题）
  if (length == 2 && payload[0] == 'o' && payload[1] == 'n') {
    digitalWrite(SWITCH_LED_PIN, HIGH);
    Serial.println("[开关灯] 🔴 打开 (常亮)");
    publishLEDState("on");
  } else if (length == 3 && payload[0] == 'o' && payload[1] == 'f' && payload[2] == 'f') {
    digitalWrite(SWITCH_LED_PIN, LOW);
    Serial.println("[开关灯] ⚫ 关闭");
    publishLEDState("off");
  } else {
   
    Serial.print("[开关灯] ⚠️ 未知指令, 长度=");
    Serial.print(length);
    Serial.print(" 内容=\"");
    Serial.print(msgBuffer);
    Serial.println("\"");
    // 显示每个字节的数值
    Serial.print("[开关灯] BYTES: ");
    for (unsigned int i = 0; i < length; i++) {
      Serial.print((int)payload[i]);
      Serial.print(" ");
    }
    Serial.println();
  }
}


