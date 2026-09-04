// ============================================================================
//  ΣΠΙΤΙ / HUB — ESP32 dev + Core1121-XF (LR1121)
// ----------------------------------------------------------------------------
//  ΡΟΛΟΣ: ο "κόμβος" του σπιτιού. Λαμβάνει BoilerStatus από τον S3 (LoRa),
//         στέλνει HouseCmd (setpoint + εποχή), και επιπλέον:
//           * WiFi
//           * Telegram bot (ειδοποιήσεις + /status, /set, /winter, /summer)
//           * Τοπική web σελίδα (προβολή + αλλαγή από browser στο σπίτι)
//         (Αργότερα: γέφυρα UART προς το CYD.)
//
//  Διαπιστευτήρια στο include/secrets.h (ιδιωτικό). LoRa στη βιβλιοθήκη HeatLink.
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <WebServer.h>
#include <HTTPClient.h>    // Google Sheets logging (HTTPS GET προς Apps Script)
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HeatLink.h>
#include <UartLink.h>
#include <Preferences.h>
#include <time.h>          // NTP: ώρα/ημερομηνία για ημερήσιο/μηνιαίο κόστος
#include <Wire.h>          // I2C για τον αισθητήρα SHT40 (θερμοκρασία + υγρασία δωματίου)
#include "secrets.h"

// ---- SHT40 (Sensirion) θερμοκρασία+υγρασία δωματίου, I2C ----
#define SHT40_ADDR 0x44
#define SHT_SDA    21
#define SHT_SCL    22

// ---- Pins LoRa (Core1121-XF στο ESP32 dev) ----
#define LORA_CS    5
#define LORA_DIO9  26
#define LORA_RST   14
#define LORA_BUSY  27
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define ONE_WIRE_BUS 4

// ---- UART γέφυρα προς το CYD (Serial2) ----
//  4 καλώδια: 5V(CYD)->VIN(εδώ), GND κοινό, και ΣΤΑΥΡΩΤΑ τα data:
//    CYD_RX(16) <- TX της οθόνης   |   CYD_TX(17) -> RX της οθόνης
#define CYD_RX   16
#define CYD_TX   17
#define CYD_BAUD 38400
#define HUB_LED  2          // onboard LED -> ανάβει όταν δουλεύει ο κυκλοφορητής

HeatLink lora(LORA_CS, LORA_DIO9, LORA_RST, LORA_BUSY, LORA_SCK, LORA_MISO, LORA_MOSI);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

WiFiClientSecure secured;
UniversalTelegramBot bot(BOT_TOKEN, secured);
WebServer web(80);
Preferences prefs;                 // μόνιμη μνήμη (NVS) για setpoint/εποχή

// ---- Κατάσταση ----
float   desiredSetpoint = 22.5;
uint8_t season          = SEASON_WINTER;
bool    fancoilOnly     = false;   // ψύξη: false = ενδοδαπέδια ΕΝΕΡΓΗ (όριο 18°C), true = μόνο fancoil (13°C)
bool    systemOff       = false;   // master OFF (/off): σταματά τα πάντα (η αντιπαγετική στον S3 παραμένει)
float   savedSetpoint   = 22.5;    // τελευταία αποθηκευμένη τιμή (για write-on-change)
uint8_t savedSeason     = SEASON_WINTER;
bool    savedFancoilOnly = false;
bool    savedSystemOff   = false;
bool          homeReq    = false;  // αίτημα re-home βάνας (/home) — one-shot, κρατιέται ~20s να το λάβει ο S3
unsigned long homeReqMs  = 0;
BoilerStatus lastStatus = {};
bool    haveStatus      = false;
unsigned long lastRx    = 0;       // πότε ήρθε το τελευταίο BoilerStatus
float   lastRssi        = 0;
bool    commsLostNotified = false;
unsigned long hpFaultSince = 0;    // πότε ξεκίνησε «WAIT + ~0 κατανάλωση» (0 = ανενεργό)
bool    hpFaultNotified = false;
uint16_t hpFaultW       = 500;     // όριο W: κάτω από αυτό + WAIT -> πιθανή βλάβη αντλίας (NVS «hpw», /hpwatt)
const unsigned long HP_FAULT_MS = 30UL * 60UL * 1000UL;   // 30 λεπτά ζήτηση χωρίς κατανάλωση -> alert

float   houseTempC      = NAN;     // τελευταία θερμοκρασία σπιτιού (cache για την οθόνη)
float   houseHumidity   = NAN;     // τελευταία υγρασία δωματίου (%RH) από SHT40
bool    i2cOk           = true;    // false αν ο I2C bus είναι κολλημένος (short) -> παρακάμπτουμε τον SHT40

uart_link::Reader uartRx;          // (πρώην UART· τώρα η ζεύξη CYD είναι UDP)
WiFiUDP udp;                       // ασύρματη ζεύξη με CYD (broadcast τιμές / λήψη εντολών)
unsigned long lastSend = 0, lastBotPoll = 0, lastDisp = 0;
const unsigned long SEND_INTERVAL = 15000;   // LoRa cmd προς S3
const unsigned long BOT_INTERVAL  = 2500;    // poll Telegram
const unsigned long COMMS_TIMEOUT = 90000;   // 90s χωρίς λήψη -> alert
const unsigned long DISP_INTERVAL = 1000;    // στέλνε τιμές στο CYD 1×/δευτ

