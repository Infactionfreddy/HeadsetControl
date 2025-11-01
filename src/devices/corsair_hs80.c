// ============================================================================
// Corsair HS80 RGB Wireless Gaming Headset Driver
// ============================================================================
// Supports: HS80 RGB USB (0x0A69), HS80 RGB Wireless (0x0A6B),
//           HS80 RGB White USB (0x0A71), HS80 RGB White Wireless (0x0A73)
//
// Protocol Reference:
// - HS80_Library.cpp (Windows C++ reference implementation)
// - Corsair_Headset_Controller.js (JavaScript protocol analyzer)
// ============================================================================

#include "../device.h"
#include "../utility.h"
#include "corsair_hs80.h"

#include <hidapi.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "../output.h"

// Debug print macros
#ifndef HSC_DEBUG
#define HSC_DEBUG(fmt, ...) do { if (!output_is_json()) fprintf(stderr, fmt, ##__VA_ARGS__); } while(0)
#endif

// Keepalive-specific debug prints (enabled at build time via -DENABLE_HS80_KEEPALIVE_DEBUG=ON)
#ifdef ENABLE_HS80_KEEPALIVE_DEBUG
#define HS80_KEEPALIVE_DEBUG(fmt, ...) HSC_DEBUG(fmt, ##__VA_ARGS__)
#else
#define HS80_KEEPALIVE_DEBUG(fmt, ...) do { } while(0)
#endif

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
#define HS80_CMD_INACTIVE_PREPARE  0x0D  // Prepare inactive time setting
#define HS80_CMD_INACTIVE_SET   0x0E  // Set inactive time value (milliseconds)
#define HS80_CMD_OPEN_LIGHTING  0x0D
#define HS80_CMD_READ_BATTERY   0x0F
#define HS80_CMD_READ_CHARGING  0x10
#define HS80_CMD_MIC_LED_BRIGHTNESS 0x13  // Microphone LED brightness
#define HS80_CMD_READ_MIC       0xA6

// Event Types (from HS80_Library.h)
#define HS80_EVENT_PREFIX       0x03
#define HS80_EVENT_MUTE         0xA6
#define HS80_EVENT_BATTERY      0x0F
#define HS80_EVENT_CHARGING     0x10

// State tracking
static bool hs80_software_mode_active = false;

