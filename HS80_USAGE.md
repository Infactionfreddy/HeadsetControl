# Corsair HS80 RGB Wireless - HeadsetControl Integration

## Übersicht

Die Integration unterstützt folgende Corsair HS80 Modelle:
- **HS80 RGB USB** (0x0A69)
- **HS80 RGB Wireless** (0x0A6B)
- **HS80 RGB White USB** (0x0A71)
- **HS80 RGB White Wireless** (0x0A73)

## Unterstützte Funktionen

### 1. RGB-Beleuchtung (alle Modelle)
Das HS80 hat 3 RGB-Zonen:
- **Logo** - Corsair-Logo auf der Ohrmuschel
- **Power** - Power-LED
- **Mic** - Mikrofon-LED

#### Verwendung:
```bash
# LEDs einschalten (Standard: Corsair Blau)
./headsetcontrol -l 1

# LEDs ausschalten
./headsetcontrol -l 0
```

### 2. Akkustatus (nur Wireless)
Bei Wireless-Modellen kann der Akkustand und Ladestatus abgefragt werden.

#### Verwendung:
```bash
# Akku-Status abfragen
./headsetcontrol -b
```

Ausgabe:
- Akkustand in Prozent (0-100%)
- Ladestatus (Charging/Available)

## Technische Details

### HID-Interfaces
Das HS80 verwendet zwei HID-Interfaces:
- **Interface 6** (Usage Page 0xFF42, Usage 0x0001) - RGB-Kontrolle
- **Interface 7** (Usage Page 0xFF42, Usage 0x0002) - Event-Monitoring (noch nicht implementiert)

### Protokoll

#### Software-Modus aktivieren
Der Software-Modus muss aktiviert werden, um RGB zu steuern:

1. **Enable Software Mode** (0x02, mode, 0x01, 0x03, 0x00, 0x02)
2. **Open Lighting Endpoint** (0x02, mode, 0x0D, 0x00, 0x01)
3. **Set Hardware Brightness** (0x02, mode, 0x01, 0x02, 0x00, low_byte, high_byte)

Dabei ist `mode`:
- `0x08` für USB (Wired)
- `0x09` für Wireless

#### RGB-Daten senden
Format: `0x02, mode, 0x06, 0x00, 0x09, 0x00, 0x00, 0x00, R₁, R₂, R₃, G₁, G₂, G₃, B₁, B₂, B₃`

Die RGB-Daten sind gepackt:
- Bytes 8-10: Rot-Werte für Logo, Power, Mic
- Bytes 11-13: Grün-Werte für Logo, Power, Mic
- Bytes 14-16: Blau-Werte für Logo, Power, Mic

#### Akku abfragen (Wireless)
1. **Battery Level** (0x02, 0x09, 0x02, 0x0F, 0x00)
   - Antwort: 16-bit Little Endian Wert (0-1000) bei Byte 4-5
   - Prozent = Wert / 10

2. **Charging Status** (0x02, 0x09, 0x02, 0x10, 0x00)
   - Antwort: Byte 4 = 0x01 wenn lädt, sonst 0x00

### Helligkeit
Die Helligkeit wird als 16-bit Little Endian Wert von 0-1000 gespeichert:
- 0 = 0%
- 1000 = 100%

Standard ist 1000 (100%).

## Implementierung

Die Implementierung basiert auf:
- `HS80_Library.cpp/h` - Windows C++ Referenzimplementierung
- `Corsair_Headset_Controller.js` - SignalRGB Plugin mit Protokoll-Details

### Dateien
- `src/devices/corsair_hs80.c` - Hauptimplementierung
- `src/devices/corsair_hs80.h` - Header-Datei
- `src/device_registry.c` - Device-Registrierung

## Zukünftige Erweiterungen

Mögliche zukünftige Features:
- **Event-Monitoring** (Interface 7)
  - Mikrofon Mute-Status (Event 0xA6)
  - Akku-Änderungen (Event 0x0F)
  - Lade-Status-Änderungen (Event 0x10)
- **Einzelne LED-Zonen** steuern
- **Helligkeit** über CLI steuern
- **RGB-Effekte** (Rainbow, Pulse, etc.)
- **Sidetone** (falls vom HS80 unterstützt)
- **Idle Timeout** konfigurieren

## Beispiele

### Headset erkennen
```bash
./headsetcontrol -h
```

### LEDs einschalten
```bash
./headsetcontrol -l 1
```

### Akku prüfen (Wireless)
```bash
./headsetcontrol -b
```

### Alle Funktionen anzeigen
```bash
./headsetcontrol -?
```

## Debugging

Bei Problemen kann die Implementierung Debug-Ausgaben erzeugen:
```c
printf("[HS80] Debug-Nachricht\n");
```

Diese erscheinen in der Console wenn `headsetcontrol` ausgeführt wird.

## Quellen

- [HeadsetControl GitHub](https://github.com/Sapd/HeadsetControl)
- [SignalRGB Corsair Plugin](https://github.com/signalrgb)
- HS80 Protokoll-Analyse aus Library und JS-Dateien
