// ============================================================================
//  CYD 3.5" (ESP32-3248S035, ILI9488 480x320) — Dashboard θέρμανσης
// ----------------------------------------------------------------------------
//  Μεγάλα κουμπιά setpoint (συχνή χρήση), μικρό κουμπί εποχής (1×/χρόνο) με
//  ΕΠΙΒΕΒΑΙΩΣΗ. "Outdoor" = εξωτερική θερμοκρασία από τον S3 (για αντιστάθμιση).
//  ΔΕΔΟΜΕΝΑ: αληθινές τιμές από τον κόμβο σπιτιού (ESP32 dev) μέσω UART (Serial1).
//  Όταν δεν υπάρχει σύνδεση -> δείχνει "--" και κόκκινο LED στο header.
// ============================================================================
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <UartLink.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"     // ίδιο WiFi με τον hub

TFT_eSPI tft = TFT_eSPI();

// ---- UART γέφυρα προς τον κόμβο σπιτιού (Serial1 σε GPIO22/35) ----
//  ΓΙΑΤΙ ΟΧΙ το connector 5V/TX/RX/GND (UART0 = GPIO1/3): πάνω στο CYD το UART0
//  το κρατάει το onboard CH340 (USB-serial), που τροφοδοτείται από την πλακέτα και
//  "μπλοκάρει" τη γραμμή RX -> δεν περνάει εξωτερικό UART (δεν δούλεψε σε καμία φορά).
//  Λύση: ξεχωριστά GPIO χωρίς CH340.
//    HUB_RX=GPIO35 (input-only, OK για RX) <- TX σπιτιού (GPIO17)
//    HUB_TX=GPIO22 -> RX σπιτιού (GPIO16)
//  Τροφοδοσία: 5V & GND ΑΠΟ το connector 5V/TX/RX/GND (αγνόησε τα TX/RX του).
#define HUB_RX   35
#define HUB_TX   22
#define HUB_BAUD 38400
WiFiUDP udp;                 // ασύρματη ζεύξη με τον hub (UDP/WiFi)
IPAddress hubIP;             // IP του hub (μαθαίνεται από το πρώτο πακέτο τιμών)

// ---- Καιρός (Open-Meteo, Μαραθώνας/Αττική — δωρεάν, χωρίς κλειδί) ----
#define WX_LAT "38.15"
#define WX_LON "23.96"
struct WxData {
  bool  valid = false;
  float t = NAN, h = NAN, w = NAN;   // τρέχουσα: θερμοκρασία, υγρασία, άνεμος (km/h)
  float feels = NAN;                 // αίσθηση (apparent temperature)
  int   code = -1;                   // WMO weather code
  int   fCode[4] = {-1,-1,-1,-1};    // πρόβλεψη 4 ημερών (0=σήμερα)
  float fHi[4] = {NAN,NAN,NAN,NAN}, fLo[4] = {NAN,NAN,NAN,NAN};
  int   fProb[4] = {0,0,0,0};        // πιθανότητα βροχής % ανά ημέρα
  char  fDay[4][6] = {"--","--","--","--"};   // ετικέτες ημερών (Today/Mon/...)
  unsigned long fetched = 0;
} wx;
int page = 0;                        // 0 = Θέρμανση, 1 = Καιρός (swipe)

#define C_BG     0x0841
#define C_PANEL  0x2124
#define C_PANEL2 0x18E3
#define C_HEAD   0x4B12      // απαλό μπλε-γκρι (pastel)
#define C_HOT    0xFB00
#define C_COLD   0x05FF
#define C_WINTER_BTN 0xC36A  // απαλό ζεστό (χειμώνας)
#define C_SUMMER_BTN 0x6559  // απαλό γαλάζιο (καλοκαίρι)
#define C_GREEN  0x2667
#define C_RED    0xF1C3
#define C_YELLOW 0xFE60
#define C_OUT    0x9FF3
#define C_WHITE  0xFFFF
#define C_GREY   0x9CD3
#define C_DARK   0x528A

const int TX_MIN = 486,  TX_MAX = 3474;
const int TY_MIN = 412,  TY_MAX = 3662;
const int SCR_W = 480, SCR_H = 320;

// Τιμές οθόνης — ξεκινούν "άγνωστες" (NAN -> "--") μέχρι να έρθει πακέτο μέσω UART
float hotTop = NAN, outTemp = NAN, coldBuf = NAN, valveOut = NAN, roomTemp = NAN;
float setpoint = 22.5;
bool  commsOk = false;             // επικοινωνία σπίτι<->λεβητοστάσιο (από το πακέτο)
bool  summer = false;
int   rssi = -120;
bool  haveData = false;            // ήρθε έστω ένα πακέτο;
unsigned long lastPacket = 0;      // πότε ήρθε το τελευταίο πακέτο UART
unsigned long lastLocalEdit = 0;   // πότε άλλαξε ο χρήστης κάτι τοπικά (για να μην το πατάει το σπίτι)
const unsigned long LINK_TIMEOUT = 20000;  // 20s χωρίς UART -> "χάθηκε" (ανεκτικό σε στιγμιαία παγώματα hub: Telegram/boot)

// ---- Φωτισμός (PWM): χαμηλώνει στο 30% μετά από αδράνεια, full με άγγιγμα ----
#define BL_PIN 27
#define BL_CH  0
const unsigned long SCREEN_TIMEOUT = 180000;  // 3 λεπτά αδράνειας -> dim 30% + auto-lock
const uint8_t BL_FULL = 255;
const uint8_t BL_DIM  = 77;                   // ~30%
bool  dimmed = false;
unsigned long lastActivity = 0;
void setBrightness(uint8_t v) { ledcWrite(BL_CH, v); }