// Current per-zone brightness (percent) and cached RGB values
// Zones: 0 = Logo, 1 = Power, 2 = Mic
static uint8_t hs80_zone_brightness[3] = { 100, 100, 100 };
static uint8_t hs80_current_rgb[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// Store last prepared 64-byte packet for debugging
static uint8_t hs80_last_packet[64] = {0};

// Forward declaration for new API
HS80_EXPORT int hs80_set_zone_brightness(hid_device* device_handle, uint8_t zone, uint8_t brightness);
/* Forward prototypes for helper functions used by hs80_keep_alive */
static bool hs80_is_wireless(uint16_t product_id);
static int hs80_init_software_mode(hid_device* device_handle, bool is_wireless);
/* hs80_set_led_colors is defined later but used by hs80_keep_alive; forward-declare it */
static int hs80_set_led_colors(hid_device* device_handle, const uint8_t* rgb_colors);
HS80_EXPORT int hs80_keep_alive(hid_device* device_handle)
{
    uint16_t product_id = device_hs80.idProduct;
    bool is_wireless = hs80_is_wireless(product_id);

    // If software mode is not active, initialize it (this will also open lighting endpoint)
    if (!hs80_software_mode_active) {
        int r = hs80_init_software_mode(device_handle, is_wireless);
        if (r < 0)
            return r;
        // init sets hs80_software_mode_active = true
    }

    // Send a small keep-alive / open-lighting packet to refresh the device state
    /* Instead of sending an open-lighting packet (which some firmwares
     * interpret as zeros and thus turn the LEDs off), resend the
     * current RGB lighting packet. This mirrors the behavior of the
     * reference HS80 library which periodically re-sends colors to keep
     * software mode active without changing the visible color. */
    int r = hs80_set_led_colors(device_handle, hs80_current_rgb);
    /* Use build-flag controlled debug macro so these prints can be enabled
     * without editing the code. By default they are compiled out. */
    HS80_KEEPALIVE_DEBUG("[HS80 DEBUG keepalive] hs80_set_led_colors returned %d\n", r);

    /* hs80_set_led_colors already saved the last packet into hs80_last_packet;
     * print it for debugging so user can inspect the exact bytes sent. */
    HS80_KEEPALIVE_DEBUG("[HS80 DEBUG keepalive] packet:");
    for (int i = 0; i < 64; i++) {
        HS80_KEEPALIVE_DEBUG(" %02X", hs80_last_packet[i]);
    }
    HS80_KEEPALIVE_DEBUG("\n");

    return r;
}

// Forward declarations
static int hs80_request_mic_status(hid_device* device_handle);
static int hs80_send_inactive_time(hid_device* device_handle, uint8_t minutes);
static int hs80_send_mic_led_brightness(hid_device* device_handle, uint8_t brightness);

// Rotate-to-mute stub: HS80 does not expose a command to enable/disable
// hardware auto-muting via mic rotation. Provide a stub so the CLI can
// display the option; calls will return HSC_ERROR (not supported).

// Rotate-to-mute stub implementation
static int hs80_switch_rotate_to_mute(hid_device* device_handle, uint8_t on)
{
    UNUSED(device_handle);
    if (on)
        fprintf(stderr, "[HS80] rotate-to-mute: enable requested, but not supported by HS80 hardware\n");
    else
        fprintf(stderr, "[HS80] rotate-to-mute: disable requested, but not supported by HS80 hardware\n");

    /* Indicate feature-specific error: not supported by this driver/hardware */
    return HSC_ERROR;
}

// LED Zone definitions
#define HS80_ZONE_LOGO  0
#define HS80_ZONE_POWER 1
#define HS80_ZONE_MIC   2
#define HS80_ZONE_ALL   255

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
    
    fprintf(stderr, "[HS80] Initialisiere Software-Modus (Wireless: %s)\n", is_wireless ? "Ja" : "Nein");
    
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
    fprintf(stderr, "[HS80] Fehler beim Aktivieren des Software-Modus\n");
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
    fprintf(stderr, "[HS80] Fehler beim Öffnen des Lighting-Endpoints\n");
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
    fprintf(stderr, "[HS80] Fehler beim Setzen der Helligkeit\n");
        return result;
    }
    
    usleep(100000);
    
    hs80_software_mode_active = true;
    fprintf(stderr, "[HS80] Software-Modus erfolgreich aktiviert\n");
    
    return 0;
}

