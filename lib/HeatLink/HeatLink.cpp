#include "HeatLink.h"

// Raw LR11x0 IRQ bit για RX_DONE. Το getIrqFlags() ΔΕΝ είναι standardized!
static const uint32_t LR11X0_IRQ_RX_DONE = (1UL << 3);  // = 0x08

HeatLink::HeatLink(int8_t cs, int8_t irq, int8_t rst, int8_t busy,
                   int8_t sck, int8_t miso, int8_t mosi, uint32_t spiHz)
  : _cs(cs), _sck(sck), _miso(miso), _mosi(mosi),
    _mod(new Module(cs, irq, rst, busy, SPI, SPISettings(spiHz, MSBFIRST, SPI_MODE0))),
    _radio(_mod) {}

int16_t HeatLink::begin() {
  SPI.begin(_sck, _miso, _mosi, _cs);
  int16_t st = _radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                            LORA_SYNCWORD, LORA_POWER, LORA_PREAMBLE);
  if (st != RADIOLIB_ERR_NONE) return st;

  st = _radio.setCRC(false);            // ΟΧΙ hardware CRC — χρησιμοποιούμε CRC16
  if (st != RADIOLIB_ERR_NONE) return st;
  st = _radio.implicitHeader(LORA_FRAME_LEN);
  if (st != RADIOLIB_ERR_NONE) return st;

  return _radio.startReceive();
}

// ---- Εκπομπή ----
bool HeatLink::_txFrame(const uint8_t* data, size_t len) {
  uint8_t frame[LORA_FRAME_LEN] = {0};
  memcpy(frame, data, len);
  int16_t st = _radio.transmit(frame, LORA_FRAME_LEN);
  _radio.startReceive();                // πάντα ξαναμπαίνουμε σε λήψη
  return st == RADIOLIB_ERR_NONE;
}

bool HeatLink::send(BoilerStatus pkt) {
  pkt.type    = MSG_BOILER_STATUS;
  pkt.version = PROTO_VERSION;
  pkt.seq     = _txSeq++;
  stampCrc(pkt);
  return _txFrame((const uint8_t*)&pkt, sizeof(pkt));
}

bool HeatLink::send(HouseCmd pkt) {
  pkt.type    = MSG_HOUSE_CMD;
  pkt.version = PROTO_VERSION;
  pkt.seq     = _txSeq++;
  stampCrc(pkt);
  return _txFrame((const uint8_t*)&pkt, sizeof(pkt));
}

// ---- Λήψη ----
bool HeatLink::_grabFrame(uint8_t* buf) {
  if (!(_radio.getIrqFlags() & LR11X0_IRQ_RX_DONE)) return false;
  _radio.readData(buf, LORA_FRAME_LEN);   // implicit + no CRC: τα bytes είναι έγκυρα
  _radio.startReceive();                  // re-arm αμέσως
  return true;
}

bool HeatLink::receive(BoilerStatus& out) {
  uint8_t buf[LORA_FRAME_LEN] = {0};
  if (!_grabFrame(buf)) return false;
  if (buf[0] != MSG_BOILER_STATUS) return false;
  BoilerStatus tmp;
  memcpy(&tmp, buf, sizeof(tmp));
  if (tmp.version != PROTO_VERSION || !checkCrc(tmp)) return false;
  out = tmp;
  return true;
}

bool HeatLink::receive(HouseCmd& out) {
  uint8_t buf[LORA_FRAME_LEN] = {0};
  if (!_grabFrame(buf)) return false;
  if (buf[0] != MSG_HOUSE_CMD) return false;
  HouseCmd tmp;
  memcpy(&tmp, buf, sizeof(tmp));
  if (tmp.version != PROTO_VERSION || !checkCrc(tmp)) return false;
  out = tmp;
  return true;
}
