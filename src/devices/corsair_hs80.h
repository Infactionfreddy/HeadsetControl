#pragma once

#include "../device.h"

// ============================================================================
// Corsair HS80 RGB Wireless Gaming Headset Driver
// ============================================================================
// Supports: HS80 RGB USB/Wireless and HS80 RGB White USB/Wireless
// Protocol based on: HS80_Library.cpp and Corsair_Headset_Controller.js
//
// Features:
// - Per-zone RGB control (Logo, Power, Mic)
// - Per-zone brightness control (0-100%)
// - Battery status (wireless models only)
// - Microphone mute status query
// - Microphone LED brightness (0-3: Off, Low, Medium, High)
// - Auto-sleep timer (0-90 minutes)
// - Keep-alive to maintain software mode
// ============================================================================

void hs80_init(struct device** device);

// Export macro for Windows DLL builds
#ifdef _WIN32
    #define HS80_EXPORT __declspec(dllexport)
#else
    #define HS80_EXPORT
#endif

// ============================================================================
// Public API - RGB Zone Control
// ============================================================================

// Set RGB color for a specific zone (0=Logo, 1=Power, 2=Mic)
// Parameters: zone (0-2), r/g/b (0-255)
// Returns: 0 on success, negative on error
HS80_EXPORT int hs80_set_zone_rgb(hid_device* device_handle, uint8_t zone, 
                                    uint8_t r, uint8_t g, uint8_t b);

// Set RGB color for all zones at once
// Parameters: r/g/b (0-255)
// Returns: 0 on success, negative on error
HS80_EXPORT int hs80_set_all_rgb(hid_device* device_handle, 
                                  uint8_t r, uint8_t g, uint8_t b);

// Set brightness for a specific zone (software scaling)
// Parameters: zone (0-2), brightness (0-100%)
// Returns: 0 on success, negative on error
HS80_EXPORT int hs80_set_zone_brightness(hid_device* device_handle, 
                                          uint8_t zone, uint8_t brightness);

// ============================================================================
// Public API - Maintenance
// ============================================================================

// Keep software mode active (prevents auto-revert to hardware mode)
// Should be called periodically (e.g., every 5 seconds) when RGB control is active
// Returns: 0 on success, negative on error
HS80_EXPORT int hs80_keep_alive(hid_device* device_handle);

// ============================================================================
// Debug Helpers
// ============================================================================

// Print the last 64-byte HID packet that was sent to the device
// Useful for debugging protocol issues
HS80_EXPORT void hs80_dump_last_packet(void);
