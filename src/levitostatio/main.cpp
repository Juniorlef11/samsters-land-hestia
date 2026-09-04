// ============================================================================
//  ΛΕΒΗΤΟΣΤΑΣΙΟ — ESP32-S3 + Core1121-XF (LR1121 @ 868 MHz)
// ----------------------------------------------------------------------------
//  ΡΟΛΟΣ: διαβάζει τα DS18B20 (buffer ζεστού/κρύου + τρίοδη), μετράει την
//         κατανάλωση της αντλίας (αργότερα) και στέλνει BoilerStatus στο σπίτι.
//         Λαμβάνει HouseCmd (setpoint + εποχή) από το σπίτι.
//         Εδώ θα ζήσει ΟΛΗ η έξυπνη λογική ελέγχου (αντιστάθμιση τρίοδης κ.λπ.).
//
//  ΑΣΦΑΛΕΙΑ: σε αυτή τη φάση ΔΕΝ ελέγχονται relays / αντλία — μόνο επικοινωνία
//            & θερμοκρασίες. Τα relay pins είναι σχολιασμένα.
//
//  Η επικοινωνία LoRa είναι κλειδωμένη στη βιβλιοθήκη HeatLink (lib/HeatLink).
// ============================================================================
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HeatLink.h>
#include <U8g2lib.h>   // οθονάκι OLED 0.96" SSD1306 (μπλε/κίτρινο, I2C) — τοπικό service display
#include <Wire.h>      // μόνο για το διαγνωστικό I2C scan στο boot
#include <Preferences.h>   // NVS: θέση βάνας (επιβιώνει διακοπή ρεύματος)

// ---- Pins LoRa (Core1121-XF στο ESP32-S3) ----
#define LORA_CS    10
#define LORA_DIO9  7    // IRQ (δεν χρησιμοποιείται — λήψη με polling)
#define LORA_RST   9
#define LORA_BUSY  8
#define LORA_SCK   12
#define LORA_MISO  13
#define LORA_MOSI  11

// ---- DS18B20 (1-Wire) ----
#define ONE_WIRE_BUS 4

