#include "../device.h"
#include "../utility.h"

#include <hidapi.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static struct device device_hs80;

// ============================================================================
// HS80 Product IDs and Constants
// ============================================================================

#define ID_CORSAIR_HS80_RGB_USB              0x0A69
#define ID_CORSAIR_HS80_RGB_WIRELESS         0x0A6B
#define ID_CORSAIR_HS80_RGB_WHITE_USB        0x0A71
#define ID_CORSAIR_HS80_RGB_WHITE_WIRELESS   0x0A73

static const uint16_t PRODUCT_IDS[] = {
    ID_CORSAIR_HS80_RGB_USB,
    ID_CORSAIR_HS80_RGB_WIRELESS,
    ID_CORSAIR_HS80_RGB_WHITE_USB,
    ID_CORSAIR_HS80_RGB_WHITE_WIRELESS,
};

// HID Interface Constants (from JS and Library)
#define HS80_RGB_USAGE_PAGE     0xFF42
#define HS80_RGB_USAGE          0x0001  // Interface 6 for RGB control
#define HS80_EVENT_USAGE        0x0002  // Interface 7 for events

// Headset Mode Constants
#define HS80_MODE_WIRED         0x08
#define HS80_MODE_WIRELESS      0x09

// Command Types
#define HS80_CMD_PREFIX         0x02
#define HS80_CMD_SOFTWARE_MODE  0x01
#define HS80_CMD_BRIGHTNESS     0x02
#define HS80_CMD_LIGHTING       0x06
#define HS80_CMD_OPEN_LIGHTING  0x0D
#define HS80_CMD_READ_BATTERY   0x0F
#define HS80_CMD_READ_CHARGING  0x10
#define HS80_CMD_READ_MIC       0xA6

// Event Types (from HS80_Library.h)
#define HS80_EVENT_PREFIX       0x03
#define HS80_EVENT_MUTE         0xA6
#define HS80_EVENT_BATTERY      0x0F
#define HS80_EVENT_CHARGING     0x10

// State tracking
static bool hs80_software_mode_active = false;
static uint8_t hs80_current_brightness = 100; // 0-100%

// ============================================================================
// Helper Functions
// ============================================================================

// Check if device is wireless based on product ID
static bool hs80_is_wireless(uint16_t product_id)
{
    return (product_id == ID_CORSAIR_HS80_RGB_WIRELESS ||
            product_id == ID_CORSAIR_HS80_RGB_WHITE_WIRELESS);
}

// Convert 16-bit value to little endian bytes
static void hs80_write_le16(uint8_t* buffer, uint16_t value)
{
    buffer[0] = value & 0xFF;
    buffer[1] = (value >> 8) & 0xFF;
}

// Read 16-bit little endian value
static uint16_t hs80_read_le16(const uint8_t* buffer)
{
    return buffer[0] | (buffer[1] << 8);
}

// ============================================================================
// Low-Level Protocol Functions
// ============================================================================