// ---- Ενεργειακό κόστος αντλίας θερμότητας (SCT-013 στον S3 -> pumpPower) ----
float  eurPerKwh      = 0.15f;   // €/kWh — placeholder· αλλάζει με /price ή στο boot από NVS
double energyWh       = 0.0;     // ΣΥΝΟΛΟ ενέργειας (Wh) — resettable με /resetkwh
double energyWhDay    = 0.0;     // σήμερα (auto-reset σε αλλαγή μέρας)
double energyWhMonth  = 0.0;     // τρέχων μήνας (auto-reset σε αλλαγή μήνα)
int    curDay = -1, curMonth = -1;   // για ανίχνευση αλλαγής μέρας/μήνα (από NTP)
bool   timeOk = false;           // συγχρονίστηκε το ρολόι (NTP);
unsigned long lastEnergyCalc = 0;  // χρονισμός ολοκλήρωσης ενέργειας
unsigned long lastEnergySave = 0;  // throttle αποθήκευσης NVS (φθορά flash)
const unsigned long ENERGY_CALC_MS = 10000;    // ολοκλήρωση ενέργειας κάθε 10s
const unsigned long ENERGY_SAVE_MS = 600000;   // αποθήκευση NVS κάθε 10' (χάνεις το πολύ 10')

// ---- Google Sheets logging (καταγραφή για ανάλυση ρεύματος/θερμοκρασιών) ----
//  Βάλε το /exec URL του Apps Script Web App (Deploy -> Web app -> Access: Anyone).
// SHEETS_URL ορίζεται στο secrets.h (ΜΥΣΤΙΚΟ, gitignored) — δεν μπαίνει στον κώδικα.
unsigned long lastSheetLog = 0;
const unsigned long SHEET_INTERVAL = 180000;   // κάθε 3 λεπτά (μικρότερο = πιο πυκνά Watt)

// ---- Ανθεκτικότητα WiFi (κλιμακωτή ανάκαμψη — χωρίς χειροκίνητο restart) ----
//  Επίπεδο 1: WiFi.reconnect() κάθε 10s
//  Επίπεδο 2: μετά από 5 αποτυχίες -> ΠΛΗΡΕΣ re-init του stack (το reconnect() «κολλάει» μόνιμα
//             σε reboot router / μεγάλη διακοπή — γνωστό ESP32 θέμα)
//  Επίπεδο 3: >30 λεπτά χωρίς σύνδεση -> ESP.restart() (ΑΣΦΑΛΕΣ: το NVS κρατά setpoint/εποχή/
//             ενέργεια, κι ο S3 είναι αυτόνομος με δικό του buffer thermostat)
unsigned long wifiDownSince = 0;    // πότε χάθηκε (0 = συνδεδεμένο)
unsigned long lastWifiTry   = 0;
uint8_t       wifiRetries   = 0;
const unsigned long WIFI_RETRY_MS  = 10000;             // προσπάθεια κάθε 10s
const uint8_t       WIFI_REINIT_AT = 5;                 // μετά από τόσες -> πλήρες re-init
const unsigned long WIFI_REBOOT_MS = 30UL * 60UL * 1000UL;  // 30 min -> επανεκκίνηση

// Διαβάζει SHT40 (T+RH, high-precision): ενημερώνει houseHumidity, επιστρέφει T (ή NAN)
float readHouseTemp() {
  if (!i2cOk) { houseHumidity = NAN; return NAN; }   // bus κολλημένος -> μην αγγίξεις I2C (κρεμάει)
  Wire.beginTransmission(SHT40_ADDR);
  Wire.write(0xFD);                                  // εντολή μέτρησης υψηλής ακρίβειας
  if (Wire.endTransmission() != 0) { houseHumidity = NAN; return NAN; }
  delay(10);                                         // ~8.2ms χρόνος μέτρησης
  if (Wire.requestFrom((uint8_t)SHT40_ADDR, (uint8_t)6) != 6) { houseHumidity = NAN; return NAN; }
  uint8_t d[6]; for (int i = 0; i < 6; i++) d[i] = Wire.read();
  uint16_t tT  = (d[0] << 8) | d[1];                 // d[2]=CRC (παραλείπεται)
  uint16_t rhT = (d[3] << 8) | d[4];                 // d[5]=CRC
  float t  = -45.0f + 175.0f * tT  / 65535.0f;
  float rh = -6.0f  + 125.0f * rhT / 65535.0f;
  houseHumidity = constrain(rh, 0.0f, 100.0f);
  return t;
}

// Σημείο δρόσου (τύπος Magnus) από θερμοκρασία + υγρασία· NAN αν λείπουν
float computeDewPoint(float T, float RH) {
  if (isnan(T) || isnan(RH) || RH <= 0.0f) return NAN;
  const float a = 17.62f, b = 243.12f;
  float g = logf(RH / 100.0f) + (a * T) / (b + T);
  return (b * g) / (a - g);
}

String fmt(int16_t v) {
  float f = decodeTemp(v);
  return isnan(f) ? String("--") : String(f, 1);
}

// ---- Telegram ----
// Κωδικός κατάστασης (CtrlStatus από τον S3) -> κείμενο για άνθρωπο
String ctrlStatusGr(uint8_t cs) {
  switch (cs) {
    case CS_HEATING:   return "🔥 Θέρμανση ενεργή";
    case CS_COOLING:   return "❄️ Ψύξη ενεργή";
    case CS_WAIT_HOT:  return "⏳ Αναμονή ζεστού (buffer όχι έτοιμο)";
    case CS_WAIT_COLD: return "⏳ Αναμονή κρύου (buffer όχι έτοιμο)";
    case CS_FROST:     return "🧊 Αντιπαγετική προστασία";
    case CS_SAFETY:    return "⚠️ Ασφάλεια";
    case CS_SYSTEM_OFF:return "🛑 ΣΒΗΣΤΟ (OFF)";
    default:           return "✅ OK (ικανοποιήθηκε)";
  }
}

