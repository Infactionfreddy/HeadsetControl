#include "HS80_Library.h"
#include <iostream>
#include <thread>
#include <chrono>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace HS80 {

// ============================================================================
// Hilfsfunktionen
// ============================================================================

static HANDLE OpenHIDDevice(const std::string& path, bool overlapped) {
    DWORD flags = overlapped ? FILE_FLAG_OVERLAPPED : 0;
    return CreateFileA(path.c_str(),
                      GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                      nullptr,
                      OPEN_EXISTING,
                      flags,
                      nullptr);
}

static bool SendHIDReport(HANDLE device, const unsigned char* data, size_t size) {
    // HS80 verwendet direkt 64-Byte Pakete ohne Report-ID
    // (Kein 0x00 Prefix nötig wie bei anderen HID-Geräten)
    DWORD bytesWritten = 0;
    BOOL result = WriteFile(device, data, static_cast<DWORD>(size), &bytesWritten, nullptr);
    
    if (!result) {
        std::cerr << "[ERROR] WriteFile failed! Error: " << GetLastError() << std::endl;
        return false;
    }
    
    if (bytesWritten != size) {
        std::cerr << "[ERROR] Wrote " << bytesWritten << " bytes, expected " << size << std::endl;
        return false;
    }
    
    return true;
}

// ============================================================================
// Device Discovery
// ============================================================================

std::vector<DeviceInfo> enumerateDevices(unsigned short vid, unsigned short pid) {
    std::vector<DeviceInfo> devices;
    
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsA(&hidGuid, nullptr, nullptr,
                                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return devices;
    }
    
    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, nullptr, &hidGuid,
                                                    i, &deviceInterfaceData); i++) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailA(deviceInfoSet, &deviceInterfaceData,
                                          nullptr, 0, &requiredSize, nullptr);
        
        std::vector<char> buffer(requiredSize);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detailData =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_A>(buffer.data());
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        
        if (SetupDiGetDeviceInterfaceDetailA(deviceInfoSet, &deviceInterfaceData,
                                              detailData, requiredSize, nullptr, nullptr)) {
            HANDLE hDevice = OpenHIDDevice(detailData->DevicePath, false);
            
            if (hDevice != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attributes;
                attributes.Size = sizeof(HIDD_ATTRIBUTES);
                
                if (HidD_GetAttributes(hDevice, &attributes)) {
                    // Filter nach VID/PID wenn angegeben
                    if ((vid == 0 || attributes.VendorID == vid) &&
                        (pid == 0 || attributes.ProductID == pid)) {
                        
                        PHIDP_PREPARSED_DATA preparsedData;
                        if (HidD_GetPreparsedData(hDevice, &preparsedData)) {
                            HIDP_CAPS caps;
                            if (HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS) {
                                DeviceInfo info;
                                info.path = detailData->DevicePath;
                                info.vendorId = attributes.VendorID;
                                info.productId = attributes.ProductID;
                                info.usagePage = caps.UsagePage;
                                info.usage = caps.Usage;
                                
                                // Strings auslesen
                                wchar_t buffer[256];
                                if (HidD_GetManufacturerString(hDevice, buffer, sizeof(buffer))) {
                                    info.manufacturer = buffer;
                                }
                                if (HidD_GetProductString(hDevice, buffer, sizeof(buffer))) {
                                    info.product = buffer;
                                }
                                
                                devices.push_back(info);
                            }
                            HidD_FreePreparsedData(preparsedData);
                        }
                    }
                }
                CloseHandle(hDevice);
            }
        }
    }
    
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return devices;
}

bool findDeviceByUsage(unsigned short vid, unsigned short pid,
                       unsigned short usagePage, unsigned short usage,
                       DeviceInfo& outInfo) {
    auto devices = enumerateDevices(vid, pid);
    
    for (const auto& dev : devices) {
        if (dev.usagePage == usagePage && dev.usage == usage) {
            outInfo = dev;
            return true;
        }
    }
    
    return false;
}

// ============================================================================
// RGBController Implementation
// ============================================================================

