# Quick Start - Corsair HS80 mit HeadsetControl

## 🚀 Schnellstart

### 1. Headset verbinden
Verbinde dein Corsair HS80 RGB Headset per USB oder über den Wireless-Dongle.

### 2. Testen
```bash
# Headset erkennen
./build/headsetcontrol -h

# LEDs einschalten
./build/headsetcontrol -l 1

# Akku prüfen (nur Wireless)
./build/headsetcontrol -b
```

### 3. Test-Script verwenden
```bash
./test_hs80.sh
```

## 💡 Häufige Befehle

```bash
# RGB-LEDs
./build/headsetcontrol -l 1    # LEDs AN (Corsair Blau)
./build/headsetcontrol -l 0    # LEDs AUS

# Akku (Wireless)
./build/headsetcontrol -b      # Akku-Status abfragen

# Hilfe
./build/headsetcontrol -?      # Alle Optionen anzeigen
./build/headsetcontrol -h      # Device-Info
```

## 🎨 Was funktioniert

✅ **RGB-Beleuchtung**
- 3 LED-Zonen (Logo, Power, Mikrofon)
- Ein/Aus-Steuerung
- Corsair Blau als Standard-Farbe

✅ **Akkustatus** (nur Wireless)
- Akkulevel in Prozent
- Ladestatus

✅ **Automatische Erkennung**
- USB vs. Wireless-Modus
- Alle 4 HS80-Varianten

## 🔮 Kommt später

Die Basis für folgende Features ist bereits im Code vorbereitet:
- Einzelne LED-Zonen steuern
- RGB-Helligkeit einstellen
- RGB-Effekte (Rainbow, Pulse)
- Event-Monitoring (Mikrofon Mute)

Siehe `HS80_EXTENDED_FEATURES.c` für Beispiel-Code.

## 🆘 Probleme?

**Headset wird nicht erkannt?**
- Prüfe USB-Verbindung / Wireless-Dongle
- Versuche anderen USB-Port
- Unter macOS: Erlaube ggf. HID-Zugriff

**LEDs funktionieren nicht?**
- Software-Modus wird automatisch aktiviert
- Bei Problemen: Headset neu verbinden

**Akku zeigt nichts?**
- Normal bei USB-Modellen (nur Wireless hat Akku)

## 📖 Mehr Infos

- **Vollständige Doku**: `HS80_USAGE.md`
- **Integration**: `INTEGRATION_SUMMARY.md`
- **Erweiterte Features**: `HS80_EXTENDED_FEATURES.c`

---

**Viel Spaß mit deinem Corsair HS80!** 🎧✨