// ASCII κωδικός κατάστασης για URL/Sheets (χωρίς emoji/ελληνικά που σπάνε στο query)
const char* statusCode(uint8_t cs) {
  switch (cs) {
    case CS_HEATING:   return "HEAT";
    case CS_COOLING:   return "COOL";
    case CS_WAIT_HOT:  return "WAIT_HOT";
    case CS_WAIT_COLD: return "WAIT_COLD";
    case CS_FROST:     return "FROST";
    case CS_SAFETY:    return "SAFETY";
    case CS_SYSTEM_OFF:return "OFF";
    default:           return "OK";
  }
}

// ---- Google Sheets: μία γραμμή δεδομένων ανά κλήση (HTTPS GET με query params) ----
static String shT(int16_t v) { float f = decodeTemp(v); return isnan(f) ? String("nan") : String(f, 1); }
static String shF(float f)   { return isnan(f) ? String("nan") : String(f, 1); }

void logToSheet() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!haveStatus || millis() - lastRx > COMMS_TIMEOUT) return;   // μη λογάρεις μπαγιάτικα δεδομένα

  WiFiClientSecure cli;
  cli.setInsecure();                 // όπως το Telegram — χωρίς έλεγχο cert
  cli.setHandshakeTimeout(5);
  HTTPClient https;
  https.setConnectTimeout(6000);
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);        // Apps Script κάνει 302

  String url = String(SHEETS_URL) + "?"
    "hotTop="   + shT(lastStatus.bufHotTop) +      // ζεστό buffer (πάνω)
    "&outdoor=" + shT(lastStatus.bufHotBot) +      // bufHotBot = εξωτερική θερμοκρασία
    "&cold="    + shT(lastStatus.bufCold) +        // κρύο buffer (καλοκαίρι)
    "&supply="  + shT(lastStatus.valveTemp) +      // προσαγωγή (έξοδος τρίοδης)
    "&power="   + String(lastStatus.pumpPower) +   // W (SCT-013 CT)
    "&room="    + shF(houseTempC) +
    "&rh="      + shF(houseHumidity) +
    "&setpoint="+ shF(desiredSetpoint) +
    "&dew="     + shF(computeDewPoint(houseTempC, houseHumidity)) +
    "&status="  + String(statusCode(lastStatus.ctrlStatus)) +
    "&season="  + String(season == SEASON_SUMMER ? "summer" : "winter") +
    "&rssi="    + String((int)lastRssi) +
    "&kwhDay="  + String(energyWhDay / 1000.0, 3);  // ημερήσια kWh (μετρητής hub)

  if (https.begin(cli, url)) {
    int code = https.GET();
    Serial.printf("[SHEET] HTTP %d\n", code);
    https.end();
  } else {
    Serial.println("[SHEET] begin() απέτυχε");
  }
}

String statusText() {
  String s = "🔥 *Θέρμανση*\n\n";
  if (systemOff) s += "🛑 *ΣΥΣΤΗΜΑ OFF* (χειροκίνητα — /on για άναμμα)\n\n";
  s += "🏠 Δωμάτιο: " + (isnan(houseTempC) ? String("--") : String(houseTempC, 1)) + "°C\n";
  s += "💧 Υγρασία: " + (isnan(houseHumidity) ? String("--") : String(houseHumidity, 0)) + "%\n";
  { float dp = computeDewPoint(houseTempC, houseHumidity);
    s += "🌫 Σημείο δρόσου: " + (isnan(dp) ? String("--") : String(dp, 1)) + "°C\n"; }
  if (!haveStatus) {
    s += "⚠️ Καμία λήψη από το λεβητοστάσιο ακόμα.\n";
  } else {
    s += "🌡 Hot buffer: " + fmt(lastStatus.bufHotTop) + "°C\n";
    s += "🌳 Εξωτερική:  " + fmt(lastStatus.bufHotBot) + "°C\n";
    s += "❄️ Cold buffer: " + fmt(lastStatus.bufCold) + "°C\n";
    s += "💧 Τρίοδη:     " + fmt(lastStatus.valveTemp) + "°C\n";
    s += "⚡ Αντλ.θερμ.: " + String(lastStatus.pumpPower) + " W";
    s += "  (" + String((lastStatus.pumpPower / 1000.0f) * eurPerKwh, 2) + " €/ώρα)\n";
    s += "🔧 Κατάσταση: " + ctrlStatusGr(lastStatus.ctrlStatus) + "\n";
    unsigned long age = (millis() - lastRx) / 1000;
    s += "📶 RSSI " + String((int)lastRssi) + " dBm  (πριν " + String(age) + "s)\n";
  }
  s += "\n🎯 Setpoint: " + String(desiredSetpoint, 1) + "°C";
  s += "\n🗓 Εποχή: " + String(season == SEASON_SUMMER ? "Καλοκαίρι ❄️" : "Χειμώνας 🔥");
  if (season == SEASON_SUMMER)
    s += "\n🧊 Ενδοδαπέδια ψύξη: " + String(fancoilOnly ? "OFF (μόνο fancoil)" : "ON (όριο 18°C)");
  double kwhD = energyWhDay / 1000.0, kwhM = energyWhMonth / 1000.0, kwh = energyWh / 1000.0;
  s += "\n\n📊 *Κατανάλωση Α/Θ*";
  s += "\n  Σήμερα: " + String(kwhD, 1) + " kWh = " + String(kwhD * eurPerKwh, 2) + " €";
  s += "\n  Μήνας:  " + String(kwhM, 1) + " kWh = " + String(kwhM * eurPerKwh, 2) + " €";
  s += "\n  Σύνολο: " + String(kwh, 1) + " kWh = " + String(kwh * eurPerKwh, 2) + " €";
  s += "\n  (τιμή " + String(eurPerKwh, 3) + " €/kWh)";
  struct tm t;
  if (getLocalTime(&t, 0)) {
    char buf[24]; strftime(buf, sizeof(buf), "%d/%m %H:%M", &t);
    s += "\n🕐 " + String(buf);
  } else {
    s += "\n🕐 (ρολόι: συγχρονισμός...)";
  }
  return s;
}