RGBController::RGBController()
    : m_device(INVALID_HANDLE_VALUE)
    , m_isWireless(false)
    , m_initialized(false)
    , m_keepAliveThread(nullptr)
    , m_keepAliveRunning(false)
    , m_currentBrightness(1000) {  // Standard: 100%
    InitializeCriticalSection(&m_lock);
}

RGBController::~RGBController() {
    disconnect();
    DeleteCriticalSection(&m_lock);
}

bool RGBController::connect(unsigned short vid, unsigned short pid) {
    if (isConnected()) {
        disconnect();
    }
    
    // Suche RGB-Interface (Usage 0x0001)
    DeviceInfo rgbDevice;
    if (!findDeviceByUsage(vid, pid, RGB_USAGE_PAGE, RGB_USAGE, rgbDevice)) {
        std::cerr << "[RGB] Kein RGB-Interface gefunden!" << std::endl;
        return false;
    }
    
    std::cout << "[RGB] RGB-Interface gefunden: " << std::endl;
    std::wcout << L"      " << rgbDevice.manufacturer << L" - " << rgbDevice.product << std::endl;
    std::cout << "      Usage Page: 0x" << std::hex << rgbDevice.usagePage
              << ", Usage: 0x" << rgbDevice.usage << std::dec << std::endl;
    
    m_device = OpenHIDDevice(rgbDevice.path, false);
    if (m_device == INVALID_HANDLE_VALUE) {
        std::cerr << "[RGB] Fehler beim Oeffnen! Error: " << GetLastError() << std::endl;
        return false;
    }
    
    m_isWireless = (pid == HS80_WIRELESS_PID);
    std::cout << "[RGB] Verbunden! (Modus: " << (m_isWireless ? "Wireless" : "Wired") << ")" << std::endl;
    
    return true;
}

void RGBController::disconnect() {
    stopKeepAlive();
    
    if (m_initialized) {
        setHardwareMode();
    }
    
    if (isConnected()) {
        CloseHandle(m_device);
        m_device = INVALID_HANDLE_VALUE;
        m_initialized = false;
        std::cout << "[RGB] Getrennt." << std::endl;
    }
}

bool RGBController::initialize() {
    if (!isConnected()) {
        std::cerr << "[RGB] Nicht verbunden!" << std::endl;
        return false;
    }
    
    std::cout << "[RGB] Initialisiere Software-Modus..." << std::endl;
    
    const unsigned char headsetMode = m_isWireless ? 0x09 : 0x08;
    
    // Paket 1: Enable Software Mode
    unsigned char packet1[64] = {0};
    packet1[0] = 0x02;
    packet1[1] = headsetMode;
    packet1[2] = 0x01;
    packet1[3] = 0x03;
    packet1[4] = 0x00;
    packet1[5] = 0x02;
    
    if (!SendHIDReport(m_device, packet1, 64)) {
        std::cerr << "[RGB] Fehler bei Paket 1 (Software-Modus)!" << std::endl;
        return false;
    }
    
    Sleep(100);
    
    // Paket 2: Open lighting endpoint
    unsigned char packet2[64] = {0};
    packet2[0] = 0x02;
    packet2[1] = headsetMode;
    packet2[2] = 0x0D;
    packet2[3] = 0x00;
    packet2[4] = 0x01;
    
    if (!SendHIDReport(m_device, packet2, 64)) {
        std::cerr << "[RGB] Fehler bei Paket 2 (Lighting oeffnen)!" << std::endl;
        return false;
    }
    
    Sleep(100);
    
    // Paket 3: Set Hardware Brightness to 100%
    unsigned char packet3[64] = {0};
    packet3[0] = 0x02;
    packet3[1] = headsetMode;
    packet3[2] = 0x01;
    packet3[3] = 0x02;
    packet3[4] = 0x00;
    packet3[5] = 0xE8; // 1000 = 100% (little endian low byte)
    packet3[6] = 0x03; // high byte
    
    if (!SendHIDReport(m_device, packet3, 64)) {
        std::cerr << "[RGB] Fehler bei Paket 3 (Helligkeit)!" << std::endl;
        return false;
    }
    
    Sleep(100);
    
    m_initialized = true;
    std::cout << "[RGB] Software-Modus aktiviert!" << std::endl;
    return true;
}

