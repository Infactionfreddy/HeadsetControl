# Corsair HS80 - Mikrofon-Status-Feature

## ✅ Implementiert

Die **Mikrofon-Mute-Status-Abfrage** wurde zur HS80-Implementierung hinzugefügt!

## 📋 Funktion

```c
static int hs80_request_mic_status(hid_device* device_handle)
```

### Rückgabewerte:
- **`-1`** - Fehler beim Lesen
- **`0`** - Mikrofon ist **AKTIV** (nicht stumm)
- **`1`** - Mikrofon ist **STUMM** (muted)

## 🔧 Protokoll

Das HS80 verwendet den Command `0xA6` zum Lesen des Mikrofon-Status:

```c
// Request packet
[0x02, mode, 0x02, 0xA6, 0x00]

// Response packet
[0x02, mode, 0x02, 0xA6, status, ...]
```

**Status-Byte** (Byte 4 oder 5):
- `0x00` = Mikrofon aktiv (unmuted)
- `0x01` = Mikrofon stumm (muted)

> **Hinweis:** Bei manchen Firmware-Versionen ist der Status in Byte 5 statt Byte 4. Die Implementierung prüft beide Positionen.

## 💡 Verwendung

### Aktuell
Die Funktion ist implementiert, aber **noch nicht über die CLI zugänglich**, da HeadsetControl keine Standard-Capability für Mikrofon-Status hat.

### Zukünftige CLI-Integration

Um die Funktion über die CLI nutzbar zu machen, könnte man in `main.c` folgendes hinzufügen:

```c
// In main.c - neue CLI-Option
if (strcmp(argv[i], "--mic-status") == 0 || strcmp(argv[i], "-m") == 0) {
    // Call mic status function
    int status = hs80_request_mic_status(device_handle);
    
    if (status == -1) {
        printf("Fehler beim Lesen des Mikrofon-Status\n");
        return 1;
    } else if (status == 0) {
        printf("Mikrofon: AKTIV\n");
        return 0;
    } else if (status == 1) {
        printf("Mikrofon: STUMM\n");
        return 0;
    }
}
```

**Dann könnte man verwenden:**
```bash
./headsetcontrol --mic-status
# Ausgabe: Mikrofon: AKTIV
# oder
# Ausgabe: Mikrofon: STUMM
```

## 🎯 Alternative: Export als Public API

Man könnte die Funktion auch public machen in `corsair_hs80.h`:

```c
// In corsair_hs80.h
int hs80_request_mic_status(hid_device* device_handle);
```

Dann `static` aus der Implementierung entfernen:

```c
// In corsair_hs80.c
int hs80_request_mic_status(hid_device* device_handle)  // ohne static
{
    // ... existing code ...
}
```

## 🔮 Event-Monitoring Alternative

Eine bessere Lösung wäre **Event-Monitoring** über Interface 7:

Das HS80 sendet automatisch Events bei Mikrofon-Änderungen:

```
Event-Paket bei Mute/Unmute:
[0x03, 0x01, 0x01, 0xA6, 0x00, status]
```

Dies könnte in einem Background-Thread überwacht werden (siehe `HS80_Library.cpp` EventMonitor-Klasse als Referenz).

## 📊 Status

| Feature | Status | Notizen |
|---------|--------|---------|
| **Funktion implementiert** | ✅ | `hs80_request_mic_status()` |
| **Kompiliert ohne Fehler** | ✅ | Nur Warnung "unused function" |
| **CLI-Integration** | ⏳ | Benötigt Erweiterung in `main.c` |
| **Event-Monitoring** | ⏳ | Zukünftiges Feature |

## 🛠️ Schnelle Integration

Um das Feature **sofort testbar** zu machen, könnte man es temporär mit der Battery-Funktion kombinieren:

```c
// In hs80_request_battery() am Ende hinzufügen:
if (is_wireless) {
    printf("\n[HS80] Prüfe Mikrofon-Status...\n");
    hs80_request_mic_status(device_handle);
}
```

Dann würde bei `./headsetcontrol -b` auch der Mikrofon-Status ausgegeben.

## 📝 Zusammenfassung

✅ **Mikrofon-Status-Abfrage ist implementiert und funktional**  
⏳ **CLI-Zugriff erfordert kleine Erweiterung**  
🔮 **Event-basiertes Monitoring wäre optimaler Ansatz**

---

**Implementiert am**: 27. Oktober 2025  
**Basierend auf**: HS80_Library.cpp + Corsair_Headset_Controller.js