struct Btn { int x, y, w, h; };
Btn btnMinus  = {26, 236, 78, 60};     // μεγάλα κουμπιά setpoint
Btn btnPlus   = {248, 236, 78, 60};
Btn btnSeason = {360, 206, 108, 98};   // μικρό κουμπί εποχής (γωνία)
Btn btnYes    = {110, 186, 110, 56};
Btn btnNo     = {262, 186, 110, 56};
bool inBtn(Btn b, int x, int y) { return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h; }

bool confirmOpen = false;
bool pendingSummer = false;
bool floorDialogOpen = false;        // 2ο dialog (καλοκαίρι): ενδοδαπέδια ψύξη ΝΑΙ/ΟΧΙ
bool fancoilOnly = false;            // ψύξη μόνο fancoil (δάπεδο απομονωμένο)
float pumpW = NAN;                   // κατανάλωση αντλίας θερμότητας (W)
float kwhDay = NAN, kwhMonth = NAN, kwhTotal = NAN, eurKwh = 0.15;   // ενέργεια/κόστος (σελίδα Ενέργειας)
float humidity = NAN;                // υγρασία δωματίου (%RH) από SHT40 (μέσω hub)
int   ctrlStatus = 0;                // κατάσταση S3: 0=OFF/OK 1=θέρμ 2=ψύξη 3=αναμ.ζεστού 4=αναμ.κρύου 5=παγετός 6=ασφάλεια
uint8_t tHH = 0, tMM = 0, tDD = 0, tMO = 0;   // ώρα/ημερομηνία από hub (NTP)
bool timeOk = false;

// ---- Κλείδωμα γονικού ελέγχου ----
bool locked = false;
Btn  btnLock = {250, 0, 230, 44};     // ΜΕΓΑΛΗ περιοχή (δεξί μισό header) για εύκολο πάτημα
const int LOCK_ICON_X = 351, LOCK_ICON_Y = 16;
unsigned long holdStart = 0;
bool  holdHandled = false;
unsigned long lastTouchMs = 0;
unsigned long toastUntil = 0;
const unsigned long LOCK_HOLD = 1500; // 1.5s παρατεταμένο πάτημα

bool getTouchPt(int &sx, int &sy) {
  if (tft.getTouchRawZ() < 350) return false;   // πιο ευαίσθητο (εύκολο πάτημα)
  uint16_t rx, ry;
  tft.getTouchRaw(&rx, &ry);
  float fx = (float)(ry - TY_MIN) / (TY_MAX - TY_MIN);
  float fy = (float)(rx - TX_MIN) / (TX_MAX - TX_MIN);
  sx = constrain((int)(fx * SCR_W), 0, SCR_W - 1);          // αφή γυρισμένη 180° μαζί με την οθόνη
  sy = constrain((int)((1.0 - fy) * SCR_H), 0, SCR_H - 1);
  return true;
}

// ---- Εικονίδια ----
void iconFlame(int cx, int cy, uint16_t col) {
  tft.fillTriangle(cx, cy - 12, cx - 8, cy + 8, cx + 8, cy + 8, col);
  tft.fillCircle(cx, cy + 5, 8, col);
  tft.fillCircle(cx, cy + 3, 4, C_YELLOW);
}
void iconSnow(int cx, int cy, uint16_t col) {
  for (int i = 0; i < 3; i++) {
    float a = i * PI / 3.0;
    int dx = (int)(11 * cos(a)), dy = (int)(11 * sin(a));
    tft.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, col);
    tft.drawLine(cx - dx, cy - dy + 1, cx + dx, cy + dy + 1, col);
  }
  tft.fillCircle(cx, cy, 3, col);
}
void iconDrop(int cx, int cy, uint16_t col) {
  tft.fillTriangle(cx, cy - 11, cx - 7, cy + 3, cx + 7, cy + 3, col);
  tft.fillCircle(cx, cy + 4, 7, col);
}
void iconThermo(int cx, int cy, uint16_t col) {
  tft.fillRoundRect(cx - 3, cy - 12, 6, 16, 3, col);
  tft.fillRect(cx - 1, cy - 8, 2, 14, C_RED);
  tft.fillCircle(cx, cy + 7, 6, col);
  tft.fillCircle(cx, cy + 7, 4, C_RED);
}
void iconBars(int x, int y, int r) {
  uint16_t on = (r > -85) ? C_GREEN : (r > -105 ? C_YELLOW : C_RED);
  int n = (r > -60) ? 4 : (r > -80 ? 3 : (r > -100 ? 2 : 1));
  for (int i = 0; i < 4; i++) {
    int h = 5 + i * 5;
    tft.fillRect(x + i * 8, y + 22 - h, 6, h, i < n ? on : C_DARK);
  }
}

void iconLock(int cx, int cy, bool isLocked, uint16_t col) {
  int sh = isLocked ? cy - 4 : cy - 7;          // ψηλότερο shackle = "ανοιχτό"
  tft.drawCircle(cx, sh, 6, col);
  tft.drawCircle(cx, sh, 7, col);
  tft.fillRoundRect(cx - 9, cy, 18, 13, 3, col);
  tft.fillCircle(cx, cy + 6, 2, C_HEAD);
}
void drawLock() {
  tft.fillRect(LOCK_ICON_X - 16, 2, 34, 38, C_HEAD);   // καθάρισε μόνο την περιοχή του εικονιδίου
  iconLock(LOCK_ICON_X, LOCK_ICON_Y, locked, locked ? C_YELLOW : 0x6B6D);
}

void drawHeader() {
  tft.fillRect(0, 0, 480, 42, C_HEAD);
  drawLock();   // η αριστερή ένδειξη κατάστασης ζωγραφίζεται από το updateStatusText()
}