bool RGBController::setColors(const LEDZones& zones) {
    EnterCriticalSection(&m_lock);
    m_currentZones = zones;
    LeaveCriticalSection(&m_lock);
    
    return sendColorsInternal(zones);
}

bool RGBController::sendColorsInternal(const LEDZones& zones) {
    if (!isConnected()) {
        return false;
    }
    
    if (!m_initialized && !initialize()) {
        return false;
    }
    
    const unsigned char headsetMode = m_isWireless ? 0x09 : 0x08;
    
    unsigned char packet[64] = {0};
    packet[0] = 0x02;
    packet[1] = headsetMode;
    packet[2] = 0x06;
    packet[3] = 0x00;
    packet[4] = 0x09; // 9 Bytes RGB-Daten
    packet[5] = 0x00;
    packet[6] = 0x00;
    packet[7] = 0x00;
    
    // Aus dem JS: RGB-Werte werden als R,R,R,G,G,G,B,B,B pro Zone gesendet
    // Aber das Original HS80.cpp nutzt: LED_LOGO_R, LED_POWER_R, LED_MIC_R, dann +3 für G, +6 für B
    
    // R-Werte an Index 0, 1, 2 (offset 8)
    packet[8] = zones.logo.r;   // LED_LOGO_R
    packet[9] = zones.power.r;  // LED_POWER_R
    packet[10] = zones.mic.r;   // LED_MIC_R
    
    // G-Werte an Index 3, 4, 5 (offset 8)
    packet[11] = zones.logo.g;  // LED_LOGO_G
    packet[12] = zones.power.g; // LED_POWER_G
    packet[13] = zones.mic.g;   // LED_MIC_G
    
    // B-Werte an Index 6, 7, 8 (offset 8)
    packet[14] = zones.logo.b;  // LED_LOGO_B
    packet[15] = zones.power.b; // LED_POWER_B
    packet[16] = zones.mic.b;   // LED_MIC_B
    
    return SendHIDReport(m_device, packet, 64);
}

bool RGBController::setColor(RGBColor color) {
    return setColors(LEDZones(color));
}

// ============================================================================
// Einzelne Zonen-Kontrolle
// ============================================================================

bool RGBController::setZone(LEDZone zone, RGBColor color) {
    EnterCriticalSection(&m_lock);
    
    switch (zone) {
    case LEDZone::Logo:
        m_currentZones.logo = color;
        break;
    case LEDZone::Power:
        m_currentZones.power = color;
        break;
    case LEDZone::Mic:
        m_currentZones.mic = color;
        break;
    case LEDZone::All:
        m_currentZones.logo = color;
        m_currentZones.power = color;
        m_currentZones.mic = color;
        break;
    }
    
    LEDZones zones = m_currentZones;
    LeaveCriticalSection(&m_lock);
    
    return sendColorsInternal(zones);
}

bool RGBController::setLogoColor(RGBColor color) {
    return setZone(LEDZone::Logo, color);
}

bool RGBController::setPowerColor(RGBColor color) {
    return setZone(LEDZone::Power, color);
}

bool RGBController::setMicColor(RGBColor color) {
    return setZone(LEDZone::Mic, color);
}

// ============================================================================
// Helligkeit (Brightness)
// ============================================================================

bool RGBController::setBrightness(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    // Konvertiere Prozent (0-100) zu Raw-Wert (0-1000)
    int brightness = (percent * 1000) / 100;
    return setBrightnessRaw(brightness);
}

bool RGBController::setBrightnessRaw(int brightness) {
    if (brightness < 0) brightness = 0;
    if (brightness > 1000) brightness = 1000;
    
    EnterCriticalSection(&m_lock);
    m_currentBrightness = brightness;
    LeaveCriticalSection(&m_lock);
    
    return sendBrightnessInternal(brightness);
}

bool RGBController::sendBrightnessInternal(int brightness) {
    if (!isConnected()) {
        return false;
    }
    
    if (!m_initialized && !initialize()) {
        return false;
    }
    
    const unsigned char headsetMode = m_isWireless ? 0x09 : 0x08;
    
    // Brightness Paket (wie in initialize() Paket 3)
    unsigned char packet[64] = {0};
    packet[0] = 0x02;
    packet[1] = headsetMode;
    packet[2] = 0x01;
    packet[3] = 0x02;
    packet[4] = 0x00;
    
    // Brightness als Little Endian 16-bit (0-1000)
    packet[5] = brightness & 0xFF;        // Low byte
    packet[6] = (brightness >> 8) & 0xFF; // High byte
    
    return SendHIDReport(m_device, packet, 64);
}