// ---- OLED 0.96" SSD1306 128x64 (μπλε/κίτρινο, I2C) — τοπικό service display στο λεβητοστάσιο ----
//  ⚠️ ΟΧΙ στα default I2C 8/9 — τα πιάνει το LoRa (BUSY=8, RST=9)! Ορίζουμε ΕΛΕΥΘΕΡΑ pins.
//  Software-I2C (bitbang) -> δουλεύει σίγουρα σε όποια pins, χωρίς σύγκρουση με το Wire/LoRa.
//  Καλωδίωση: VCC->3.3V, GND->GND, SCL->GPIO16, SDA->GPIO15. (Αν λείπει η οθόνη: OLED_ENABLE false.)
#define OLED_ENABLE true
#define OLED_SCL 16
#define OLED_SDA 15
#if OLED_ENABLE
U8G2_SSD1306_128X64_NONAME_F_SW_I2C oled(U8G2_R0, OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
void oledMsg(const char* l1, const char* l2, const char* l3);   // prototypes (ορίζονται πριν το setup)
void oledInit();
void oledRender();
#endif

// ---- SCT-013-030 (30A/1V) — μέτρηση κατανάλωσης ΑΝΤΛΙΑΣ ΘΕΡΜΟΤΗΤΑΣ (Emmeti) ----
//  ΔΕΝ την ελέγχουμε (έχει δικό της Crono-TH) — ΜΟΝΟ μετράμε «πόσο καίει».
//  Voltage-output CT (εσωτερική burden) -> 1 αναλογικό pin + bias 2×10k->1.65V + 10µF.
//  Καλωδίωση: 3.3V-[10k]-A-[10k]-GND, A-[10µF+]-GND, CT#1->A, CT#2->GPIO5.
#define CT_ENABLE   true
#define CT_PIN      5         // ADC1 (GPIO5), ελεύθερο
#define CT_A_PER_V  30.0f     // SCT-013-030: 1V RMS = 30A
#define CT_CAL      1.00f     // βαθμονόμηση: CT_CAL *= (πραγματικό / μετρημένο) με γνωστό φορτίο
#define MAINS_V     230.0f    // W = A × V (inverter PF~0.95 -> κοντά στο πραγματικό)
#define CT_NOISE_FLOOR 100    // W: κάτω από αυτό -> 0 (κόβει θόρυβο ADC/EMI· η αντλία τραβάει εκατοντάδες W έως kW όταν δουλεύει)

// ---- 4ch relay (optocoupler) ----
//  Τρίοδη Seltron = 2 relays (άνοιγμα/κλείσιμο), κυκλοφορητής = 1, +1 εφεδρικό.
//  ΕΠΙΒΕΒΑΙΩΣΕ ότι αυτά τα GPIO είναι ελεύθερα στο δικό σου S3 board (το LoRa πιάνει 7-13).
#define RELAY_OPEN_PIN   14   // τρίοδη ΑΝΟΙΓΜΑ   (IN1)
#define RELAY_CLOSE_PIN  21   // τρίοδη ΚΛΕΙΣΙΜΟ  (IN2)
#define RELAY_PUMP_PIN   47   // κυκλοφορητής     (IN3)
#define RELAY_SPARE_PIN  48   // εφεδρικό         (IN4)
// Τα περισσότερα optocoupler boards είναι ACTIVE-LOW (LOW=ON). Αν τα κλικ βγουν ανάποδα -> false.
#define RELAY_ACTIVE_LOW true
// RELAY_SELFTEST=true -> στο loop ΜΟΝΟ χτυπάει τα 4 relay κυκλικά (για έλεγχο πολικότητας/καλωδίωσης).
#define RELAY_SELFTEST   false
// CALIBRATE_VALVE=true -> στο loop ΜΟΝΟ τρέχει η ΒΑΘΜΟΝΟΜΗΣΗ της βάνας (μέτρηση χρόνου διαδρομής).
//  ΑΠΑΙΤΕΙ: 230V στα relay + κινητήρας Seltron συνδεμένος στις εξόδους (δες οδηγίες). Κλείνει πλήρως,
//  μετά ανοίγει με ζωντανό χρονόμετρο — διαβάζεις σε πόσα s σταμάτησε ο δείκτης = VALVE_TRAVEL_MS.
#define CALIBRATE_VALVE  false
// VALVE_HOME_ON_BOOT=true -> στο live boot (DRY_RUN=false) κλείνει τη βάνα μέχρι τέρμα ώστε να
//  συγχρονίσει τη θέση στο 0% (3-σημείων χωρίς αισθητήρα θέσης). Αναγκαίο μετά από DRY_RUN
//  (η εκτιμώμενη θέση «άνοιξε» στα χαρτιά χωρίς να κινηθεί η βάνα). false = εμπιστεύσου το NVS.
#define VALVE_HOME_ON_BOOT false  // 2026-06-28: trust-NVS. Η Seltron δεν κουνιέται χωρίς ρεύμα ->
                                  // η αποθηκευμένη θέση μένει έγκυρη -> ΑΚΑΡΙΑΙΑ επαναφορά (χωρίς 2λεπτο homing
                                  // σε κάθε διακοπή/αναβόσβημα). Το re-reference στα end-stops διορθώνει drift.

HeatLink lora(LORA_CS, LORA_DIO9, LORA_RST, LORA_BUSY, LORA_SCK, LORA_MISO, LORA_MOSI);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ---- Κατάσταση ----
unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 10000;   // στέλνει status κάθε 10s

// Τιμές που λαμβάνουμε από το σπίτι (για μελλοντική λογική ελέγχου)
float   houseSetpoint = NAN;
float   houseTemp     = NAN;
uint8_t season        = SEASON_WINTER;
uint8_t savedSeasonNvs = SEASON_WINTER;  // NVS: τελευταία γνωστή εποχή (επιβιώνει reboot ακόμα & με LoRa κάτω)
float   savedSpNvs     = NAN;            // NVS: τελευταίο setpoint (κρατιέται σε commsLost/reboot — όχι override 18°)
uint32_t lastLoraReinit = 0;            // πότε έγινε τελευταίο re-init του LoRa radio (auto-recovery)
bool    gFancoilOnly  = false;     // από HouseCmd.flags: true = δάπεδο απομονωμένο στην ψύξη

// ========================= ΕΛΕΓΧΟΣ ΤΡΙΟΔΗΣ — ΦΑΣΗ 1: DRY-RUN =========================
//  DRY_RUN=true  -> ΚΑΜΙΑ έξοδος relay· μόνο υπολογισμός + εκτύπωση «τι θα έκανα».
//  Όταν επιβεβαιώσουμε τη λογική -> Φάση 2 (relays + interlock + μη-μπλοκαριστικοί παλμοί).
#define DRY_RUN false   // LIVE (2026-06-28): οδηγεί πραγματικά relays (βάνα + αντλία) με interlock.
                        // (Στο Στάδιο 3 με πραγματικούς αισθητήρες: SIM_MODE=false + DRY_RUN=false)

// SIM_MODE=true -> ΠΡΟΣΩΡΙΝΟ: εικονικοί αισθητήρες + θερμικό μοντέλο, για να ΔΟΥΜΕ τον
//  εγκέφαλο να συγκλίνει χωρίς boiler/αισθητήρες. Βάλε false για πραγματικούς αισθητήρες.
#define SIM_MODE false

// --- Αντιστοίχιση αισθητήρων (index στον OneWire bus) ---
//  ΠΡΟΣΟΧΗ: το index εξαρτάται από τη ROM διεύθυνση, ΟΧΙ τη σειρά καλωδίωσης!
//  Στη Φάση 2 «κλειδώνουμε» συγκεκριμένες ROM διευθύνσεις σε ρόλους (δες printSensorAddresses).
#define IDX_HOTBUF   0   // ζεστό buffer (πηγή θερμότητας)
#define IDX_OUTDOOR  1   // εξωτερική (αντιστάθμιση)
#define IDX_COLD     2   // κρύο buffer (καλοκαίρι)
#define IDX_SUPPLY   3   // προσαγωγή = έξοδος τρίοδης (η ελεγχόμενη μεταβλητή)

// --- Καμπύλη αντιστάθμισης (TUNABLE) — ενδοδαπέδιο + fancoil σε κοινό νερό ---
const float CURVE_SLOPE    = 0.7f;    // κλίση καμπύλης (ήπια για ενδοδαπέδιο)
const float SUPPLY_MAX     = 40.0f;   // ΣΚΛΗΡΟ ΟΡΙΟ προσαγωγής (προστασία δαπέδου!)
const float SUPPLY_MIN     = 25.0f;
const float ROOM_TRIM_GAIN = 1.5f;    // °C στόχου ανά °C σφάλματος δωματίου
const float DEADBAND       = 0.7f;    // ΔΙΟΡΘ. 2026-06-29: 1.5->0.7 (πιο σφιχτό, ~0.8° πιο κρύα προσαγωγή για Ελλάδα)
const float ROOM_HYST      = 0.3f;    // υστέρηση ζήτησης δωματίου
const float BUFFER_MARGIN  = 2.0f;    // ΔΙΟΡΘ. 2026-07-09: 3.0->2.0 — με ζεστότερο buffer (15-18°) το 3°
                                      // έκοβε πολύ νωρίς σε WAIT COLD. Το buffer πρέπει > στόχος + αυτό
                                      // (θέρμανση) ή < στόχος − αυτό (ψύξη).

// --- Ψύξη/δροσισμός (καλοκαίρι) — fancoil + ΠΡΟΣΟΧΗ: ενδοδαπέδιο ψύξη κινδυνεύει με ΣΥΜΠΥΚΝΩΣΗ! ---
const float COOL_SUPPLY_MIN_FLOOR   = 18.0f; // με ενεργό ενδοδαπέδιο: προστασία δαπέδου (σημείο δρόσου)
const float COOL_SUPPLY_MIN_FANCOIL = 13.0f; // μόνο fancoil (δάπεδο απομονωμένο): πιο δυνατή ψύξη
const float COOL_SUPPLY_MAX = 22.0f;  // ΔΙΟΡΘ. 2026-06-29: 24->22 (ψύχει από νωρίς, λιγότερο droop — Ελλάδα)
const float COOL_ROOM_GAIN  = 3.0f;   // ΔΙΟΡΘ. 2026-06-29: 2->3 (ανεβαίνει η ψύξη πιο γρήγορα ανά °C σφάλματος)
const float COOL_KI         = 0.015f; // ΝΕΟ 2026-06-30: ολοκληρωτικός όρος ψύξης ανά tick — εξαλείφει το droop
                                      //  (μαζεύει το μικρό σφάλμα -> κατεβάζει την προσαγωγή ώσπου να ΠΙΑΣΕΙ τον στόχο).
                                      //  Συντηρητικό (το δάπεδο είναι αργό)· αν αργεί, ανέβασέ το. Με anti-windup.
const float DEWPOINT_MARGIN = 2.0f;   // °C πάνω από το σημείο δρόσου (περιθώριο αντι-συμπύκνωσης)
const float COOL_SUPPLY_MIN_FLOOR_ABS = 16.0f;  // απόλυτο κάτω όριο δαπέδου (ακόμα κι αν ξηρός αέρας)

// --- Heat pump takeover (αντλία θερμότητας μέσω K4->ακροδέκτες 7-8 «Remote On/Off») ---
// ΔΙΟΡΘ. 2026-07-09 — ΚΡΙΣΙΜΟ: με pump setpoint 12.5° το BUFFER δεν φτάνει ΠΟΤΕ 13° (το φορτίο
// των 3 σπιτιών το κρατά 15-18°) -> το takeover δεν έσβηνε ΠΟΤΕ -> αντλία 24/7 -> ~8.6 kWh/μέρα
// παρασιτικά (359W idle × 24h). Τα κατώφλια πάνε στο ΠΡΑΓΜΑΤΙΚΟ εύρος λειτουργίας του buffer.
const float    HP_COLD_HIGH    = 17.5f;   // cold buffer >= αυτό -> ENABLE αντλία (χρειάζεται ψύξη)
const float    HP_COLD_LOW     = 15.0f;   // cold buffer <= αυτό -> DISABLE αντλία (αρκετά κρύο -> ΞΕΚΟΥΡΑΣΗ)
// Χειμώνας (hot buffer thermostat + SOLAR PRIORITY): tunable. Το ίδιο buffer τρέφει ΚΑΙ ντους (~48°)
// ΚΑΙ δάπεδο (μιξ στους 35°). Πρέπει HP_HOT_HIGH > setpoint αντλίας (par 215/216) ώστε το DISABLE να
// ενεργοποιείται από τα 6 ΗΛΙΑΚΑ (solar priority), όχι από την ίδια την αντλία.
const float    HP_HOT_LOW      = 47.0f;   // hot buffer <= αυτό -> ENABLE αντλία (θέλει ζέστα/ντους)
const float    HP_HOT_HIGH     = 53.0f;   // hot buffer >= αυτό -> DISABLE (ήλιος ζέστανε αρκετά -> άσ' τον)
// ΔΙΟΡΘ. 2026-07-24: ΑΣΥΜΜΕΤΡΟ anti-short-cycle. Το μικρό buffer (300L) + η υψηλή ζήτηση 3 σπιτιών
// κάνει το νερό να πάει 18° σε ~5min στο OFF -> τα σπίτια «πεινάνε». Λύση: ΜΕΓΑΛΟ min-ON (προστασία
// συμπιεστή + κρύο απόθεμα) αλλά ΜΙΚΡΟ min-OFF -> γρήγορη επανεκκίνηση όταν το buffer ζεσταθεί.
//   Αυτο-προσαρμόζεται: υψηλή ζήτηση -> re-enable σε ~4min = σχεδόν συνεχής (όχι starving)·
//                       χαμηλή ζήτηση -> το OFF επιμηκύνεται μόνο του (buffer ζεσταίνεται αργά) = ξεκούραση.
const uint32_t HP_MIN_ON_MS  = 600000;  // 10 min ελάχιστο ON (προστασία συμπιεστή + απόθεμα κρύου)
const uint32_t HP_MIN_OFF_MS = 240000;  // 4 min ελάχιστο OFF (η αντλία έχει και δικό της 3min internal)

// --- Βάνα Seltron 3-σημείων (TUNABLE) ---
#if SIM_MODE
const uint32_t VALVE_TRAVEL_MS = 20000;   // SIM: γρήγορη διαδρομή για να συγκλίνει στο demo
#else
const uint32_t VALVE_TRAVEL_MS = 110000;  // ΔΙΟΡΘ. 2026-06-28: live ο S3 «100%»=φυσικά 75% -> πραγμ.~103s.
                                          // 110s (γενναιόδωρο) ώστε να ανοίγει/κλείνει ΤΕΡΜΑ (end-stop). Fine-tune με re-calibrate.
#endif
const float    PULSE_PER_DEG   = 1500.0f; // ms παλμού ανά °C σφάλματος
const uint32_t PULSE_MIN_MS    = 1000;
const uint32_t PULSE_MAX_MS    = 8000;
#if SIM_MODE
const uint32_t SETTLE_MS       = 6000;    // SIM: γρήγορο settle για το demo
#else
const uint32_t SETTLE_MS       = 30000;   // ΔΙΟΡΘ. 2026-06-28: 90s->30s για γρηγορότερη απόκριση (ήταν υπερβολικά αργό)
#endif

// --- Failsafe / χρονισμοί ---
const float    FAILSAFE_SETPOINT = 18.0f;  // αν χαθεί το σπίτι -> ήπια προστασία (όχι κρύο)
const uint32_t CMD_TIMEOUT       = 300000; // 5' χωρίς εντολή = comms lost
const uint32_t LORA_REINIT_MS    = 60000;  // 60s χωρίς RX -> re-init το radio (auto-recovery, ΧΩΡΙΣ reboot ESP32)
const uint32_t CONTROL_INTERVAL  = 2000;   // πόσο συχνά τρέχει ο έλεγχος
const uint32_t TEMP_REQ_INTERVAL = 5000;   // πόσο συχνά ζητάμε νέα μέτρηση (ασύγχρονα)

// --- Premium χαρακτηριστικά (TUNABLE): αντιπαγετική / anti-seize / εξομάλυνση / anti-short-cycle ---
const float    FROST_ROOM_C      = 5.0f;   // δωμάτιο < αυτό -> αντιπαγετική
const float    FROST_SUPPLY_C    = 5.0f;   // προσαγωγή < αυτό -> αντιπαγετική
const float    FROST_TARGET      = 25.0f;  // στόχος προσαγωγής σε αντιπαγετική
const uint32_t PUMP_MIN_STATE_MS = 60000;  // ελάχ. χρόνος ON/OFF αντλίας (anti-short-cycle)
#if SIM_MODE
const uint32_t ANTI_SEIZE_INTERVAL = 20000;       // sim: 20s αδράνειας -> ξεμούδιασμα (για demo)
const uint32_t ANTI_SEIZE_RUN_MS   = 12000;       // sim: 12s εξάσκηση
const float    OUTDOOR_ALPHA       = 0.07f;       // sim: γρήγορη εξομάλυνση εξωτερικής
#else
const uint32_t ANTI_SEIZE_INTERVAL = 604800000UL; // 7 ημέρες αδράνειας -> ξεμούδιασμα αντλίας/βάνας
const uint32_t ANTI_SEIZE_RUN_MS   = 60000;       // 1 λεπτό εξάσκηση
const float    OUTDOOR_ALPHA       = 0.0006f;     // ~1h σταθερά χρόνου εξομάλυνσης (CONTROL_INTERVAL/τ)
#endif

// --- State ελέγχου ---
float valvePosPct = 0;          // εκτιμώμενη θέση βάνας 0-100% (ολοκλήρωση χρόνου παλμών)
uint8_t supplyNanCount = 0;     // διαδοχικά άκυρα reads προσαγωγής (debounce για SENSOR!)
const uint8_t SUPPLY_NAN_LIMIT = 3;   // μετά από τόσα ΣΥΝΕΧΟΜΕΝΑ -> ασφάλεια (αγνοεί στιγμιαία glitch DS18B20)
unsigned long lastValveAction = 0, lastControl = 0, lastCmdRx = 0, lastTempReq = 0;
bool  gOpen = false, gClose = false, gPump = false;   // intent (για telemetry/εκτύπωση)
float gTarget = NAN;            // τρέχων στόχος προσαγωγής
float gDewPoint = NAN;          // σημείο δρόσου από hub (για δυναμικό όριο ψύξης)
float gCoolMin  = NAN;          // τρέχον ΕΛΑΧΙΣΤΟ επιτρεπτό νερό ψύξης (δρόσος/ρύθμιση)
                                //  -> το heatPumpControl βγάζει από αυτό ΔΥΝΑΜΙΚΑ κατώφλια buffer
bool  gSystemOff = false;       // master OFF από hub (/off): κυκλοφορητής off, βάνα ως έχει
bool  gHomeReq   = false;       // αίτημα re-home βάνας (από /home) — εκτελείται στο loop
bool  gHomeArmed = false;       // latch: re-home μία φορά ανά αίτημα (rising-edge του CMD_FLAG_HOME)
float gCoolI     = 0.0f;        // ψύξη: ολοκληρωτικός συσσωρευτής (κατεβάζει την προσαγωγή — εξαλείφει droop)
bool  gCoolActive = false;      // ψύξη: stateful υστέρηση (κλείνει στον στόχο, ξανανοίγει στο sp+ROOM_HYST)
bool     gHpEnabled    = true;  // takeover αντλίας: true = αφήνουμε την αντλία (7-8 κλειστό). Boot=enabled (ασφαλές).
uint32_t gHpLastChange = 0;     // takeover αντλίας: για min on/off time (anti-short-cycle)
float outdoorFilt = NAN;        // εξομαλυμένη εξωτερική (EMA — αντι-spike)
bool  gPumpState = false;       // κατάσταση αντλίας με anti-short-cycle
unsigned long lastPumpChange = 0, lastOperation = 0, exerciseStart = 0;
bool  aseizeOpened = false, aseizeClosed = false;
unsigned long valvePulseEnd = 0;   // πότε λήγει ο τρέχων παλμός βάνας (0 = ανενεργό)
unsigned long lastOled = 0;        // χρονισμός ανανέωσης OLED
uint16_t gHeatPumpW = 0;           // κατανάλωση αντλίας θερμότητας (W) από SCT-013
unsigned long lastCt = 0;          // χρονισμός δειγματοληψίας CT
Preferences vprefs;                // NVS για τη θέση βάνας
float savedValvePos = -1.0f;       // τελευταία αποθηκευμένη θέση (write-on-change)
unsigned long lastValveSave = 0;   // throttle αποθήκευσης (φθορά flash)

// --- Στιγμιότυπο για το OLED (γεμίζει στο controlTick -> ισχύει & σε SIM & σε πραγματικό) ---
float   gSupply=NAN, gOutRaw=NAN, gHotBuf=NAN, gCold=NAN, gRoom=NAN, gSp=NAN;
uint8_t gSeasonEff = SEASON_WINTER;
bool    gCommsLost = false;
char    gStatus[16] = "BOOT";   // σύντομη κατάσταση: HEAT/COOL/OFF/FROST/SENSOR!/MAX 40!/COND!
bool    gAlarm = false;         // true -> εμφανίζεται με «!» (κίτρινη ζώνη)
inline void setStatus(const char* s, bool al) {
  strncpy(gStatus, s, sizeof(gStatus)-1); gStatus[sizeof(gStatus)-1] = 0; gAlarm = al;
}
// Κωδικός κατάστασης (CtrlStatus) από το status string -> στέλνεται σε CYD/Telegram
inline uint8_t ctrlStatusCode() {
  if (!strcmp(gStatus, "SYS OFF"))   return CS_SYSTEM_OFF;
  if (!strcmp(gStatus, "HEAT"))      return CS_HEATING;
  if (!strcmp(gStatus, "COOL"))      return CS_COOLING;
  if (!strcmp(gStatus, "WAIT HOT"))  return CS_WAIT_HOT;
  if (!strcmp(gStatus, "WAIT COLD")) return CS_WAIT_COLD;
  if (!strcmp(gStatus, "FROST"))     return CS_FROST;
  if (gAlarm)                        return CS_SAFETY;   // SENSOR! / MAX 40! / COND!
  return CS_OFF;                                          // ικανοποιήθηκε
}

// --- ROM διευθύνσεις ΚΛΕΙΔΩΜΕΝΕΣ ανά ρόλο (ταυτοποιήθηκαν 2026-06-18) ---
// index: 0=ζεστό buffer, 1=εξωτερική, 2=κρύο buffer, 3=προσαγωγή (όπως τα IDX_*).
// Ο ρόλος ΔΕΝ εξαρτάται πια από τη σειρά ανίχνευσης. Αν αλλάξεις αισθητήρα,
// ενημέρωσε ΜΟΝΟ το ROM του εδώ (το νέο ROM τυπώνεται στο boot, printSensorAddresses).
DeviceAddress romTable[4] = {
  {0x28,0x3A,0xD5,0x6F,0x00,0x00,0x00,0xBA},  // 0  ζεστό buffer
  {0x28,0x29,0xD0,0x6F,0x00,0x00,0x00,0x6A},  // 1  εξωτερική
  {0x28,0xB5,0x96,0x71,0x00,0x00,0x00,0xFE},  // 2  κρύο buffer
  {0x28,0x53,0x43,0x71,0x00,0x00,0x00,0xE9},  // 3  προσαγωγή (έξοδος τρίοδης)
};

// Διαβάζει DS18B20 ΑΝΑ ΡΟΛΟ μέσω κλειδωμένης ROM (όχι σειράς)· NAN αν λείπει/σφάλμα
float readSensor(uint8_t index) {
  if (index >= 4) return NAN;
  float t = sensors.getTempC(romTable[index]);
  return (t <= DEVICE_DISCONNECTED_C) ? NAN : t;
}

// Εκτυπώνει τις ROM διευθύνσεις όλων των DS18B20 — για να «κλειδώσουμε» ρόλους στη Φάση 2.
// (Ζέστανε/κράτα έναν αισθητήρα τη φορά για να δεις ποιο index αλλάζει = ποιος είναι ποιος.)
void printSensorAddresses() {
  uint8_t n = sensors.getDeviceCount();
  Serial.printf("DS18B20 που βρέθηκαν: %d\n", n);
  for (uint8_t i = 0; i < n; i++) {
    DeviceAddress a;
    if (sensors.getAddress(a, i)) {
      Serial.printf("  index %d  ROM: ", i);
      for (uint8_t j = 0; j < 8; j++) Serial.printf("%02X", a[j]);
      Serial.println();
    }
  }
}

// ---- Relay helpers ----
inline void relayWrite(uint8_t pin, bool on) {
  // ACTIVE_LOW: on -> LOW. Αλλιώς on -> HIGH.
  digitalWrite(pin, (on == RELAY_ACTIVE_LOW) ? LOW : HIGH);
}
void relaysInit() {
  pinMode(RELAY_OPEN_PIN, OUTPUT);  relayWrite(RELAY_OPEN_PIN, false);
  pinMode(RELAY_CLOSE_PIN, OUTPUT); relayWrite(RELAY_CLOSE_PIN, false);
  pinMode(RELAY_PUMP_PIN, OUTPUT);  relayWrite(RELAY_PUMP_PIN, false);
  pinMode(RELAY_SPARE_PIN, OUTPUT); relayWrite(RELAY_SPARE_PIN, false);
}
void relaySelfTest() {
  const uint8_t pins[4] = { RELAY_OPEN_PIN, RELAY_CLOSE_PIN, RELAY_PUMP_PIN, RELAY_SPARE_PIN };
  const char* names[4]  = { "OPEN", "CLOSE", "PUMP", "SPARE" };
  Serial.println(F("[RELAY-TEST] κάθε relay: ON 0.7s -> OFF 0.4s (πρέπει να ακούς κλικ ΣΤΟ ON):"));
#if OLED_ENABLE
  oledMsg("RELAY TEST", "K1..K4 cycling", "listen clicks");
#endif
  for (int i = 0; i < 4; i++) {
    Serial.printf("   K%d %-5s -> ON\n",  i + 1, names[i]); relayWrite(pins[i], true);  delay(700);
    Serial.printf("   K%d %-5s -> OFF\n", i + 1, names[i]); relayWrite(pins[i], false); delay(400);
  }
}

#if OLED_ENABLE
// Απλό μήνυμα 3 γραμμών (boot / self-test / calibrate)
void oledMsg(const char* l1, const char* l2, const char* l3) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x14B_tf); if (l1) oled.drawStr(0, 13, l1);
  oled.setFont(u8g2_font_6x10_tf);
  if (l2) oled.drawStr(0, 34, l2);
  if (l3) oled.drawStr(0, 50, l3);
  oled.sendBuffer();
}
void oledInit() {
  // 1) Διαγνωστικό I2C scan (hardware Wire) στα δικά μας pins — μαθαίνουμε αν/πού απαντά η οθόνη
  Wire.begin(OLED_SDA, OLED_SCL, 100000);
  delay(50);
  uint8_t found = 0, count = 0;
  Serial.println(F("[OLED] I2C scan @ SDA=15 SCL=16 ..."));
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      count++;
      Serial.printf("   βρέθηκε device @ 0x%02X\n", addr);
      if (addr == 0x3C || addr == 0x3D) found = addr;
    }
  }
  Wire.end();
  if (!found) {
    Serial.printf("[OLED] ΔΕΝ βρέθηκε οθόνη (devices=%d)! Έλεγξε VCC=3.3V / GND / SDA=15 / SCL=16.\n", count);
    found = 0x3C;   // δοκίμασε ούτως ή άλλως την τυπική
  } else {
    Serial.printf("[OLED] OK -> χρήση διεύθυνσης 0x%02X\n", found);
  }

  // 2) Init με software-I2C στη διεύθυνση που βρέθηκε
  oled.setI2CAddress(found << 1);   // U8g2 θέλει 8-bit μορφή (addr<<1)
  oled.begin();
  oled.setBusClock(400000);
  oledMsg("ESP32Home S3", "Levitostatio", "booting...");
}
// "12.3" ή "--" (NaN)
static void f1(char* b, size_t n, float v) { if (isnan(v)) snprintf(b,n,"--"); else snprintf(b,n,"%.1f",v); }