void showToast(const char* m) {
  toastUntil = millis() + 1200;
  tft.fillRoundRect(150, 128, 180, 64, 10, C_PANEL2);
  tft.drawRoundRect(150, 128, 180, 64, 10, C_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_WHITE, C_PANEL2);
  tft.drawString(m, 240, 160, 4);
}

// ---- Anti-flicker: ξαναζωγραφίζουμε ΜΟΝΟ ό,τι άλλαξε (όχι όλα κάθε πακέτο) ----
bool  gForce = false;                 // true στο redrawAll -> ζωγράφισε τα πάντα
float dRoom=NAN, dPump=NAN, dHum=NAN, dHot=NAN, dOut=NAN, dCold=NAN, dValve=NAN;
int   dRssi=999, dComms=-1, dCtrlStatus=-1; char dTime[16] = {0};
inline bool chg(float& store, float v, float eps = 0.05f) {
  bool c = (isnan(store) != isnan(v)) || (!isnan(v) && fabsf(store - v) > eps);
  if (c) store = v;
  return c || gForce;
}

void updateRoom() {
  if (chg(dRoom, roomTemp)) {                      // θερμοκρασία δωματίου
    tft.fillRect(18, 78, 214, 88, C_PANEL);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_WHITE, C_PANEL);
    if (isnan(roomTemp)) tft.drawString("--", 118, 126, 6);
    else                 tft.drawFloat(roomTemp, 1, 118, 126, 6);
    tft.setTextColor(C_GREY, C_PANEL);
    tft.drawCircle(197, 93, 2, C_GREY);       // ° (κυκλάκι βαθμών) — λίγο πιο πάνω/αριστερά
    tft.drawString("C", 210, 106, 4);
  }
  bool hc = chg(dHum, humidity, 0.5f);
  bool pc = chg(dPump, pumpW, 2.0f);
  if (hc || pc) {                                  // υγρασία δωματίου + κατανάλωση αντλίας θερμότητας
    tft.fillRect(18, 168, 214, 24, C_PANEL);
    char hs[10], ps[10], b[24];
    if (isnan(humidity)) snprintf(hs, sizeof(hs), "RH--");
    else                 snprintf(hs, sizeof(hs), "RH%d%%", (int)(humidity + 0.5f));
    if (isnan(pumpW))    snprintf(ps, sizeof(ps), "HP--");
    else                 snprintf(ps, sizeof(ps), "HP%dW", (int)(pumpW + 0.5f));
    snprintf(b, sizeof(b), "%s  %s", hs, ps);
    tft.setTextColor(C_GREY, C_PANEL);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(b, 118, 178, 2);
  }
}

void updateSetpoint() {
  tft.fillRect(108, 236, 136, 60, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_YELLOW, C_PANEL);
  tft.drawFloat(setpoint, 1, 176, 266, 6);
}

void statRow(int y, void (*icon)(int, int, uint16_t), uint16_t icol,
             const char* label, float val, uint16_t vcol) {
  tft.fillRect(256, y, 206, 34, C_PANEL);
  icon(274, y + 17, icol);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_GREY, C_PANEL);
  tft.drawString(label, 296, y + 9, 2);
  tft.setTextColor(vcol, C_PANEL);
  if (isnan(val)) {
    tft.setTextDatum(MR_DATUM);
    tft.drawString("--", 456, y + 20, 4);
  } else {
    char b[8]; snprintf(b, sizeof(b), "%.1f", val);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(b, 430, y + 20, 4);        // αριθμός
    tft.drawCircle(438, y + 9, 2, vcol);      // ° (κυκλάκι βαθμών — ζωγραφισμένο)
    tft.setTextDatum(ML_DATUM);
    tft.drawString("C", 443, y + 20, 4);      // C
  }
}

void updateStats() {
  if (chg(dHot,   hotTop))   statRow(56,  iconFlame,  C_HOT,   "Hot buf",  hotTop,   C_HOT);
  if (chg(dOut,   outTemp))  statRow(92,  iconThermo, C_OUT,   "Outdoor",  outTemp,  C_OUT);
  if (chg(dCold,  coldBuf))  statRow(128, iconSnow,   C_COLD,  "Cold buf", coldBuf,  C_COLD);
  if (chg(dValve, valveOut)) statRow(164, iconDrop,   C_YELLOW,"Valve",    valveOut, C_YELLOW);
}

void updateSeason() {
  uint16_t col = summer ? C_SUMMER_BTN : C_WINTER_BTN;
  tft.fillRoundRect(btnSeason.x, btnSeason.y, btnSeason.w, btnSeason.h, 10, col);
  if (summer) iconSnow(btnSeason.x + btnSeason.w / 2, btnSeason.y + 40, C_WHITE);
  else        iconFlame(btnSeason.x + btnSeason.w / 2, btnSeason.y + 40, C_WHITE);
  tft.setTextColor(C_WHITE, col);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(summer ? "SUMMER" : "WINTER", btnSeason.x + btnSeason.w / 2, btnSeason.y + 78, 2);
}

