#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// ================= PIN =================
#define DHTPIN 15
#define DHTTYPE DHT22
#define PUMP_PIN 18
#define FAN_PIN 19

// ================= WIFI =================
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// ================= MQTT =================
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

const char* topic_temp = "lab2_nhom05/esp32/temperature";
const char* topic_humi = "lab2_nhom05/esp32/humidity";
const char* topic_mode = "lab2_nhom05/esp32/mode";
const char* topic_pump = "lab2_nhom05/esp32/pump";
const char* topic_fan  = "lab2_nhom05/esp32/fan";

// ================= OBJECT =================
DHT dht(DHTPIN, DHTTYPE);

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ================= SENSOR DATA =================
struct SensorData {
  float temperature;
  float humidity;
};

// ================= CONTROL COMMAND =================
enum CommandType {
  CMD_MODE,
  CMD_PUMP,
  CMD_FAN
};

struct ControlCommand {
  CommandType type;
  int value;
};

// ================= QUEUES =================
QueueHandle_t dataQueue;
QueueHandle_t dataAlert;
QueueHandle_t commandQueue;

// ================= MQTT CALLBACK =================
// Chi TaskTransmitData xu ly MQTT callback
void mqttCallback(char* topic, byte* payload,
                  unsigned int length) {

  String message = "";

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  message.trim();
  message.toUpperCase();

  ControlCommand cmd;

  if (strcmp(topic, topic_mode) == 0) {

    cmd.type = CMD_MODE;

    if (message == "AUTO") {
      cmd.value = 1;
    }
    else if (message == "MANUAL") {
      cmd.value = 0;
    }
    else {
      return;
    }
  }

  else if (strcmp(topic, topic_pump) == 0) {

    cmd.type = CMD_PUMP;

    if (message == "ON") {
      cmd.value = 1;
    }
    else if (message == "OFF") {
      cmd.value = 0;
    }
    else {
      return;
    }
  }

  else if (strcmp(topic, topic_fan) == 0) {

    cmd.type = CMD_FAN;

    if (message == "ON") {
      cmd.value = 1;
    }
    else if (message == "OFF") {
      cmd.value = 0;
    }
    else {
      return;
    }
  }

  else {
    return;
  }

  if (commandQueue != NULL) {
    if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
      Serial.println("Command Queue is full!");
    }
  }
}