void notify(const String& msg) {
  if (WiFi.status() == WL_CONNECTED) bot.sendMessage(CHAT_ID, msg, "Markdown");
}

void handleTelegram() {
  int n = bot.getUpdates(bot.last_message_received + 1);
  for (int i = 0; i < n; i++) {
    String chat = bot.messages[i].chat_id;
    String text = bot.messages[i].text;
    if (chat != CHAT_ID) {           // κλειδωμένο μόνο σε σένα
      bot.sendMessage(chat, "Δεν επιτρέπεται.", "");
      continue;
    }
    text.trim();
    if (text == "/start" || text == "/help") {
      bot.sendMessage(chat,
        "Εντολές:\n/status — τιμές\n/set 22.5 — setpoint\n/winter — χειμώνας\n/summer — καλοκαίρι\n/off — σβήσε σύστημα\n/on — άναψε σύστημα\n/home — re-home βάνας (μετά από εργασία)\n/floorcool on|off — ψύξη ενδοδαπέδιου\n/price 0.18 — τιμή €/kWh\n/resetkwh — μηδένισε μετρητή ενέργειας\n/hpwatt 500 — όριο alert βλάβης αντλίας (W)", "");
    } else if (text == "/status") {
      bot.sendMessage(chat, statusText(), "Markdown");
    } else if (text.startsWith("/set")) {
      float v = text.substring(4).toFloat();
      if (v >= 5 && v <= 30) { desiredSetpoint = v; bot.sendMessage(chat, "✅ Setpoint: " + String(v, 1) + "°C", ""); }
      else bot.sendMessage(chat, "Δώσε τιμή 5–30, π.χ. /set 22.5", "");
    } else if (text == "/winter") {
      season = SEASON_WINTER; bot.sendMessage(chat, "🔥 Χειμώνας", "");
    } else if (text == "/summer") {
      season = SEASON_SUMMER; bot.sendMessage(chat, "❄️ Καλοκαίρι", "");
    } else if (text == "/off") {
      systemOff = true;  bot.sendMessage(chat, "🛑 Σύστημα *OFF* — κυκλοφορητής σταμάτησε, βάνα ως έχει.\n(Αντιπαγετική + anti-seize παραμένουν.)", "Markdown");
    } else if (text == "/on") {
      systemOff = false; bot.sendMessage(chat, "✅ Σύστημα *ON* — ο έλεγχος ξανάρχισε.", "Markdown");
    } else if (text == "/home") {
      homeReq = true; homeReqMs = millis();
      bot.sendMessage(chat, "🏠 *Re-home βάνας* — ο S3 οδηγεί ΚΛΕΙΣΙΜΟ στο τέρμα & μηδενίζει τη θέση (~2 λεπτά).\nΧρήσιμο μετά από εργασία/διακοπή ρεύματος στα φορτία.", "Markdown");
    } else if (text.startsWith("/floorcool")) {
      String a = text.substring(10); a.trim();
      if (a == "on")       { fancoilOnly = false; bot.sendMessage(chat, "🧊 Ενδοδαπέδια ψύξη: *ON* (όριο 18°C, προστασία δαπέδου)", "Markdown"); }
      else if (a == "off") { fancoilOnly = true;  bot.sendMessage(chat, "💨 Ενδοδαπέδια ψύξη: *OFF* — μόνο fancoil (έως 13°C).\nΈκλεισες τη βάνα του collector;", "Markdown"); }
      else bot.sendMessage(chat, "Χρήση: /floorcool on  ή  /floorcool off", "");
    } else if (text.startsWith("/price")) {
      float v = text.substring(6).toFloat();
      if (v > 0 && v < 5) { eurPerKwh = v; prefs.putFloat("eur", v);
        bot.sendMessage(chat, "💶 Τιμή ρεύματος: " + String(v, 3) + " €/kWh", ""); }
      else bot.sendMessage(chat, "Δώσε €/kWh, π.χ. /price 0.18", "");
    } else if (text == "/resetkwh") {
      energyWh = 0; prefs.putDouble("ewh", 0.0);
      bot.sendMessage(chat, "🔄 Μηδενίστηκε ο μετρητής ενέργειας (0 kWh).", "");
    } else if (text.startsWith("/hpwatt")) {
      int v = text.substring(7).toInt();
      if (v >= 100 && v <= 5000) { hpFaultW = v; prefs.putUShort("hpw", v);
        bot.sendMessage(chat, "🛠 Όριο alert βλάβης αντλίας: " + String(v) + "W (κατανάλωση < αυτό + WAIT για 30' → ειδοποίηση)", ""); }
      else bot.sendMessage(chat, "Δώσε W 100–5000, π.χ. /hpwatt 500", "");
    } else {
      bot.sendMessage(chat, "Άγνωστη εντολή. /help", "");
    }
  }
}