void updateHeaderInfo() {
  // Σήμα + LED: μόνο όταν αλλάζει RSSI ή comms
  if (gForce || dRssi != rssi || dComms != (int)commsOk) {
    dRssi = rssi; dComms = (int)commsOk;
    tft.fillRect(396, 8, 84, 28, C_HEAD);
    iconBars(396, 8, rssi);
    tft.fillCircle(452, 21, 9, commsOk ? C_GREEN : C_RED);   // πράσινο = ζωντανή σύνδεση
    tft.drawCircle(452, 21, 9, C_WHITE);
  }
  // Ώρα/ημερομηνία (από hub NTP): μόνο όταν αλλάζει το λεπτό (όχι κάθε δευτ.)
  char tb[16];
  if (timeOk) snprintf(tb, sizeof(tb), "%02d/%02d %02d:%02d", tDD, tMO, tHH, tMM);
  else        snprintf(tb, sizeof(tb), "--:--");
  if (gForce || strcmp(tb, dTime) != 0) {
    strncpy(dTime, tb, sizeof(dTime) - 1);
    tft.fillRect(150, 6, 180, 32, C_HEAD);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(timeOk ? C_WHITE : C_GREY, C_HEAD);
    tft.drawString(tb, 240, 21, 4);        // ημ/ώρα: μεγαλύτερη + έντονη (bold)
    tft.drawString(tb, 241, 21, 4);        // overprint +1px -> εφέ bold
  }
}

// Ένδειξη κατάστασης (αριστερά στο header): γιατί OFF / τι κάνει — με χρώμα
void updateStatusText() {
  if (!gForce && ctrlStatus == dCtrlStatus) return;
  dCtrlStatus = ctrlStatus;
  const char* t; uint16_t col;
  switch (ctrlStatus) {
    case 1: t = "HEATING";   col = C_HOT;    break;
    case 2: t = "COOLING";   col = C_COLD;   break;
    case 3: t = "WAIT HOT";  col = C_YELLOW; break;
    case 4: t = "WAIT COLD"; col = C_YELLOW; break;
    case 5: t = "FROST";     col = C_COLD;   break;
    case 6: t = "SAFETY";    col = C_RED;    break;
    case 7: t = "SYS OFF";   col = C_RED;    break;   // master OFF (/off)
    default: t = "OFF";      col = C_GREY;   break;   // 0 = ικανοποιήθηκε
  }
  tft.fillRect(8, 6, 138, 30, C_HEAD);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(col, C_HEAD);
  tft.drawString(t, 14, 21, 4);
}

void drawLayout() {
  tft.fillScreen(C_BG);
  drawHeader();
  // Αριστερό card: ROOM (μεγάλο)
  tft.fillRoundRect(12, 50, 226, 148, 8, C_PANEL);
  tft.setTextColor(C_GREY, C_PANEL);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("ROOM", 26, 66, 2);
  // Δεξί card: στατιστικά
  tft.fillRoundRect(250, 50, 218, 148, 8, C_PANEL);
  // Κάτω αριστερά: SETPOINT (μεγάλα κουμπιά −/+)
  tft.fillRoundRect(12, 206, 338, 98, 8, C_PANEL);
  tft.setTextColor(C_GREY, C_PANEL);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("SET POINT", 26, 218, 2);
  tft.fillRoundRect(btnMinus.x, btnMinus.y, btnMinus.w, btnMinus.h, 8, C_DARK);
  tft.fillRoundRect(btnPlus.x,  btnPlus.y,  btnPlus.w,  btnPlus.h,  8, C_DARK);
  // Σύμβολα −/+ minimal (λεπτές γραμμές· η font6 δεν έχει '+')
  int mcx = btnMinus.x + btnMinus.w / 2, mcy = btnMinus.y + btnMinus.h / 2;
  int pcx = btnPlus.x  + btnPlus.w  / 2, pcy = btnPlus.y  + btnPlus.h  / 2;
  tft.fillRect(mcx - 15, mcy - 2, 30, 4, C_GREY);
  tft.fillRect(pcx - 15, pcy - 2, 30, 4, C_GREY);
  tft.fillRect(pcx - 2,  pcy - 15, 4, 30, C_GREY);
}

// ================= ΣΕΛΙΔΑ ΚΑΙΡΟΥ =================
int weekday(int y, int m, int d) {   // Sakamoto· 0=Κυριακή
  static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y -= 1;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}