// Note: Hardware mode restoration removed - HS80 automatically reverts
// to hardware mode after device disconnect or timeout

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
    
    // Ensure software mode is active (required for all RGB commands)
    if (!hs80_software_mode_active) {
    fprintf(stderr, "[HS80] Aktiviere Software-Modus für RGB-Steuerung...\n");
        int result = hs80_init_software_mode(device_handle, is_wireless);
        if (result < 0) {
            fprintf(stderr, "[HS80] Fehler: Software-Modus konnte nicht aktiviert werden\n");
            return result;
        }
        hs80_software_mode_active = true;
    }
    
    // Build RGB packet
    // Format from JS: [R,R,R,G,G,G,B,B,B] for all 3 LEDs
    // Apply per-zone brightness scaling before sending. We keep the provided rgb_colors
    // in the cached hs80_current_rgb array so future brightness changes can re-apply.
    for (int i = 0; i < 9; i++) {
        hs80_current_rgb[i] = rgb_colors[i];
    }

    uint8_t scaled_rgb[9];
    for (int zone = 0; zone < 3; zone++) {
        uint8_t zbright = hs80_zone_brightness[zone];
        int offset = zone * 3;
        for (int c = 0; c < 3; c++) {
            // Scale channel by zone brightness (0-100)
            scaled_rgb[offset + c] = (uint8_t)((hs80_current_rgb[offset + c] * zbright) / 100);
        }
    }
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_LIGHTING;
    packet[3] = 0x00;
    packet[4] = 0x09; // 9 bytes of RGB data
    packet[5] = 0x00;
    packet[6] = 0x00;
    packet[7] = 0x00;
    
    // Pack RGB data: R values at offset 8-10, G at 11-13, B at 14-16
    // scaled_rgb layout matches hs80_current_rgb layout
    packet[8]  = scaled_rgb[0];  // Logo R
    packet[9]  = scaled_rgb[3];  // Power R
    packet[10] = scaled_rgb[6];  // Mic R

    packet[11] = scaled_rgb[1];  // Logo G
    packet[12] = scaled_rgb[4];  // Power G
    packet[13] = scaled_rgb[7];  // Mic G

    packet[14] = scaled_rgb[2];  // Logo B
    packet[15] = scaled_rgb[5];  // Power B
    packet[16] = scaled_rgb[8];  // Mic B
    
    /* Save last packet for optional later dumping (do not print here) */
    memcpy(hs80_last_packet, packet, 64);

    int ret = hid_write(device_handle, packet, 64);
    return ret;
}

/* Public helper to print the last-prepared packet (useful to control ordering from caller) */
HS80_EXPORT void hs80_dump_last_packet(void)
{
    fprintf(stderr, "[HS80 DEBUG] packet:");
    for (int i = 0; i < 64; i++) {
        fprintf(stderr, " %02X", hs80_last_packet[i]);
    }
    fprintf(stderr, "\n");
}

// ============================================================================
// Lights Control (HeadsetControl Standard API)
// ============================================================================

// Standard lights on/off control - compatible with HeadsetControl API
// Uses Corsair blue (0, 155, 222) when turning on, black when turning off
static int hs80_lights(hid_device* device_handle, uint8_t on)
{
    if (on) {
        // Turn on: Set all zones to Corsair signature blue
        return hs80_set_all_rgb(device_handle, 0, 155, 222);
    } else {
        // Turn off: Set all zones to black
        return hs80_set_all_rgb(device_handle, 0, 0, 0);
    }
}

// ============================================================================
// Battery Status
// ============================================================================

