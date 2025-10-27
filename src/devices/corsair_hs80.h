#pragma once

#include "../device.h"

// Corsair HS80 RGB Wireless Gaming Headset Support
// Based on protocol analysis from HS80_Library and Corsair_Headset_Controller.js

void hs80_init(struct device** device);

// Additional utility function (not yet exposed via standard HeadsetControl CLI)
// Returns: -1 on error, 0 if unmuted, 1 if muted
// int hs80_request_mic_status(hid_device* device_handle);
// Note: This function is available but requires CLI integration to be accessible