// ---- Web ----
String webPage() {
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='5'>"
    "<title>Θέρμανση</title><style>"
    "body{font-family:sans-serif;background:#0b1020;color:#eee;margin:0;padding:16px}"
    ".c{background:#1a2030;border-radius:12px;padding:14px 18px;margin:10px 0}"
    ".v{font-size:30px;font-weight:bold}.l{color:#8aa;font-size:13px}"
    "a.btn{display:inline-block;background:#2a3550;color:#fff;text-decoration:none;"
    "padding:14px 22px;border-radius:10px;margin:6px;font-size:20px}"
    ".hot{color:#ff7a3c}.cold{color:#3cd2ff}.set{color:#ffd23c}</style></head><body>";
  h += "<h2>🔥 Θέρμανση</h2>";
  h += "<div class='c'><span class='l'>Δωμάτιο</span><br><span class='v'>" + (isnan(houseTempC) ? String("--") : String(houseTempC, 1)) + "°C</span> &nbsp; <span class='l'>💧 " + (isnan(houseHumidity) ? String("--") : String(houseHumidity, 0)) + "%</span></div>";
  if (haveStatus) {
    h += "<div class='c'><span class='l'>Hot buffer</span><br><span class='v hot'>" + fmt(lastStatus.bufHotTop) + "°C</span></div>";
    h += "<div class='c'><span class='l'>Εξωτερική</span><br><span class='v'>" + fmt(lastStatus.bufHotBot) + "°C</span></div>";
    h += "<div class='c'><span class='l'>Cold buffer</span><br><span class='v cold'>" + fmt(lastStatus.bufCold) + "°C</span></div>";
    h += "<div class='c'><span class='l'>Τρίοδη</span><br><span class='v'>" + fmt(lastStatus.valveTemp) + "°C</span></div>";
    h += "<div class='c'><span class='l'>⚡ Αντλία θερμότητας</span><br><span class='v'>" + String(lastStatus.pumpPower) + " W</span> &nbsp; <span class='l'>" + String((lastStatus.pumpPower / 1000.0f) * eurPerKwh, 2) + " €/ώρα</span></div>";
  } else {
    h += "<div class='c'>⚠️ Καμία λήψη ακόμα</div>";
  }
  double kwhD = energyWhDay / 1000.0, kwhM = energyWhMonth / 1000.0, kwh = energyWh / 1000.0;
  h += "<div class='c'><span class='l'>📊 Κατανάλωση Α/Θ</span><br>";
  h += "<span class='l'>Σήμερα</span> <span class='v set'>" + String(kwhD, 1) + " kWh</span> = " + String(kwhD * eurPerKwh, 2) + " €<br>";
  h += "<span class='l'>Μήνας</span> <span class='v'>" + String(kwhM, 1) + " kWh</span> = " + String(kwhM * eurPerKwh, 2) + " €<br>";
  h += "<span class='l'>Σύνολο</span> " + String(kwh, 1) + " kWh = " + String(kwh * eurPerKwh, 2) + " € &nbsp; <span class='l'>(" + String(eurPerKwh, 3) + " €/kWh)</span></div>";
  { struct tm t; if (getLocalTime(&t, 0)) { char b[24]; strftime(b, sizeof(b), "%d/%m %H:%M", &t); h += "<p class='l'>🕐 " + String(b) + "</p>"; } }
  h += "<div class='c'><span class='l'>Setpoint</span><br><span class='v set'>" + String(desiredSetpoint, 1) + "°C</span><br>";
  h += "<a class='btn' href='/down'>−</a><a class='btn' href='/up'>+</a></div>";
  h += "<div class='c'><span class='l'>Εποχή</span><br><span class='v'>" + String(season == SEASON_SUMMER ? "Καλοκαίρι" : "Χειμώνας") + "</span><br>";
  h += "<a class='btn' href='/toggle'>Αλλαγή</a></div>";
  h += "<p class='l'>RSSI " + String((int)lastRssi) + " dBm</p></body></html>";
  return h;
}