int RGBController::getBrightness() const {
    // Konvertiere Raw-Wert (0-1000) zu Prozent (0-100)
    return (m_currentBrightness * 100) / 1000;
}

int RGBController::getBrightnessRaw() const {
    return m_currentBrightness;
}

bool RGBController::setHardwareMode() {
    if (!isConnected()) {
        return false;
    }
    
    std::cout << "[RGB] Stelle Hardware-Modus wieder her..." << std::endl;
    
    const unsigned char headsetMode = m_isWireless ? 0x09 : 0x08;
    
    unsigned char packet[64] = {0};
    packet[0] = 0x02;
    packet[1] = headsetMode;
    packet[2] = 0x01;
    packet[3] = 0x03;
    packet[4] = 0x00;
    packet[5] = 0x01; // Hardware mode
    
    bool result = SendHIDReport(m_device, packet, 64);
    m_initialized = false;
    
    Sleep(100);
    
    return result;
}

bool RGBController::rainbow(int durationMs, int stepMs) {
    if (!isConnected()) {
        return false;
    }
    
    std::cout << "[RGB] Starte Regenbogen-Animation..." << std::endl;
    
    int steps = durationMs / stepMs;
    for (int i = 0; i < steps; i++) {
        float hue = (i * 360.0f) / steps;
        
        // HSV zu RGB
        float r = 0, g = 0, b = 0;
        if (hue < 60) { r = 255; g = (hue / 60) * 255; }
        else if (hue < 120) { r = ((120 - hue) / 60) * 255; g = 255; }
        else if (hue < 180) { g = 255; b = ((hue - 120) / 60) * 255; }
        else if (hue < 240) { g = ((240 - hue) / 60) * 255; b = 255; }
        else if (hue < 300) { b = 255; r = ((hue - 240) / 60) * 255; }
        else { b = ((360 - hue) / 60) * 255; r = 255; }
        
        setColor(RGBColor((unsigned char)r, (unsigned char)g, (unsigned char)b));
        Sleep(stepMs);
    }
    
    return true;
}

bool RGBController::pulse(RGBColor color, int cycles, int stepMs) {
    if (!isConnected()) {
        return false;
    }
    
    std::cout << "[RGB] Starte Puls-Animation..." << std::endl;
    
    for (int c = 0; c < cycles; c++) {
        // Fade in
        for (int i = 0; i <= 255; i += 15) {
            float factor = i / 255.0f;
            RGBColor faded(
                (unsigned char)(color.r * factor),
                (unsigned char)(color.g * factor),
                (unsigned char)(color.b * factor)
            );
            setColor(faded);
            Sleep(stepMs);
        }
        
        // Fade out
        for (int i = 255; i >= 0; i -= 15) {
            float factor = i / 255.0f;
            RGBColor faded(
                (unsigned char)(color.r * factor),
                (unsigned char)(color.g * factor),
                (unsigned char)(color.b * factor)
            );
            setColor(faded);
            Sleep(stepMs);
        }
    }
    
    return true;
}

bool RGBController::off() {
    return setColor(RGBColor(0, 0, 0));
}

// ============================================================================
// Keep-Alive für Software-Modus
// ============================================================================

