# 🔧 Samster's Land HESTIA — Baseboard PCB (Λεβητοστάσιο)

> **Πλακέτα-βάση χαμηλής τάσης (carrier).** Κουμπώνουν τα δοκιμασμένα modules· τα
> 230V/relays μένουν στο ΞΕΧΩΡΙΣΤΟ relay board. Όλη η πλακέτα = **3.3V/5V → ασφαλής,
> 2-layer, χωρίς θέματα creepage.** Σχεδίαση: Lefevr Konstantinos.

---

## 1. Φιλοσοφία
- **Δεν** ξανασχεδιάζουμε mains-switching (relay board) ή RF (LoRa module) — τα κρίσιμα/δύσκολα μένουν δοκιμασμένα modules.
- Η baseboard = «καθαρή καλωδίωση» + passives + κλέμες + σήμανση, αντί για jumper χάος.
- Τα modules μπαίνουν σε **θηλυκά headers** (αλλάζονται/επισκευάζονται).

## 2. Block diagram
```
            ┌──────────────────────── BASEBOARD (3.3V/5V) ────────────────────────┐
  5V DIN ──►│ [κλέμα] ─►[Schottky SS34]─► 5V → ESP32-S3 devkit (3V3 out → ραγα 3.3V)│
            │                                                                       │
   DS18B20 ►│ [κλέμα 3pin] + 4.7kΩ pull-up ───────────────────► GPIO4               │
   SCT-013 ►│ [κλέμα 2pin] + bias(2×10k+10µF) ─────────────────► GPIO5 (ADC)         │
            │                                                                       │
            │  ┌─ header: ESP32-S3 devkit ─┐   ┌─ header: LoRa Core1121-XF ─┐        │
            │  │  (όλα τα GPIO routing)     │◄─►│  SPI+CS+RST+BUSY+DIO9       │──► κεραία (edge)
            │  └───────────────────────────┘   └────────────────────────────┘        │
            │  ┌─ header 6p → RELAY BOARD ─┐   ┌─ header 4p → OLED ─┐                 │
            │  │ 5V,GND,IN1-4 (14/21/47/48)│   │ 3V3,GND,SCL16,SDA15│                 │
            │  └───────────────────────────┘   └────────────────────┘                 │
            └───────────────────────────────────────────────────────────────────────┘
  (USB-C του devkit στην ΑΚΡΗ → flash επί τόπου)
```

## 3. NETLIST — κάθε σύνδεση (S3 ↔ προορισμός)
| S3 GPIO | Πάει σε | Σημείωση |
|---|---|---|
| 10 | LoRa **CS** | header LoRa |
| 7  | LoRa **DIO9** | |
| 9  | LoRa **RST** | |
| 8  | LoRa **BUSY** | |
| 12 | LoRa **SCK** | |
| 13 | LoRa **MISO** | |
| 11 | LoRa **MOSI** | |
| 4  | **DS18B20 DATA** + 4.7kΩ→3.3V | bus (4 αισθητήρες) |
| 5  | **SCT** (σημείο A bias) | ADC1· bias 2×10k→1.65V + 10µF |
| 14 | Relay **IN1** (τρίοδη OPEN) | header relay |
| 21 | Relay **IN2** (τρίοδη CLOSE) | |
| 47 | Relay **IN3** (κυκλοφορητής) | |
| 48 | Relay **IN4** (εφεδρικό/ΖΝΧ) | |
| 15 | **OLED SDA** | SW-I2C |
| 16 | **OLED SCL** | |
| 3V3 | ράγα 3.3V → DS18B20, OLED, SCT bias, LoRa VCC | από τον regulator του devkit |
| 5V  | από Schottky (DIN) → 5V devkit + 5V relay board | |
| GND | κοινό | όλα τα GND |

## 4. Onboard εξαρτήματα (BOM) — THT για εύκολο κόλλημα
| Ref | Τιμή | Τύπος | Ρόλος |
|---|---|---|---|
| R1 | 4.7kΩ | THT 1/4W | pull-up DS18B20 (GPIO4↔3.3V) |
| R2, R3 | 10kΩ | THT 1/4W | διαιρέτης SCT (3.3V-Α-GND) |
| C1 | 10µF | ηλεκτρ. THT (Lelon) | bias SCT (Α↔GND, + στο Α) |
| C2 | 100nF | κεραμικός | decoupling 3.3V (κοντά στο LoRa) |
| C3 | 10µF | ηλεκτρ. | decoupling 3.3V rail |
| D1 | SS34 / 1N5822 | Schottky | OR-ing PSU 5V → devkit (flash επί τόπου) |
| LED1 + R4 | LED + 1kΩ | THT | ένδειξη τροφοδοσίας 3.3V |