const char* wxDesc(int c) {
  if (c==0) return "Clear";
  if (c==1) return "Mostly clear";
  if (c==2) return "Partly cloudy";
  if (c==3) return "Overcast";
  if (c==45||c==48) return "Fog";
  if (c>=51&&c<=57) return "Drizzle";
  if (c>=61&&c<=65) return "Rain";
  if (c==66||c==67) return "Freezing rain";
  if (c>=71&&c<=77) return "Snow";
  if (c>=80&&c<=82) return "Showers";
  if (c==85||c==86) return "Snow showers";
  if (c>=95) return "Thunderstorm";
  return "--";
}
void drawSun(int cx, int cy, int r, uint16_t col) {
  for (int i = 0; i < 8; i++) { float a = i*PI/4;
    tft.drawLine(cx+cos(a)*(r+3), cy+sin(a)*(r+3), cx+cos(a)*(r+8), cy+sin(a)*(r+8), col);
    tft.drawLine(cx+cos(a)*(r+3)+1, cy+sin(a)*(r+3), cx+cos(a)*(r+8)+1, cy+sin(a)*(r+8), col); }
  tft.fillCircle(cx, cy, r, col);
}
void drawCloud(int cx, int cy, int r, uint16_t col) {
  tft.fillCircle(cx - r, cy, r*0.62, col);
  tft.fillCircle(cx + r, cy, r*0.72, col);
  tft.fillCircle(cx, cy - r*0.5, r*0.85, col);
  tft.fillRoundRect(cx - r*1.5, cy - r*0.15, r*3, r*0.95, r*0.45, col);
}
void drawWxIcon(int cx, int cy, int sz, int code) {
  const uint16_t SUN=C_YELLOW, CLOUD=0xC618, RAIN=C_COLD, SNOW=C_WHITE, BOLT=C_YELLOW;
  if (code==0 || code==1) { drawSun(cx, cy, sz, SUN); return; }
  if (code==2) { drawSun(cx-sz*0.5, cy-sz*0.5, sz*0.6, SUN); drawCloud(cx+sz*0.2, cy+sz*0.35, sz*0.75, CLOUD); return; }
  drawCloud(cx, cy - sz*0.15, sz*0.9, CLOUD);
  int by = cy + sz*0.6;
  if (code==45||code==48) { for (int i=0;i<3;i++) tft.drawFastHLine(cx-sz*0.8, by+i*5, sz*1.6, C_GREY); }
  else if ((code>=51&&code<=67)||(code>=80&&code<=82)) { for (int i=-1;i<=1;i++) tft.drawLine(cx+i*sz*0.5, by, cx+i*sz*0.5-sz*0.25, by+sz*0.6, RAIN); }
  else if ((code>=71&&code<=77)||code==85||code==86) { for (int i=-1;i<=1;i++) tft.fillCircle(cx+i*sz*0.5, by+sz*0.2, 2, SNOW); }
  else if (code>=95) { tft.fillTriangle(cx, by-2, cx-sz*0.35, by+sz*0.7, cx+sz*0.05, by+sz*0.25, BOLT);
                       tft.fillTriangle(cx+sz*0.05, by+sz*0.25, cx+sz*0.35, by-sz*0.05, cx, by+sz*0.55, BOLT); }
}
void drawPageDots() {
  int y = 313, gap = 16, n = 3, x0 = SCR_W/2 - (n-1)*gap/2;
  for (int i = 0; i < n; i++) {
    if (i == page) tft.fillCircle(x0 + i*gap, y, 4, C_WHITE);
    else { tft.fillCircle(x0 + i*gap, y, 4, C_BG); tft.drawCircle(x0 + i*gap, y, 4, C_GREY); }
  }
}
void drawWeatherPage() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 480, 42, C_HEAD);
  tft.setTextDatum(ML_DATUM); tft.setTextColor(C_WHITE, C_HEAD);
  tft.drawString("WEATHER", 14, 21, 4);
  drawLock();
  if (!wx.valid) {
    tft.setTextDatum(MC_DATUM); tft.setTextColor(C_GREY, C_BG);
    tft.drawString("Loading weather...", 240, 170, 4);
    drawPageDots(); return;
  }
  // --- ΤΩΡΑ ---
  drawWxIcon(80, 112, 30, wx.code);
  tft.setTextDatum(ML_DATUM); tft.setTextColor(C_WHITE, C_BG);
  char tb[8]; snprintf(tb, sizeof(tb), "%d", (int)lroundf(wx.t));
  tft.drawString(tb, 150, 92, 6);
  int tw = tft.textWidth(tb, 6);
  tft.drawCircle(150+tw+12, 72, 5, C_WHITE); tft.drawCircle(150+tw+12, 72, 4, C_WHITE);
  tft.setTextColor(C_OUT, C_BG);
  tft.drawString(wxDesc(wx.code), 150, 138, 4);
  char sub[40];
  snprintf(sub, sizeof(sub), "Hum %d%%   Wind %d km/h", (int)lroundf(wx.h), (int)lroundf(wx.w));
  tft.setTextColor(C_GREY, C_BG); tft.drawString(sub, 150, 164, 2);
  snprintf(sub, sizeof(sub), "Feels %d   Rain %d%%", (int)lroundf(wx.feels), wx.fProb[0]);
  tft.setTextColor(C_COLD, C_BG); tft.drawString(sub, 150, 182, 2);
  tft.drawFastHLine(20, 200, 440, C_PANEL2);
  // --- ΠΡΟΒΛΕΨΗ 4 ημερών ---
  int cx0[4] = {60, 180, 300, 420};
  for (int i = 0; i < 4; i++) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_WHITE, C_BG);
    tft.drawString(wx.fDay[i], cx0[i], 212, 2);
    drawWxIcon(cx0[i], 244, 15, wx.fCode[i]);
    char hl[16]; snprintf(hl, sizeof(hl), "%d/%d", (int)lroundf(wx.fHi[i]), (int)lroundf(wx.fLo[i]));
    tft.setTextColor(C_GREY, C_BG);
    tft.drawString(hl, cx0[i], 276, 2);
    if (wx.fProb[i] > 0) {
      char pr[8]; snprintf(pr, sizeof(pr), "%d%%", wx.fProb[i]);
      tft.setTextColor(C_COLD, C_BG);
      tft.drawString(pr, cx0[i], 296, 2);
    }
  }
  drawPageDots();
}