// Κύρια οθόνη κατάστασης (~1x/s από το loop)
void oledRender() {
  char top[24], l[24], a[8], b[8];
  oled.clearBuffer();

  // --- Κίτρινη ζώνη (πάνω): mode/setpoint ή ALARM ---
  oled.setFont(u8g2_font_7x14B_tf);
  if      (gCommsLost) snprintf(top, sizeof(top), "! NO LINK");
  else if (gAlarm)     snprintf(top, sizeof(top), "! %s", gStatus);
  else                 snprintf(top, sizeof(top), "%s set%.1f", gStatus, isnan(gSp)?0.0f:gSp);
  oled.drawStr(0, 13, top);

  // --- Μπλε ζώνη: λεπτομέρειες ---
  oled.setFont(u8g2_font_6x10_tf);
  f1(a,sizeof(a),gSupply); f1(b,sizeof(b),gTarget);
  snprintf(l,sizeof(l),"Sup%s>%s V%d%%", a, b, (int)(valvePosPct+0.5f));   oled.drawStr(0,27,l);

  f1(a,sizeof(a),gHotBuf); f1(b,sizeof(b),gCold);
  snprintf(l,sizeof(l),"BufH%s C%s P:%s", a, b, gPump?"ON":"OFF");          oled.drawStr(0,39,l);

  f1(a,sizeof(a),gOutRaw);
  snprintf(l,sizeof(l),"HP%uW Out%s [%c]", gHeatPumpW, a, gSeasonEff==SEASON_SUMMER?'S':'W'); oled.drawStr(0,51,l);

  if (gCommsLost) snprintf(l,sizeof(l),"LoRa: NO LINK");
  else {
    long age = lastCmdRx ? (long)((millis()-lastCmdRx)/1000) : -1;
    if (age < 0) snprintf(l,sizeof(l),"LoRa %ddBm --",  (int)lora.rssi());
    else         snprintf(l,sizeof(l),"LoRa %ddBm %lds", (int)lora.rssi(), age);
  }
  oled.drawStr(0,63,l);

  oled.sendBuffer();
}
#endif