## 5. Connectors
**Κλέμες πεδίου (screw terminals 3.5/5.08mm THT):**
| Κλέμα | Pins | Τι |
|---|---|---|
| J_PWR | 2 | 5V DIN (+ , −) |
| J_DS18B20 | 3 | 3.3V, DATA, GND (κοινός bus, 4 αισθητήρες) |
| J_SCT | 2 | τα 2 καλώδια του SCT (→ Α, → GPIO5) |

**Headers (2.54mm):**
| Header | Pins | Module |
|---|---|---|
| H_S3 | 2× σειρές (όσα του devkit) | ESP32-S3 devkit (θηλυκά) |
| H_LORA | κατά LoRa module | Core1121-XF (θηλυκό) |
| H_RELAY | 1×6 | 5V, GND, IN1, IN2, IN3, IN4 |
| H_OLED | 1×4 | 3V3, GND, SCL(16), SDA(15) |

## 6. Πλάνο διάταξης (layout)
- **Μέγεθος:** ~**90×90mm** (να χωρά στο ηλεκτρ. κουτί 10×10cm με περιθώριο)
- **2-layer**, πάχος 1.6mm, χαλκός 1oz (φθηνό στο PCBWay)
- **4 τρύπες M3** στις γωνίες (να ταιριάζουν με αποστάτες/βάση του κουτιού)
- **USB-C του devkit στην ΑΚΡΗ** της πλακέτας → flash χωρίς αφαίρεση
- **Κεραία LoRa** (SMA/IPEX) στην άκρη → έξω από το κουτί
- **Κλέμες όλες στη μία πλευρά** (πεδίο/καλώδια από κάτω, όπως οι τρύπες του κουτιού)
- Ράγα **GND** φαρδιά (ground pour και στις 2 πλευρές)
- LoRa: κοντά decoupling C2, μικρές διαδρομές SPI
- Σήμανση (silkscreen): όνομα κάθε κλέμας/header + «HESTIA Baseboard v1 — L.Konstantinos»

> ⚠️ **Καμία 230V πάνω στη baseboard.** Τα relay outputs/230V είναι στο relay board (συνδέεται με τον έξω κόσμο μόνο εκείνο). Έτσι η baseboard είναι 100% SELV.

## 7. Τροφοδοσία
```
5V DIN (κλέμα) ──►|── (D1 Schottky) ──┬─► 5V/VIN devkit
                  (ρίγα→εδώ)          └─► 5V relay board (H_RELAY)
devkit 3V3 out ───────────────────────► ράγα 3.3V (DS18B20, OLED, SCT bias, LoRa)
GND ── κοινό παντού
```
- Η D1 αφήνει να βάλεις **USB στο devkit για flash** χωρίς σύγκρουση με το PSU.
- Το 3.3V βγαίνει από τον regulator του **devkit** (δεν βάζουμε δικό μας regulator v1).

## 8. Από τη σχεδίαση → Gerbers → PCBWay (EasyEDA)
1. **easyeda.com** → λογαριασμός → New Project
2. **Schematic**: ρίξε τα symbols (headers, R, C, D, terminals) και σύνδεσέ τα **ακριβώς κατά το §3 netlist**
   - Headers: «Header-Female-2.54» κ.λπ. από τη βιβλιοθήκη
   - Για ESP32-S3 devkit / LoRa: ψάξε στο **LCSC/EasyEDA library** ή βάλε γενικά headers με τον σωστό αριθμό pins
3. **Convert to PCB** → τοποθέτησε εξαρτήματα κατά το §6 → όρισε **board outline 90×90** + 4 τρύπες M3
4. **Route** (auto-route αρκεί για 2-layer LV· ή χειροκίνητα τα λίγα nets)
5. **Add ground pour** (GND) και στις 2 πλευρές
6. **Generate Gerber** (Fabrication Output) → κατέβασε το .zip
7. **pcbway.com** → Quote → ανέβασε το Gerber .zip → 2-layer, 1.6mm, HASL → παραγγελία
   - (Το PCBWay δέχεται και απευθείας **EasyEDA export**, ή έχει & **assembly service** αν θες κολλημένα τα passives)

## 9. TODO πριν την παραγγελία
- [ ] Επιβεβαίωση **διαστάσεων/pinout** ESP32-S3 devkit + LoRa module (footprints)
- [ ] Επιβεβαίωση **απόστασης τρυπών** του κουτιού 10×10 (να ταιριάξουν οι M3)
- [ ] Ολοκλήρωση **Σταδίου 3** (ώστε το PCB να αποτυπώνει το τελικό, δοκιμασμένο σχέδιο)
- [ ] Διπλός έλεγχος netlist §3 πριν το Gerber