void setupWeb() {
  web.on("/", []() { web.send(200, "text/html", webPage()); });
  web.on("/up",   []() { desiredSetpoint = min(30.0f, desiredSetpoint + 0.5f); web.sendHeader("Location","/"); web.send(303); });
  web.on("/down", []() { desiredSetpoint = max(5.0f,  desiredSetpoint - 0.5f); web.sendHeader("Location","/"); web.send(303); });
  web.on("/toggle", []() { season = (season == SEASON_SUMMER) ? SEASON_WINTER : SEASON_SUMMER; web.sendHeader("Location","/"); web.send(303); });
  web.begin();
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println(F("\n[ΣΠΙΤΙ / HUB] εκκίνηση..."));
  pinMode(HUB_LED, OUTPUT); digitalWrite(HUB_LED, LOW);   // LED ένδειξη κυκλοφορητή (active-high: LOW = σβηστό)

  // Φόρτωσε setpoint/εποχή από NVS — επιβιώνουν σε διακοπή ρεύματος
  prefs.begin("heat", false);
  desiredSetpoint = prefs.getFloat("sp", 22.5f);
  season = prefs.getUChar("season", SEASON_WINTER);
  fancoilOnly = prefs.getBool("fcOnly", false);
  systemOff   = prefs.getBool("off", false);      // master OFF (επιβιώνει διακοπή)
  energyWh      = prefs.getDouble("ewh",  0.0);   // σύνολο (επιβιώνει διακοπή)
  energyWhDay   = prefs.getDouble("ewhD", 0.0);   // σήμερα
  energyWhMonth = prefs.getDouble("ewhM", 0.0);   // μήνας
  curDay        = prefs.getInt("curD", -1);
  curMonth      = prefs.getInt("curM", -1);
  eurPerKwh     = prefs.getFloat("eur", 0.15f);   // τιμή €/kWh
  hpFaultW      = prefs.getUShort("hpw", 500);    // όριο W για alert βλάβης αντλίας
  lastEnergyCalc = lastEnergySave = millis();
  savedSetpoint    = desiredSetpoint;
  savedSeason      = season;
  savedFancoilOnly = fancoilOnly;
  savedSystemOff   = systemOff;
  Serial.printf("Φόρτωση από NVS: setpoint=%.1f season=%s\n",
                desiredSetpoint, season == SEASON_SUMMER ? "summer" : "winter");

  // Έλεγχος I2C ΠΡΙΝ το Wire.begin: αν SDA/SCL είναι «σφιγμένα» LOW (short) -> ΜΗΝ αγγίξεις I2C (αλλιώς κρεμάει ο hub)
  pinMode(SHT_SCL, INPUT_PULLUP); pinMode(SHT_SDA, INPUT_PULLUP); delayMicroseconds(100);
  if (digitalRead(SHT_SCL) == LOW || digitalRead(SHT_SDA) == LOW) {
    i2cOk = false;
    Serial.printf("[I2C] ΣΦΑΛΜΑ: SCL=%d SDA=%d κολλημένο LOW (short;) -> παρακάμπτω τον SHT40\n",
                  digitalRead(SHT_SCL), digitalRead(SHT_SDA));
  } else {
    Wire.begin(SHT_SDA, SHT_SCL);
    Wire.setTimeOut(50);
    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { Serial.printf("[I2C] βρέθηκε @ 0x%02X\n", a); found++; }
    }
    Serial.printf("[I2C] σάρωση: %d συσκευές (αναμένουμε 0x44 = SHT40)\n", found);
  }
  if (lora.begin() != RADIOLIB_ERR_NONE) {
    Serial.println(F("LoRa init ΑΠΕΤΥΧΕ — σταματάω."));
    while (true) delay(1000);
  }
  Serial.println(F("LoRa OK."));

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);   // επανασύνδεση σε επίπεδο driver
  // ΔΙΟΡΘ. 2026-07-24: modem-sleep ON (default) -> ΗΣΥΧΟ WiFi RF. Το setSleep(false) κρατούσε το WiFi
  // 2.4GHz συνεχώς ενεργό δίπλα στον δέκτη LoRa του hub -> desense -> έπεσε το RSSI (-89 -> -102).
  // Το LoRa (κρίσιμο για τον έλεγχο) προηγείται της WiFi σταθερότητας· η auto-recovery καλύπτει drops.
  WiFi.setSleep(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  udp.begin(uart_link::UDP_CMD_PORT);   // ασύρματη ζεύξη με CYD (ΜΕΤΑ το WiFi.begin — αλλιώς crash!)
  Serial.print("WiFi");
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
    // Ώρα Ελλάδας (EET/EEST) με αυτόματη θερινή/χειμερινή — για ημερήσιο/μηνιαίο κόστος
    configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");
    secured.setInsecure();                 // Telegram TLS χωρίς έλεγχο cert (απλό)
    secured.setHandshakeTimeout(5);        // sec: να ΜΗΝ κολλάει ο loop σε αργό/ασταθές δίκτυο (αλλιώς «παγώνει» το CYD)
    setupWeb();
    notify("🔥 Σύστημα θέρμανσης *online*\nWeb: http://" + WiFi.localIP().toString());
  } else {
    Serial.println(F("\nWiFi ΑΠΕΤΥΧΕ — συνεχίζω μόνο με LoRa (retry στο loop)."));
  }
}

void sendCommand() {
  houseTempC = readHouseTemp();      // διάβασε & κράτα (το χρησιμοποιεί και η οθόνη)
  HouseCmd pkt = {};
  pkt.setpoint  = encodeTemp(desiredSetpoint);
  pkt.houseTemp = encodeTemp(houseTempC);
  pkt.season    = season;
  if (homeReq && millis() - homeReqMs > 20000) homeReq = false;   // λήξη one-shot αιτήματος re-home
  pkt.flags     = (fancoilOnly ? CMD_FLAG_FANCOIL_ONLY : 0)
                | (systemOff   ? CMD_FLAG_SYSTEM_OFF   : 0)
                | (homeReq     ? CMD_FLAG_HOME         : 0);
  pkt.dewPoint  = encodeTemp(computeDewPoint(houseTempC, houseHumidity));   // σημείο δρόσου -> S3
  lora.send(pkt);
}