bool RGBController::startKeepAlive(int intervalMs) {
    if (m_keepAliveRunning) {
        std::cout << "[RGB] Keep-Alive laeuft bereits!" << std::endl;
        return true;
    }
    
    if (!isConnected() || !m_initialized) {
        std::cerr << "[RGB] Nicht verbunden oder nicht initialisiert!" << std::endl;
        return false;
    }
    
    std::cout << "[RGB] Starte Keep-Alive Thread (Intervall: " << intervalMs << "ms)..." << std::endl;
    
    m_keepAliveRunning = true;
    
    // Erstelle Keep-Alive Thread
    struct ThreadParams {
        RGBController* controller;
        int intervalMs;
    };
    
    ThreadParams* params = new ThreadParams{this, intervalMs};
    
    m_keepAliveThread = CreateThread(nullptr, 0, 
        [](LPVOID param) -> DWORD {
            ThreadParams* p = static_cast<ThreadParams*>(param);
            RGBController* controller = p->controller;
            int interval = p->intervalMs;
            delete p;
            
            while (controller->m_keepAliveRunning) {
                Sleep(interval);
                
                if (!controller->m_keepAliveRunning) break;
                
                // Sende aktuelle Farben erneut
                EnterCriticalSection(&controller->m_lock);
                LEDZones zones = controller->m_currentZones;
                LeaveCriticalSection(&controller->m_lock);
                
                if (!controller->sendColorsInternal(zones)) {
                    std::cerr << "[RGB] Keep-Alive Fehler beim Senden!" << std::endl;
                }
            }
            
            return 0;
        },
        params, 0, nullptr);
    
    if (m_keepAliveThread == nullptr) {
        std::cerr << "[RGB] Fehler beim Erstellen des Keep-Alive Threads!" << std::endl;
        m_keepAliveRunning = false;
        return false;
    }
    
    std::cout << "[RGB] Keep-Alive Thread gestartet!" << std::endl;
    return true;
}

void RGBController::stopKeepAlive() {
    if (!m_keepAliveRunning) {
        return;
    }
    
    std::cout << "[RGB] Stoppe Keep-Alive Thread..." << std::endl;
    m_keepAliveRunning = false;
    
    if (m_keepAliveThread != nullptr) {
        WaitForSingleObject(m_keepAliveThread, 10000); // Max 10 Sekunden warten
        CloseHandle(m_keepAliveThread);
        m_keepAliveThread = nullptr;
    }
    
    std::cout << "[RGB] Keep-Alive gestoppt." << std::endl;
}

// ============================================================================
// EventMonitor Implementation
// ============================================================================

EventMonitor::EventMonitor()
    : m_device(INVALID_HANDLE_VALUE)
    , m_readThread(nullptr)
    , m_running(false) {
    memset(&m_overlapped, 0, sizeof(m_overlapped));
    memset(m_buffer, 0, sizeof(m_buffer));
}

EventMonitor::~EventMonitor() {
    disconnect();
}

bool EventMonitor::connect(unsigned short vid, unsigned short pid) {
    if (isConnected()) {
        disconnect();
    }
    
    // Suche Event-Interface (Usage 0x0002)
    DeviceInfo eventDevice;
    if (!findDeviceByUsage(vid, pid, RGB_USAGE_PAGE, EVENT_USAGE, eventDevice)) {
        std::cerr << "[EVENT] Kein Event-Interface gefunden!" << std::endl;
        return false;
    }
    
    std::cout << "[EVENT] Event-Interface gefunden:" << std::endl;
    std::wcout << L"        " << eventDevice.manufacturer << L" - " << eventDevice.product << std::endl;
    std::cout << "        Usage Page: 0x" << std::hex << eventDevice.usagePage
              << ", Usage: 0x" << eventDevice.usage << std::dec << std::endl;
    
    m_device = OpenHIDDevice(eventDevice.path, true); // Mit OVERLAPPED für async Reading
    if (m_device == INVALID_HANDLE_VALUE) {
        std::cerr << "[EVENT] Fehler beim Oeffnen! Error: " << GetLastError() << std::endl;
        return false;
    }
    
    m_overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!m_overlapped.hEvent) {
        CloseHandle(m_device);
        m_device = INVALID_HANDLE_VALUE;
        return false;
    }
    
    std::cout << "[EVENT] Verbunden!" << std::endl;
    return true;
}

void EventMonitor::disconnect() {
    stopMonitoring();
    
    if (m_overlapped.hEvent) {
        CloseHandle(m_overlapped.hEvent);
        m_overlapped.hEvent = nullptr;
    }
    
    if (isConnected()) {
        CloseHandle(m_device);
        m_device = INVALID_HANDLE_VALUE;
        std::cout << "[EVENT] Getrennt." << std::endl;
    }
}

