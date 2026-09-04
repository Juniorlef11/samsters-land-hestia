// ============================================================================
//  protocol.h — Κοινό πρωτόκολλο LoRa για όλο το σύστημα θέρμανσης
// ----------------------------------------------------------------------------
//  Χρησιμοποιείται ΚΑΙ από τον S3 (λεβητοστάσιο) ΚΑΙ από το ESP32 dev (σπίτι),
//  ώστε να μιλάνε ΑΚΡΙΒΩΣ την ίδια "γλώσσα". Ό,τι αλλάξεις εδώ, ισχύει και στα δύο.
//
//  Σχεδιαστικές αρχές (γιατί είναι καλύτερο από fixed-width strings "S%04d%04d"):
//   * Δομές (struct) με σταθερό μέγεθος -> καθαρό parsing, χωρίς memcpy/atoi χειροκίνητα.
//   * type + version -> ξέρεις τι έλαβες και μπορείς να αλλάξεις το format αργότερα.
//   * seq (αύξων αριθμός) -> εντοπίζεις χαμένα πακέτα.
//   * CRC16 στο payload -> πετάς αλλοιωμένα πακέτα (μαζί με το link-CRC του RadioLib).
//   * Θερμοκρασίες ως int16 ×100 -> 0.01°C ακρίβεια, μικρό πακέτο, χωρίς floats στον αέρα.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <stddef.h>   // offsetof

// ---------------------------------------------------------------------------
//  Ρυθμίσεις ραδιοφώνου LoRa — ΠΡΕΠΕΙ να είναι ΙΔΙΕΣ και στους δύο κόμβους
// ---------------------------------------------------------------------------
static const float    LORA_FREQ     = 868.0f;  // MHz (ζώνη EU868)
static const float    LORA_BW       = 125.0f;  // kHz εύρος ζώνης
static const uint8_t  LORA_SF       = 9;       // spreading factor 9 — περιθώριο εμβέλειας/διείσδυσης τοίχων για 40m.
                                               // (Το «δεν δούλευε στο SF9» ήταν bug με το RX_DONE bit, όχι το SF — λύθηκε.)
static const uint8_t  LORA_CR       = 8;       // coding rate 4/8 (ισχυρό FEC — όσο και ο header).
                                               // Ήταν 4/5: το payload μάζευε λάθη bit ενώ ο header (πάντα 4/8) επιβίωνε,
                                               // οπότε ο δέκτης του σπιτιού έβγαζε συνεχώς CRC fail. Το 4/8 το διορθώνει.
static const uint8_t  LORA_SYNCWORD = 0x12;    // sync word (ιδιωτικό δίκτυο)
static const int8_t   LORA_POWER    = 22;      // dBm ισχύς εκπομπής (μέγιστο για το module)
static const uint16_t LORA_PREAMBLE = 8;       // preamble length

// ---------------------------------------------------------------------------
//  Τύποι μηνυμάτων
// ---------------------------------------------------------------------------
static const uint8_t PROTO_VERSION = 1;

// Σταθερό μήκος πλαισίου για IMPLICIT-HEADER mode. Πρέπει να είναι >= το μεγαλύτερο
// struct (BoilerStatus=18). Με implicit header + setCRC(false) ΔΕΝ μπαίνει hardware
// CRC στον αέρα → κανένα module δεν τον υπολογίζει → καμία ασυμφωνία μεταξύ S3/σπιτιού.
// Όλη η ακεραιότητα ελέγχεται με τον δικό μας CRC16 (stampCrc/checkCrc).
static const uint8_t LORA_FRAME_LEN = 24;

enum MsgType : uint8_t {
  MSG_BOILER_STATUS = 0x01,  // S3 (λεβητοστάσιο)  -> ESP32 dev (σπίτι)
  MSG_HOUSE_CMD     = 0x02,  // ESP32 dev (σπίτι)  -> S3 (λεβητοστάσιο)
};

enum Season : uint8_t { SEASON_WINTER = 0, SEASON_SUMMER = 1 };

// Κατάσταση ελέγχου (γιατί OFF / τι κάνει) — ταξιδεύει σε OLED + CYD + Telegram
enum CtrlStatus : uint8_t {
  CS_OFF       = 0,  // ικανοποιήθηκε (έφτασε ο στόχος)
  CS_HEATING   = 1,  // θέρμανση ενεργή
  CS_COOLING   = 2,  // ψύξη ενεργή
  CS_WAIT_HOT  = 3,  // θέλει θέρμανση αλλά buffer ζεστού όχι έτοιμο
  CS_WAIT_COLD = 4,  // θέλει ψύξη αλλά buffer κρύου όχι έτοιμο
  CS_FROST     = 5,  // αντιπαγετική προστασία
  CS_SAFETY    = 6,  // ασφάλεια (αισθητήρας/υπερθέρμανση/συμπύκνωση)
  CS_SYSTEM_OFF= 7,  // χειροκίνητο OFF (master stop από /off)
};