// Initialize software mode for RGB control
static int hs80_init_software_mode(hid_device* device_handle, bool is_wireless)
{
    uint8_t packet[64] = {0};
    uint8_t headset_mode = is_wireless ? HS80_MODE_WIRELESS : HS80_MODE_WIRED;
    
    printf("[HS80] Initialisiere Software-Modus (Wireless: %s)\n", is_wireless ? "Ja" : "Nein");
    
    // WICHTIG: HS80 verwendet hid_write() statt hid_send_feature_report()
    // Die Pakete sind direkt 64 Bytes, ohne Report-ID-Prefix
    
    // Step 1: Enable Software Mode
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_SOFTWARE_MODE;
    packet[3] = 0x03;
    packet[4] = 0x00;
    packet[5] = 0x02; // Software mode
    
    int result = hid_write(device_handle, packet, 64);
    if (result < 0) {
        printf("[HS80] Fehler beim Aktivieren des Software-Modus\n");
        return result;
    }
    
    usleep(100000); // 100ms delay
    
    // Step 2: Open lighting endpoint
    memset(packet, 0, 64);
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_OPEN_LIGHTING;
    packet[3] = 0x00;
    packet[4] = 0x01;
    
    result = hid_write(device_handle, packet, 64);
    if (result < 0) {
        printf("[HS80] Fehler beim Öffnen des Lighting-Endpoints\n");
        return result;
    }
    
    usleep(100000);
    
    // Step 3: Set Hardware Brightness to 100%
    memset(packet, 0, 64);
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_SOFTWARE_MODE;
    packet[3] = HS80_CMD_BRIGHTNESS;
    packet[4] = 0x00;
    hs80_write_le16(&packet[5], 1000); // 1000 = 100%
    
    result = hid_write(device_handle, packet, 64);
    if (result < 0) {
        printf("[HS80] Fehler beim Setzen der Helligkeit\n");
        return result;
    }
    
    usleep(100000);
    
    hs80_software_mode_active = true;
    printf("[HS80] Software-Modus erfolgreich aktiviert\n");
    
    return 0;
}

// Restore hardware mode (disable software control)
static int hs80_restore_hardware_mode(hid_device* device_handle, bool is_wireless)
{
    uint8_t packet[64] = {0};
    uint8_t headset_mode = is_wireless ? HS80_MODE_WIRELESS : HS80_MODE_WIRED;
    
    printf("[HS80] Stelle Hardware-Modus wieder her\n");
    
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_SOFTWARE_MODE;
    packet[3] = 0x03;
    packet[4] = 0x00;
    packet[5] = 0x01; // Hardware mode
    
    int result = hid_write(device_handle, packet, 64);
    
    hs80_software_mode_active = false;
    
    return result;
}

// ============================================================================
// Feature Implementations
// ============================================================================

// Set LED colors (Logo, Power, Mic)
// Color format: RGB array [logo_r, logo_g, logo_b, power_r, power_g, power_b, mic_r, mic_g, mic_b]
static int hs80_set_led_colors(hid_device* device_handle, const uint8_t* rgb_colors)
{
    uint8_t packet[64] = {0};
    uint16_t product_id = device_hs80.idProduct;
    bool is_wireless = hs80_is_wireless(product_id);
    uint8_t headset_mode = is_wireless ? HS80_MODE_WIRELESS : HS80_MODE_WIRED;
    
    // Initialize software mode if not active
    if (!hs80_software_mode_active) {
        int result = hs80_init_software_mode(device_handle, is_wireless);
        if (result < 0) {
            return result;
        }
    }
    
    // Build RGB packet
    // Format from JS: [R,R,R,G,G,G,B,B,B] for all 3 LEDs
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_LIGHTING;
    packet[3] = 0x00;
    packet[4] = 0x09; // 9 bytes of RGB data
    packet[5] = 0x00;
    packet[6] = 0x00;
    packet[7] = 0x00;
    
    // Pack RGB data: R values at offset 8-10, G at 11-13, B at 14-16
    // Input: [logo_r, logo_g, logo_b, power_r, power_g, power_b, mic_r, mic_g, mic_b]
    packet[8]  = rgb_colors[0]; // Logo R
    packet[9]  = rgb_colors[3]; // Power R
    packet[10] = rgb_colors[6]; // Mic R
    
    packet[11] = rgb_colors[1]; // Logo G
    packet[12] = rgb_colors[4]; // Power G
    packet[13] = rgb_colors[7]; // Mic G
    
    packet[14] = rgb_colors[2]; // Logo B
    packet[15] = rgb_colors[5]; // Power B
    packet[16] = rgb_colors[8]; // Mic B
    
    return hid_write(device_handle, packet, 64);
}