#if CT_ENABLE
// Μετρά RMS ρεύμα από το SCT-013 (~200ms) -> Watt. Καλείται σπάνια (κάθε 5s) ώστε το μικρό
// «μπλοκάρισμα» να μην πειράζει τον αργό έλεγχο. analogReadMilliVolts = factory-calibrated ADC.
uint16_t readHeatPumpW() {
  double sum = 0, sumSq = 0; uint32_t n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 200) {        // ~10 περίοδοι @50Hz
    double mv = analogReadMilliVolts(CT_PIN);
    sum += mv; sumSq += mv * mv; n++;
  }
  if (n == 0) return 0;
  double mean = sum / n;                        // DC bias (το αφαιρούμε ΚΑΘΑΡΑ στο ίδιο παράθυρο)
  double var  = sumSq / n - mean * mean;        // διασπορά = μέση τιμή του (x-mean)²
  if (var < 0) var = 0;
  double vrms = sqrt(var) / 1000.0;             // Volt RMS (AC) στην έξοδο του CT
  double w = vrms * CT_A_PER_V * CT_CAL * MAINS_V;
  if (w < CT_NOISE_FLOOR) w = 0;        // κάτω από το κατώφλι θορύβου -> 0
  if (w > 65000)          w = 65000;
  return (uint16_t)(w + 0.5);
}
#endif

void homeValve();   // forward decl — συγχρονισμός θέσης βάνας στο live boot

void setup() {
  relaysInit();   // ΠΡΩΤΑ ΑΠ' ΟΛΑ -> σβήνει τα relay ΑΜΕΣΩΣ (ελαχιστοποιεί το «κλικ» στο boot)
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n[ΛΕΒΗΤΟΣΤΑΣΙΟ / ESP32-S3] εκκίνηση..."));

#if OLED_ENABLE
  oledInit();   // οθονάκι: δείχνει «booting...» — αν δεν ανάψει, έλεγξε καλωδίωση/pins 15-16
#endif

#if CT_ENABLE
  analogSetPinAttenuation(CT_PIN, ADC_11db);   // ~0..3.1V (το bias κάθεται στο 1.65V)
#endif

  // Θέση βάνας από NVS — η Seltron δεν κουνιέται χωρίς ρεύμα, άρα μετά από διακοπή
  // είναι ΑΚΡΙΒΩΣ εκεί που την αφήσαμε. Φορτώνουμε ώστε ο εγκέφαλος να ξέρει πού είναι.
  vprefs.begin("valve", false);
  valvePosPct   = vprefs.getFloat("pos", 0.0f);
  season        = vprefs.getUChar("season", SEASON_SUMMER);  // θυμήσου εποχή (default καλοκαίρι, ποτέ WINTER στο boot)
  savedSeasonNvs = season;
  houseSetpoint = vprefs.getFloat("sp", NAN);                // θυμήσου setpoint -> σε reboot+LoRa κάτω δεν πέφτει σε 18°
  savedSpNvs    = houseSetpoint;
  savedValvePos = valvePosPct;
  Serial.printf("[NVS] θέση βάνας από μνήμη: %.0f%%\n", valvePosPct);

  sensors.begin();
  sensors.setWaitForConversion(false);   // ασύγχρονη ανάγνωση — δεν μπλοκάρει 750ms τον βρόχο
  sensors.requestTemperatures();
  printSensorAddresses();

  int16_t st = lora.begin();
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("LoRa init ΑΠΕΤΥΧΕ: %d — σταματάω.\n", st);
    while (true) delay(1000);
  }
  Serial.println(F("LoRa OK — σε λήψη."));

  // Live boot: συγχρόνισε τη θέση της βάνας (κλείσε τέρμα -> 0%). Δες VALVE_HOME_ON_BOOT.
  if (!DRY_RUN && VALVE_HOME_ON_BOOT) homeValve();
}

// Στέλνει το τρέχον status (η ανάγνωση γίνεται ασύγχρονα στο loop)
void sendStatus() {
  BoilerStatus pkt = {};
  pkt.bufHotTop = encodeTemp(readSensor(IDX_HOTBUF));
  pkt.bufHotBot = encodeTemp(readSensor(IDX_OUTDOOR));   // = εξωτερική (αντιστάθμιση)
  pkt.bufCold   = encodeTemp(readSensor(IDX_COLD));
  pkt.valveTemp = encodeTemp(readSensor(IDX_SUPPLY));    // = προσαγωγή (έξοδος τρίοδης)
  pkt.pumpPower = gHeatPumpW;    // κατανάλωση αντλίας θερμότητας (SCT-013-030 @ GPIO5)
  pkt.relays    = (gOpen  ? RELAY_VALVE_OPEN  : 0) |    // πρόθεση ελεγκτή (dry-run/πραγματικό)
                  (gClose ? RELAY_VALVE_CLOSE : 0) |
                  (gPump  ? RELAY_PUMP        : 0);
  pkt.mode      = season;
  pkt.ctrlStatus = ctrlStatusCode();   // κατάσταση ελέγχου -> σπίτι/CYD/Telegram

  if (lora.send(pkt))
    Serial.printf("[TX] status εστάλη  (hotTop=%.2f hotBot=%.2f cold=%.2f valve=%.2f)\n",
                  decodeTemp(pkt.bufHotTop), decodeTemp(pkt.bufHotBot),
                  decodeTemp(pkt.bufCold), decodeTemp(pkt.valveTemp));
  else
    Serial.println(F("[TX] ΑΠΕΤΥΧΕ"));
}

