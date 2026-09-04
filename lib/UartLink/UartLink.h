// ============================================================================
//  UartLink.h — Κοινό πρωτόκολλο UART  (ΣΠΙΤΙ ESP32 dev  <->  CYD οθόνη)
// ----------------------------------------------------------------------------
//  Γέφυρα ΜΕΤΑΞΥ του κόμβου σπιτιού (living_room) και της οθόνης (cyd).
//  Header-only & ΧΩΡΙΣ εξαρτήσεις (όχι RadioLib) ώστε να το χρησιμοποιούν ΚΑΙ
//  τα δύο environments χωρίς να τραβάει το CYD ολόκληρη τη βιβλιοθήκη HeatLink.
//
//  Πλαίσιο (frame), ίδια λογική με το LoRa protocol.h:
//     [SYNC0=0xA5][SYNC1=0x5A][TYPE][LEN][payload...][CRC_HI][CRC_LO]
//  CRC16-CCITT πάνω σε: TYPE, LEN, payload  -> πετάμε αλλοιωμένα/μισά πλαίσια.
//
//  Ροή:
//   * ΣΠΙΤΙ -> CYD : DisplayPacket (όλες οι τιμές για την οθόνη), ~1×/δευτ.
//   * CYD -> ΣΠΙΤΙ : CommandPacket (setpoint + εποχή) όταν ο χρήστης πατήσει κουμπί.
//
//  Θερμοκρασίες: int16 ×100 (όπως στο LoRa protocol.h). TEMP_NAN = "δεν υπάρχει".
// ============================================================================
#pragma once
#include <Arduino.h>
#include <math.h>
#include <WiFiUdp.h>

namespace uart_link {

static const uint8_t SYNC0 = 0xA5;
static const uint8_t SYNC1 = 0x5A;

enum : uint8_t { PKT_DISPLAY = 0x01, PKT_COMMAND = 0x02 };

// UDP ports — ασύρματη ζεύξη hub<->CYD μέσω WiFi (αντί για UART)
static const uint16_t UDP_DISP_PORT = 49501;  // hub -> CYD (DisplayPacket, broadcast)
static const uint16_t UDP_CMD_PORT  = 49502;  // CYD -> hub (CommandPacket, unicast)

// Εποχές — ΙΔΙΕΣ τιμές με το LoRa protocol.h (0=χειμώνας, 1=καλοκαίρι)
enum : uint8_t { SEASON_WINTER = 0, SEASON_SUMMER = 1 };

// flags του DisplayPacket (ΣΠΙΤΙ -> CYD)
static const uint8_t F_COMMS_OK     = 1 << 0;   // ζωντανή επικοινωνία σπίτι<->λεβητοστάσιο (LoRa)
static const uint8_t F_PUMP_OK      = 1 << 1;   // (μελλοντικό) αντλία εντάξει
static const uint8_t F_FANCOIL_ONLY = 1 << 2;   // ψύξη μόνο fancoil (δάπεδο απομονωμένο)
static const uint8_t F_TIME_OK      = 1 << 3;   // η ώρα (hh/mm/dd/mo) είναι έγκυρη (NTP συγχρ.)

// flags του CommandPacket (CYD -> ΣΠΙΤΙ)
static const uint8_t CMD_FANCOIL_ONLY = 1 << 0; // ο χρήστης διάλεξε ψύξη μόνο fancoil στο CYD

static const int16_t TEMP_NAN = INT16_MIN;

inline int16_t encT(float c) {
  if (isnan(c) || c < -100.0f || c > 200.0f) return TEMP_NAN;
  return (int16_t)lroundf(c * 100.0f);
}
inline float decT(int16_t v) { return (v == TEMP_NAN) ? NAN : (v / 100.0f); }

#pragma pack(push, 1)

// ΣΠΙΤΙ -> CYD : όλα όσα δείχνει η οθόνη
struct DisplayPacket {
  int16_t room;       // °C ×100  θερμοκρασία σπιτιού
  int16_t hotTop;     // °C ×100  buffer ζεστού (πάνω)
  int16_t outdoor;    // °C ×100  εξωτερική (αντιστάθμιση)
  int16_t cold;       // °C ×100  buffer κρύου
  int16_t valve;      // °C ×100  έξοδος τρίοδης
  int16_t pumpW;      // Watt αντλίας
  int16_t setpoint;   // °C ×100  επιθυμητή
  uint8_t season;     // 0=χειμώνας, 1=καλοκαίρι
  uint8_t flags;      // F_COMMS_OK | F_PUMP_OK | F_FANCOIL_ONLY | F_TIME_OK
  int8_t  rssi;       // dBm του LoRa (σπίτι<->λεβητοστάσιο)
  uint8_t hh, mm, dd, mo;  // ώρα/ημερομηνία από hub (NTP)· έγκυρα μόνο αν flags & F_TIME_OK
  uint8_t humidity;   // %RH δωματίου (SHT40)· 255 = άκυρο/δεν υπάρχει
  uint8_t ctrlStatus; // κατάσταση ελέγχου: 0=OFF/OK 1=θέρμ 2=ψύξη 3=αναμ.ζεστού 4=αναμ.κρύου 5=παγετός 6=ασφάλεια
  float   kwhDay, kwhMonth, kwhTotal;  // ενέργεια αντλίας θερμότητας (kWh) — σελίδα Ενέργειας CYD
  float   eurKwh;                       // τιμή €/kWh (για υπολογισμό κόστους στο CYD)
};

// CYD -> ΣΠΙΤΙ : εντολή χρήστη
struct CommandPacket {
  int16_t setpoint;   // °C ×100
  uint8_t season;     // 0=χειμώνας, 1=καλοκαίρι
  uint8_t flags;      // CMD_FANCOIL_ONLY (επιλογή ψύξης ενδοδαπέδιου από το CYD)
};

#pragma pack(pop)

// CRC16-CCITT (poly 0x1021) — ίδιο με το LoRa protocol.h
inline uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

// ---- UDP: στέλνει/λαμβάνει struct (raw bytes + crc16) πάνω από WiFi ----
template <typename T>
inline void udpSend(WiFiUDP& udp, IPAddress ip, uint16_t port, const T& pkt) {
  uint8_t buf[sizeof(T) + 2];
  memcpy(buf, &pkt, sizeof(T));
  uint16_t c = crc16(buf, sizeof(T));
  buf[sizeof(T)] = (uint8_t)(c >> 8); buf[sizeof(T) + 1] = (uint8_t)(c & 0xFF);
  udp.beginPacket(ip, port); udp.write(buf, sizeof(buf)); udp.endPacket();
}
template <typename T>
inline bool udpRecv(WiFiUDP& udp, int len, T& out) {
  if (len != (int)sizeof(T) + 2) return false;             // λάθος μέγεθος -> αγνόησε
  uint8_t buf[sizeof(T) + 2];
  if (udp.read(buf, sizeof(buf)) != (int)sizeof(buf)) return false;
  uint16_t got = ((uint16_t)buf[sizeof(T)] << 8) | buf[sizeof(T) + 1];
  if (crc16(buf, sizeof(T)) != got) return false;          // αλλοιωμένο -> αγνόησε
  memcpy(&out, buf, sizeof(T));
  return true;
}

// ---- Αποστολή ενός πλαισίου σε οποιοδήποτε Stream (Serial1/Serial2) ----
template <typename T>
void sendFrame(Stream& io, uint8_t type, const T& pkt) {
  uint8_t buf[4 + sizeof(T) + 2];
  buf[0] = SYNC0; buf[1] = SYNC1;
  buf[2] = type;  buf[3] = (uint8_t)sizeof(T);
  memcpy(buf + 4, &pkt, sizeof(T));
  uint16_t c = crc16(buf + 2, 2 + sizeof(T));   // CRC πάνω σε TYPE,LEN,payload
  buf[4 + sizeof(T)] = (uint8_t)(c >> 8);
  buf[5 + sizeof(T)] = (uint8_t)(c & 0xFF);
  io.write(buf, sizeof(buf));
}

inline void sendDisplay(Stream& io, const DisplayPacket& p) { sendFrame(io, PKT_DISPLAY, p); }
inline void sendCommand(Stream& io, const CommandPacket& p) { sendFrame(io, PKT_COMMAND, p); }

// ---- Δέκτης: ταΐζεις byte-byte, επιστρέφει τον τύπο όταν συμπληρωθεί έγκυρο πλαίσιο ----
struct Reader {
  uint8_t state = 0, type = 0, len = 0, cnt = 0, crcHi = 0;
  uint8_t pay[32];