// ================= TASK 1: TRANSMIT DATA =================
void TaskTransmitData(void *pvParameters) {

  Serial.println("Task truyen data - Core 0");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setSocketTimeout(2);
  mqttClient.setKeepAlive(30);

  unsigned long lastWiFiAttempt = 0;
  unsigned long lastMQTTAttempt = 0;

  while (1) {

    // Kiem tra WiFi
    if (WiFi.status() != WL_CONNECTED) {

      if (millis() - lastWiFiAttempt >= 5000) {
        lastWiFiAttempt = millis();

        Serial.println("Connecting WiFi...");
        WiFi.reconnect();
      }

      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Ket noi MQTT
    if (!mqttClient.connected()) {

      if (millis() - lastMQTTAttempt >= 5000) {
        lastMQTTAttempt = millis();

        Serial.println("Connecting MQTT...");

        String clientId = "ESP32_FreeRTOS_" +
                          String(random(0xffff), HEX);

        if (mqttClient.connect(clientId.c_str())) {

          Serial.println("MQTT Connected!");

          // Dang ky nhan lenh tu Web
          mqttClient.subscribe(topic_mode);
          mqttClient.subscribe(topic_pump);
          mqttClient.subscribe(topic_fan);

          Serial.println("MQTT Subscribed!");
        }
        else {
          Serial.print("MQTT connection failed: ");
          Serial.println(mqttClient.state());
        }
      }

      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Xu ly MQTT va nhan lenh
    mqttClient.loop();

    // Nhan du lieu cam bien
    SensorData data;

    if (xQueueReceive(dataQueue, &data, 0) == pdTRUE) {

      char tempBuffer[16];
      char humiBuffer[16];

      snprintf(tempBuffer, sizeof(tempBuffer),
               "%.1f", data.temperature);

      snprintf(humiBuffer, sizeof(humiBuffer),
               "%.1f", data.humidity);

      // Retain de Web nhan duoc gia tri moi nhat
      bool tempOK = mqttClient.publish(
        topic_temp, tempBuffer, true
      );

      bool humiOK = mqttClient.publish(
        topic_humi, humiBuffer, true
      );

      if (tempOK && humiOK) {
        Serial.println("MQTT: Data published!");
      }
      else {
        Serial.println("MQTT: Publish failed!");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================= TASK 2: READ SENSOR =================
void TaskReadData(void *pvParameters) {

  Serial.println("Task doc data - Core 1");

  while (1) {

    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();

    if (!isnan(temperature) && !isnan(humidity)) {

      SensorData data;

      data.temperature = temperature;
      data.humidity = humidity;

      Serial.print("Temperature: ");
      Serial.print(temperature);
      Serial.print(" C | Humidity: ");
      Serial.print(humidity);
      Serial.println(" %");

      // Chi luu mau cam bien moi nhat
      xQueueOverwrite(dataQueue, &data);

      // Gui do am sang Task canh bao
      xQueueOverwrite(dataAlert, &humidity);
    }
    else {
      Serial.println("DHT22 read failed!");
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ================= TASK 3: ALERT AND CONTROL =================
void TaskAlert(void *pvParameters) {

  Serial.println("Task canh bao - Core 1");

  float humidity = 0;
  bool hasHumidity = false;

  bool autoMode = true;

  bool manualPump = false;
  bool manualFan = false;

  bool pumpRunning = false;

  int currentZone = -1;

  unsigned long startTime = 0;
  unsigned long runDuration = 0;

  while (1) {

    // 1. Nhan du lieu do am
    if (xQueueReceive(dataAlert, &humidity, 0) == pdTRUE) {
      hasHumidity = true;
    }

    // 2. Nhan lenh dieu khien tu MQTT
    ControlCommand cmd;

    while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) {

      if (cmd.type == CMD_MODE) {

        autoMode = (cmd.value == 1);

        // Dung bom khi chuyen che do
        analogWrite(PUMP_PIN, 0);
        digitalWrite(FAN_PIN, LOW);

        pumpRunning = false;
        manualPump = false;
        manualFan = false;

        currentZone = -1;

        if (autoMode) {
          Serial.println("MODE: AUTO");
        }
        else {
          Serial.println("MODE: MANUAL");
        }
      }

      else if (cmd.type == CMD_PUMP) {

        if (!autoMode) {

          manualPump = (cmd.value == 1);

          analogWrite(
            PUMP_PIN,
            manualPump ? 255 : 0
          );

          Serial.println(
            manualPump ? "Manual Pump ON" :
                         "Manual Pump OFF"
          );
        }
      }

      else if (cmd.type == CMD_FAN) {

        if (!autoMode) {

          manualFan = (cmd.value == 1);

          digitalWrite(
            FAN_PIN,
            manualFan ? HIGH : LOW
          );

          Serial.println(
            manualFan ? "Manual Fan ON" :
                        "Manual Fan OFF"
          );
        }
      }
    }

    // 3. Dieu khien tu dong
    if (autoMode && hasHumidity) {

      // Dieu khien quat theo do am
      if (humidity > 60) {
        digitalWrite(FAN_PIN, HIGH);
      }
      else {
        digitalWrite(FAN_PIN, LOW);
      }

      int newZone;

      if (humidity >= 40) {
        newZone = 0;
      }
      else if (humidity >= 35) {
        newZone = 1;
      }
      else if (humidity >= 30) {
        newZone = 2;
      }
      else {
        newZone = 3;
      }

      // Xu ly khi vung do am thay doi
      if (newZone != currentZone) {

        currentZone = newZone;

        analogWrite(PUMP_PIN, 0);
        pumpRunning = false;

        if (newZone == 1) {

          analogWrite(PUMP_PIN, 64);

          runDuration = 10000;
          pumpRunning = true;

          Serial.println("Pump 25% - 10 seconds");
        }

        else if (newZone == 2) {

          analogWrite(PUMP_PIN, 128);

          runDuration = 20000;
          pumpRunning = true;

          Serial.println("Pump 50% - 20 seconds");
        }

        else if (newZone == 3) {

          analogWrite(PUMP_PIN, 255);

          runDuration = 30000;
          pumpRunning = true;

          Serial.println("Pump 100% - 30 seconds");
        }

        else {
          Serial.println("Humidity >= 40% - Pump OFF");
        }

        if (pumpRunning) {
          startTime = millis();
        }
      }
    }

    // 4. Kiem tra thoi gian bom
    if (autoMode && pumpRunning &&
        millis() - startTime >= runDuration) {

      analogWrite(PUMP_PIN, 0);
      pumpRunning = false;

      Serial.println("Pump OFF - Time completed");
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ================= SETUP =================
void setup() {

  Serial.begin(115200);

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);

  analogWrite(PUMP_PIN, 0);
  digitalWrite(FAN_PIN, LOW);

  dht.begin();

  // Tao 2 Queue du lieu
  dataQueue = xQueueCreate(1, sizeof(SensorData));
  dataAlert = xQueueCreate(1, sizeof(float));

  // Queue nhan lenh dieu khien
  commandQueue = xQueueCreate(10, sizeof(ControlCommand));

  if (dataQueue == NULL ||
      dataAlert == NULL ||
      commandQueue == NULL) {

    Serial.println("Queue creation failed!");

    while (1) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  // Task 1 - Core 0
  xTaskCreatePinnedToCore(
    TaskTransmitData,
    "Task truyen data",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  // Task 2 - Core 1
  xTaskCreatePinnedToCore(
    TaskReadData,
    "Task doc data",
    4096,
    NULL,
    1,
    NULL,
    1
  );

  // Task 3 - Core 1
  xTaskCreatePinnedToCore(
    TaskAlert,
    "Task canh bao",
    4096,
    NULL,
    1,
    NULL,
    1
  );

  Serial.println("FreeRTOS System Started!");
}

// ================= LOOP =================
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