// Κινεί τη βάνα κατά «open» (true) ή «close» (false) για ms χιλιοστά.
// Κρατά εκτίμηση θέσης (0-100%) ολοκληρώνοντας τον χρόνο, με προστασία end-stop.
// ΦΑΣΗ 1: μόνο εκτύπωση (DRY_RUN). ΦΑΣΗ 2: πραγματικό relay (μη-μπλοκαριστικός παλμός + interlock).
void valveMove(bool open, uint32_t ms, bool emergency) {
  // 3-σημείων με εσωτερικά limit switches -> ΧΩΡΙΣ early-return στα όρια: αν ο ελεγκτής ζητά
  // κι άλλο άνοιγμα/κλείσιμο ενώ η ΕΚΤΙΜΗΣΗ είναι ήδη 100%/0%, σπρώχνουμε προς το ΤΕΡΜΑ (re-reference).
  // Στο τέρμα το limit switch κόβει τον κινητήρα -> ακίνδυνο, και η εκτίμηση «ξανακολλάει» στην
  // πραγματική θέση -> διορθώνει drift από ανακρίβεια VALVE_TRAVEL_MS (π.χ. «0% ενώ φυσικά 50%»).
  if (open  && valvePosPct >= 100.0f) Serial.println(F("   [VALVE] στο ΤΕΡΜΑ ανοιχτό -> re-reference end-stop"));
  if (!open && valvePosPct <= 0.0f)   Serial.println(F("   [VALVE] στο ΤΕΡΜΑ κλειστό -> re-reference end-stop"));

  float delta = (float)ms / (float)VALVE_TRAVEL_MS * 100.0f;
  valvePosPct = constrain(valvePosPct + (open ? delta : -delta), 0.0f, 100.0f);
  lastValveAction = millis();
  lastOperation   = millis();   // για το anti-seize (κάτι κινήθηκε)
  gOpen = open; gClose = !open;

  if (DRY_RUN) {
    Serial.printf("   [VALVE-DRYRUN] %s %lums%s  ->  θέση~%.0f%%\n",
                  open ? "ΑΝΟΙΓΜΑ" : "ΚΛΕΙΣΙΜΟ", (unsigned long)ms,
                  emergency ? "  (ΕΠΕΙΓΟΝ)" : "", valvePosPct);
  } else {
    // ΦΑΣΗ 2: πραγματικό relay, μη-μπλοκαριστικό (το valveService() σβήνει στο τέλος).
    relayWrite(RELAY_OPEN_PIN,  false);   // INTERLOCK: σβήσε ΚΑΙ τα δύο πρώτα
    relayWrite(RELAY_CLOSE_PIN, false);
    relayWrite(open ? RELAY_OPEN_PIN : RELAY_CLOSE_PIN, true);
    valvePulseEnd = millis() + ms;
    Serial.printf("   [VALVE] %s %lums%s  ->  θέση~%.0f%%\n",
                  open ? "ΑΝΟΙΓΜΑ" : "ΚΛΕΙΣΙΜΟ", (unsigned long)ms,
                  emergency ? "  (ΕΠΕΙΓΟΝ)" : "", valvePosPct);
  }
}

#if SIM_MODE
// --- ΠΡΟΣΟΜΟΙΩΣΗ: εικονικοί αισθητήρες + θερμικό μοντέλο + ΣΕΝΑΡΙΟ demo ---
float simSupply = 22.0f, simHotBuf = 50.0f, simCold = 10.0f;
bool  simCoolMode = false;
unsigned long simT0 = 0;
// Αλλάζει συνθήκες με τον χρόνο για να δούμε ΟΛΑ τα features ζωντανά.
void simScenario(float& outdoor, float& room, uint8_t& seasonEff) {
  if (simT0 == 0) simT0 = millis();
  unsigned long t = (millis() - simT0) / 1000;
  if      (t < 25) { outdoor =  8; room = 19; seasonEff = SEASON_WINTER; }  // [Α] κανονική θέρμανση
  else if (t < 50) { outdoor = -8; room = 19; seasonEff = SEASON_WINTER; }  // [Β] ΨΥΧΡΟ ΜΕΤΩΠΟ -> damping ανεβάζει στόχο
  else if (t < 70) { outdoor =  0; room =  3; seasonEff = SEASON_WINTER; }  // [Γ] ΠΑΓΕΤΟΣ -> αντιπαγετική
  else if (t < 95) { outdoor = 30; room = 27; seasonEff = SEASON_SUMMER; }  // [Δ] ΚΑΛΟΚΑΙΡΙ ζέστη -> ΨΥΞΗ
  else             { outdoor = 24; room = 22; seasonEff = SEASON_SUMMER; }  // [Ε] ικανοποιήθηκε -> off + anti-seize
  simCoolMode = (seasonEff == SEASON_SUMMER);
}
void simStep() {
  float source    = simCoolMode ? simCold : simHotBuf;  // πηγή: κρύο buffer (ψύξη) ή ζεστό (θέρμανση)
  float ret       = 24.0f;                              // νερό επιστροφής ~24°C
  float potential = ret + (valvePosPct / 100.0f) * (source - ret);
  simSupply += (potential - simSupply) * 0.30f;         // υστέρηση 1ης τάξης
}
#endif

// --- Αντλία με anti-short-cycle (ελάχ. χρόνος ON/OFF) ---
void pumpControl(bool want) {
  // lastPumpChange==0 -> δεν έχει αλλάξει ποτέ (boot): επίτρεψε ΑΜΕΣΩΣ το πρώτο άναμμα.
  bool canChange = (lastPumpChange == 0) || (millis() - lastPumpChange >= PUMP_MIN_STATE_MS);
  if (want != gPumpState && canChange) {
    gPumpState = want; lastPumpChange = millis();
  }
  gPump = gPumpState;
  if (gPump) lastOperation = millis();
}
void forcePump(bool on) {            // για ασφάλειες/αντιπαγετική (άμεσα, χωρίς υστέρηση)
  gPumpState = on; lastPumpChange = millis(); gPump = on;
  if (on) lastOperation = millis();
}

// --- Anti-seize: περιοδικό «ξεμούδιασμα» αντλίας+βάνας όταν το σύστημα μένει αδρανές ---
void antiSeize() {
  if (exerciseStart == 0 && millis() - lastOperation > ANTI_SEIZE_INTERVAL) {
    exerciseStart = millis(); aseizeOpened = aseizeClosed = false;
    Serial.println(F("   [ANTI-SEIZE] ξεμούδιασμα αντλίας/βάνας"));
  }
  if (exerciseStart) {
    unsigned long t = millis() - exerciseStart;
    gPump = true; gPumpState = true;
    if (!aseizeOpened && t > 2000)                     { valveMove(true,  3000, false); aseizeOpened = true; }
    if (!aseizeClosed && t > ANTI_SEIZE_RUN_MS - 6000) { valveMove(false, 3000, false); aseizeClosed = true; }
    if (t >= ANTI_SEIZE_RUN_MS) {
      exerciseStart = 0; lastOperation = millis();
      gPump = false; gPumpState = false;
      Serial.println(F("   [ANTI-SEIZE] ολοκληρώθηκε"));
    }
  } else { gPump = false; gPumpState = false; }
}

