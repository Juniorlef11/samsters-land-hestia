// ============================================================================
//  HESTIA — Google Apps Script: δέκτης logging από τον hub (ESP32)
// ----------------------------------------------------------------------------
//  1. Νέο Google Sheet -> Extensions -> Apps Script -> επικόλλησε ΟΛΟ αυτό
//  2. (προαιρετικά) βάλε το EMAIL σου για ημερήσια σύνοψη
//  3. Deploy -> New deployment -> Web app -> Execute as: me, Access: Anyone
//  4. Αντίγραψε το /exec URL -> βάλ' το στο SHEETS_URL του hub (main.cpp)
//  5. File -> Project Settings -> Time zone: (GMT+02:00) Athens
// ============================================================================

const SHEET_NAME = 'Log';
const EMAIL      = '';            // π.χ. 'papantoniou@inforder.eu' για ημερήσια σύνοψη (κενό = off)
const PRICE      = 0.15;          // €/kWh για το κόστος στη σύνοψη

const HEADER = ['ts','hotTop','outdoor','cold','supply','powerW',
                'room','rh','setpoint','dew','status','season','rssi','kwhDay'];

function doGet(e)  { return handle(e); }
function doPost(e) { return handle(e); }

function handle(e) {
  const p = (e && e.parameter) || {};
  // --- ΔΙΑΒΑΣΜΑ (δεν προσθέτει γραμμή): ?read=today | ?read=all | ?read=N (τελευταίες N) ---
  if (p.read) return readData(p.read);

  const lock = LockService.getScriptLock();
  lock.tryLock(10000);
  try {
    const ss = SpreadsheetApp.getActiveSpreadsheet();
    let sh = ss.getSheetByName(SHEET_NAME);
    if (!sh) { sh = ss.insertSheet(SHEET_NAME); sh.appendRow(HEADER); sh.setFrozenRows(1); }
    sh.appendRow([
      new Date(),
      n(p.hotTop), n(p.outdoor), n(p.cold), n(p.supply), n(p.power),
      n(p.room), n(p.rh), n(p.setpoint), n(p.dew),
      p.status || '', p.season || '', n(p.rssi), n(p.kwhDay)
    ]);
    return ContentService.createTextOutput('OK');
  } catch (err) {
    return ContentService.createTextOutput('ERR: ' + err);
  } finally {
    lock.releaseLock();
  }
}

// '' ή 'nan' -> κενό κελί· αλλιώς αριθμός
function n(v) { return (v === undefined || v === '' || v === 'nan') ? '' : Number(v); }

// --- Επιστρέφει δεδομένα ως CSV (read-only, ΔΕΝ γράφει) ---
//  ?read=today  = μόνο σημερινές γραμμές   ·   ?read=all = όλα   ·   ?read=200 = τελευταίες 200
function readData(mode) {
  const sh = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
  if (!sh) return csv([HEADER]);
  const data = sh.getDataRange().getValues();      // [0] = HEADER
  let rows = data.slice(1);
  if (mode === 'today') {
    const t0 = new Date(); t0.setHours(0,0,0,0);
    rows = rows.filter(r => new Date(r[0]) >= t0);
  } else if (mode !== 'all') {
    const nLast = parseInt(mode, 10);
    if (nLast > 0) rows = rows.slice(-nLast);
  }
  return csv([HEADER].concat(rows));
}

function csv(matrix) {
  const out = matrix.map(row => row.map(v => {
    if (v instanceof Date) return Utilities.formatDate(v, 'GMT+3', 'yyyy-MM-dd HH:mm');
    const s = String(v);
    return (s.indexOf(',') >= 0 || s.indexOf('"') >= 0) ? '"' + s.replace(/"/g,'""') + '"' : s;
  }).join(',')).join('\n');
  return ContentService.createTextOutput(out).setMimeType(ContentService.MimeType.CSV);
}

// ---------------------------------------------------------------------------
//  Ημερήσια σύνοψη με email (προαιρετικό)
//  Triggers -> Add trigger -> dailySummary -> Time-driven -> Day timer -> 23:00-24:00
// ---------------------------------------------------------------------------
function dailySummary() {
  if (!EMAIL) return;
  const sh = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
  if (!sh) return;
  const data = sh.getDataRange().getValues();
  const today = new Date(); today.setHours(0,0,0,0);

  let kwh = 0, sumW = 0, maxW = 0, cnt = 0, minCold = 999, maxCold = -999;
  for (let i = 1; i < data.length; i++) {
    const ts = new Date(data[i][0]);
    if (ts < today) continue;
    const kd = Number(data[i][13]) || 0;   // kwhDay
    const w  = Number(data[i][5])  || 0;   // powerW
    const cold = Number(data[i][3]);        // cold buffer
    kwh = Math.max(kwh, kd);
    sumW += w; maxW = Math.max(maxW, w); cnt++;
    if (!isNaN(cold)) { minCold = Math.min(minCold, cold); maxCold = Math.max(maxCold, cold); }
  }
  if (!cnt) return;
  const avgW = Math.round(sumW / cnt);
  const cost = (kwh * PRICE).toFixed(2);
  const body =
    'HESTIA — Ημερήσια σύνοψη\n\n' +
    '⚡ Κατανάλωση: ' + kwh.toFixed(1) + ' kWh  (' + cost + ' €)\n' +
    '📊 Μ.Ο. ισχύος: ' + avgW + ' W   ·   Max: ' + maxW + ' W\n' +
    '❄️ Cold buffer: ' + minCold.toFixed(1) + '–' + maxCold.toFixed(1) + ' °C\n' +
    '🕒 Δείγματα: ' + cnt;
  MailApp.sendEmail(EMAIL, 'HESTIA — ' + kwh.toFixed(1) + ' kWh σήμερα (' + cost + '€)', body);
}