// Request battery status (wireless only)
static BatteryInfo hs80_request_battery(hid_device* device_handle)
{
    BatteryInfo info = { -1, BATTERY_UNAVAILABLE, MICROPHONE_UNKNOWN };
    uint16_t product_id = device_hs80.idProduct;
    
    // Only supported on wireless models
    if (!hs80_is_wireless(product_id)) {
    fprintf(stderr, "[HS80] Akku-Status nur bei Wireless-Modellen verfügbar\n");
        return info;
    }
    
    uint8_t packet[64] = {0};
    uint8_t batteryLevelResponse[64] = {0};
    uint8_t chargingStatusResponse[64] = {0};
    
    fprintf(stderr, "[HS80] Frage Akku-Status ab...\n");
    
    // Clear any pending events from buffer (JS does this before first write)
    uint8_t discard[64];
    while (hid_read_timeout(device_handle, discard, 64, 10) > 0) {
        // Discard all pending events
    }
    
    // Request battery level
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = HS80_MODE_WIRELESS;
    packet[2] = HS80_CMD_BRIGHTNESS; // Read command (0x02)
    packet[3] = HS80_CMD_READ_BATTERY; // 0x0F
    packet[4] = 0x00;
    
    int result = hid_write(device_handle, packet, 64);
    if (result < 0) {
    fprintf(stderr, "[HS80] Fehler beim Senden der Battery-Anfrage\n");
        return info;
    }
    
    usleep(50000); // 50ms delay (matching JS implementation)
    
    // Clear buffer again before second request
    while (hid_read_timeout(device_handle, discard, 64, 10) > 0) {
        // Discard
    }
    
    // Request charging status
    memset(packet, 0, 64);
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = HS80_MODE_WIRELESS;
    packet[2] = HS80_CMD_BRIGHTNESS; // Read command (0x02)
    packet[3] = HS80_CMD_READ_CHARGING; // 0x10
    packet[4] = 0x00;
    
    result = hid_write(device_handle, packet, 64);
    if (result < 0) {
    fprintf(stderr, "[HS80] Fehler beim Senden der Charging-Anfrage\n");
        return info;
    }
    
    usleep(50000); // 50ms delay
    
    // Read responses - HS80 sends event format without command ID
    // Event format: [0x01 0x01 0x02 0x00 <data>...]
    // First response = battery level (bytes 4-5, little endian, divide by 10)
    // Second response = charging status (byte 4: 1=charging, 2=discharging, 3=full)
    
    fprintf(stderr, "[HS80] Lese Battery-Response...\n");
    result = hid_read_timeout(device_handle, batteryLevelResponse, 64, 500);
    
    if (result > 0) {
        // Show first bytes for debugging
    fprintf(stderr, "[HS80] Battery Response (%d bytes): [", result);
        for (int i = 0; i < (result < 10 ? result : 10); i++) {
            fprintf(stderr, "0x%02X ", batteryLevelResponse[i]);
        }
        printf("...]\n");
        
        // Event format: data starts at byte 4
        if (result >= 6) {
            uint16_t battery_raw = hs80_read_le16(&batteryLevelResponse[4]);
            info.level = battery_raw / 10; // Convert to percentage
            info.status = BATTERY_AVAILABLE;
            fprintf(stderr, "[HS80] Akku: %d%% (raw: %d)\n", info.level, battery_raw);
        }
    } else {
    fprintf(stderr, "[HS80] Keine Battery-Response (result: %d)\n", result);
    }
    
    fprintf(stderr, "[HS80] Lese Charging-Response...\n");
    result = hid_read_timeout(device_handle, chargingStatusResponse, 64, 500);
    
    if (result > 0) {
        // Show first bytes for debugging
    fprintf(stderr, "[HS80] Charging Response (%d bytes): [", result);
        for (int i = 0; i < (result < 10 ? result : 10); i++) {
            fprintf(stderr, "0x%02X ", chargingStatusResponse[i]);
        }
        printf("...]\n");
        
        // Event format: charging status at byte 4
        if (result >= 5) {
            uint8_t charging_status = chargingStatusResponse[4];
            // 1 = Charging, 2 = Discharging, 3 = Fully Charged
            if (charging_status == 0x01) {
                info.status = BATTERY_CHARGING;
                fprintf(stderr, "[HS80] Status: Wird geladen\n");
            } else if (charging_status == 0x03) {
                fprintf(stderr, "[HS80] Status: Vollständig geladen\n");
            } else if (charging_status == 0x02) {
                fprintf(stderr, "[HS80] Status: Entladen\n");
            } else {
                fprintf(stderr, "[HS80] Status: Unbekannt (0x%02X)\n", charging_status);
            }
        }
    } else {
    fprintf(stderr, "[HS80] Keine Charging-Response (result: %d)\n", result);
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
    
    fprintf(stderr, "[HS80] Frage Mikrofon-Status ab...\n");
    // Clear any pending events from buffer (mirror hs80_request_battery)
    uint8_t discard[64];
    while (hid_read_timeout(device_handle, discard, 64, 10) > 0) {
        /* discard */
    }

    // Request microphone status (using hid_write like battery requests)
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_BRIGHTNESS; // Read command
    packet[3] = HS80_CMD_READ_MIC;   // 0xA6 for HS80
    packet[4] = 0x00;

    int result = hid_write(device_handle, packet, 64);
    if (result < 0) {
    fprintf(stderr, "[HS80] Fehler beim Senden der Mic-Anfrage\n");
        return -1;
    }
    
    usleep(50000); // 50ms delay
    
    // Read response packets until we find one with the mic-read command echo (0xA6),
    // discarding stale/unrelated events. Try for up to 2 seconds total.
    int max_attempts = 10;
    int attempt = 0;
    int found_index = -1;
    uint8_t mute_status = 0xFF;

    while (attempt < max_attempts) {
        result = hid_read_timeout(device_handle, response, 64, 250);
        
        if (result <= 0) {
            // No data available in this read window; continue trying
            attempt++;
            continue;
        }

        // Show response for debugging (first 16 bytes for better visibility)
        fprintf(stderr, "[HS80] Mic Response #%d (%d bytes): [", attempt + 1, result);
        for (int i = 0; i < (result < 16 ? result : 16); i++) {
            fprintf(stderr, "0x%02X ", response[i]);
        }
        fprintf(stderr, "...]\n");

        // Defensive parsing: handle different event header formats
        // Format A (some FW):  [0x03 0x01 0x01 0xA6 <status> ...] - includes command echo
        // Format B (most FW):  [0x01 0x01 0x02 0x00 <status> ...] - NO command echo, status directly at byte 4
        found_index = -1;
        mute_status = 0xFF;

        // Check event header type
        if (result >= 5 && response[0] == 0x03) {
            // Event format: [0x03 0x01 0x01 <cmd> <status>]
            if (response[3] == HS80_CMD_READ_MIC) {
                found_index = 4;
                mute_status = response[4];
                fprintf(stderr, "[HS80] Event-Format 0x03 erkannt (mit Command-Echo), Status-Byte bei Index 4\n");
            }
        } else if (result >= 5 && response[0] == 0x01 && response[1] == 0x01 && response[2] == 0x02 && response[3] == 0x00) {
            // Standard event format (like battery/charging): [0x01 0x01 0x02 0x00 <status>]
            // This is the most common format - status is directly at byte 4, NO command echo
            found_index = 4;
            mute_status = response[4];
            fprintf(stderr, "[HS80] Event-Format 0x01 erkannt (Standard, kein Command-Echo), Status direkt bei Index 4\n");
        }

        // Fallback: search entire buffer for 0xA6 command byte (for exotic firmware variants)
        if (found_index == -1) {
            for (int i = 0; i < result - 1; i++) {
                if (response[i] == HS80_CMD_READ_MIC) {
                    found_index = i + 1;
                    mute_status = response[found_index];
                    fprintf(stderr, "[HS80] Fallback-Suche: Command-Echo bei Index %d, Status bei Index %d\n", i, found_index);
                    break;
                }
            }
        }

        if (found_index != -1) {
            // Found a packet with the mic-read command echo
            if (mute_status == 0x01) {
                fprintf(stderr, "[HS80] Mikrofon: STUMM (Muted)\n");
                return 1; // Muted
            } else if (mute_status == 0x00) {
                fprintf(stderr, "[HS80] Mikrofon: AKTIV (Unmuted)\n");
                return 0; // Not muted
            } else {
                // Unknown numeric payload - return raw byte so caller can inspect
                fprintf(stderr, "[HS80] Mikrofon: Unbekannter Status an Index %d (0x%02X)\n", found_index, mute_status);
                return (int)mute_status;
            }
        } else {
            // This packet does not contain the mic-read echo; discard and try next read
            fprintf(stderr, "[HS80] Paket #%d verworfen (kein Mic-Echo), warte auf nächstes...\n", attempt + 1);
        }

        attempt++;
    }

    // Exhausted all attempts without finding a matching response
    fprintf(stderr, "[HS80] Keine gültige Mic-Response nach %d Versuchen empfangen\n", max_attempts);
    return HSC_READ_TIMEOUT; // Use common read-timeout error code
}

// Set inactive/auto-sleep time (0-90 minutes, 0 = disabled)
// JS Reference: setIdleTimeout() uses commands 0x0D and 0x0E with milliseconds as 24-bit LE
static int hs80_send_inactive_time(hid_device* device_handle, uint8_t minutes)
{
    uint16_t product_id = device_hs80.idProduct;
    bool is_wireless = hs80_is_wireless(product_id);
    uint8_t headset_mode = is_wireless ? HS80_MODE_WIRELESS : HS80_MODE_WIRED;
    
    // Clamp to 0-90 minutes
    if (minutes > 90) {
        minutes = 90;
    }
    
    uint8_t packet[64] = {0};
    
    if (minutes == 0) {
        // Disable auto-sleep: [0x02, headsetMode, 0x01, 0x0D]
    fprintf(stderr, "[HS80] Deaktiviere Auto-Sleep Timer\n");
        
        packet[0] = HS80_CMD_PREFIX;
        packet[1] = headset_mode;
        packet[2] = HS80_CMD_SOFTWARE_MODE;
        packet[3] = HS80_CMD_INACTIVE_PREPARE;
        
        int result = hid_write(device_handle, packet, 64);
        if (result < 0) {
            fprintf(stderr, "[HS80] Fehler beim Deaktivieren der Inaktivitätszeit\n");
            return -1;
        }
    } else {
        // Enable auto-sleep with specific timeout
    fprintf(stderr, "[HS80] Setze Auto-Sleep Timer: %d Minuten\n", minutes);
        
        // Step 1: Prepare command [0x02, headsetMode, 0x01, 0x0D, 0x01]
        packet[0] = HS80_CMD_PREFIX;
        packet[1] = headset_mode;
        packet[2] = HS80_CMD_SOFTWARE_MODE;
        packet[3] = HS80_CMD_INACTIVE_PREPARE;
        packet[4] = 0x01;
        
        int result = hid_write(device_handle, packet, 64);
        if (result < 0) {
            fprintf(stderr, "[HS80] Fehler beim Vorbereiten der Inaktivitätszeit\n");
            return -1;
        }
        
        usleep(1000); // 1ms pause (JS: device.pause(1))
        
        // Step 2: Prepare command [0x02, headsetMode, 0x01, 0x0D, 0x00, 0x01]
        memset(packet, 0, 64);
        packet[0] = HS80_CMD_PREFIX;
        packet[1] = headset_mode;
        packet[2] = HS80_CMD_SOFTWARE_MODE;
        packet[3] = HS80_CMD_INACTIVE_PREPARE;
        packet[4] = 0x00;
        packet[5] = 0x01;
        
        result = hid_write(device_handle, packet, 64);
        if (result < 0) {
            fprintf(stderr, "[HS80] Fehler beim zweiten Vorbereitungsschritt\n");
            return -1;
        }
        
        usleep(1000); // 1ms pause
        
        // Step 3: Set timeout value [0x02, headsetMode, 0x01, 0x0E, 0x00, <timeout_ms_le_24bit>]
        // Convert minutes to milliseconds: minutes * 60000
        uint32_t timeout_ms = (uint32_t)minutes * 60000;
        
        memset(packet, 0, 64);
        packet[0] = HS80_CMD_PREFIX;
        packet[1] = headset_mode;
        packet[2] = HS80_CMD_SOFTWARE_MODE;
        packet[3] = HS80_CMD_INACTIVE_SET;
        packet[4] = 0x00; // Byte 4 is skipped in JS
        // 24-bit little endian at bytes 5-7
        packet[5] = (timeout_ms) & 0xFF;          // Low byte
        packet[6] = (timeout_ms >> 8) & 0xFF;     // Middle byte
        packet[7] = (timeout_ms >> 16) & 0xFF;    // High byte
        
        result = hid_write(device_handle, packet, 64);
        if (result < 0) {
            fprintf(stderr, "[HS80] Fehler beim Setzen der Inaktivitätszeit\n");
            return -1;
        }
        
    fprintf(stderr, "[HS80] Auto-Sleep Timer auf %d Minuten gesetzt (%u ms)\n", minutes, timeout_ms);
    }
    
    return 0;
}

// Set microphone mute LED brightness (0-3: Off, Low, Medium, High)
static int hs80_send_mic_led_brightness(hid_device* device_handle, uint8_t brightness)
{
    uint16_t product_id = device_hs80.idProduct;
    bool is_wireless = hs80_is_wireless(product_id);
    uint8_t headset_mode = is_wireless ? HS80_MODE_WIRELESS : HS80_MODE_WIRED;
    
    // Clamp to 0-3
    if (brightness > 3) {
        brightness = 3;
    }
    
    const char* brightness_names[] = {"Aus", "Niedrig", "Mittel", "Hoch"};
    fprintf(stderr, "[HS80] Setze Mikrofon-LED Helligkeit: %d (%s)\n", brightness, brightness_names[brightness]);
    
    // Ensure software mode is active (required for all RGB/LED commands)
    if (!hs80_software_mode_active) {
    fprintf(stderr, "[HS80] Aktiviere Software-Modus für LED-Steuerung...\n");
        int result = hs80_init_software_mode(device_handle, is_wireless);
        if (result < 0) {
            fprintf(stderr, "[HS80] Fehler: Software-Modus konnte nicht aktiviert werden\n");
            return -1;
        }
        hs80_software_mode_active = true;
    }
    
    uint8_t packet[64] = {0};
    packet[0] = HS80_CMD_PREFIX;
    packet[1] = headset_mode;
    packet[2] = HS80_CMD_MIC_LED_BRIGHTNESS;
    packet[3] = 0x00;
    packet[4] = brightness;  // 0=Off, 1=Low, 2=Medium, 3=High
    
    int result = hid_write(device_handle, packet, 64);
    if (result < 0) {
    fprintf(stderr, "[HS80] Fehler beim Setzen der Mikrofon-LED Helligkeit\n");
        return -1;
    }
    
    fprintf(stderr, "[HS80] Mikrofon-LED Helligkeit erfolgreich gesetzt\n");
    return 0;
}

// ============================================================================
// RGB Zone Control Functions
// ============================================================================

/**
 * @brief Set RGB color for a specific LED zone
 * Public API callable from main.c
 */
HS80_EXPORT int hs80_set_zone_rgb(hid_device* device_handle, uint8_t zone, uint8_t r, uint8_t g, uint8_t b)
{
    if (zone > 2) {
        fprintf(stderr, "[HS80] Ungültige Zone: %d (muss 0-2 sein)\n", zone);
        return -1;
    }

    uint16_t product_id = device_hs80.idProduct;
    bool is_wireless = hs80_is_wireless(product_id);

    // Auto-activate software mode if needed
    if (!hs80_software_mode_active) {
    fprintf(stderr, "[HS80] Aktiviere Software-Modus für RGB-Zonen-Steuerung...\n");
        if (hs80_init_software_mode(device_handle, is_wireless) < 0) {
            return -2;
        }
    }

    // Update cached RGB values for the specified zone
    uint8_t zone_offset = zone * 3;
    hs80_current_rgb[zone_offset + 0] = r;
    hs80_current_rgb[zone_offset + 1] = g;
    hs80_current_rgb[zone_offset + 2] = b;

    // Send the full RGB command (hs80_set_led_colors will apply per-zone brightness)
    return hs80_set_led_colors(device_handle, hs80_current_rgb);
}

/**
 * @brief Set RGB color for all LED zones at once
 * Public API callable from main.c
 */
HS80_EXPORT int hs80_set_all_rgb(hid_device* device_handle, uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t product_id = device_hs80.idProduct;
    bool is_wireless = hs80_is_wireless(product_id);

    // Auto-activate software mode if needed
    if (!hs80_software_mode_active) {
    fprintf(stderr, "[HS80] Aktiviere Software-Modus für RGB-Steuerung...\n");
        if (hs80_init_software_mode(device_handle, is_wireless) < 0) {
            return -1;
        }
    }

    // Set all zones to the same color and update cached values
    for (int zone = 0; zone < 3; zone++) {
        int off = zone * 3;
        hs80_current_rgb[off + 0] = r;
        hs80_current_rgb[off + 1] = g;
        hs80_current_rgb[off + 2] = b;
    }

    return hs80_set_led_colors(device_handle, hs80_current_rgb);
}

/**
 * @brief Set brightness for a specific RGB zone (0-100%).
 * This is emulated by scaling the RGB values for the zone and re-sending
 * the full RGB packet. If the device supports true per-zone brightness
 * natively in the future, this function can be updated to use that.
 */
HS80_EXPORT int hs80_set_zone_brightness(hid_device* device_handle, uint8_t zone, uint8_t brightness)
{
    if (zone > 2) {
        fprintf(stderr, "[HS80] Ungültige Zone für Helligkeit: %d (muss 0-2 sein)\n", zone);
        return -1;
    }

    if (brightness > 100) {
        brightness = 100;
    }

    hs80_zone_brightness[zone] = brightness;

    // Re-send current RGB values so the new brightness takes effect
    return hs80_set_led_colors(device_handle, hs80_current_rgb);
}

// ============================================================================
// Device Registration
// ============================================================================

void hs80_init(struct device** device)
{
    // Dummy calls to ensure RGB zone functions are included in binary
    // Never actually executed (would crash with NULL), but forces linker to include symbols
    if ((void*)device == (void*)0xDEADBEEF) {
        hs80_set_zone_rgb(NULL, 0, 0, 0, 0);
        hs80_set_all_rgb(NULL, 0, 0, 0);
    }
    
    device_hs80.idVendor            = VENDOR_CORSAIR;
    device_hs80.idProductsSupported = PRODUCT_IDS;
    device_hs80.numIdProducts       = sizeof(PRODUCT_IDS) / sizeof(PRODUCT_IDS[0]);

    strncpy(device_hs80.device_name, "Corsair HS80 RGB", sizeof(device_hs80.device_name));

    // Supported capabilities
    device_hs80.capabilities = B(CAP_LIGHTS) | B(CAP_RGB_ZONES) | B(CAP_BATTERY_STATUS) | 
                               B(CAP_INACTIVE_TIME) | B(CAP_MICROPHONE_MUTE_LED_BRIGHTNESS) | B(CAP_ROTATE_TO_MUTE);
    
    // RGB control uses interface with usage page 0xFF42, usage 0x0001
    device_hs80.capability_details[CAP_LIGHTS] = (struct capability_detail) {
        .usagepage = HS80_RGB_USAGE_PAGE,
        .usageid = HS80_RGB_USAGE,
        .interface = 0
    };
    
    // All capabilities use the same HID interface (RGB control interface)
    struct capability_detail hs80_interface = {
        .usagepage = HS80_RGB_USAGE_PAGE,
        .usageid = HS80_RGB_USAGE,
        .interface = 0
    };
    
    device_hs80.capability_details[CAP_LIGHTS]                          = hs80_interface;
    device_hs80.capability_details[CAP_BATTERY_STATUS]                  = hs80_interface;
    device_hs80.capability_details[CAP_INACTIVE_TIME]                   = hs80_interface;
    device_hs80.capability_details[CAP_MICROPHONE_MUTE_LED_BRIGHTNESS]  = hs80_interface;

    // Function pointers - standard HeadsetControl API
    device_hs80.switch_lights                       = &hs80_lights;
    device_hs80.request_battery                     = &hs80_request_battery;
    device_hs80.request_mic_status                  = &hs80_request_mic_status;
    device_hs80.send_inactive_time                  = &hs80_send_inactive_time;
    device_hs80.send_microphone_mute_led_brightness = &hs80_send_mic_led_brightness;
    device_hs80.switch_rotate_to_mute               = &hs80_switch_rotate_to_mute;
    
    // Function pointers - HS80-specific RGB zone API
    device_hs80.set_zone_rgb                        = &hs80_set_zone_rgb;
    device_hs80.set_all_rgb                         = &hs80_set_all_rgb;
    device_hs80.set_zone_brightness                 = &hs80_set_zone_brightness;

    *device = &device_hs80;
}