// Ο «εγκέφαλος»: κάθε CONTROL_INTERVAL. ΘΕΡΜΑΝΣΗ (χειμώνας/αντιπαγετική) ή ΨΥΞΗ (καλοκαίρι),
// με αντιστάθμιση, εξομάλυνση εξωτερικής, anti-seize, anti-short-cycle, ασφάλειες.
// Η ίδια τρίοδη: «ΑΝΟΙΓΜΑ» = περισσότερη πηγή (ζεστό buffer τον χειμώνα, κρύο το καλοκαίρι).
void controlTick() {
  gOpen = false; gClose = false;

#if SIM_MODE
  uint8_t seasonEff; float outdoor, room;
  simScenario(outdoor, room, seasonEff);
  simStep();
  float supply = simSupply, hotBuf = simHotBuf, cold = simCold;
#else
  uint8_t seasonEff = season;
  float supply  = readSensor(IDX_SUPPLY);
  float outdoor = readSensor(IDX_OUTDOOR);
  float hotBuf  = readSensor(IDX_HOTBUF);
  float cold    = readSensor(IDX_COLD);
  float room    = houseTemp;
#endif
  // Στο commsLost ΚΡΑΤΑΜΕ το τελευταίο γνωστό setpoint (persisted) — ΟΧΙ override σε 18°!
  // (Το 18° το καλοκαίρι υπερψύχει + φέρνει την προσαγωγή πιο κοντά στη δρόσο. Το τελευταίο setpoint
  //  του χρήστη είναι ασφαλές. Default αν ποτέ δεν λήφθηκε: καλοκαίρι 26°, χειμώνας FAILSAFE_SETPOINT.)
  float sp = isnan(houseSetpoint) ? (season == SEASON_SUMMER ? 26.0f : FAILSAFE_SETPOINT) : houseSetpoint;
  bool commsLost = (millis() - lastCmdRx > CMD_TIMEOUT);

  // Στιγμιότυπο για το OLED (ισχύει & σε SIM & σε πραγματικό)
  gSupply=supply; gOutRaw=outdoor; gHotBuf=hotBuf; gCold=cold; gRoom=room;
  gSeasonEff=seasonEff; gCommsLost=commsLost; gSp=sp;

  // Εξομάλυνση εξωτερικής (EMA) — αγνοούμε στιγμιαία spikes (ήλιος/ριπές αέρα)
  if (!isnan(outdoor)) {
    if (isnan(outdoorFilt)) outdoorFilt = outdoor;
    else outdoorFilt += (outdoor - outdoorFilt) * OUTDOOR_ALPHA;
  }

  Serial.printf("[CTRL] %s sp=%.1f room=%s outRaw=%s out~=%s hot=%s cold=%s supply=%s hp=%s%s\n",
                seasonEff == SEASON_SUMMER ? "ΚΑΛΟΚΑΙΡΙ" : "ΧΕΙΜΩΝΑΣ", sp,
                isnan(room)?"--":String(room,1).c_str(),
                isnan(outdoor)?"--":String(outdoor,1).c_str(),
                isnan(outdoorFilt)?"--":String(outdoorFilt,1).c_str(),
                isnan(hotBuf)?"--":String(hotBuf,1).c_str(),
                isnan(cold)?"--":String(cold,1).c_str(),
                isnan(supply)?"--":String(supply,1).c_str(),
                gHpEnabled ? "ON" : "OFF",
                commsLost?"  [COMMS LOST->κρατά setpoint]":"");

  // --- ΑΣΦΑΛΕΙΑ 1: σφάλμα αισθητήρα προσαγωγής (debounce: αγνοεί στιγμιαία glitch) ---
  if (isnan(supply)) {
    if (++supplyNanCount >= SUPPLY_NAN_LIMIT) {
      forcePump(false);
      setStatus("SENSOR!", true);
      Serial.printf("   [SAFETY] λείπει προσαγωγή x%u -> pump OFF + βάνα ΚΛΕΙΣΙΜΟ\n", supplyNanCount);
      valveMove(false, PULSE_MAX_MS, true);
    } else {
      Serial.printf("   [WARN] προσαγωγή nan (%u/%u) — στιγμιαίο; κρατάω κατάσταση\n", supplyNanCount, SUPPLY_NAN_LIMIT);
    }
    return;   // με άκυρη προσαγωγή δεν ελέγχουμε (ούτε στιγμιαία ούτε μόνιμα)
  }
  supplyNanCount = 0;   // έγκυρη ένδειξη -> μηδένισε τον μετρητή (μόνο ΣΥΝΕΧΟΜΕΝΑ μετράνε)

  bool  frost = (!isnan(room) && room < FROST_ROOM_C) || (supply < FROST_SUPPLY_C);
  float target = NAN, errToOpen = 0.0f;
  bool  cooling = false;

  if (frost) {
    // --- ΑΝΤΙΠΑΓΕΤΙΚΗ (υπερισχύει εποχής/ζήτησης) ---
    setStatus("FROST", true);
    Serial.println(F("   [FROST] ΑΝΤΙΠΑΓΕΤΙΚΗ -> αναγκαστική θέρμανση"));
    target = FROST_TARGET;
    forcePump(true);
    errToOpen = target - supply;
  }
  else if (gSystemOff) {
    // --- MASTER OFF (/off): σταματά τα πάντα· η αντιπαγετική παραπάνω ΥΠΕΡΙΣΧΥΕΙ ---
    gTarget = NAN;
    gCoolI = 0.0f; gCoolActive = false;   // καθάρισε τον έλεγχο ψύξης (καθαρή επανεκκίνηση στο /on)
    forcePump(false);             // κυκλοφορητής OFF
    setStatus("SYS OFF", false);  // βάνα μένει ως έχει (καμία κίνηση)
    Serial.println(F("   [OFF] σύστημα σβηστό (/off) -> pump OFF, βάνα ως έχει"));
    antiSeize();                  // anti-seize παραμένει (να μην κολλήσει σε μακρύ OFF)
    return;
  }
  else if (seasonEff == SEASON_WINTER) {
    // --- ΘΕΡΜΑΝΣΗ (αντιστάθμιση) ---
    if (supply > SUPPLY_MAX) {
      setStatus("MAX 40!", true);
      Serial.printf("   [SAFETY] προσαγωγή %.1f > MAX %.1f -> ΕΠΕΙΓΟΝ ΚΛΕΙΣΙΜΟ\n", supply, SUPPLY_MAX);
      forcePump(true); valveMove(false, PULSE_MAX_MS, true); return;
    }
    if (isnan(outdoorFilt)) target = sp + 12.0f;
    else                    target = sp + CURVE_SLOPE * (sp - outdoorFilt);
    if (!isnan(room))       target += ROOM_TRIM_GAIN * (sp - room);
    target = constrain(target, SUPPLY_MIN, SUPPLY_MAX - DEADBAND);
    bool heatAvail = !isnan(hotBuf) && hotBuf > target + BUFFER_MARGIN;
    bool demand    = isnan(room) ? true : (room < sp + ROOM_HYST);
    if (!demand || !heatAvail) {
      gTarget = NAN;
      setStatus(heatAvail ? "OFF" : "WAIT HOT", false);   // διακρίνει «ικανοποιήθηκε» από «περιμένει ζεστό»
      Serial.println(!heatAvail ? F("   (χωρίς διαθέσιμη ζέστη) -> OFF") : F("   ικανοποιήθηκε -> OFF"));
      antiSeize();
      return;
    }
    setStatus("HEAT", false);
    pumpControl(true);
    errToOpen = target - supply;            // ανοίγω για να ΑΝΕΒΕΙ η προσαγωγή
  }
  else {
    // --- ΨΥΞΗ / ΔΡΟΣΙΣΜΟΣ (καλοκαίρι) ---
    cooling = true;
    // Όριο ψύξης (αντι-συμπύκνωση):
    //  • μόνο fancoil -> 13°C (το fancoil αποστραγγίζει)
    //  • ενδοδαπέδιο ενεργό -> ΔΥΝΑΜΙΚΑ: σημείο δρόσου + περιθώριο (αν ξέρουμε υγρασία)
    //  • fallback (χωρίς dew point) -> σταθερό 18°C
    float coolMin;
    if (gFancoilOnly)           coolMin = COOL_SUPPLY_MIN_FANCOIL;
    else if (!isnan(gDewPoint)) coolMin = constrain(gDewPoint + DEWPOINT_MARGIN,
                                                    COOL_SUPPLY_MIN_FLOOR_ABS, COOL_SUPPLY_MAX - DEADBAND);
    else                        coolMin = COOL_SUPPLY_MIN_FLOOR;
    gCoolMin = coolMin;                     // -> heatPumpControl (δυναμικά κατώφλια buffer)
    Serial.printf("   [ΨΥΞΗ] dewPoint=%s -> coolMin=%.1f\n",
                  isnan(gDewPoint) ? "--" : String(gDewPoint, 1).c_str(), coolMin);
    if (supply < coolMin) {                 // σκληρό όριο: πολύ κρύα -> ΚΙΝΔΥΝΟΣ ΣΥΜΠΥΚΝΩΣΗΣ
      setStatus("COND!", true);
      Serial.printf("   [SAFETY] προσαγωγή %.1f < MIN ψύξης %.1f (%s) -> ΕΠΕΙΓΟΝ ΚΛΕΙΣΙΜΟ\n",
                    supply, coolMin, gFancoilOnly ? "fancoil" : "+δάπεδο");
      forcePump(true); valveMove(false, PULSE_MAX_MS, true); return;
    }
    float over = isnan(room) ? 0.0f : (room - sp);    // πόσο πάνω από setpoint
    // Stateful υστέρηση: ΚΛΕΙΣΕ όταν πιάσει τον στόχο (room<=sp), ΞΑΝΑΖΗΤΑ στο sp+ROOM_HYST.
    //  -> ο κυκλοφορητής ΞΕΚΟΥΡΑΖΕΤΑΙ στον στόχο αντί να τρέχει αέναα ~0.3° πιο πάνω.
    if (!isnan(room)) {
      if      (room <= sp)             gCoolActive = false;   // έφτασε -> ικανοποιήθηκε
      else if (room > sp + ROOM_HYST)  gCoolActive = true;    // ανέβηκε -> ξανα-ζήτα
    }
    // Ολοκληρωτικός όρος (I): μαζεύει το μικρό σφάλμα ώσπου να ΠΙΑΣΕΙ τον στόχο (σβήνει το droop).
    //  Anti-windup: συσσωρεύει μόνο όσο room>sp ΚΑΙ υπάρχει περιθώριο πριν το όριο δρόσου.
    float lowLim  = coolMin + DEADBAND;
    float pTarget = COOL_SUPPLY_MAX - COOL_ROOM_GAIN * over;
    if (gCoolActive && over > 0.0f && (pTarget - gCoolI) > lowLim) gCoolI += COOL_KI * over;
    else if (!gCoolActive)                                         gCoolI  = 0.0f;   // reset στον στόχο
    gCoolI = constrain(gCoolI, 0.0f, COOL_SUPPLY_MAX - lowLim);
    target = constrain(pTarget - gCoolI, lowLim, COOL_SUPPLY_MAX);
    bool coldAvail  = !isnan(cold) && cold < target - BUFFER_MARGIN;
    bool coolDemand = isnan(room) ? false : gCoolActive;
    if (!coolDemand || !coldAvail) {
      gCoolI = 0.0f;                          // καθάρισε τον I όταν δεν ψύχουμε
      gTarget = NAN;
      setStatus(coldAvail ? "OFF" : "WAIT COLD", false);  // διακρίνει «ικανοποιήθηκε» από «περιμένει κρύο»
      Serial.println(!coldAvail ? F("   (χωρίς διαθέσιμο κρύο) -> OFF") : F("   ικανοποιήθηκε -> OFF"));
      antiSeize();
      return;
    }
    setStatus("COOL", false);
    pumpControl(true);
    Serial.printf("   [ΨΥΞΗ] over=%.2f I=%.2f lowLim=%.1f\n", over, gCoolI, lowLim);
    errToOpen = supply - target;            // ανοίγω (περισσότερο κρύο) για να ΠΕΣΕΙ η προσαγωγή
  }
  gTarget = target;

  Serial.printf("   %s στόχος=%.1f προσαγωγή=%.1f θέση~%.0f%% pump=%s\n",
                cooling ? "[ΨΥΞΗ]" : "[ΘΕΡΜ]", target, supply, valvePosPct, gPump ? "ON" : "OFF");

  // --- Έλεγχος βάνας: settle -> deadband -> αναλογικός παλμός ---
  if (millis() - lastValveAction < SETTLE_MS) {
    Serial.printf("   [VALVE] settle (%lus)\n",
                  (unsigned long)((SETTLE_MS - (millis() - lastValveAction)) / 1000));
    return;
  }
  if (fabs(errToOpen) <= DEADBAND) { Serial.println(F("   [VALVE] deadband -> HOLD")); return; }
  uint32_t pulse = constrain((uint32_t)(fabs(errToOpen) * PULSE_PER_DEG), PULSE_MIN_MS, PULSE_MAX_MS);
  valveMove(errToOpen > 0, pulse, false);   // >0 -> ΑΝΟΙΓΜΑ (περισσότερη πηγή)
}