// Switch lights on/off
static int hs80_lights(hid_device* device_handle, uint8_t on)
{
    // Default color: Corsair blue (0, 155, 222)
    uint8_t rgb_colors[9];
    
    if (on) {
        // All LEDs to Corsair blue
        for (int i = 0; i < 3; i++) {
            rgb_colors[i * 3 + 0] = 0;   // R
            rgb_colors[i * 3 + 1] = 155; // G
            rgb_colors[i * 3 + 2] = 222; // B
        }
    } else {
        // All LEDs off (black)
        memset(rgb_colors, 0, sizeof(rgb_colors));
    }
    
    return hs80_set_led_colors(device_handle, rgb_colors);
}

// Set brightness (0-100%)
static int hs80_set_brightness(hid_device* device_handle, uint8_t brightness)
{
    uint8_t packet[64] = {0};
    uint16_t product_id = device_hs80.idProduct;
    bool is_wireless = hs80_is_wireless(product_id);
    uint8_t headset_mode = is_wireless ? HS80_MODE_WIRELESS : HS80_MODE_WIRED;
    
    // Clamp brightness to 0-100
    if (brightness > 100) {
        brightness = 100;
    }
    
    // Convert percent to raw value (0-1000)
    uint16_t brightness_raw = (brightness * 1000) / 100;
    
    printf("[HS80] Setze Helligkeit: %d%% (%d raw)\n", brightness, brightness_raw);
    
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_SOFTWARE_MODE;
    packet[3] = HS80_CMD_BRIGHTNESS;
    packet[4] = 0x00;
    hs80_write_le16(&packet[5], brightness_raw);
    
    int result = hid_write(device_handle, packet, 64);
    
    if (result >= 0) {
        hs80_current_brightness = brightness;
    }
    
    return result;
}

// Request battery status (wireless only)
static BatteryInfo hs80_request_battery(hid_device* device_handle)
{
    BatteryInfo info = { -1, BATTERY_UNAVAILABLE, MICROPHONE_UNKNOWN };
    uint16_t product_id = device_hs80.idProduct;
    
    // Only supported on wireless models
    if (!hs80_is_wireless(product_id)) {
        printf("[HS80] Akku-Status nur bei Wireless-Modellen verfügbar\n");
        return info;
    }
    
    uint8_t packet[64] = {0};
    uint8_t response[64] = {0};
    
    // Request battery level
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = HS80_MODE_WIRELESS;
    packet[2] = HS80_CMD_BRIGHTNESS; // Read command
    packet[3] = HS80_CMD_READ_BATTERY;
    packet[4] = 0x00;
    
    hid_send_feature_report(device_handle, packet, 64);
    usleep(50000); // 50ms delay
    
    int result = hid_get_feature_report(device_handle, response, 64);
    
    if (result > 0 && response[3] == HS80_CMD_READ_BATTERY) {
        // Battery level is 16-bit little endian at bytes 4-5, range 0-1000
        uint16_t battery_raw = hs80_read_le16(&response[4]);
        info.level = battery_raw / 10; // Convert to percentage
        info.status = BATTERY_AVAILABLE;
        
        printf("[HS80] Akku: %d%%\n", info.level);
    }
    
    // Request charging status
    memset(packet, 0, 64);
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = HS80_MODE_WIRELESS;
    packet[2] = HS80_CMD_BRIGHTNESS;
    packet[3] = HS80_CMD_READ_CHARGING;
    packet[4] = 0x00;
    
    hid_send_feature_report(device_handle, packet, 64);
    usleep(50000);
    
    memset(response, 0, 64);
    result = hid_get_feature_report(device_handle, response, 64);
    
    if (result > 0 && response[3] == HS80_CMD_READ_CHARGING) {
        uint8_t charging = response[4];
        if (charging == 0x01) {
            info.status = BATTERY_CHARGING;
            printf("[HS80] Status: Wird geladen\n");
        }
    }
    
    return info;
}

