#pragma once

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdio>

// ============================================================================
// HS80 HID Library - Corsair HS80 RGB Wireless Gaming Headset
// ============================================================================

namespace HS80 {

// Konstanten
constexpr unsigned short CORSAIR_VID = 0x1B1C;
constexpr unsigned short HS80_WIRELESS_PID = 0x0A6B;

// Interface-Definitionen
constexpr unsigned short RGB_USAGE_PAGE = 0xFF42;
constexpr unsigned short RGB_USAGE = 0x0001;        // Interface 6
constexpr unsigned short EVENT_USAGE = 0x0002;      // Interface 7

// RGB-Struktur
struct RGBColor {
    unsigned char r, g, b;
    
    RGBColor(unsigned char red = 0, unsigned char green = 0, unsigned char blue = 0)
        : r(red), g(green), b(blue) {}
};

// LED-Zonen
struct LEDZones {
    RGBColor logo;
    RGBColor power;
    RGBColor mic;
    
    LEDZones() : logo(0, 155, 222), power(0, 155, 222), mic(0, 155, 222) {}
    LEDZones(RGBColor all) : logo(all), power(all), mic(all) {}
};

// Einzelne LED-Zonen (für gezielte Updates)
enum class LEDZone {
    Logo = 0,
    Power = 1,
    Mic = 2,
    All = 255
};

// Event-Typen
enum class EventType {
    Unknown = 0x00,
    EventPacket = 0x03,  // HS80 Event-Header
    Mute = 0xA6,
    Battery = 0x0F,
    Charging = 0x10
};

// Event-Daten
struct HeadsetEvent {
    EventType type;
    unsigned char data[64];
    size_t dataSize;
    
    // Event-Analyse (HS80 Format: [0x03][0x01][0x01][EventCode][Data...])
    EventType getActualEventType() const {
        if (dataSize >= 4 && data[0] == 0x03) {
            return static_cast<EventType>(data[3]);
        }
        return type;
    }
    
    // Hilfsfunktionen
    bool isMuted() const {
        if (dataSize >= 6 && data[0] == 0x03 && data[3] == 0xA6) {
            return data[5] == 0x01; // Byte 5: 0x01=Muted, 0x00=Unmuted
        }
        return false;
    }
    
    bool isUnmuted() const {
        if (dataSize >= 6 && data[0] == 0x03 && data[3] == 0xA6) {
            return data[5] == 0x00;
        }
        return false;
    }
    
    int getBatteryLevel() const {
        if (dataSize >= 7 && data[0] == 0x03 && data[3] == 0x0F) {
            // Battery ist 16-bit Little Endian in Bytes 5-6, dann /10 für Prozent
            // Beispiel: 03 01 01 0F 00 B2 02 => 0x02B2 = 690 => 69.0%
            int raw = data[5] | (data[6] << 8);  // Little Endian
            return raw / 10;  // 0-1000 → 0-100%
        }
        return -1;
    }
    
    int getBatteryLevelRaw() const {
        if (dataSize >= 7 && data[0] == 0x03 && data[3] == 0x0F) {
            // Raw 16-bit Little Endian (0-1000)
            return data[5] | (data[6] << 8);
        }
        return -1;
    }
    
    bool isCharging() const {
        if (dataSize >= 6 && data[0] == 0x03 && data[3] == 0x10) {
            return data[5] == 0x01;
        }
        return false;
    }
    
    // Deutsche Beschreibung des Events
    std::string getDescription() const {
        if (dataSize < 4 || data[0] != 0x03) {
            return "Unbekanntes Event-Format";
        }
        
        EventType actualType = static_cast<EventType>(data[3]);
        
        switch (actualType) {
        case EventType::Mute:
            if (dataSize >= 6) {
                return data[5] == 0x01 ? "Mikrofon STUMM" : "Mikrofon AKTIV";
            }
            return "Mikrofon-Status";
            
        case EventType::Battery:
            if (dataSize >= 7) {
                // Battery ist 16-bit Little Endian in Bytes 5-6, dann /10
                int raw = data[5] | (data[6] << 8);
                int percent = raw / 10;
                return "Akku: " + std::to_string(percent) + "%";
            }
            return "Akku-Status";
            
        case EventType::Charging:
            if (dataSize >= 6) {
                return data[5] == 0x01 ? "Wird geladen" : "Nicht am Laden";
            }
            return "Lade-Status";
            
        default:
            return "Unbekannter Event-Typ: 0x" + toHexString(data[3]);
        }
    }
    
private:
    std::string toHexString(unsigned char value) const {
        char buf[5];
        sprintf(buf, "%02X", value);
        return std::string(buf);
    }
};

// Event-Callback
using EventCallback = std::function<void(const HeadsetEvent&)>;

// ============================================================================
// RGB-Controller
// ============================================================================
class RGBController {
private:
    HANDLE m_device;
    bool m_isWireless;
    bool m_initialized;
    