// Έλεγχος αντλίας θερμότητας μέσω K4 (RELAY_SPARE) -> ακροδέκτες 7-8 «Remote On/Off».
// FAILSAFE NC: K4 ΧΩΡΙΣ ρεύμα = 7-8 ΚΛΕΙΣΤΟ = αντλία ENABLED (αν πέσει ο S3 -> νερό για όλους).
//   gHpEnabled=true  -> K4 de-energized -> 7-8 κλειστό -> enabled
//   gHpEnabled=false -> K4 energized     -> 7-8 ανοιχτό -> disabled (η αντιπαγετική της αντλίας ΜΕΝΕΙ)
// Buffer-based -> εξυπηρετεί ΟΛΑ τα σπίτια. Failsafe (enable ΑΜΕΣΑ) σε: /off, χειμώνα, commsLost, σφάλμα αισθ.
void heatPumpControl() {
  static bool firstRun = true;
  // ΣΗΜΑΝΤΙΚΟ: το commsLost ΔΕΝ μπαίνει στο failsafe! Ο buffer thermostat δουλεύει με τους
  // ΤΟΠΙΚΟΥΣ αισθητήρες (gCold καλοκαίρι / gHotBuf χειμώνα) — δεν χρειάζεται καθόλου το LoRa.
  // Αν χανόταν το LoRa και αναγκάζαμε ENABLE, το Crono-TH έτρεχε ελεύθερο (η παλιά 24/7 βλάβη).
  // Αν ΠΕΘΑΝΕΙ ο S3 (όχι απλώς το LoRa) -> hardware NC failsafe (K4 χωρίς ρεύμα = 7-8 κλειστό) καλύπτει.
  bool  summer = (season == SEASON_SUMMER);
  float buf    = summer ? gCold : gHotBuf;   // ΚΑΛΟΚΑΙΡΙ: κρύο buffer · ΧΕΙΜΩΝΑΣ: ζεστό buffer

  // Failsafe -> ENABLE άμεσα (χωρίς min-time): master OFF ή σφάλμα αισθητήρα του τρέχοντος buffer.
  bool failsafe = gSystemOff || isnan(buf);

  if (failsafe) {
    // Παράδοση ελέγχου / ασφάλεια -> ENABLE άμεσα (χωρίς min-time)
    if (!gHpEnabled) {
      gHpEnabled = true; gHpLastChange = millis(); firstRun = false;
      Serial.println(F("[HP] FAILSAFE -> ENABLE (7-8 κλειστό, αντλία ελεύθερη)"));
    }
  } else {
    // Θερμοστάτης buffer με υστέρηση + anti-short-cycle:
    //  ΚΑΛΟΚΑΙΡΙ (ψύξη):  buf ΨΗΛΑ -> enable (θέλει ψύξη),   buf ΧΑΜΗΛΑ -> disable
    //  ΧΕΙΜΩΝΑΣ (θέρμ.):  buf ΧΑΜΗΛΑ -> enable (θέλει ζέστα), buf ΨΗΛΑ -> disable
    //   => SOLAR PRIORITY: όταν τα 6 ηλιακά ζεστάνουν το buffer πάνω από HP_HOT_HIGH, η αντλία
    //      ΣΒΗΝΕΙ κι αφήνει τον ήλιο (δωρεάν) — και δεν παλεύει με ζεστό νερό (τέλος τα SUrr/κολλήματα).
    bool want;
    float loTh, hiTh;
    if (summer) {
      // ΔΥΝΑΜΙΚΑ κατώφλια — ακολουθούν τον στόχο ψύξης (δρόσος/ρύθμιση), όπως και η προσαγωγή:
      //   bGood = πόσο κρύο ΠΡΕΠΕΙ να είναι το buffer ώστε ο S3 να μπορεί να μιξάρει (coldAvail).
      //   Υγρή μέρα (coolMin ~20°) -> bGood ~18° -> επιτρέπει ΖΕΣΤΟ buffer -> η αντλία ΞΕΚΟΥΡΑΖΕΤΑΙ.
      //   Ξηρή μέρα (coolMin 16°)  -> bGood ~14° -> απαιτεί ΚΡΥΟ buffer -> δουλεύει (σωστά, θες ψύξη).
      if (!isnan(gCoolMin)) {
        float bGood = gCoolMin - BUFFER_MARGIN;
        loTh = bGood - 2.0f;                        // αρκετά κρύο -> DISABLE (ξεκούραση)
        hiTh = bGood - 0.5f;                        // ζεσταίνεται -> ENABLE
      } else {
        loTh = HP_COLD_LOW; hiTh = HP_COLD_HIGH;    // fallback (χωρίς δεδομένα ψύξης)
      }
      // ΚΛΕΙΔΙ (2026-07-24): ΜΗ ΣΒΗΝΕΙΣ όσο το σπίτι μου ΘΕΛΕΙ ψύξη (gCoolActive) -> ΑΠΟΔΟΣΗ!
      //  Η ξεκούραση αντλίας ΔΕΝ πρέπει να διακόπτει τον κυκλοφορητή/την ψύξη. Ξεκούραση ΜΟΝΟ όταν
      //  το σπίτι ικανοποιήθηκε ΚΑΙ το buffer είναι κρύο (κανείς δεν τραβάει). Έτσι: υψηλή ζήτηση ->
      //  συνεχής ΗΠΙΑ λειτουργία (ο inverter modulάρει, όχι ριπές)· ικανοποίηση -> πραγματική ξεκούραση.
      if      (gCoolActive || buf >= hiTh) want = true;    // ζήτηση (σπίτι μου Ή buffer ζεστό) -> ON
      else if (buf <= loTh)                want = false;   // ικανοποιημένο + κρύο buffer -> ΞΕΚΟΥΡΑΣΗ
      else                                 want = gHpEnabled;
    } else {
      // ΧΕΙΜΩΝΑΣ = ΚΑΘΑΡΑ buffer-driven (ΟΧΙ demand-gated όπως το gCoolActive του καλοκαιριού!).
      //  ΓΙΑΤΙ: τη ζέστη την κάνουν ΚΑΙ ο ήλιος ΚΑΙ η αντλία. Αν έβαζα «gHeatActive -> ON», η αντλία
      //  θα έτρεχε ΚΟΝΤΡΑ σε ζεστό solar buffer (σπατάλη + SUrr overheating). Το buffer κρατιέται 47-53°
      //  για το ΝΤΟΥΣ (DHW), που είναι πολύ πάνω από τους ~37° της θέρμανσης -> το σπίτι ΠΟΤΕ δεν πεινάει
      //  ακόμα κι όταν η αντλία ξεκουράζεται. Το σήμα «χρειάζεται ζέστη» ΕΙΝΑΙ η ίδια η θερμοκρασία buffer.
      //  ⚠️ ΦΘΙΝΟΠΩΡΟ TODO (θέλει δεδομένα): κούρδισμα HP_HOT_LOW/HIGH με άνεση ντους + par 215/216 αντλίας.
      loTh = HP_HOT_LOW; hiTh = HP_HOT_HIGH;
      if      (buf >= HP_HOT_HIGH)  want = false;   // αρκετά ζεστό (ήλιος/αντλία) -> σβήσε (SOLAR PRIORITY)
      else if (buf <= HP_HOT_LOW)   want = true;    // κρύο (ντους/θέρμανση το τράβηξε) -> άναψε
      else                          want = gHpEnabled;
    }
    // ON->OFF: περίμενε min-ON (μη σβήσεις πρόωρα)· OFF->ON: περίμενε min-OFF (γρήγορη επαναφορά)
    uint32_t minTime = gHpEnabled ? HP_MIN_ON_MS : HP_MIN_OFF_MS;
    if (want != gHpEnabled && (firstRun || millis() - gHpLastChange >= minTime)) {
      gHpEnabled = want; gHpLastChange = millis(); firstRun = false;
      Serial.printf("[HP] %s %s (buffer=%.1f° | κατώφλια off<=%.1f / on>=%.1f)\n",
                    summer ? "ΨΥΞΗ" : "ΘΕΡΜ",
                    gHpEnabled ? "ENABLE 7-8 κλειστό -> αντλία ΟΝ"
                               : "DISABLE 7-8 ανοιχτό -> αντλία OFF", buf, loTh, hiTh);
    }
  }

  // Οδήγηση K4 (failsafe NC): enabled -> de-energized (false)· disabled -> energized (true)
  if (!DRY_RUN) relayWrite(RELAY_SPARE_PIN, !gHpEnabled);
}

// Μη-μπλοκαριστικό τέλος παλμού βάνας: σβήνει τα relay όταν περάσει ο χρόνος.
void valveService() {
  if (valvePulseEnd && millis() >= valvePulseEnd) {
    relayWrite(RELAY_OPEN_PIN,  false);
    relayWrite(RELAY_CLOSE_PIN, false);
    valvePulseEnd = 0;
  }
}

// ---- HOMING: συγχρονισμός θέσης βάνας στο live boot ----
//  Οδηγεί ΚΛΕΙΣΙΜΟ μέχρι το τέρμα -> γνωστό 0%. Η Seltron 3-σημείων δεν έχει αισθητήρα θέσης·
//  τα εσωτερικά τερματικά σταματούν τον κινητήρα στο άκρο, άρα είναι ασφαλές να μείνει το relay
//  ON λίγο παραπάνω (περιθώριο). Καλείται ΜΟΝΟ όταν DRY_RUN=false && VALVE_HOME_ON_BOOT.
void homeValve() {
  Serial.println(F("\n[HOMING] συγχρονισμός βάνας -> ΚΛΕΙΣΙΜΟ μέχρι τέρμα..."));
#if OLED_ENABLE
  oledMsg("HOMING", "valve -> CLOSE", "sync to 0%");
#endif
  relayWrite(RELAY_OPEN_PIN,  false);   // interlock: ποτέ ΚΑΙ τα δύο
  relayWrite(RELAY_CLOSE_PIN, true);    // ΚΛΕΙΣΙΜΟ
  uint32_t homeMs = VALVE_TRAVEL_MS + VALVE_TRAVEL_MS / 6;   // +~17% περιθώριο για σίγουρο τέρμα
  unsigned long t0 = millis();
  while (millis() - t0 < homeMs) {
    delay(1000);
    unsigned long s = (millis() - t0) / 1000;
    if (s % 5 == 0) Serial.printf("   [HOMING] κλείσιμο... %lus / %lus\n", s, (unsigned long)(homeMs / 1000));
  }
  relayWrite(RELAY_CLOSE_PIN, false);
  valvePosPct = savedValvePos = 0.0f;
  vprefs.putFloat("pos", 0.0f);
  Serial.println(F("[HOMING] OK -> βάνα ΚΛΕΙΣΤΗ, θέση = 0% συγχρονισμένη"));
#if OLED_ENABLE
  oledMsg("HOMING OK", "valve = 0%", "control start");
#endif
}