// ---- UART: στέλνει όλες τις τιμές στο CYD ----
void sendDisplayToCyd() {
  uart_link::DisplayPacket dp = {};
  bool ok = haveStatus && (millis() - lastRx < COMMS_TIMEOUT);
  dp.room     = uart_link::encT(houseTempC);
  dp.setpoint = uart_link::encT(desiredSetpoint);
  dp.season   = season;
  dp.flags    = (ok ? uart_link::F_COMMS_OK : 0) | (fancoilOnly ? uart_link::F_FANCOIL_ONLY : 0);
  dp.rssi     = (int8_t)lastRssi;
  if (haveStatus) {
    // Οι θερμοκρασίες του BoilerStatus είναι ΗΔΗ int16 ×100 (ίδια κωδικοποίηση) -> ευθεία αντιγραφή
    dp.hotTop  = lastStatus.bufHotTop;
    dp.outdoor = lastStatus.bufHotBot;   // bufHotBot = εξωτερική (αντιστάθμιση)
    dp.cold    = lastStatus.bufCold;
    dp.valve   = lastStatus.valveTemp;
    dp.pumpW   = (int16_t)lastStatus.pumpPower;
  } else {
    dp.hotTop = dp.outdoor = dp.cold = dp.valve = uart_link::TEMP_NAN;
  }
  // Ώρα/ημερομηνία από NTP -> στο CYD (το CYD δεν έχει internet/ρολόι)
  struct tm t;
  if (getLocalTime(&t, 0)) {
    dp.hh = t.tm_hour; dp.mm = t.tm_min; dp.dd = t.tm_mday; dp.mo = t.tm_mon + 1;
    dp.flags |= uart_link::F_TIME_OK;
  }
  dp.humidity = isnan(houseHumidity) ? 255 : (uint8_t)(houseHumidity + 0.5f);   // %RH (255=άκυρο)
  dp.ctrlStatus = ok ? lastStatus.ctrlStatus : CS_OFF;   // κατάσταση ελέγχου του S3 -> CYD
  dp.kwhDay   = (float)(energyWhDay   / 1000.0);   // -> σελίδα Ενέργειας CYD
  dp.kwhMonth = (float)(energyWhMonth / 1000.0);
  dp.kwhTotal = (float)(energyWh      / 1000.0);
  dp.eurKwh   = eurPerKwh;
  if (WiFi.status() == WL_CONNECTED)
    uart_link::udpSend(udp, WiFi.broadcastIP(), uart_link::UDP_DISP_PORT, dp);   // broadcast -> CYD (WiFi)
}

// ---- UDP: δέχεται εντολή (setpoint/εποχή/floorcool) από το CYD ----
void handleCyd() {
  int n = udp.parsePacket();
  uart_link::CommandPacket c;
  if (n > 0 && uart_link::udpRecv(udp, n, c)) {
    float sp = uart_link::decT(c.setpoint);
    if (!isnan(sp) && sp >= 5 && sp <= 30) desiredSetpoint = sp;
    season = (c.season == uart_link::SEASON_SUMMER) ? SEASON_SUMMER : SEASON_WINTER;
    fancoilOnly = (c.flags & uart_link::CMD_FANCOIL_ONLY);   // επιλογή ψύξης ενδοδαπέδιου από CYD
    sendCommand();                     // προώθησε ΑΜΕΣΩΣ στον S3 (LoRa)
    lastSend = millis();
    Serial.printf("[CYD-> ] setpoint=%.1f season=%s floorCool=%s (UDP %s)\n",
                  desiredSetpoint, season == SEASON_SUMMER ? "SUMMER" : "WINTER",
                  fancoilOnly ? "OFF" : "ON", udp.remoteIP().toString().c_str());
  }
}