bool EventMonitor::startMonitoring(EventCallback callback) {
    if (!isConnected()) {
        std::cerr << "[EVENT] Nicht verbunden!" << std::endl;
        return false;
    }
    
    if (m_running) {
        std::cerr << "[EVENT] Monitoring laeuft bereits!" << std::endl;
        return false;
    }
    
    m_callback = callback;
    m_running = true;
    
    m_readThread = CreateThread(nullptr, 0, ReadThreadProc, this, 0, nullptr);
    if (!m_readThread) {
        m_running = false;
        return false;
    }
    
    std::cout << "[EVENT] Monitoring gestartet!" << std::endl;
    return true;
}

void EventMonitor::stopMonitoring() {
    if (m_running) {
        m_running = false;
        
        if (m_readThread) {
            WaitForSingleObject(m_readThread, 2000);
            CloseHandle(m_readThread);
            m_readThread = nullptr;
        }
        
        std::cout << "[EVENT] Monitoring gestoppt." << std::endl;
    }
}

DWORD WINAPI EventMonitor::ReadThreadProc(LPVOID param) {
    EventMonitor* monitor = static_cast<EventMonitor*>(param);
    monitor->readLoop();
    return 0;
}

void EventMonitor::readLoop() {
    std::cout << "[EVENT] Read-Loop gestartet..." << std::endl;
    
    while (m_running) {
        ResetEvent(m_overlapped.hEvent);
        
        DWORD bytesRead = 0;
        BOOL result = ReadFile(m_device, m_buffer, sizeof(m_buffer), &bytesRead, &m_overlapped);
        
        if (!result && GetLastError() == ERROR_IO_PENDING) {
            DWORD waitResult = WaitForSingleObject(m_overlapped.hEvent, 1000);
            
            if (waitResult == WAIT_OBJECT_0) {
                if (GetOverlappedResult(m_device, &m_overlapped, &bytesRead, FALSE)) {
                    result = TRUE;
                }
            } else if (waitResult == WAIT_TIMEOUT) {
                CancelIo(m_device);
                continue;
            } else {
                break;
            }
        }
        
        if (result && bytesRead > 0 && m_callback) {
            HeadsetEvent event;
            event.type = static_cast<EventType>(m_buffer[0]);
            event.dataSize = bytesRead;
            memcpy(event.data, m_buffer, bytesRead);
            
            m_callback(event);
        }
    }
    
    std::cout << "[EVENT] Read-Loop beendet." << std::endl;
}

// ============================================================================
// HeadsetManager Implementation
// ============================================================================

HeadsetManager::HeadsetManager()
    : m_autoReconnect(false) {
}

HeadsetManager::~HeadsetManager() {
    disconnect();
}

bool HeadsetManager::connect(bool autoReconnect) {
    m_autoReconnect = autoReconnect;
    
    std::cout << "\n=== Verbinde mit HS80 Headset ===" << std::endl;
    
    bool rgbOk = m_rgb.connect();
    bool eventOk = m_events.connect();
    
    if (!rgbOk || !eventOk) {
        std::cerr << "\n[FEHLER] Konnte nicht alle Interfaces verbinden!" << std::endl;
        std::cerr << "  RGB:   " << (rgbOk ? "OK" : "FEHLGESCHLAGEN") << std::endl;
        std::cerr << "  Event: " << (eventOk ? "OK" : "FEHLGESCHLAGEN") << std::endl;
        return false;
    }
    
    std::cout << "\n=== Alle Interfaces verbunden! ===" << std::endl;
    return true;
}

void HeadsetManager::disconnect() {
    m_events.stopMonitoring();
    m_events.disconnect();
    m_rgb.disconnect();
}

bool HeadsetManager::isConnected() const {
    return m_rgb.isConnected() && m_events.isConnected();
}

bool HeadsetManager::setLEDs(RGBColor color) {
    return m_rgb.setColor(color);
}

bool HeadsetManager::setLEDs(const LEDZones& zones) {
    return m_rgb.setColors(zones);
}

bool HeadsetManager::setZone(LEDZone zone, RGBColor color) {
    return m_rgb.setZone(zone, color);
}

bool HeadsetManager::setBrightness(int percent) {
    return m_rgb.setBrightness(percent);
}

bool HeadsetManager::startEventMonitoring(EventCallback callback) {
    return m_events.startMonitoring(callback);
}

} // namespace HS80