  // Επιστρέφει PKT_* (>0) όταν αποκωδικοποιηθεί πλήρες & έγκυρο πλαίσιο στο pay[], αλλιώς 0.
  uint8_t feed(uint8_t b) {
    switch (state) {
      case 0: if (b == SYNC0) state = 1; break;
      case 1: state = (b == SYNC1) ? 2 : (b == SYNC0 ? 1 : 0); break;
      case 2: type = b; state = 3; break;
      case 3:
        len = b;
        if (len > sizeof(pay)) { state = 0; break; }  // απίθανο μήκος -> reset
        cnt = 0; state = len ? 4 : 5;
        break;
      case 4:
        pay[cnt++] = b;
        if (cnt >= len) state = 5;
        break;
      case 5: crcHi = b; state = 6; break;
      case 6: {
        uint16_t got = ((uint16_t)crcHi << 8) | b;
        state = 0;
        uint8_t tmp[2 + sizeof(pay)];
        tmp[0] = type; tmp[1] = len;
        memcpy(tmp + 2, pay, len);
        if (crc16(tmp, 2 + len) == got) return type;   // OK!
        break;
      }
    }
    return 0;
  }

  // Βοήθημα: αδειάζει ό,τι ήρθε στο Stream. Γράφει στο out τον τελευταίο τύπο που ολοκληρώθηκε.
  // (Καλείται συνεχώς στο loop· επιστρέφει true αν ήρθε νέο πλαίσιο.)
  bool poll(Stream& io, uint8_t& outType) {
    bool got = false;
    while (io.available()) {
      uint8_t t = feed((uint8_t)io.read());
      if (t) { outType = t; got = true; }   // κράτα το πιο πρόσφατο
    }
    return got;
  }

  const DisplayPacket& asDisplay() const { return *(const DisplayPacket*)pay; }
  const CommandPacket& asCommand() const { return *(const CommandPacket*)pay; }
};

}  // namespace uart_link