void loop() {
  // --- LoRa λήψη ---
  BoilerStatus s;
  if (lora.receive(s)) {
    lastStatus = s; haveStatus = true; lastRx = millis(); lastRssi = lora.rssi();
    if (commsLostNotified) { notify("✅ Επανήλθε η επικοινωνία με το λεβητοστάσιο."); commsLostNotified = false; }
  }

  // --- UART: εντολές από το CYD + αποστολή τιμών στην οθόνη ---
  handleCyd();
  if (millis() - lastDisp >= DISP_INTERVAL) { lastDisp = millis(); sendDisplayToCyd(); }

  // --- LED onboard (μπλε GPIO2): ανάβει όταν δουλεύει ο κυκλοφορητής (από BoilerStatus.relays) ---
  digitalWrite(HUB_LED, (haveStatus && millis() - lastRx < COMMS_TIMEOUT && (lastStatus.relays & RELAY_PUMP)) ? HIGH : LOW);

  // --- Αποθήκευση σε NVS ΜΟΝΟ σε πραγματική αλλαγή (απ' όπου κι αν ήρθε:
  //     Telegram / web / CYD) ώστε να επιβιώνει σε διακοπή ρεύματος ---
  if (desiredSetpoint != savedSetpoint || season != savedSeason || fancoilOnly != savedFancoilOnly || systemOff != savedSystemOff) {
    savedSetpoint    = desiredSetpoint;
    savedSeason      = season;
    savedFancoilOnly = fancoilOnly;
    savedSystemOff   = systemOff;
    prefs.putFloat("sp", desiredSetpoint);
    prefs.putUChar("season", season);
    prefs.putBool("fcOnly", fancoilOnly);
    prefs.putBool("off", systemOff);
    Serial.printf("[NVS] αποθήκευση setpoint=%.1f season=%s floorCool=%s\n",
                  desiredSetpoint, season == SEASON_SUMMER ? "summer" : "winter",
                  fancoilOnly ? "OFF" : "ON");
  }

  // --- Ολοκλήρωση ενέργειας αντλίας θερμότητας (Wh) + ημερήσιο/μηνιαίο + αποθήκευση NVS ---
  if (millis() - lastEnergyCalc >= ENERGY_CALC_MS) {
    double dtH = (millis() - lastEnergyCalc) / 3600000.0;   // ms -> ώρες (από millis, ακριβές)
    lastEnergyCalc = millis();
    if (haveStatus && millis() - lastRx < COMMS_TIMEOUT) {
      double wh = (double)lastStatus.pumpPower * dtH;        // W × ώρες = Wh
      energyWh += wh;                                        // σύνολο (πάντα)
      struct tm t;
      if (getLocalTime(&t, 0)) {                             // αν έχει συγχρονιστεί το ρολόι
        timeOk = true;
        if (t.tm_mday != curDay)  { energyWhDay   = 0; curDay   = t.tm_mday; }  // νέα μέρα
        if (t.tm_mon  != curMonth){ energyWhMonth = 0; curMonth = t.tm_mon;  }  // νέος μήνας
        energyWhDay   += wh;
        energyWhMonth += wh;
      }
    }
    if (millis() - lastEnergySave >= ENERGY_SAVE_MS) {
      lastEnergySave = millis();
      prefs.putDouble("ewh",  energyWh);
      prefs.putDouble("ewhD", energyWhDay);
      prefs.putDouble("ewhM", energyWhMonth);
      prefs.putInt("curD", curDay);
      prefs.putInt("curM", curMonth);
    }
  }

  // --- Google Sheets logging (ανάλυση ρεύματος/θερμοκρασιών) ---
  if (WiFi.status() == WL_CONNECTED && millis() - lastSheetLog >= SHEET_INTERVAL) {
    lastSheetLog = millis();
    logToSheet();
  }

  // --- LoRa αποστολή εντολής ---
  if (millis() - lastSend >= SEND_INTERVAL) { lastSend = millis(); sendCommand(); }

  // --- Alert χαμένης επικοινωνίας ---
  if (haveStatus && !commsLostNotified && millis() - lastRx > COMMS_TIMEOUT) {
    notify("⚠️ *Χάθηκε η επικοινωνία* με το λεβητοστάσιο (>90s).");
    commsLostNotified = true;
  }

  // --- Alert πιθανής βλάβης αντλίας θερμότητας ---
  //  Ζήτηση + buffer ΟΧΙ έτοιμο (WAIT_HOT/WAIT_COLD) + κατανάλωση < όριο (=ούτε ανεμιστήρας
  //  δουλεύει σωστά) για >30' -> η αντλία μάλλον δεν δουλεύει. Συμβουλευτικό, δεν σταματά τίποτα.
  bool hpWaiting = haveStatus && (millis() - lastRx < COMMS_TIMEOUT) &&
                   (lastStatus.ctrlStatus == CS_WAIT_HOT || lastStatus.ctrlStatus == CS_WAIT_COLD);
  if (hpWaiting && lastStatus.pumpPower < hpFaultW) {
    if (hpFaultSince == 0) hpFaultSince = millis();
    if (!hpFaultNotified && millis() - hpFaultSince >= HP_FAULT_MS) {
      notify("⚠️ *Πιθανή βλάβη αντλίας θερμότητας*\nΖήτηση + buffer όχι έτοιμο, αλλά κατανάλωση < "
             + String(hpFaultW) + "W εδώ και 30'. Έλεγξε την αντλία (συμπιεστής/τροφοδοσία).");
      hpFaultNotified = true;
    }
  } else {
    hpFaultSince = 0; hpFaultNotified = false;   // η συνθήκη έσπασε -> reset + re-arm
  }

  // --- WiFi / Telegram / Web (με κλιμακωτή ανάκαμψη — δεν θέλει χειροκίνητο restart) ---
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiDownSince) {                        // μόλις επανήλθε
      Serial.printf("[WiFi] ΕΠΑΝΗΛΘΕ μετά %lus -> %s\n",
                    (millis() - wifiDownSince) / 1000, WiFi.localIP().toString().c_str());
      wifiDownSince = 0; wifiRetries = 0;
    }
    web.handleClient();
    if (millis() - lastBotPoll >= BOT_INTERVAL) { lastBotPoll = millis(); handleTelegram(); }
  } else {
    if (!wifiDownSince) {                       // μόλις χάθηκε
      wifiDownSince = millis(); wifiRetries = 0; lastWifiTry = 0;
      Serial.println(F("[WiFi] ΧΑΘΗΚΕ -> ξεκινούν προσπάθειες επανασύνδεσης"));
    }
    if (millis() - lastWifiTry >= WIFI_RETRY_MS) {
      lastWifiTry = millis();
      if (++wifiRetries <= WIFI_REINIT_AT) {
        Serial.printf("[WiFi] reconnect #%u\n", wifiRetries);
        WiFi.reconnect();                       // ΕΠΙΠΕΔΟ 1: ελαφρύ
      } else {
        Serial.println(F("[WiFi] ΠΛΗΡΕΣ RE-INIT (disconnect + begin)"));
        WiFi.disconnect(true);                  // ΕΠΙΠΕΔΟ 2: ξαναχτίζει το stack
        WiFi.mode(WIFI_OFF);
        delay(200);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.setSleep(true);       // ήσυχο WiFi RF -> δεν χαλάει το LoRa RSSI
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        udp.begin(uart_link::UDP_CMD_PORT);     // ξανα-δέσμευση socket για το CYD
        wifiRetries = 0;                        // κύκλος από την αρχή
      }
    }
    // ΕΠΙΠΕΔΟ 3 (έσχατο): πολλή ώρα χωρίς WiFi -> επανεκκίνηση.
    //  Ασφαλές: NVS κρατά setpoint/εποχή/ενέργεια· ο S3 συνεχίζει αυτόνομος (buffer thermostat).
    if (millis() - wifiDownSince >= WIFI_REBOOT_MS) {
      Serial.println(F("[WiFi] >30min χωρίς σύνδεση -> ΕΠΑΝΕΚΚΙΝΗΣΗ"));
      delay(100);
      ESP.restart();
    }
  }

  // --- Heartbeat debug (κάθε 5s) — δείχνει state ζωντανά στο serial monitor ---
  static unsigned long lastDbg = 0;
  if (millis() - lastDbg >= 5000) {
    lastDbg = millis();
    Serial.printf("[HUB] room=%.1fC set=%.1f %s | boiler=%s rssi=%d | wifi=%s\n",
                  houseTempC, desiredSetpoint, season == SEASON_SUMMER ? "summer" : "winter",
                  haveStatus ? "OK" : "--", (int)lastRssi,
                  WiFi.status() == WL_CONNECTED ? "OK" : "no");
  }
}