// ================= ΣΕΛΙΔΑ ΕΝΕΡΓΕΙΑΣ =================
void updateEnergyPage() {   // ενημερώνει μόνο τις τιμές που άλλαξαν (anti-flicker)
  char b[24];
  static float cN = -1e9, cD = -1e9, cM = -1e9, cT = -1e9;
  float now = isnan(pumpW) ? 0 : pumpW;
  if (gForce || fabs(now - cN) > 8) {
    cN = now;
    tft.fillRect(80, 58, 232, 40, C_PANEL);
    tft.setTextDatum(ML_DATUM); tft.setTextColor(C_HOT, C_PANEL);
    snprintf(b, sizeof(b), "%d W", (int)lroundf(now)); tft.drawString(b, 90, 78, 4);
    tft.fillRect(314, 58, 148, 40, C_PANEL);
    tft.setTextDatum(MR_DATUM); tft.setTextColor(C_YELLOW, C_PANEL);
    snprintf(b, sizeof(b), "%.2f EUR/h", now / 1000.0 * eurKwh); tft.drawString(b, 456, 78, 4);
  }
  float kwh[3] = {isnan(kwhDay)?0:kwhDay, isnan(kwhMonth)?0:kwhMonth, isnan(kwhTotal)?0:kwhTotal};
  float* cc[3] = {&cD, &cM, &cT};
  for (int i = 0; i < 3; i++) {
    if (gForce || fabs(kwh[i] - *cc[i]) > 0.05) {
      *cc[i] = kwh[i]; int y = 116 + i*54;
      tft.fillRect(130, y+4, 332, 40, C_PANEL);
      tft.setTextDatum(MC_DATUM); tft.setTextColor(C_WHITE, C_PANEL);
      snprintf(b, sizeof(b), "%.1f kWh", kwh[i]); tft.drawString(b, 250, y+24, 4);
      tft.setTextDatum(MR_DATUM); tft.setTextColor(C_YELLOW, C_PANEL);
      snprintf(b, sizeof(b), "%.2f EUR", kwh[i] * eurKwh); tft.drawString(b, 452, y+24, 4);
    }
  }
}
void drawEnergyPage() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 480, 42, C_HEAD);
  tft.setTextDatum(ML_DATUM); tft.setTextColor(C_WHITE, C_HEAD);
  tft.drawString("ENERGY", 14, 21, 4);
  drawLock();
  tft.fillRoundRect(12, 50, 456, 56, 8, C_PANEL);            // NOW card
  tft.setTextDatum(ML_DATUM); tft.setTextColor(C_GREY, C_PANEL);
  tft.drawString("NOW", 28, 78, 2);
  const char* lbl[3] = {"TODAY", "MONTH", "TOTAL"};          // 3 γραμμές
  for (int i = 0; i < 3; i++) {
    int y = 116 + i*54;
    tft.fillRoundRect(12, y, 456, 48, 8, C_PANEL);
    tft.setTextDatum(ML_DATUM); tft.setTextColor(C_OUT, C_PANEL);
    tft.drawString(lbl[i], 28, y+24, 4);
  }
  char b[24];
  tft.setTextDatum(MC_DATUM); tft.setTextColor(C_GREY, C_BG);
  snprintf(b, sizeof(b), "@ %.3f EUR/kWh", eurKwh);
  tft.drawString(b, 240, 292, 2);
  updateEnergyPage();   // τιμές (gForce ενεργό από το redrawAll)
  drawPageDots();
}

void redrawAll() {
  gForce = true;   // μετά από πλήρες καθάρισμα -> ζωγράφισε τα πάντα
  if (page == 1) {
    drawWeatherPage();
    updateHeaderInfo();              // ώρα + RSSI (κοινά στο header)
  } else if (page == 2) {
    drawEnergyPage();
    updateHeaderInfo();
  } else {
    drawLayout(); updateRoom(); updateSetpoint(); updateStats(); updateSeason(); updateHeaderInfo(); updateStatusText();
    drawPageDots();
  }
  gForce = false;
}

void drawConfirm() {
  pendingSummer = !summer;
  tft.fillRoundRect(70, 70, 340, 182, 12, C_PANEL2);
  tft.drawRoundRect(70, 70, 340, 182, 12, C_WHITE);
  tft.drawRoundRect(71, 71, 338, 180, 12, C_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_WHITE, C_PANEL2);
  tft.drawString("Change season?", 240, 100, 4);
  tft.setTextColor(pendingSummer ? C_SUMMER_BTN : C_WINTER_BTN, C_PANEL2);
  tft.drawString(pendingSummer ? "to SUMMER" : "to WINTER", 240, 138, 4);
  tft.fillRoundRect(btnYes.x, btnYes.y, btnYes.w, btnYes.h, 8, C_GREEN);
  tft.fillRoundRect(btnNo.x,  btnNo.y,  btnNo.w,  btnNo.h,  8, C_RED);
  tft.setTextColor(C_WHITE);
  tft.drawString("YES", btnYes.x + btnYes.w / 2, btnYes.y + btnYes.h / 2, 4);
  tft.drawString("NO",  btnNo.x + btnNo.w / 2,  btnNo.y + btnNo.h / 2,  4);
  confirmOpen = true;
}

// 2ο dialog (μόνο όταν πάμε σε ΚΑΛΟΚΑΙΡΙ): ενδοδαπέδια ψύξη ΝΑΙ/ΟΧΙ
void drawFloorConfirm() {
  tft.fillRoundRect(70, 70, 340, 182, 12, C_PANEL2);
  tft.drawRoundRect(70, 70, 340, 182, 12, C_WHITE);
  tft.drawRoundRect(71, 71, 338, 180, 12, C_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_WHITE, C_PANEL2);
  tft.drawString("Floor cooling?", 240, 98, 4);
  tft.setTextColor(C_GREY, C_PANEL2);
  tft.drawString("YES = floor + fancoil", 240, 128, 2);
  tft.drawString("NO = fancoil only", 240, 150, 2);
  tft.fillRoundRect(btnYes.x, btnYes.y, btnYes.w, btnYes.h, 8, C_GREEN);
  tft.fillRoundRect(btnNo.x,  btnNo.y,  btnNo.w,  btnNo.h,  8, C_SUMMER_BTN);
  tft.setTextColor(C_WHITE);
  tft.drawString("YES", btnYes.x + btnYes.w / 2, btnYes.y + btnYes.h / 2, 4);
  tft.drawString("NO",  btnNo.x + btnNo.w / 2,  btnNo.y + btnNo.h / 2,  4);
  floorDialogOpen = true;
}

// ---- UART: στείλε την εντολή του χρήστη (setpoint/εποχή/floorcool) στον κόμβο σπιτιού ----
void sendCmd() {
  uart_link::CommandPacket c;
  c.setpoint = uart_link::encT(setpoint);
  c.season   = summer ? uart_link::SEASON_SUMMER : uart_link::SEASON_WINTER;
  c.flags    = fancoilOnly ? uart_link::CMD_FANCOIL_ONLY : 0;
  if ((uint32_t)hubIP)        // ξέρουμε το IP του hub; -> στείλε 3× για σιγουριά (UDP)
    for (int i = 0; i < 3; i++) uart_link::udpSend(udp, hubIP, uart_link::UDP_CMD_PORT, c);
  lastLocalEdit = millis();   // "φρέσκια" τοπική αλλαγή -> μην την πατήσει το επόμενο πακέτο
}

