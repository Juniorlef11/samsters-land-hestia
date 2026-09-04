# 🔥 Samster's Land HESTIA

[🇬🇧 English](README.md) · **🇬🇷 Ελληνικά**

Αυτόνομο (χωρίς cloud, χωρίς Home Assistant) σύστημα ελέγχου **θέρμανσης & δροσισμού** με
**LoRa point-to-point**, για μια **κοινή αντλία θερμότητας** που εξυπηρετεί πολλά σπίτια από
κοινό buffer. Χτισμένο σε **ESP32 / PlatformIO**.

Το σύστημα αναλαμβάνει έξυπνα την αντλία (takeover μέσω dry-contact), κρατά δυναμική
προσαγωγή με προστασία σημείου δρόσου, εξισορροπεί ηλιακή προτεραιότητα τον χειμώνα, και
καταγράφει τα πάντα σε Google Sheet + Telegram.

## 🧩 Αρχιτεκτονική

```mermaid
flowchart TD
    subgraph home["🏠 Σπίτι"]
        CYD["📟 cyd — οθόνη CYD"]
        HUB["📡 living_room — ESP32 hub<br/>θερμοστάτης · WiFi<br/>Telegram · logging Sheets"]
    end

    subgraph plant["🔧 Λεβητοστάσιο"]
        S3["🧠 levitostatio — ESP32-S3<br/>τρίοδη βάνα · κυκλοφορητής<br/>takeover αντλίας"]
        VALVE(["τρίοδη βάνα ανάμιξης"])
        BUF[("Κοινό buffer<br/>κρύο / ζεστό")]
        HP["♨️ Αντλία Emmeti<br/>(κοινή, πολλά σπίτια)"]
    end

    CYD <-->|UART| HUB
    HUB <-->|LoRa 868 MHz| S3
    S3 -->|επαφή 7-8| HP
    S3 --> VALVE
    HP --> BUF
    VALVE --> BUF
    BUF -->|προσαγωγή| home
```

| Environment | Board | Ρόλος |
|-------------|-------|-------|
| `levitostatio` | ESP32-S3 | Ελεγκτής λεβητοστασίου: τρίοδη βάνα, κυκλοφορητής, takeover αντλίας, αισθητήρες buffer |
| `living_room` | ESP32 dev | Κόμβος σπιτιού: θερμοστάτης, WiFi, Telegram, logging στο Sheet, γέφυρα UART προς CYD |
| `cyd` | CYD (ESP32 + οθόνη) | Οθόνη/χειριστήριο μέσω UART |

Επικοινωνία κόμβων: **LoRa 868 MHz** (LR1121 / RadioLib).

## ⚙️ Στήσιμο

```bash
# 1. Κλωνοποίηση
git clone https://github.com/Juniorlef11/samsters-land-hestia.git
cd samsters-land-hestia

# 2. Διαπιστευτήρια (ΥΠΟΧΡΕΩΤΙΚΟ — δεν ανεβαίνουν ποτέ)
cp include/secrets.h.example include/secrets.h
#   -> βάλε WiFi, Telegram token/chat id, (προαιρετικά) Google Sheets URL

# 3. Build / Upload ανά κόμβο
pio run -e levitostatio -t upload
pio run -e living_room  -t upload
pio run -e cyd          -t upload
```

Απαιτείται [PlatformIO](https://platformio.org/). Οι βιβλιοθήκες κατεβαίνουν αυτόματα.

## 📁 Δομή

```
src/levitostatio/   ελεγκτής λεβητοστασίου (S3)
src/living_room/    κόμβος σπιτιού / hub
src/cyd/            οθόνη
include/secrets.h   ΙΔΙΩΤΙΚΟ (gitignored) — από το .example
tools/              Google Apps Script + βοηθητικά scripts
docs/               τεχνικό εγχειρίδιο, σχέδιο PCB, checklist
```

## 📚 Τεκμηρίωση

- `docs/Samsters Land HESTIA - Manual.pdf` — τεχνικό εγχειρίδιο
- `docs/Baseboard_PCB_Design.md` — σχέδιο baseboard PCB
- `docs/Stage3_Tuning_Checklist.md` — λίστα κουρδίσματος

## 📄 Άδεια χρήσης

**PolyForm Noncommercial 1.0.0** — δείτε [`LICENSE`](LICENSE).

Ελεύθερο για μη-εμπορική χρήση (μελέτη, hobby, εκπαίδευση). **Η εμπορική χρήση
απαγορεύεται** χωρίς γραπτή άδεια. Για εμπορική αδειοδότηση, επικοινωνήστε με τον δημιουργό.

© 2026 Konstantinos Lefevr — Marathon, Greece.