#if CALIBRATE_VALVE
// ---- ΒΑΘΜΟΝΟΜΗΣΗ ΒΑΝΑΣ — μέτρηση χρόνου πλήρους διαδρομής (Seltron AVC05) ----
//  Τρέχει ΜΙΑ ΦΟΡΑ: κλείνει τέρμα (αναφορά 0%), μετά ανοίγει με χρονόμετρο.
//  ΚΟΙΤΑ τον δείκτη θέσης της βάνας: μόλις ΣΤΑΜΑΤΗΣΕΙ να κινείται (end-stop), διάβασε τα s.
//  Ο κινητήρας 3-σημείων έχει εσωτερικά τερματικά -> ασφαλές να μείνει το relay ON στο τέρμα.
void calibrateValve() {
  Serial.println(F("\n========== ΒΑΘΜΟΝΟΜΗΣΗ ΒΑΝΑΣ (Seltron AVC05) =========="));
  Serial.println(F("ΠΡΟΣΟΧΗ: 230V στα relay + κινητήρας συνδεμένος. Κοίτα τον ΔΕΙΚΤΗ ΘΕΣΗΣ της βάνας."));
  Serial.println(F("Ξεκινά σε 3s..."));
#if OLED_ENABLE
  oledMsg("CALIBRATE", "Seltron AVC05", "starting 3s...");
#endif
  relayWrite(RELAY_OPEN_PIN, false); relayWrite(RELAY_CLOSE_PIN, false);
  delay(3000);

  // [1/2] Πλήρες ΚΛΕΙΣΙΜΟ -> σταθερό σημείο αναφοράς (0%). 150s με περιθώριο.
  Serial.println(F("\n[1/2] ΚΛΕΙΣΙΜΟ μέχρι τέρμα (150s — σταματά μόνο του στο end-stop)..."));
  relayWrite(RELAY_CLOSE_PIN, true);
  for (int s = 1; s <= 150; s++) {
    delay(1000);
    if (s % 5 == 0) Serial.printf("   κλείσιμο... %d s\n", s);
#if OLED_ENABLE
    char buf[20]; snprintf(buf, sizeof(buf), "CLOSING %ds/150", s);
    oledMsg("CALIBRATE", buf, "-> ref 0%");
#endif
  }
  relayWrite(RELAY_CLOSE_PIN, false);
  Serial.println(F("   -> ΤΕΛΕΙΩΣ ΚΛΕΙΣΤΗ (0%). Σημείωσε τη θέση του δείκτη. Παύση 4s."));
#if OLED_ENABLE
  oledMsg("CALIBRATE", "CLOSED (0%)", "open in 4s...");
#endif
  delay(4000);

  // [2/2] ΑΝΟΙΓΜΑ με ζωντανό χρονόμετρο -> διάβασε πότε σταματά ο δείκτης.
  Serial.println(F("\n[2/2] ΑΝΟΙΓΜΑ ΤΩΡΑ — ΚΟΙΤΑ ΤΟΝ ΔΕΙΚΤΗ! Σε πόσα s φτάνει στο ΤΕΡΜΑ ΑΝΟΙΧΤΟ;"));
  relayWrite(RELAY_OPEN_PIN, true);
  for (int s = 1; s <= 160; s++) {
    delay(1000);
    Serial.printf("   ΑΝΟΙΓΜΑ: %d s\n", s);
#if OLED_ENABLE
    char buf[20]; snprintf(buf, sizeof(buf), "OPENING  %d s", s);
    oledMsg("CALIBRATE", buf, "watch valve!");
#endif
  }
  relayWrite(RELAY_OPEN_PIN, false);
#if OLED_ENABLE
  oledMsg("CALIB DONE", "read serial:", "set TRAVEL_MS");
#endif

  Serial.println(F("\n========== ΟΛΟΚΛΗΡΩΘΗΚΕ =========="));
  Serial.println(F("1) Ο χρόνος που σταμάτησε ο δείκτης = VALVE_TRAVEL_MS (π.χ. 120 s -> 120000)."));
  Serial.println(F("2) Επιβεβαίωσε: ΑΝΟΙΓΜΑ = περισσότερο ΖΕΣΤΟ (χειμώνας) / ΚΡΥΟ (καλοκαίρι)."));
  Serial.println(F("   Αν γύρισε ανάποδα -> αντάλλαξε τα καλώδια open<->close στα relay 1/2."));
  Serial.println(F("3) Βάλε CALIBRATE_VALVE=false, γράψε τον χρόνο, ξανα-flash."));
  while (true) delay(1000);   // τέλος — μένει εδώ
}
#endif

void loop() {
#if CALIBRATE_VALVE
  calibrateValve();  // ΜΟΝΟ βαθμονόμηση βάνας — τίποτα άλλο δεν τρέχει
  return;
#endif
#if RELAY_SELFTEST
  relaySelfTest();   // ΜΟΝΟ έλεγχος relay (κυκλικά) — τίποτα άλλο δεν τρέχει
  return;
#endif

  // Λήψη εντολής από το σπίτι
  HouseCmd cmd;
  if (lora.receive(cmd)) {
    houseSetpoint = decodeTemp(cmd.setpoint);
    houseTemp     = decodeTemp(cmd.houseTemp);
    season        = cmd.season;
    if (season != savedSeasonNvs) { savedSeasonNvs = season; vprefs.putUChar("season", season); }  // persist εποχή στο NVS
    if (!isnan(houseSetpoint) && houseSetpoint != savedSpNvs) { savedSpNvs = houseSetpoint; vprefs.putFloat("sp", houseSetpoint); }  // persist setpoint
    gFancoilOnly  = (cmd.flags & CMD_FLAG_FANCOIL_ONLY);
    gDewPoint     = decodeTemp(cmd.dewPoint);   // σημείο δρόσου (για δυναμικό όριο ψύξης)
    gSystemOff    = (cmd.flags & CMD_FLAG_SYSTEM_OFF);   // master OFF από /off
    bool homeFlag = (cmd.flags & CMD_FLAG_HOME);         // re-home βάνας on-demand (/home)
    if (homeFlag && !gHomeArmed) { gHomeReq = true; gHomeArmed = true; }  // rising-edge -> μία εκτέλεση
    if (!homeFlag) gHomeArmed = false;                    // οπλίζει ξανά όταν λήξει το αίτημα
    lastCmdRx     = millis();
    Serial.printf("[RX #%u] εντολή σπιτιού: setpoint=%.2f°C σπίτι=%.2f°C εποχή=%s  (RSSI %.0f dBm, SNR %.1f dB)\n",
                  cmd.seq, houseSetpoint, houseTemp,
                  season == SEASON_SUMMER ? "καλοκαίρι" : "χειμώνας", lora.rssi(), lora.snr());
  }

  // LoRa AUTO-RECOVERY: αν το radio κρασάρει (TX ΑΠΕΤΥΧΕ + καθόλου RX), re-init το radio
  // ΧΩΡΙΣ reboot του ESP32 (ο τοπικός έλεγχος συνεχίζει). Λύνει το «κολλάει & δεν επανέρχεται».
  if (millis() - lastCmdRx > LORA_REINIT_MS && millis() - lastLoraReinit > LORA_REINIT_MS) {
    lastLoraReinit = millis();
    Serial.println(F("[LoRa] χωρίς RX 60s -> re-init radio (auto-recovery)"));
    int16_t st = lora.begin();
    Serial.printf("[LoRa] re-init -> %s\n", st == 0 ? "OK" : String(st).c_str());
  }

  // Re-home βάνας on-demand (/home): οδηγεί ΚΛΕΙΣΙΜΟ στο τέρμα & μηδενίζει τη θέση.
  // Μπλοκάρει ~2 λεπτά (όπως το boot-home) — εκτελείται μία φορά ανά αίτημα.
  if (gHomeReq) {
    gHomeReq = false;
    if (!DRY_RUN) { Serial.println(F("[HOME] re-home βάνας (από /home)")); homeValve(); }
    else            Serial.println(F("[HOME] (DRY_RUN) θα γινόταν re-home βάνας"));
  }

  // Ασύγχρονη ανανέωση θερμοκρασιών (δεν μπλοκάρει)
  if (millis() - lastTempReq >= TEMP_REQ_INTERVAL) {
    lastTempReq = millis();
    sensors.requestTemperatures();
  }

  // Έξυπνος έλεγχος τρίοδης + αντλίας
  if (millis() - lastControl >= CONTROL_INTERVAL) {
    lastControl = millis();
    controlTick();
    heatPumpControl();   // takeover αντλίας (K4->7-8, cold buffer thermostat — ανεξάρτητο από έλεγχο σπιτιού)
  }

  // ΦΑΣΗ 2: οδήγηση πραγματικών relays (μόνο όταν DRY_RUN=false)
  if (!DRY_RUN) {
    relayWrite(RELAY_PUMP_PIN, gPump);
    valveService();
  }

  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();
    sendStatus();
  }

#if CT_ENABLE
  if (millis() - lastCt >= 5000) {
    lastCt = millis(); gHeatPumpW = readHeatPumpW();
    Serial.printf("[CT] αντλία θερμότητας: %u W\n", gHeatPumpW);
  }
#endif

  // Αποθήκευση θέσης βάνας στο NVS (επιβιώνει διακοπή): write-on-change ≥1%, max 1×/30s
  if (fabs(valvePosPct - savedValvePos) >= 1.0f && millis() - lastValveSave >= 30000) {
    lastValveSave = millis();
    savedValvePos = valvePosPct;
    vprefs.putFloat("pos", valvePosPct);
  }

#if OLED_ENABLE
  if (millis() - lastOled >= 1000) { lastOled = millis(); oledRender(); }
#endif
}