// ---- UART: εφάρμοσε τιμές που ήρθαν από το σπίτι ----
void applyDisplay(const uart_link::DisplayPacket& dp) {
  roomTemp = uart_link::decT(dp.room);
  hotTop   = uart_link::decT(dp.hotTop);
  outTemp  = uart_link::decT(dp.outdoor);
  coldBuf  = uart_link::decT(dp.cold);
  valveOut = uart_link::decT(dp.valve);
  rssi     = dp.rssi;
  commsOk  = dp.flags & uart_link::F_COMMS_OK;
  pumpW    = (float)dp.pumpW;                       // κατανάλωση αντλίας θερμότητας
  humidity = (dp.humidity == 255) ? NAN : (float)dp.humidity;   // %RH δωματίου (SHT40)
  ctrlStatus = dp.ctrlStatus;                      // κατάσταση ελέγχου του S3
  kwhDay = dp.kwhDay; kwhMonth = dp.kwhMonth; kwhTotal = dp.kwhTotal; eurKwh = dp.eurKwh;   // ενέργεια
  timeOk   = dp.flags & uart_link::F_TIME_OK;
  if (timeOk) { tHH = dp.hh; tMM = dp.mm; tDD = dp.dd; tMO = dp.mo; }
  haveData = true;
  lastPacket = millis();

  // setpoint/εποχή: υιοθέτησέ τα από το σπίτι (π.χ. αλλαγή από Telegram/web)
  // ΜΟΝΟ αν δεν μόλις τα άλλαξε ο χρήστης τοπικά (αλλιώς θα "παλεύαμε").
  if (millis() - lastLocalEdit > 4000) {
    fancoilOnly = dp.flags & uart_link::F_FANCOIL_ONLY;   // υιοθέτησε από hub (π.χ. /floorcool)
    float sp = uart_link::decT(dp.setpoint);
    if (!isnan(sp) && fabs(sp - setpoint) > 0.01) {
      setpoint = sp;
      if (!confirmOpen && !floorDialogOpen && !toastUntil) updateSetpoint();
    }
    bool s = (dp.season == uart_link::SEASON_SUMMER);
    if (s != summer && !confirmOpen && !floorDialogOpen) {
      summer = s;
      if (page == 0 && !toastUntil) { redrawAll(); return; }   // άλλαξε header+κουμπί -> πλήρες redraw
    }
  }
  if (!confirmOpen && !floorDialogOpen && !toastUntil) { if (page == 0) { updateRoom(); updateStats(); updateStatusText(); } else if (page == 2) updateEnergyPage(); updateHeaderInfo(); }
}

// Κατεβάζει τρέχοντα καιρό + πρόβλεψη 4 ημερών (Open-Meteo). true αν πέτυχε.
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" WX_LAT "&longitude=" WX_LON
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
               "&timezone=auto&forecast_days=4";
  http.setConnectTimeout(4000);
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    DynamicJsonDocument doc(6144);
    String payload = http.getString();                       // αποκωδικοποιεί σωστά (chunked/content-length)
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      JsonObject cur = doc["current"];
      wx.t = cur["temperature_2m"] | NAN;
      wx.h = cur["relative_humidity_2m"] | NAN;
      wx.code = cur["weather_code"] | -1;
      wx.w = cur["wind_speed_10m"] | NAN;
      wx.feels = cur["apparent_temperature"] | NAN;
      JsonObject d = doc["daily"];
      static const char* DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
      for (int i = 0; i < 4; i++) {
        wx.fCode[i] = d["weather_code"][i] | -1;
        wx.fHi[i]   = d["temperature_2m_max"][i] | NAN;
        wx.fLo[i]   = d["temperature_2m_min"][i] | NAN;
        wx.fProb[i] = d["precipitation_probability_max"][i] | 0;
        const char* ds = d["time"][i] | "";
        int yy, mm, dd;
        if (i == 0) strcpy(wx.fDay[i], "Today");
        else if (sscanf(ds, "%d-%d-%d", &yy, &mm, &dd) == 3) { strncpy(wx.fDay[i], DOW[weekday(yy,mm,dd)], 5); wx.fDay[i][5] = 0; }
        else strcpy(wx.fDay[i], "--");
      }
      wx.valid = true; wx.fetched = millis(); ok = true;
      if (page == 1) drawWeatherPage();    // αν βλέπουμε ήδη τον καιρό -> ανανέωσε
      Serial.printf("[WX] %.1fC %d%% wind=%.0f code=%d | fc %d/%d %d/%d %d/%d %d/%d\n",
        wx.t, (int)wx.h, wx.w, wx.code,
        (int)wx.fHi[0],(int)wx.fLo[0], (int)wx.fHi[1],(int)wx.fLo[1],
        (int)wx.fHi[2],(int)wx.fLo[2], (int)wx.fHi[3],(int)wx.fLo[3]);
    } else Serial.printf("[WX] JSON err: %s\n", err.c_str());
  } else Serial.printf("[WX] HTTP %d\n", code);
  http.end();
  return ok;
}

