#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// ===================== 配置区 =====================
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// 注意：使用 broker.hivemq.com 替代 test.mosquitto.org
// 因为 test.mosquitto.org 在宿主机网络下不可达
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

// ===================== 函数声明 =====================
void initScreen();
void showStatus(const char* status, uint16_t color);
void connectWiFi();
void mqttCallback(char* topic, byte* payload, unsigned int length);
bool reconnectMQTT();

// ===================== 辅助函数 =====================
void blinkLED(int count, int delayMs) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    if (i < count - 1) delay(delayMs);
  }
}

// ===================== setup =====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("====================================");
  Serial.println("ESP32 MQTT + ILI9341 + LED 启动 v2.0");
  Serial.println("====================================");

  pinMode(LED_PIN, OUTPUT);

  // LED 自检：快闪 3 次确认 LED 引脚正常
  Serial.print("LED 自检 (GPIO 13): ");
  blinkLED(3, 200);
  Serial.println("OK");

  Serial.print("MQTT 服务器: ");
  Serial.print(MQTT_SERVER);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  Serial.print("MQTT 主题: ");
  Serial.println(MQTT_TOPIC);

  initScreen();
  showStatus("System Starting...", ILI9341_BLUE);
  connectWiFi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

// ===================== loop =====================
void loop() {
  static unsigned long lastBlink = 0;
  static unsigned long lastPrint = 0;
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi Lost", ILI9341_RED);
    Serial.println("[WiFi] 连接断开，正在重连...");
    connectWiFi();
  }

  if (!mqttClient.connected()) {
    Serial.println("[MQTT] 连接断开，正在重连...");
    reconnectMQTT();
    delay(1000);
  }

  mqttClient.loop();

  // LED 心跳：MQTT 连接成功后每 5 秒闪一下，表示正常运行
  if (mqttClient.connected() && now - lastBlink > 5000) {
    lastBlink = now;
    digitalWrite(LED_PIN, HIGH);
    delay(50);
    digitalWrite(LED_PIN, LOW);
  }

  // 每 10 秒输出一次运行状态
  if (now - lastPrint > 10000) {
    lastPrint = now;
    Serial.print("[状态] 运行中 | WiFi: ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "已连接" : "未连接");
    Serial.print(" | MQTT: ");
    Serial.print(mqttClient.connected() ? "已连接" : "未连接");
    Serial.print(" | 运行时长: ");
    Serial.print(now / 1000);
    Serial.println("s");
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

// ===================== WiFi连接 =====================
void connectWiFi() {
  showStatus("Connecting WiFi...", ILI9341_ORANGE);
  Serial.print("[WiFi] 正在连接 ");
  Serial.print(WIFI_SSID);
  Serial.print(" ...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("[WiFi] ✅ 连接成功！");
    Serial.print("[WiFi] IP地址: ");
    Serial.println(WiFi.localIP());
    showStatus("WiFi Connected", ILI9341_GREEN);
  } else {
    Serial.println();
    Serial.print("[WiFi] ❌ 连接失败，状态码: ");
    Serial.println(WiFi.status());
    showStatus("WiFi Failed", ILI9341_RED);
  }
}

// ===================== MQTT回调 =====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msgBuffer[256];
  for (int i = 0; i < length && i < 255; i++) {
    msgBuffer[i] = (char)payload[i];
  }
  msgBuffer[length] = '\0';

  Serial.print("[MQTT] 收到消息 | 主题: ");
  Serial.print(topic);
  Serial.print(" | 长度: ");
  Serial.print(length);
  Serial.print(" | 内容: \"");
  Serial.print(msgBuffer);
  Serial.println("\"");

  // 显示消息到屏幕
  showStatus(msgBuffer, ILI9341_YELLOW);

  // 控制 LED
  if (strcmp(msgBuffer, "on") == 0) {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("[LED  🔴] 打开");
  } else if (strcmp(msgBuffer, "off") == 0) {
    digitalWrite(LED_PIN, LOW);
    Serial.println("[LED  ⚫] 关闭");
  } else {
    Serial.print("[LED  ⚠️] 未知指令: ");
    Serial.println(msgBuffer);
  }
}

// ===================== MQTT重连 =====================
bool reconnectMQTT() {
  showStatus("Connecting MQTT...", ILI9341_ORANGE);
  String clientId = MQTT_CLIENTID + String(random(0xffff), HEX);

  Serial.print("[MQTT] 正在连接...");
  Serial.print(" 服务器: ");
  Serial.print(MQTT_SERVER);
  Serial.print(":");
  Serial.print(MQTT_PORT);
  Serial.print(" 客户端ID: ");
  Serial.println(clientId);

  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("[MQTT] ✅ 连接成功！");
    blinkLED(5, 100);              // 快闪5次 → MQTT已连接
    Serial.print("[MQTT] 订阅主题: ");
    Serial.println(MQTT_TOPIC);

    if (mqttClient.subscribe(MQTT_TOPIC)) {
      Serial.println("[MQTT] ✅ 订阅成功");
    } else {
      Serial.println("[MQTT] ❌ 订阅失败");
    }

    showStatus("MQTT Connected!", ILI9341_GREEN);
    Serial.println("[MQTT] 等待消息...");
    return true;
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
    blinkLED(3, 500);              // 慢闪3次 → MQTT连接失败
    showStatus("MQTT Failed", ILI9341_RED);
    return false;
  }
}