// Request microphone mute status
static int hs80_request_mic_status(hid_device* device_handle)
{
    uint16_t product_id = device_hs80.idProduct;
    bool is_wireless = hs80_is_wireless(product_id);
    uint8_t headset_mode = is_wireless ? HS80_MODE_WIRELESS : HS80_MODE_WIRED;
    
    uint8_t packet[64] = {0};
    uint8_t response[64] = {0};
    
    // Request microphone status
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_BRIGHTNESS; // Read command
    packet[3] = HS80_CMD_READ_MIC;   // 0xA6 for HS80
    packet[4] = 0x00;
    
    hid_send_feature_report(device_handle, packet, 64);
    usleep(50000); // 50ms delay
    
    int result = hid_get_feature_report(device_handle, response, 64);
    
    if (result > 0 && response[3] == HS80_CMD_READ_MIC) {
        // Byte 4: 0x00 = unmuted, 0x01 = muted
        // (sometimes byte 5 is used instead, depends on firmware)
        uint8_t mute_status = response[4];
        
        if (mute_status == 0x01) {
            printf("[HS80] Mikrofon: STUMM\n");
            return 1; // Muted
        } else if (mute_status == 0x00) {
            printf("[HS80] Mikrofon: AKTIV\n");
            return 0; // Not muted
        } else {
            // Try byte 5 as fallback
            mute_status = response[5];
            printf("[HS80] Mikrofon: %s\n", (mute_status == 0x01) ? "STUMM" : "AKTIV");
            return mute_status;
        }
    }
    
    printf("[HS80] Fehler beim Lesen des Mikrofon-Status\n");
    return -1; // Error
}

// Send sidetone (not directly supported, kept as stub)
static int hs80_send_sidetone(hid_device* device_handle, uint8_t num)
{
    printf("[HS80] Sidetone wird vom HS80 nicht direkt unterstützt\n");
    return -1;
}

// Play notification sound (not directly supported, kept as stub)
static int hs80_notification_sound(hid_device* device_handle, uint8_t soundid)
{
    printf("[HS80] Notification Sound wird vom HS80 nicht direkt unterstützt\n");
    return -1;
}

// ============================================================================
// Device Registration
// ============================================================================

void hs80_init(struct device** device)
{
    device_hs80.idVendor            = VENDOR_CORSAIR;
    device_hs80.idProductsSupported = PRODUCT_IDS;
    device_hs80.numIdProducts       = sizeof(PRODUCT_IDS) / sizeof(PRODUCT_IDS[0]);

    strncpy(device_hs80.device_name, "Corsair HS80 RGB", sizeof(device_hs80.device_name));

    // Supported capabilities
    // Note: Microphone mute status can be read via hs80_request_mic_status()
    // but HeadsetControl doesn't have a standard capability for this yet.
    // The function is available for future CLI integration.
    device_hs80.capabilities = B(CAP_LIGHTS) | B(CAP_BATTERY_STATUS);
    
    // RGB control uses interface with usage page 0xFF42, usage 0x0001
    device_hs80.capability_details[CAP_LIGHTS] = (struct capability_detail) {
        .usagepage = HS80_RGB_USAGE_PAGE,
        .usageid = HS80_RGB_USAGE,
        .interface = 0
    };
    
    // Battery status (wireless only) uses same interface
    device_hs80.capability_details[CAP_BATTERY_STATUS] = (struct capability_detail) {
        .usagepage = HS80_RGB_USAGE_PAGE,
        .usageid = HS80_RGB_USAGE,
        .interface = 0
    };

    // Function pointers
    device_hs80.switch_lights      = &hs80_lights;
    device_hs80.request_battery    = &hs80_request_battery;
    device_hs80.send_sidetone      = &hs80_send_sidetone;
    device_hs80.notification_sound = &hs80_notification_sound;

    *device = &device_hs80;
    
    //printf("[HS80] Corsair HS80 RGB Treiber initialisiert\n");
    //printf("[HS80] Hinweis: Mikrofon-Mute-Status verfügbar (hs80_request_mic_status)\n");
}