void setup() {
  Serial.begin(115200);     // USB debug (ελεύθερο πάλι, αφού δεν χρησιμοποιούμε UART0)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);       // ίδιο δίκτυο με τον hub (secrets.h)
  udp.begin(uart_link::UDP_DISP_PORT);    // ακούει τις τιμές από τον hub (broadcast)
  tft.init();
  tft.setRotation(3);   // 180° (κουτί ανάποδα για σωστό αερισμό αισθητήρα)
  // Backlight PWM ΜΕΤΑ το tft.init() — αλλιώς το TFT_eSPI ξαναπαίρνει το pin 27
  // (το κρατούσε σταθερό HIGH) και το dimming δεν δούλευε.
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(BL_PIN, BL_CH);
  setBrightness(BL_FULL);
  lastActivity = millis();
  redrawAll();
  Serial.println("CYD 3.5 dashboard ready (WiFi/UDP link)");
}

void loop() {
  // --- UDP: τιμές από τον hub (WiFi) ---
  {
    int n = udp.parsePacket();
    uart_link::DisplayPacket dp;
    if (n > 0 && uart_link::udpRecv(udp, n, dp)) {
      hubIP = udp.remoteIP();        // μάθε το IP του hub (για αποστολή εντολών)
      applyDisplay(dp);
    }
  }

  // --- Καιρός: 1η λήψη ~8s μετά το boot, μετά κάθε 15' (1' αν αποτύχει) ---
  static unsigned long lastWx = 0;
  static unsigned long wxInterval = 8000;
  if (WiFi.status() == WL_CONNECTED && millis() - lastWx >= wxInterval) {
    lastWx = millis();
    wxInterval = fetchWeather() ? (15UL * 60 * 1000) : 60000;
  }

  // --- Watchdog UART: αν κοπεί η σύνδεση -> "--" αντί για παλιές τιμές ---
  if (haveData && millis() - lastPacket > LINK_TIMEOUT) {
    haveData = false; commsOk = false;
    roomTemp = hotTop = outTemp = coldBuf = valveOut = NAN; pumpW = NAN; humidity = NAN; ctrlStatus = 0;
    kwhDay = kwhMonth = kwhTotal = NAN;
    if (!confirmOpen && !floorDialogOpen && !toastUntil) { if (page == 0) { updateRoom(); updateStats(); updateStatusText(); } else if (page == 2) updateEnergyPage(); updateHeaderInfo(); }
  }

  int tx, ty;
  static int startX = -1, startY = 0, lastX = 0;
  static unsigned long lastTap = 0;
  if (getTouchPt(tx, ty)) {
    lastActivity = millis(); lastTouchMs = millis();
    if (dimmed) { setBrightness(BL_FULL); dimmed = false; }   // επαναφορά φωτεινότητας
    if (startX < 0) { startX = tx; startY = ty; }             // αρχή νέου αγγίγματος
    lastX = tx;
    // Κλείδωμα: παρατεταμένο πάτημα στο εικονίδιο (κατά τη διάρκεια)
    if (inBtn(btnLock, startX, startY)) {
      if (holdStart == 0) holdStart = millis();
      if (!holdHandled && millis() - holdStart > LOCK_HOLD) {
        locked = !locked; holdHandled = true; drawLock();
        showToast(locked ? "LOCKED" : "UNLOCKED");
      }
    } else holdStart = 0;
  }
  // Απελευθέρωση (ανοχή 120ms στο τρεμόπαιγμα της resistive αφής) -> swipe ή tap;
  if (startX >= 0 && millis() - lastTouchMs > 120) {
    int dx = lastX - startX;
    bool dialog = confirmOpen || floorDialogOpen;
    if (!holdHandled && !locked && abs(dx) > 70 && !dialog) {          // SWIPE -> αλλαγή σελίδας
      int np = constrain(page + (dx < 0 ? 1 : -1), 0, 2);             // αριστερά = επόμενη, δεξιά = προηγούμενη
      if (np != page) { page = np; redrawAll(); }
    } else if (!holdHandled && !locked && millis() - lastTap > 200) {  // TAP -> κουμπί στη θέση εκκίνησης
      lastTap = millis();
      int bx = startX, by = startY;
      if (confirmOpen) {
        if (inBtn(btnYes, bx, by)) {
          if (pendingSummer) { confirmOpen = false; drawFloorConfirm(); }
          else { summer = pendingSummer; confirmOpen = false; redrawAll(); sendCmd(); }
        } else if (inBtn(btnNo, bx, by)) { confirmOpen = false; redrawAll(); }
      } else if (floorDialogOpen) {
        if (inBtn(btnYes, bx, by))     { fancoilOnly = false; summer = pendingSummer; floorDialogOpen = false; redrawAll(); sendCmd(); }
        else if (inBtn(btnNo, bx, by)) { fancoilOnly = true;  summer = pendingSummer; floorDialogOpen = false; redrawAll(); sendCmd(); }
      } else if (page == 0) {           // κουμπιά μόνο στη σελίδα θέρμανσης
        if (inBtn(btnMinus, bx, by)) { setpoint -= 0.5; if (setpoint < 5)  setpoint = 5;  updateSetpoint(); sendCmd(); }
        else if (inBtn(btnPlus, bx, by)) { setpoint += 0.5; if (setpoint > 30) setpoint = 30; updateSetpoint(); sendCmd(); }
        else if (inBtn(btnSeason, bx, by)) { drawConfirm(); }
      }
    }
    startX = -1; holdStart = 0; holdHandled = false;       // reset για επόμενο άγγιγμα
  }

  // Καθάρισμα toast
  if (toastUntil && millis() > toastUntil) { toastUntil = 0; redrawAll(); }

  // Μετά από αδράνεια: χαμήλωμα φωτεινότητας 30% + αυτόματο κλείδωμα (γονικό)
  if (!dimmed && millis() - lastActivity > SCREEN_TIMEOUT) {
    setBrightness(BL_DIM); dimmed = true;
    if (!locked) { locked = true; drawLock(); }
  }
}
