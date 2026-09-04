// ============================================================================
//  HeatLink — Λεπτή βιβλιοθήκη LoRa για το σύστημα θέρμανσης (LR1121 / RadioLib)
// ----------------------------------------------------------------------------
//  Κλειδώνει μέσα ΟΛΗ τη δουλεμένη config ώστε ο κώδικας των πλακετών να μένει
//  καθαρός. Χρησιμοποιείται ΚΑΙ από τον S3 (λεβητοστάσιο) ΚΑΙ από το σπίτι.
//
//  Τι κλειδώνει (μάθαμε με τον δύσκολο τρόπο — ΜΗΝ τα ξαναπειράζεις):
//   * implicit header + σταθερό μήκος LORA_FRAME_LEN (zero-padded) — κανένας
//     hardware CRC στον αέρα ώστε να μην υπάρχει ασυμφωνία μεταξύ των modules.
//   * setCRC(false) — η ακεραιότητα ελέγχεται ΜΟΝΟ με τον δικό μας CRC16.
//   * Λήψη με POLLING του RX_DONE μέσω SPI. ΠΡΟΣΟΧΗ: το getIrqFlags() του LR11x0
//     επιστρέφει RAW flags, ΟΧΙ standardized → RX_DONE = bit 3 = 0x08.
//   * SF/BW/CR/syncword/power/preamble από το protocol.h.
//   * Χαμηλό SPI clock (default 500 kHz) — ανεκτικό σε dupont καλωδίωση.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "protocol.h"

class HeatLink {
public:
  HeatLink(int8_t cs, int8_t irq, int8_t rst, int8_t busy,
           int8_t sck, int8_t miso, int8_t mosi, uint32_t spiHz = 500000);

  // Αρχικοποίηση SPI + radio. Επιστρέφει RADIOLIB_ERR_NONE (0) σε επιτυχία.
  int16_t begin();

  // Εκπομπή. Η βιβλιοθήκη βάζει type/version/seq + CRC16 και κάνει zero-pad.
  // Ο καλών γεμίζει μόνο τα πεδία δεδομένων. Επιστρέφει true σε επιτυχία.
  bool send(BoilerStatus pkt);   // ο S3 στέλνει status
  bool send(HouseCmd pkt);       // το σπίτι στέλνει εντολή

  // Λήψη (polling — κάλεσέ τη συχνά στο loop()). Γεμίζει out & επιστρέφει true
  // ΜΟΝΟ αν ήρθε έγκυρο πακέτο σωστού τύπου (type+version+software CRC16).
  bool receive(BoilerStatus& out);  // το σπίτι λαμβάνει status
  bool receive(HouseCmd& out);      // ο S3 λαμβάνει εντολή

  float rssi() { return _radio.getRSSI(); }
  float snr()  { return _radio.getSNR(); }

private:
  int8_t  _cs, _sck, _miso, _mosi;
  Module* _mod;
  LR1121  _radio;
  uint8_t _txSeq = 0;

  bool _grabFrame(uint8_t* buf);          // true αν διαβάστηκε frame (RX_DONE)
  bool _txFrame(const uint8_t* data, size_t len);
};
