# HESTIA — Logging σε Google Sheets + Ανάλυση ρεύματος

Ο hub (ESP32) στέλνει κάθε **3 λεπτά** μία γραμμή δεδομένων σε ένα Google Sheet.
Από εκεί βγαίνουν γραφήματα για κατανάλωση, θερμοκρασίες, EER κ.λπ.

## 1. Στήσιμο (5 λεπτά)

1. Νέο **Google Sheet** (sheets.new).
2. **Extensions → Apps Script** → σβήσε ό,τι υπάρχει → επικόλλησε το `Code.gs`.
3. (προαιρετικά) βάλε το `EMAIL` σου για ημερήσια σύνοψη.
4. **Deploy → New deployment → Web app**
   - *Execute as*: **Me**
   - *Who has access*: **Anyone**
   - Copy το URL που τελειώνει σε `/exec`.
5. Στο `src/living_room/main.cpp`, βάλε το URL στο:
   ```cpp
   const char* SHEETS_URL = "https://script.google.com/macros/s/.../exec";
   ```
6. **File → Project Settings → Time zone → (GMT+02:00) Athens** (για σωστές ημερομηνίες).
7. Flash τον hub. Σε 3 λεπτά εμφανίζεται το πρώτο row στο tab **Log**.

> Το serial του hub γράφει `[SHEET] HTTP 200` όταν πετυχαίνει.

## 2. Στήλες που καταγράφονται

| Στήλη | Πεδίο | |
|-------|-------|---|
| A | ts | timestamp (Google) |
| B | hotTop | ζεστό buffer (πάνω) |
| C | outdoor | εξωτερική θερμοκρασία (`bufHotBot`) |
| D | cold | κρύο buffer (καλοκαίρι) |
| E | supply | προσαγωγή (έξοδος τρίοδης) |
| F | powerW | **ισχύς αντλίας (W)** — SCT-013 CT |
| G | room | θερμοκρασία δωματίου |
| H | rh | υγρασία % |
| I | setpoint | στόχος |
| J | dew | σημείο δρόσου |
| K | status | COOL / WAIT_COLD / HEAT / OFF … |
| L | season | summer / winter |
| M | rssi | σήμα LoRa |
| N | kwhDay | **ημερήσια kWh** (μετρητής hub, reset τα μεσάνυχτα) |

## 3. Ημερήσια ανάλυση (νέο tab «Daily»)

Σε άδειο κελί (π.χ. A1 στο tab «Daily»):

```
=QUERY(Log!A2:N,
 "select toDate(A), max(N), avg(F), max(F), avg(C), avg(D), avg(E)
  group by toDate(A) order by toDate(A) desc
  label toDate(A) 'Ημ/νία', max(N) 'kWh', avg(F) 'Μ.Ο. W', max(F) 'Max W',
        avg(C) 'Έξω °C', avg(D) 'Cold °C', avg(E) 'Supply °C'", 0)
```

- **kWh/μέρα** = `max(N)` → η τιμή του μετρητή του hub λίγο πριν το reset (ακριβής, ολοκλήρωση 10s).
- **Κόστος** → δίπλα: `= [kWh] * 0.15`
- Cross-check kWh (αν ο hub έκανε reboot μέσα στη μέρα): `avg(F) * 24 / 1000`.

## 4. Γραφήματα (Insert → Chart)

| Γράφημα | Δεδομένα | Δείχνει |
|---------|----------|---------|
| Ισχύς (W) στον χρόνο | Log!A (X) · Log!F (Y) | ήπιο ~1800 vs αιχμές 4700 |
| kWh ανά μέρα (μπάρες) | Daily: Ημ/νία · kWh | κατανάλωση/μέρα |
| Buffer στον χρόνο | Log!A · Log!D,B | κύκλοι + solar boost |
| kWh vs Έξω °C | Daily: Έξω °C (X) · kWh (Y) | εξάρτηση από καιρό |
| EER proxy | Log: cold (X) · powerW (Y) | αποδοτικότητα ανά θερμοκρασία |

## 5. Live μετρητές (οπουδήποτε)

```
Τρέχοντα W:   =INDEX(Log!F:F, COUNTA(Log!F:F))
kWh σήμερα:   =INDEX(Log!N:N, COUNTA(Log!N:N))
Κόστος σήμ.:  =INDEX(Log!N:N, COUNTA(Log!N:N)) * 0.15
```

## 6. Παλιά δεδομένα από Telegram (προαιρετικά)

Telegram Desktop → chat bot → ⋮ → **Export chat history** → JSON.
Στείλε το export και γράφεται parser σε CSV για να μπουν κι αυτά στο Sheet.
