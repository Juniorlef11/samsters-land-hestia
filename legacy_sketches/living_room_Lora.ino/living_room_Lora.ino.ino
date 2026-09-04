#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define LORA_CS    5
#define LORA_DIO9  26
#define LORA_RST   14
#define LORA_BUSY  27
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23

#define ONE_WIRE_BUS 4

LR1121 radio = new Module(LORA_CS, LORA_DIO9, LORA_RST, LORA_BUSY);

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

void setup() {
  Serial.begin(115200);
  delay(2000);

  sensors.begin();

  Serial.print("Living room sensors found: ");
  Serial.println(sensors.getDeviceCount());

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  int state = radio.begin(868.0, 125.0, 7, 5, 0x12, 22, 8);
  radio.setCRC(false);

  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("Living room LoRa init OK");
  } else {
    Serial.print("Living room LoRa init failed: ");
    Serial.println(state);
    while (true) delay(1000);
  }
}

unsigned long lastSetSend = 0;
const unsigned long setSendInterval = 17000;

void loop() {
  unsigned long now = millis();

  // Στέλνει setpoint κάθε 17 sec για test
  if (now - lastSetSend >= setSendInterval) {
    lastSetSend = now;

  sensors.requestTemperatures();
  float roomTemp = sensors.getTempCByIndex(0);

  Serial.print("Raw room temp: ");
  Serial.println(roomTemp);

  float wantedTemp = 22.50;

  int setInt = (int)(wantedTemp * 100);
  int roomInt = (int)(roomTemp * 100);

    char msg[10];
    snprintf(msg, sizeof(msg), "S%04d%04d", setInt, roomInt);

    Serial.print("Living room sending: ");
    Serial.println(msg);

    radio.transmit((uint8_t*)msg, 9);

    int txState = radio.transmit((uint8_t*)msg, 6);

    if (txState == RADIOLIB_ERR_NONE) {
      Serial.println("Living room send OK");
    } else {
      Serial.print("Living room send failed: ");
      Serial.println(txState);
    }

    delay(300);
  }

  // Ακούει για θερμοκρασία τρίοδης
  uint8_t rx[7];
  size_t len = 6;

  int rxState = radio.receive(rx, len);

  if (rxState == RADIOLIB_ERR_NONE) {
    rx[6] = '\0';

    Serial.print("Valve Temp received: ");
    Serial.println((char*)rx);

    if (rx[0] == 'V') {
      int valveInt = atoi((char*)rx + 1);
      float valveTemp = valveInt / 100.0;

      Serial.print("Valve temp: ");
      Serial.print(valveTemp);
      Serial.println(" C");
    }

    Serial.print("RSSI: ");
    Serial.println(radio.getRSSI());
  }
}