    // Keep-Alive für Software-Modus
    HANDLE m_keepAliveThread;
    bool m_keepAliveRunning;
    LEDZones m_currentZones;
    int m_currentBrightness;  // 0-1000 (0-100%)
    CRITICAL_SECTION m_lock;
    
    static DWORD WINAPI KeepAliveThreadProc(LPVOID param);
    void keepAliveLoop();
    bool sendColorsInternal(const LEDZones& zones);
    bool sendBrightnessInternal(int brightness);

public:
    RGBController();
    ~RGBController();
    
    // Verbindung
    bool connect(unsigned short vid = CORSAIR_VID, unsigned short pid = HS80_WIRELESS_PID);
    void disconnect();
    bool isConnected() const { return m_device != INVALID_HANDLE_VALUE; }
    
    // RGB-Kontrolle
    bool initialize();
    bool setColors(const LEDZones& zones);
    bool setColor(RGBColor color);
    bool setHardwareMode();
    
    // Einzelne Zonen-Kontrolle
    bool setZone(LEDZone zone, RGBColor color);
    bool setLogoColor(RGBColor color);
    bool setPowerColor(RGBColor color);
    bool setMicColor(RGBColor color);
    
    // Helligkeit (0-100% oder 0-1000)
    bool setBrightness(int percent);           // 0-100%
    bool setBrightnessRaw(int brightness);     // 0-1000 (raw value)
    int getBrightness() const;                 // Gibt 0-100% zurück
    int getBrightnessRaw() const;              // Gibt 0-1000 zurück
    
    // Keep-Alive Kontrolle (verhindert Rückfall in Hardware-Modus)
    bool startKeepAlive(int intervalMs = 5000);  // Standard: alle 5 Sekunden
    void stopKeepAlive();
    bool isKeepAliveRunning() const { return m_keepAliveRunning; }
    
    // Vordefinierte Effekte
    bool rainbow(int durationMs = 10000, int stepMs = 100);
    bool pulse(RGBColor color, int cycles = 3, int stepMs = 50);
    bool off();
};

// ============================================================================
// Event-Monitor
// ============================================================================
class EventMonitor {
private:
    HANDLE m_device;
    HANDLE m_readThread;
    OVERLAPPED m_overlapped;
    bool m_running;
    EventCallback m_callback;
    unsigned char m_buffer[65];

    static DWORD WINAPI ReadThreadProc(LPVOID param);
    void readLoop();

public:
    EventMonitor();
    ~EventMonitor();
    
    // Verbindung
    bool connect(unsigned short vid = CORSAIR_VID, unsigned short pid = HS80_WIRELESS_PID);
    void disconnect();
    bool isConnected() const { return m_device != INVALID_HANDLE_VALUE; }
    
    // Event-Monitoring
    bool startMonitoring(EventCallback callback);
    void stopMonitoring();
    bool isMonitoring() const { return m_running; }
};

// ============================================================================
// Headset-Manager (High-Level Interface)
// ============================================================================
class HeadsetManager {
private:
    RGBController m_rgb;
    EventMonitor m_events;
    bool m_autoReconnect;

public:
    HeadsetManager();
    ~HeadsetManager();
    
    // Verbindung
    bool connect(bool autoReconnect = true);
    void disconnect();
    bool isConnected() const;
    
    // RGB-Zugriff
    RGBController& rgb() { return m_rgb; }
    const RGBController& rgb() const { return m_rgb; }
    
    // Event-Zugriff
    EventMonitor& events() { return m_events; }
    const EventMonitor& events() const { return m_events; }
    
    // Schnellzugriff
    bool setLEDs(RGBColor color);
    bool setLEDs(const LEDZones& zones);
    bool setZone(LEDZone zone, RGBColor color);
    bool setBrightness(int percent);  // 0-100%
    bool startEventMonitoring(EventCallback callback);
};

// ============================================================================
// Device-Discovery
// ============================================================================
struct DeviceInfo {
    std::string path;
    unsigned short vendorId;
    unsigned short productId;
    unsigned short usagePage;
    unsigned short usage;
    std::wstring manufacturer;
    std::wstring product;
};

std::vector<DeviceInfo> enumerateDevices(unsigned short vid = 0, unsigned short pid = 0);
bool findDeviceByUsage(unsigned short vid, unsigned short pid, 
                       unsigned short usagePage, unsigned short usage,
                       DeviceInfo& outInfo);

} // namespace HS80
