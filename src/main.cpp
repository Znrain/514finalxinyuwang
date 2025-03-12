#include <Arduino.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <MS5837.h>  // 需安装 BlueRobotics_MS5837_Library

// 与 Client 一致的 UUID
#define SERVICE_UUID        "2f22b6ad-51df-4f4c-9a3c-9f902d12c464"
#define CHARACTERISTIC_UUID "f7089b06-e82a-44bc-935a-2f31628f11c9"

MS5837 sensor;
BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// 每隔 1 秒发送一次数据
unsigned long previousMillis = 0;
const long interval = 1000; // 1 秒

// ========== 1. 定义滑动平均滤波器 ==========
#define FILTER_SIZE 5  // 缓冲区大小，可根据需要调整
float pressureBuffer[FILTER_SIZE];
int bufferIndex = 0;
bool bufferFilled = false;

// 初始化滤波缓冲
void initFilter() {
  for (int i = 0; i < FILTER_SIZE; i++) {
    pressureBuffer[i] = 0.0f;
  }
  bufferIndex = 0;
  bufferFilled = false;
}

// 向滤波缓冲区添加新数据
void addPressure(float p) {
  pressureBuffer[bufferIndex] = p;
  bufferIndex = (bufferIndex + 1) % FILTER_SIZE;
  if (bufferIndex == 0) {
    bufferFilled = true;  // 环形缓冲转一圈后标记为填满
  }
}

// 获取当前缓冲区的平均值（若未填满，则只平均已存的数据）
float getAveragePressure() {
  int count = bufferFilled ? FILTER_SIZE : bufferIndex;
  if (count == 0) return 0.0f;

  float sum = 0.0f;
  for (int i = 0; i < count; i++) {
    sum += pressureBuffer[i];
  }
  return sum / count;
}

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("Client connected.");
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("Client disconnected.");
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("Starting MS5837 BLE Server with DSP (Moving Average)...");

  // 1. 初始化 I2C
  Wire.begin();
  // 2. 初始化 MS5837
  if (!sensor.init()) {
    Serial.println("MS5837 sensor not found. Check wiring!");
    while (1) { delay(10); }
  }
  sensor.setModel(MS5837::MS5837_30BA); // 如果是 02BA, 改成 MS5837_02BA
  sensor.setFluidDensity(997);         // 淡水约997, 海水约1029~1030

  // 初始化滤波器
  initFilter();

  // 3. 初始化 BLE
  BLEDevice::init("WaterPressure_Server");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // 4. 创建 Service & Characteristic
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902()); // 允许 notify
  pService->start();

  // 5. 广播
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  Serial.println("BLE Server started, now advertising...");
}

void loop() {
  // 若客户端已连接，每秒读取 MS5837 并发送通知
  if (deviceConnected) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;

      // 1. 读取传感器
      sensor.read();
      float rawPressure_mbar = sensor.pressure();   // 原始压力 (mbar)
      float temperature_C = sensor.temperature();    // 温度 (°C)

      // 2. 滤波：将最新压力值加入缓冲，取平均值
      addPressure(rawPressure_mbar);
      float smoothedPressure = getAveragePressure();

      // 3. 拼接字符串 (发送平滑后的压力)
      char buffer[64];
      snprintf(buffer, sizeof(buffer),
               "P=%.2f mbar, T=%.2f C", smoothedPressure, temperature_C);

      // 4. 设置特征值并 notify
      pCharacteristic->setValue(buffer);
      pCharacteristic->notify();

      Serial.print("Notify (DSP): ");
      Serial.println(buffer);
    }
  }

  // 处理断开后重新广播
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("Re-advertising BLE...");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
}