// Bitmask για το πεδίο HouseCmd.flags (εντολές/επιλογές από το σπίτι)
enum HouseCmdFlags : uint8_t {
  // 1 = ψύξη ΜΟΝΟ μέσω fancoil (ενδοδαπέδιο απομονωμένο) -> επιτρέπεται χαμηλή προσαγωγή.
  // 0 (default) = ενδοδαπέδιο ενεργό -> ΣΥΝΤΗΡΗΤΙΚΟ όριο (προστασία από συμπύκνωση).
  CMD_FLAG_FANCOIL_ONLY = 1 << 0,
  CMD_FLAG_SYSTEM_OFF   = 1 << 1,   // master OFF: κυκλοφορητής off, βάνα ως έχει (αντιπαγετική παραμένει)
  CMD_FLAG_HOME         = 1 << 2,   // re-home βάνας on-demand (/home): οδήγησε ΚΛΕΙΣΙΜΟ στο τέρμα -> θέση=0%
};

// Bitmask για την κατάσταση των 4 relay.
//  Η τρίοδη Seltron (3-σημείων / 3 καλώδια: κοινό + άνοιγμα + κλείσιμο) θέλει
//  ΔΥΟ relays — ένα ανά φορά περιστροφής. ΠΟΤΕ τα δύο μαζί (interlock στον κώδικα).
enum RelayBits : uint8_t {
  RELAY_VALVE_OPEN  = 1 << 0,  // τρίοδη: ΑΝΟΙΓΜΑ (μία φορά) — relay 1
  RELAY_VALVE_CLOSE = 1 << 1,  // τρίοδη: ΚΛΕΙΣΙΜΟ (αντίθετη φορά) — relay 2
  RELAY_PUMP        = 1 << 2,  // κυκλοφορητής
  RELAY_SPARE       = 1 << 3,  // εφεδρικό
};

// ---------------------------------------------------------------------------
//  Δομές πακέτων (packed = χωρίς padding, ίδιο μέγεθος σε κάθε compiler)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

// Στέλνεται από τον S3 (λεβητοστάσιο)
struct BoilerStatus {
  uint8_t  type;        // = MSG_BOILER_STATUS
  uint8_t  version;     // = PROTO_VERSION
  uint8_t  seq;         // αύξων αριθμός πακέτου
  int16_t  bufHotTop;   // °C ×100  DS18B20 #1 (buffer ζεστού, πάνω)
  int16_t  bufHotBot;   // °C ×100  DS18B20 #2 (buffer ζεστού, κάτω)
  int16_t  bufCold;     // °C ×100  DS18B20 #3 (buffer κρύου, μόνο σε καλοκαίρι)
  int16_t  valveTemp;   // °C ×100  DS18B20 #4 (έξοδος τρίοδης)
  uint16_t pumpPower;   // Watt — κατανάλωση αντλίας (SCT-013-030)
  uint8_t  relays;      // bitmask RelayBits (κατάσταση εξόδων)
  uint8_t  mode;        // Season (0=χειμώνας, 1=καλοκαίρι)
  uint8_t  ctrlStatus;  // CtrlStatus (γιατί OFF / τι κάνει)
  uint16_t crc;         // CRC16 όλων των προηγούμενων bytes
};

// Στέλνεται από το ESP32 dev (σπίτι)
struct HouseCmd {
  uint8_t  type;        // = MSG_HOUSE_CMD
  uint8_t  version;     // = PROTO_VERSION
  uint8_t  seq;         // αύξων αριθμός πακέτου
  int16_t  setpoint;    // °C ×100  επιθυμητή θερμοκρασία σπιτιού
  int16_t  houseTemp;   // °C ×100  τρέχουσα θερμοκρασία σπιτιού
  uint8_t  season;      // Season (0=χειμώνας, 1=καλοκαίρι)
  uint8_t  flags;       // HouseCmdFlags (CMD_FLAG_FANCOIL_ONLY)
  int16_t  dewPoint;    // °C ×100  σημείο δρόσου (από hub T+RH)· TEMP_NAN αν άγνωστο
  uint16_t crc;         // CRC16 όλων των προηγούμενων bytes
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
//  Βοηθητικά: κωδικοποίηση θερμοκρασίας & CRC
// ---------------------------------------------------------------------------
static const int16_t TEMP_NAN = INT16_MIN;   // "δεν υπάρχει αισθητήρας / σφάλμα"

inline int16_t encodeTemp(float c) {
  if (isnan(c) || c < -100.0f || c > 200.0f) return TEMP_NAN;
  return (int16_t)lroundf(c * 100.0f);
}
inline float decodeTemp(int16_t v) {
  return (v == TEMP_NAN) ? NAN : (v / 100.0f);
}

// CRC16-CCITT (poly 0x1021) — ίδιο και στους δύο κόμβους
inline uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

// Γεμίζει το πεδίο crc μιας δομής (το crc είναι ΠΑΝΤΑ το τελευταίο πεδίο)
template <typename T> inline void stampCrc(T& pkt) {
  pkt.crc = crc16((const uint8_t*)&pkt, offsetof(T, crc));
}
// Ελέγχει αν το crc μιας ληφθείσας δομής είναι σωστό
template <typename T> inline bool checkCrc(const T& pkt) {
  return pkt.crc == crc16((const uint8_t*)&pkt, offsetof(T, crc));
}
