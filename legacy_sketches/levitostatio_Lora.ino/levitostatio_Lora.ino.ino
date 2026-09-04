#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define LORA_CS    10
#define LORA_DIO9  7
#define LORA_RST   9
#define LORA_BUSY  8
#define LORA_SCK   12
#define LORA_MISO  13
#define LORA_MOSI  11

#define ONE_WIRE_BUS 4

LR1121 radio = new Module(LORA_CS, LORA_DIO9, LORA_RST, LORA_BUSY);

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.print("doyleyw");

  sensors.begin();

  Serial.print("S3 sensors found: ");
  Serial.println(sensors.getDeviceCount());

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  int state = radio.begin(868.0, 125.0, 7, 5, 0x12, 22, 8);
  radio.setCRC(false);

  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("Levitostasio LoRa init OK");
  } else {
    Serial.print("Levitostasio LoRa init failed: ");
    Serial.println(state);
    while (true) delay(1000);
  }
}

unsigned long lastStatusSend = 0;
const unsigned long statusInterval = 10000;

void loop() {
  unsigned long now = millis();

  // Στέλνει κάθε 10 sec
  if (now - lastStatusSend >= statusInterval) {
    lastStatusSend = now;

    sensors.requestTemperatures();
    float valveTemp = sensors.getTempCByIndex(0);

    int valveInt = (int)(valveTemp * 100);

    char msg[7];
    snprintf(msg, sizeof(msg), "V%05d", valveInt);

    Serial.print("S3 sending: ");
    Serial.println(msg);

    int txState = radio.transmit((uint8_t*)msg, 6);

    if (txState == RADIOLIB_ERR_NONE) {
      Serial.println("S3 send OK");
    } else {
      Serial.print("S3 send failed: ");
      Serial.println(txState);
    }

    delay(300);
  }

  // Ακούει για setpoint
  uint8_t rx[10];
  memset(rx, 0, sizeof(rx));

  size_t len = 9;
  int rxState = radio.receive(rx, len);

  if (rxState == RADIOLIB_ERR_NONE) {
    rx[9] = '\0';

    Serial.print("S3 received raw: ");
    Serial.println((char*)rx);

    if (rx[0] == 'S') {
      char setStr[5];
      char roomStr[5];

      memcpy(setStr, rx + 1, 4);
      setStr[4] = '\0';

      memcpy(roomStr, rx + 5, 4);
      roomStr[4] = '\0';

      float setTemp = atoi(setStr) / 100.0;
      float roomTemp = atoi(roomStr) / 100.0;

      Serial.print("Setpoint: ");
      Serial.println(setTemp);

      Serial.print("Room temp updated: ");
      Serial.println(roomTemp);
    }
  }
}