# Lelit Mara X — Serielles Protokoll & Firmware-Unterschiede

> Dokumentation der UART-Schnittstelle der Mara X, wie sie der MaraX-Timer auswertet.
> Wichtigste Erkenntnis: **Nicht jede Firmware sendet den Pumpenstatus.**

## Anschluss

- TTL-UART, **9600 Baud**, 8N1, 3.3 V
- Die Maschine sendet alle **~400–500 ms** eine Zeile, abgeschlossen mit `\n`
- Der ESP32 stößt den Datenstrom mit dem Byte `0x11` an (alle ~5 s wiederholt, falls nichts kommt)
- ESP32-Pins: **RX = GPIO 16**, **TX = GPIO 17** (GPIO 12/14 funktionieren NICHT — GPIO 12 ist Strapping-Pin)

## Frame-Format

Beispiel: `C1.06,116,124,093,0840,1,0\n`

Die Felder sind **kommagetrennt**. Sie IMMER per Komma parsen — **nie über feste Zeichen-Positionen**, weil die Temperaturen je nach Zustand 3- oder 4-stellig sein können und sich die Offsets dann verschieben.

| Feld | Beispiel | Bedeutung |
|------|----------|-----------|
| [0]  | `C1.06`  | Modus (`C` = Kaffee, `V` = Dampf) + Firmware-Version |
| [1]  | `116`    | aktuelle Dampftemperatur (°C) |
| [2]  | `124`    | Ziel-Dampftemperatur (°C) — steigt im **Boost** beim Brühen |
| [3]  | `093`    | aktuelle HX-/Brühkreis-Temperatur (°C) |
| [4]  | `0840`   | Countdown für den Boost-Modus |
| [5]  | `1`      | Heizelement an/aus |
| [6]  | `0`      | **Pumpe an/aus** — ⚠️ **nur bei fw 1.06 vorhanden** |

## ⚠️ Firmware-Unterschied (wichtig!)

Nicht jede Mara-X-Firmware sendet alle Felder.

### fw 1.06 — **7 Felder, MIT Pumpenstatus**
```
C1.06,116,124,093,0840,1,0
                        ^ Feld [6] = Pumpe
```
Pumpenerkennung rein über die Schnittstelle möglich (so macht es z. B.
[SaibotFlow/marax-monitor](https://github.com/SaibotFlow/marax-monitor)). Kein Reed-Sensor nötig.

### fw 1.23 — **6 Felder, OHNE Pumpenstatus**
```
C1.23,121,112,100,0000,0
                       ^ Feld [5] = Heizung — danach ist SCHLUSS
```
Das Pumpenfeld `[6]` **fehlt komplett.** Über die Schnittstelle lässt sich der
Brühvorgang **nicht direkt** erkennen.

**Konsequenz:** Auf fw 1.23 (und vermutlich anderen neueren Versionen) ist für die
Shot-Erkennung ein **Reed-Sensor** (oder gleichwertiger Pumpensensor) an GPIO 18
nötig. Die serielle Schnittstelle liefert weiterhin die **Temperaturen**.

## Indirektes Boost-Signal (verifiziert am 2026-06-27, fw 1.23)

Beim Brühen reagieren zwei Felder, weil die Maschine intern den Bezug erkennt und
den Boost-Modus aktiviert:

| | Feld [2] (Dampf-Ziel) | Feld [5] (Heizung) | Feld [3] (HX) |
|---|---|---|---|
| Leerlauf | 112 | 0 | 100 |
| beim Brühen | **136** | **1** | 100 → 105 |

**Warum das für Shot-Timing nicht reicht:**
- Feld [5] (Heizung) taktet auch im Leerlauf → allein nicht aussagekräftig.
- Der Boost (Feld [2]) bleibt **nach** dem Shot noch aktiv (Kessel heizt nach),
  d. h. die gemessene Shot-Dauer wäre deutlich zu lang.

Deshalb: für präzise Zeiten → Reed-Sensor. Das Boost-Signal taugt höchstens als
grobe, ungenaue Heuristik.

## Wie der MaraX-Timer das umsetzt

- Parser: `applyMachineTelemetryFromLine()` trennt per Komma, akzeptiert 6 **und** 7 Felder.
  - Temperaturen aus [1]/[3], Heizung aus [5], Boost aus [4].
  - Pumpe (`g_serialPumpActive`) nur gesetzt, wenn Feld [6] existiert (`n >= 7`).
- Feldanzahl im Status sichtbar: `/api/status` → `frameFields` (6 = ohne Pumpe, 7 = mit Pumpe).
  Auch live auf der `/test`-Seite als Frame-Felder-Aufschlüsselung.
- Pump-Quelle umschaltbar (`g_useSerialPump`): **Reed** (Standard) oder **Seriell**.
- Reed-Polarität (`reedOpenSensor`) per `/test` umschaltbar und in NVS persistent —
  Schließer (NO) vs. Öffner (NC), kein Neu-Flashen nötig.

## Diagnose-Tipps

- **`/test`-Seite:** zeigt Roh-Frame, Feld-Aufschlüsselung mit Index, Pumpe, Temps,
  `serialAge` (Sekunden seit letztem UART-Byte). Bei „keine Daten" → RX/TX tauschen.
- **USB-Serial (9600 Baud):** Der ESP32 echot jede empfangene Zeile und gibt alle 5 s
  eine `[NET]`-Diagnosezeile aus (WLAN-Status, RSSI, IP, freier Heap, Fallback-AP).

---

## English summary

The Mara X sends a comma-separated telemetry line over UART (9600 baud) every ~400 ms.
**Crucially, the pump-state field is firmware-dependent:**

- **fw 1.06** → 7 fields, field `[6]` is pump on/off → pump detectable over serial, no reed needed.
- **fw 1.23** → only 6 fields, **no pump field** → a reed sensor (GPIO 18) is required for shot detection; serial still provides temperatures.

Always parse by comma, never by fixed character offsets (temperatures vary between 3 and 4 digits). On fw 1.23 the brew triggers an indirect "boost" signal (field [2] jumps, field [5]/heating turns on), but it lags and persists after the shot, so it's unsuitable for precise shot timing.
