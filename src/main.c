/***
    Copyright (C) 2016-2024 Denis Arnst (Sapd) <https://github.com/Sapd>

    This file is part of HeadsetControl.

    HeadsetControl is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    HeadsetControl is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with HeadsetControl.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "dev.h"
#include "device.h"
#include "device_registry.h"
#include "devices/corsair_hs80.h"
#include "hid_utility.h"
#include "output.h"
#include "utility.h"
#include "version.h"

#include <hidapi.h>

#include <assert.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#ifdef interface
#undef interface
#endif
#else
#include <pthread.h>
#include <time.h>
#endif

/* Globals used by HS80 keep-alive thread helpers */
static hid_device** g_device_handles = NULL;
static char** g_hid_paths = NULL;
static uint8_t* g_last_rgb = NULL; /* flattened [i*3 + 0..2] */
static bool* g_keepalive_running = NULL;
#ifdef _WIN32
static HANDLE* g_keepalive_threads = NULL;
#else
static pthread_t* g_keepalive_threads = NULL;
#endif
static int g_headset_available = 0;

#ifdef _WIN32
static DWORD WINAPI hs80_keepalive_thread_win(LPVOID arg) {
    int idx = *(int*)arg;
    free(arg);
    while (g_keepalive_running[idx]) {
        Sleep(2000);
        hid_device* h = g_device_handles[idx];
        if (h) {
            hs80_keep_alive(h);
        }
    }
    return 0;
}
#else
static void* hs80_keepalive_thread_posix(void* arg) {
    int idx = *(int*)arg;
    free(arg);
    while (g_keepalive_running[idx]) {
        struct timespec ts = {2, 0};
        nanosleep(&ts, NULL);
        hid_device* h = g_device_handles[idx];
        if (h) {
            hs80_keep_alive(h);
        }
    }
    return NULL;
}
#endif

/* Local keep-alive structure for server-mode instance */
struct LocalKeepAlive {
    hid_device* handle;
    uint8_t last_rgb[3];
    volatile int running;
};

#ifdef _WIN32
static DWORD WINAPI local_keepalive_thread_win(LPVOID arg) {
    struct LocalKeepAlive* ka = (struct LocalKeepAlive*)arg;
    while (ka->running) {
        Sleep(2000);
        if (ka->handle) {
            /* Use hs80_keep_alive to refresh software mode without changing colors */
            hs80_keep_alive(ka->handle);
        }
    }
    free(ka);
    return 0;
}
#else
static void* local_keepalive_thread_posix(void* arg) {
    struct LocalKeepAlive* ka = (struct LocalKeepAlive*)arg;
    while (ka->running) {
        struct timespec ts = {2, 0};
        nanosleep(&ts, NULL);
        if (ka->handle) {
            /* Use hs80_keep_alive to refresh software mode without changing colors */
            hs80_keep_alive(ka->handle);
        }
    }
    free(ka);
    return NULL;
}
#endif

int test_profile = 0;

int hsc_device_timeout = 5000;

typedef struct {
    int vendor_id;
    int product_id;
    int test_device;
} SearchParameters;

/**
 * @brief Finds and initializes a list of devices.
 *
 * This function allocates a list of devices.
 * If the test_device flag is set, it attempts to add a test device to the list.
 *
 * @param device_list A pointer to the list of devices to be populated.
 * @param test_device A flag indicating whether to add a test device to the list.
 * @return The number of devices found and added to the list.
 */
static int find_devices(DeviceList** device_list, SearchParameters parameters)
{
    DeviceListNode* devices_found = malloc(sizeof(DeviceListNode));
    devices_found->element        = malloc(sizeof(struct device));
    DeviceListNode* curr_node     = devices_found;
    int found                     = 0;

    // Adding test device
    if (parameters.test_device) {
        if (!get_device(devices_found->element, VENDOR_TESTDEVICE, PRODUCT_TESTDEVICE)) {
            curr_node->next          = malloc(sizeof(struct DeviceListNode));
            curr_node->next->element = malloc(sizeof(struct device));
            curr_node                = devices_found->next;
            found++;
        }
    }

    struct hid_device_info* devs;
    struct hid_device_info* cur_dev;
    devs               = hid_enumerate(parameters.vendor_id, parameters.product_id);
    cur_dev            = devs;
    bool already_found = false;

    // Iterate through all devices and the compatible to device_found list
    while (cur_dev) {
        already_found             = false;
        DeviceListNode* temp_node = devices_found;
        while (temp_node != curr_node) {
            if (temp_node->element->idVendor == cur_dev->vendor_id && temp_node->element->idProduct == cur_dev->product_id) {
                already_found = true;
                break;
            }
            temp_node = temp_node->next;
        }
        if (already_found) {
            cur_dev = cur_dev->next;
            continue;
        }
        if (!get_device(curr_node->element, cur_dev->vendor_id, cur_dev->product_id)) {
            found++;
            curr_node->next          = malloc(sizeof(struct DeviceListNode));
            curr_node->next->element = malloc(sizeof(struct device));
            curr_node                = curr_node->next;
        }
        cur_dev = cur_dev->next;
    }
    free(curr_node->element);
    free(curr_node);

    // Copy by address the found devices to the device_list
    *device_list = malloc(sizeof(DeviceList) * found);
    curr_node    = devices_found;
    for (int i = 0; i < found; i++) {
        DeviceList* device_element      = *device_list + i;
        device_element->device          = curr_node->element;
        device_element->num_devices     = found - i;
        device_element->featureRequests = NULL;
        device_element->size            = 0;

        curr_node = curr_node->next;
        free(devices_found);
        devices_found = curr_node;
    }
    hid_free_enumeration(devs);

    return found;
}

/**
 * @brief Generates udev rules, and prints them to STDOUT
 *
 * Goes through the implementation of all devices, and generates udev rules for use by Linux systems
 */
static void print_udevrules()
{
    int i = 0;
    struct device* device_found;

    printf("ACTION!=\"add|change\", GOTO=\"headset_end\"\n");
    printf("\n");

    while (iterate_devices(i++, &device_found) == 0) {
        printf("# %s\n", device_found->device_name);

        for (int i = 0; i < device_found->numIdProducts; i++)
            printf("KERNEL==\"hidraw*\", SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"%04x\", ATTRS{idProduct}==\"%04x\", TAG+=\"uaccess\"\n",
                (unsigned int)device_found->idVendor, (unsigned int)device_found->idProductsSupported[i]);

        printf("\n");
    }

    printf("LABEL=\"headset_end\"\n");
}

static void print_readmetable()
{
    int i = 0;
    struct device* device_found;

    printf("| Device |");
    for (int j = 0; j < NUM_CAPABILITIES; j++) {
        printf(" %s |", capabilities_str[j]);
    }
    printf("\n");

    printf("| --- |");
    for (int j = 0; j < NUM_CAPABILITIES; j++) {
        printf(" --- |");
    }
    printf("\n");

    while (iterate_devices(i++, &device_found) == 0) {
        printf("| %s |", device_found->device_name);

        for (int j = 0; j < NUM_CAPABILITIES; j++) {
            if (device_has_capability(device_found, j)) {
                printf(" x |");
            } else {
                printf("   |");
            }
        }
        printf("\n");
    }
}

/**
 * @brief Checks if an existing connection exists, and either uses it, or closes it and creates a new one
 *
 * A device - depending on the feature - needs different Endpoints/Connections
 * Instead of opening and keeping track of multiple connections, we close and open connections no demand
 *
 *
 *
 * @param existing_hid_path an existing connection path or NULL if none yet
 * @param device_handle an existing device handle or NULL if none yet
 * @param device headsetcontrol struct, containing vendor and productid
 * @param cap which capability to use, to determine interfaceid and usageids
 * @return hid_device pointer if successful, or NULL (error in hid_error)
 */
static hid_device* dynamic_connect(char** existing_hid_path, hid_device* device_handle,
    struct device* device, enum capabilities cap)
{
    // Generate the path which is needed
    char* hid_path = get_hid_path(device->idVendor, device->idProduct,
        device->capability_details[cap].interface, device->capability_details[cap].usagepage, device->capability_details[cap].usageid);

    if (!hid_path) {
        return NULL;
    }

    // A connection already exists
    if (device_handle != NULL) {
        // The connection which exists is the same we need, so simply return it
        if (strcmp(*existing_hid_path, hid_path) == 0) {
            return device_handle;
        } else { // if its not the same, and already one connection is open, close it so we can make a new one
            hid_close(device_handle);
        }
    }

    free(*existing_hid_path);

    device_handle = hid_open_path(hid_path);
    if (device_handle == NULL) {
        *existing_hid_path = NULL;
        return NULL;
    }

    hid_get_manufacturer_string(device_handle, device->device_hid_vendorname, sizeof(device->device_hid_vendorname) / sizeof(device->device_hid_vendorname[0]));
    hid_get_product_string(device_handle, device->device_hid_productname, sizeof(device->device_hid_productname) / sizeof(device->device_hid_productname[0]));

    *existing_hid_path = hid_path;
    return device_handle;
}

/**
 * @brief Handle a requested feature
 *
 * @param device_found the headset to use
 * @param device_handle points to an already open device_handle (if connection already exists) or points to null
 * @param hid_path points to an already used path used to connect, or points to null
 * @param cap requested feature
 * @param param first parameter of the feature
 * @return FeatureResult which saves the result or failure of the requested feature
 */
static FeatureResult handle_feature(struct device* device_found, hid_device** device_handle, char** hid_path, enum capabilities cap, void* param)
{
    FeatureResult result;

    // Check if the headset implements the requested feature
    if ((device_found->capabilities & B(cap)) == 0) {
        result.status = FEATURE_ERROR;
        result.value  = -1;
        _asprintf(&result.message, "This headset doesn't support %s", capabilities_str[cap]);
        return result;
    }

    if (device_found->idProduct != PRODUCT_TESTDEVICE) {
        *device_handle = dynamic_connect(hid_path, *device_handle,
            device_found, cap);

        if (!device_handle | !(*device_handle)) {
            result.status = FEATURE_DEVICE_FAILED_OPEN;
            result.value  = 0;
            _asprintf(&result.message, "Could not open device. Error: %ls", hid_error(*device_handle));
            return result;
        }
    } else {
        *device_handle = NULL;
    }

    int ret;

    switch (cap) {
    case CAP_SIDETONE:
        /* Forward sidetone request to device implementation */
        ret = device_found->send_sidetone(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_BATTERY_STATUS: {
        BatteryInfo battery = device_found->request_battery(*device_handle);

        result.status2 = battery.status;
        if (battery.status == BATTERY_AVAILABLE) {
            result.status = FEATURE_SUCCESS;
            result.value  = battery.level;
            _asprintf(&result.message, "Battery: %d%%", battery.level);
        } else if (battery.status == BATTERY_CHARGING) {
            result.status  = FEATURE_INFO;
            result.value   = battery.level;
            result.message = strdup("Charging");
        } else if (battery.status == BATTERY_UNAVAILABLE) {
            result.status  = FEATURE_INFO;
            result.value   = BATTERY_UNAVAILABLE;
            result.message = strdup("Battery status unavailable");
        } else if (battery.status == BATTERY_TIMEOUT) {
            result.status  = FEATURE_ERROR;
            result.value   = BATTERY_TIMEOUT;
            result.message = strdup("Battery status request timed out");
        } else { // Handle errors
            result.status = FEATURE_ERROR;
            result.value  = (int)battery.status;

            if (device_found->idProduct != PRODUCT_TESTDEVICE)
                _asprintf(&result.message, "Error retrieving battery status. Error: %ls", hid_error(*device_handle));
            else // dont call hid_error on test device
                _asprintf(&result.message, "Error retrieving battery status");
        }
        return result;
    }

    case CAP_NOTIFICATION_SOUND:
        ret = device_found->notification_sound(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_LIGHTS:
        ret = device_found->switch_lights(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_INACTIVE_TIME:
        ret = device_found->send_inactive_time(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_CHATMIX_STATUS:
        ret = device_found->request_chatmix(*device_handle);

        if (ret >= 0) {
            result.status = FEATURE_SUCCESS;
            result.value  = ret;
            _asprintf(&result.message, "Chat-Mix: %d", ret);
        } else {
            result.status  = FEATURE_ERROR;
            result.value   = ret;
            result.message = strdup("Error retrieving chatmix status");
        }

        return result;

    case CAP_VOICE_PROMPTS:
        ret = device_found->switch_voice_prompts(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_ROTATE_TO_MUTE:
        ret = device_found->switch_rotate_to_mute(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_EQUALIZER_PRESET:
        ret = device_found->send_equalizer_preset(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_EQUALIZER:
        ret = device_found->send_equalizer(*device_handle, (struct equalizer_settings*)param);
        break;

    case CAP_PARAMETRIC_EQUALIZER:
        ret = device_found->send_parametric_equalizer(*device_handle, (struct parametric_equalizer_settings*)param);
        break;

    case CAP_MICROPHONE_MUTE_LED_BRIGHTNESS:
        ret = device_found->send_microphone_mute_led_brightness(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_MICROPHONE_VOLUME:
        ret = device_found->send_microphone_volume(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_VOLUME_LIMITER:
        ret = device_found->send_volume_limiter(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_BT_WHEN_POWERED_ON:
        ret = device_found->send_bluetooth_when_powered_on(*device_handle, (uint8_t) * (int*)param);
        break;

    case CAP_BT_CALL_VOLUME:
        ret = device_found->send_bluetooth_call_volume(*device_handle, (uint8_t) * (int*)param);
        break;

    case NUM_CAPABILITIES:
    default:
        ret = -99; // silence warning
        UNUSED(ret);

        assert(0);
        break;
    }

    // Handle success
    if (ret >= 0) {
        result.status  = FEATURE_SUCCESS;
        result.value   = ret;
        result.message = NULL;
        return result;
    }

    result.status = FEATURE_ERROR;
    result.value  = ret;

    switch (ret) {
    case HSC_READ_TIMEOUT:
        _asprintf(&result.message, "Failed to set/request %s, because of timeout", capabilities_str[cap]);
        break;
    case HSC_ERROR:
        _asprintf(&result.message, "Failed to set/request %s. HeadsetControl Error", capabilities_str[cap]);
        break;
    case HSC_OUT_OF_BOUNDS:
        _asprintf(&result.message, "Failed to set/request %s. Provided parameter out of boundaries", capabilities_str[cap]);
        break;
    case HSC_INVALID_ARG:
        _asprintf(&result.message, "Failed to set/request %s. Provided parameter invalid", capabilities_str[cap]);
        break;
    default: // Must be a HID error
        if (device_found->idProduct != PRODUCT_TESTDEVICE)
            _asprintf(&result.message, "Failed to set/request %s. Error: %d: %ls", capabilities_str[cap], ret, hid_error(*device_handle));
        else // dont call hid_error on test device, it will confuse users/devs because it will show success
            _asprintf(&result.message, "Failed to set/request %s. Error: %d", capabilities_str[cap], ret);

        break;
    }

    return result;
}

void print_help(char* programname, struct device* device_found, bool show_all)
{
    printf("HeadsetControl by Sapd (Denis Arnst)\n\thttps://github.com/Sapd/HeadsetControl\n\n");
    printf("Version: %s\n\n", VERSION);
    // printf("Usage: %s [options]\n", programname);
    // printf("Options:\n");

    printf("Select device:\n");
    printf("  -d, --device vendorid:productid\n");
    printf("\t\t\t\tVendor ID and product ID separated by a colon.\n");
    printf("\n");

    if (show_all || device_has_capability(device_found, CAP_SIDETONE)) {
        printf("Sidetone:\n");
        printf("  -s, --sidetone LEVEL\t\tSet sidetone level (0-128)\n");
        printf("\n");
    }

    if (show_all || device_has_capability(device_found, CAP_BATTERY_STATUS)) {
        printf("Battery:\n");
        printf("  -b, --battery\t\t\tCheck battery level\n");
        printf("\n");
    }

    // ------ Category: lights and notifications
    bool show_lights        = show_all || device_has_capability(device_found, CAP_LIGHTS);
    bool show_voice_prompts = show_all || device_has_capability(device_found, CAP_VOICE_PROMPTS);

    if (show_lights || show_voice_prompts) {
        printf("%s:\n", (show_lights && show_voice_prompts) ? "Lights and Voice Prompts" : (show_lights ? "Lights" : "Voice Prompts"));
        if (show_lights) {
            printf("  -l, --light [0|1]\t\tTurn lights off (0) or on (1)\n");
            printf("  --rgb-logo R,G,B\t\tSet logo RGB color (0-255 each)\n");
            printf("  --rgb-power R,G,B\t\tSet power RGB color (0-255 each)\n");
            printf("  --rgb-mic R,G,B\t\tSet mic RGB color (0-255 each)\n");
            printf("  --rgb-all R,G,B\t\tSet all zones to same RGB color (0-255 each)\n");
            printf("  --rgb-logo-brightness NUMBER\tSet logo brightness (0-100)\n");
            printf("  --rgb-power-brightness NUMBER\tSet power brightness (0-100)\n");
            printf("  --rgb-mic-brightness NUMBER\tSet mic brightness (0-100)\n");
        }
        if (show_voice_prompts) {
            printf("  -v, --voice-prompt [0|1]\tTurn voice prompts off (0) or on (1)\n");
        }
        printf("\n");
    }
    // ------

    // ------ Category: Features
    bool show_inactive_time      = show_all || device_has_capability(device_found, CAP_INACTIVE_TIME);
    bool show_chatmix_status     = show_all || device_has_capability(device_found, CAP_CHATMIX_STATUS);
    bool show_notification_sound = show_all || device_has_capability(device_found, CAP_NOTIFICATION_SOUND);
    bool show_volume_limiter     = show_all || device_has_capability(device_found, CAP_VOLUME_LIMITER);

    if (show_inactive_time || show_chatmix_status || show_notification_sound) {
        printf("Features:\n");
        if (show_inactive_time) {
            printf("  -i, --inactive-time MINUTES\tSet inactive time (0-90 minutes, 0 disables)\n");
        }
        if (show_chatmix_status) {
            printf("  -m, --chatmix\t\t\tGet chat-mix-dial level (0-128, <64 for game, >64 for chat)\n");
        }
        if (show_notification_sound) {
            printf("  -n, --notificate SOUNDID\tPlay notification sound (SOUNDID depends on device)\n");
        }
        if (show_volume_limiter) {
            printf("  --volume-limiter [0|1]\tTurn Volume limiter off (0) or on (1)\n");
        }
        printf("\n");
    }
    // ------

    // ------ Category: Equalizer
    bool show_equalizer            = show_all || device_has_capability(device_found, CAP_EQUALIZER);
    bool show_equalizer_preset     = show_all || device_has_capability(device_found, CAP_EQUALIZER_PRESET);
    bool show_parametric_equalizer = show_all || device_has_capability(device_found, CAP_PARAMETRIC_EQUALIZER);

    if (show_equalizer || show_equalizer_preset) {
        printf("Equalizer:\n");
        if (show_equalizer) {
            printf("  -e, --equalizer STRING\tSet equalizer curve (values separated by spaces, commas, or new-lines)\n");
        }
        if (show_equalizer_preset) {
            printf("  -p, --equalizer-preset NUMBER\tSet equalizer preset (0-3, 0 for default)\n");
        }
        printf("\n");
    }

    if (show_parametric_equalizer) {
        // TODO parametric equalizer
        printf("Parametric Equalizer:\n");
        printf("  --parametric-equalizer STRING\t\tSet equalizer bands (bands separated by semicolon)\n");
        printf("      Band format:\t\t\tFREQUENCY,GAIN,Q_FACTOR,FILTER_TYPE\n");
        printf("      Available filter types:\t\t");
        for (int i = 0; i < NUM_EQ_FILTER_TYPES; i++) {
            if (show_all || has_capability(device_found->parametric_equalizer->filter_types, i)) {
                printf("%s, ", equalizer_filter_type_str[i]);
            }
        }
        printf("\n\n");
        printf("      Examples:\t--parametric-equalizer\t'300,3.5,1.5,peaking;14000,-2,1.414,highshelf'\n");
        printf("      \t\t\t\t\tSets a 300Hz +3.5dB Q1.5 peaking filter\n\t\t\t\t\tand a 14kHz -2dB Q1.414 highshelf filter\n");
        printf("\n");
        printf("      \t\t--parametric-equalizer\treset\n");
        printf("      \t\t\t\t\tResets/disables all bands");
        printf("\n");
    }
    // ------

    // ------ Category: Microphone
    bool show_rotate_to_mute                 = show_all || device_has_capability(device_found, CAP_ROTATE_TO_MUTE);
    bool show_microphone_mute_led_brightness = show_all || device_has_capability(device_found, CAP_MICROPHONE_MUTE_LED_BRIGHTNESS);
    bool show_microphone_volume              = show_all || device_has_capability(device_found, CAP_MICROPHONE_VOLUME);

    if (show_rotate_to_mute || show_microphone_mute_led_brightness || show_microphone_volume) {
        printf("Microphone:\n");
        if (show_rotate_to_mute) {
            printf("  -r, --rotate-to-mute [0|1]\t\t\tToggle rotate to mute (0 = off, 1 = on)\n");
        }
        if (show_microphone_mute_led_brightness || show_rotate_to_mute) {
            /* Keep existing mic options grouped */
        }
        if (show_microphone_mute_led_brightness) {
            printf("  --microphone-mute-led-brightness NUMBER\tSet mic mute LED brightness (0-3)\n");
        }
        /* New explicit mic-status query */
        printf("  --mic-status\t\t\t	Query microphone mute status (prints 0=unmuted,1=muted)\n");
        if (show_microphone_mute_led_brightness) {
        }
        if (show_microphone_volume) {
            printf("  --microphone-volume NUMBER\t\t\tSet microphone volume (0-128)\n");
        }
        printf("\n");
    }
    // ------

    // ------ Category: Bluetooth
    bool show_bt_when_powered_on = show_all || device_has_capability(device_found, CAP_BT_WHEN_POWERED_ON);
    bool show_bt_call_volume     = show_all || device_has_capability(device_found, CAP_BT_CALL_VOLUME);

    if (show_bt_when_powered_on || show_bt_call_volume) {
        printf("Bluetooth:\n");
        if (show_bt_when_powered_on) {
            printf("  --bt-when-powered-on [0|1]\tToggle bluetooth turning off (0) or on (1) when turning on headpohnes\n");
        }
        if (show_bt_call_volume) {
            printf("  --bt-call-volume NUMBER\tSet headphones volume during a bluetooth call by lowering pc volume (0-2)\n");
        }
        printf("\n");
    }
    // ------

    if (show_all) {
        printf("Advanced:\n");
        printf("  -f, --follow [SECS]\t\tRe-run commands after SECS seconds (default 2 seconds if not specified)\n");
        printf("  --timeout MS\t\t\tSet timeout for reading data (0-100000 ms, default 5000)\n");
        printf("  -?, --capabilities\t\tList supported features of the connected headset\n\n");

        printf("Miscellaneous:\n");
        printf("  -u\t\t\t\tOutput udev rules to stdout/console\n");
        printf("  --dev\t\t\t\tDevelopment options\n");
        printf("  --readme-helper\t\tOutput table of device features for README.md\n");
        printf("  --test-device [profile]\tUse a built-in test device instead of a real one\n");
        printf("                         \t profile is an optional number for different tests\n");
        printf("  --connected\t\t\tCheck if device connected (for scripting purposes)\n");
        printf("  -o, --output FORMAT\t\tOutput format (JSON, YAML, ENV, STANDARD)\n");
        printf("\n");
    }

    if (show_all || device_has_capability(device_found, CAP_SIDETONE) || device_has_capability(device_found, CAP_BATTERY_STATUS)) {
        printf("Examples:\n");
        if (show_all || device_has_capability(device_found, CAP_BATTERY_STATUS))
            printf("  %s -b\t\tCheck the battery level\n", programname);
        if (show_all || device_has_capability(device_found, CAP_SIDETONE))
            printf("  %s -s 64\tSet sidetone level to 64\n", programname);
        if (show_all || (device_has_capability(device_found, CAP_LIGHTS) && device_has_capability(device_found, CAP_SIDETONE)))
            printf("  %s -l 1 -s 0\tTurn on lights and deactivate sidetone\n", programname);
        printf("\n");
    }

    if (!show_all && device_found)
        printf("Hint:\tOptions were filtered to your device (%s)\n\tUse --help-all to show all options (including advanced ones)\n", device_found->device_name);
}

// for --follow
volatile sig_atomic_t follow = false;

void interruptHandler(int signal_number)
{
    UNUSED(signal_number);
    follow = false;
}

// Makes parsing of optional arguments easier
// Credits to https://cfengine.com/blog/2021/optional-arguments-with-getopt-long/
#define OPTIONAL_ARGUMENT_IS_PRESENT                             \
    ((optarg == NULL && optind < argc && argv[optind][0] != '-') \
            ? (bool)(optarg = argv[optind++])                    \
            : (optarg != NULL))

/**
 * @brief Parse RGB color string in format "R,G,B"
 * 
 * @param str Input string (e.g., "255,128,0")
 * @param rgb Output array [3] for R, G, B values
 * @return 0 on success, -1 on error
 */
static int parse_rgb_string(const char* str, int rgb[3])
{
    if (!str || !rgb)
        return -1;

    char* endptr;
    char* str_copy = strdup(str);
    char* token;
    int count = 0;

    token = strtok(str_copy, ",");
    while (token != NULL && count < 3) {
        long value = strtol(token, &endptr, 10);
        if (*endptr != '\0' || endptr == token || value < 0 || value > 255) {
            free(str_copy);
            return -1;
        }
        rgb[count++] = (int)value;
        token         = strtok(NULL, ",");
    }

    free(str_copy);
    return (count == 3) ? 0 : -1;
}

int main(int argc, char* argv[])
{
    int c;

    int selected_vendor_id  = 0;
    int selected_product_id = 0;

    int should_print_help                                      = 0;
    int should_print_help_all                                  = 0;
    int print_udev_rules                                       = 0;
    int sidetone_loudness                                      = -1;
    int request_battery                                        = 0;
    int request_chatmix                                        = 0;
    int request_mic_status                                     = 0;
    int request_connected                                      = 0;
    int notification_sound                                     = -1;
    int lights                                                 = -1;
    int inactive_time                                          = -1;
    int voice_prompts                                          = -1;
    int rotate_to_mute                                         = -1;
    int print_capabilities                                     = -1;
    int equalizer_preset                                       = -1;
    int microphone_mute_led_brightness                         = -1;
    int microphone_volume                                      = -1;
    int volume_limiter                                         = -1;
    int bt_when_powered_on                                     = -1;
    int bt_call_volume                                         = -1;
    int dev_mode                                               = 0;
    unsigned follow_sec                                        = 2;
    struct equalizer_settings* equalizer                       = NULL;
    struct parametric_equalizer_settings* parametric_equalizer = NULL;
    
    // RGB Zone control (HS80 specific)
    int rgb_logo[3]  = { -1, -1, -1 };  // R, G, B
    int rgb_power[3] = { -1, -1, -1 };  // R, G, B
    int rgb_mic[3]   = { -1, -1, -1 };  // R, G, B
    int rgb_all[3]   = { -1, -1, -1 };  // R, G, B
    int rgb_logo_brightness  = -1; // 0-100
    int rgb_power_brightness = -1; // 0-100
    int rgb_mic_brightness_percent   = -1; // 0-100 (percentage for mic zone)
    int server_mode = 0;
    int debug_mode = 0;

    OutputType output_format = OUTPUT_STANDARD;
    int test_device          = 0;

#define BUFFERLENGTH 1024
    float* read_buffer = calloc(BUFFERLENGTH, sizeof(float));

    struct option opts[] = {
        { "device", required_argument, NULL, 'd' },
        { "battery", no_argument, NULL, 'b' },
        { "bt-call-volume", required_argument, NULL, 0 },
        { "bt-when-powered-on", required_argument, NULL, 0 },
        { "capabilities", no_argument, NULL, '?' },
        { "chatmix", no_argument, NULL, 'm' },
        { "connected", no_argument, NULL, 0 },
        { "dev", no_argument, NULL, 0 },
        { "help", no_argument, NULL, 'h' },
        { "help-all", no_argument, NULL, 0 },
        { "equalizer", required_argument, NULL, 'e' },
        { "equalizer-preset", required_argument, NULL, 'p' },
        { "parametric-equalizer", required_argument, NULL, 0 },
        { "microphone-mute-led-brightness", required_argument, NULL, 0 },
        { "microphone-volume", required_argument, NULL, 0 },
        { "inactive-time", required_argument, NULL, 'i' },
        { "light", required_argument, NULL, 'l' },
        { "rgb-logo", required_argument, NULL, 0 },
        { "rgb-power", required_argument, NULL, 0 },
        { "rgb-mic", required_argument, NULL, 0 },
    { "rgb-logo-brightness", required_argument, NULL, 0 },
    { "rgb-power-brightness", required_argument, NULL, 0 },
    { "rgb-mic-brightness", required_argument, NULL, 0 },
    { "server", no_argument, NULL, 'S' },
        { "debug", no_argument, NULL, 'G' },
        { "rgb-all", required_argument, NULL, 0 },
    { "mic-status", no_argument, NULL, 0 },
        { "output", optional_argument, NULL, 'o' },
        { "follow", optional_argument, NULL, 'f' },
        { "notificate", required_argument, NULL, 'n' },
        /* Make rotate-to-mute optional argument so calling `-r` alone
         * doesn't produce a getopt error like "option requires an argument".
         * If no argument is supplied we print a helpful usage message. */
        { "rotate-to-mute", optional_argument, NULL, 'r' },
        { "sidetone", required_argument, NULL, 's' },
        { "short-output", no_argument, NULL, 'c' },
        { "timeout", required_argument, NULL, 0 },
        { "voice-prompt", required_argument, NULL, 'v' },
        { "volume-limiter", required_argument, NULL, 0 },
        { "test-device", optional_argument, NULL, 0 },
        { "readme-helper", no_argument, NULL, 0 },
        { 0, 0, 0, 0 }
    };

    int option_index = 0;

    /* Note: 'r' changed to 'r::' to accept an optional argument. */
    while ((c = getopt_long(argc, argv, "d:bchi:l:f::mn:o::r::s:uv:p:e:?SG", opts, &option_index)) != -1) {
        char* endptr = NULL; // for strtol

        switch (c) {
        case 'd': {
            int parsed_correctly = get_two_ids(optarg, &selected_vendor_id, &selected_product_id);
            if (parsed_correctly == 1) {
                fprintf(stderr, "Usage: %s -d, --device [vendorid:deviceid]\n", argv[0]);
                return 1;
            }
            break;
        }
        case 'b':
            request_battery = 1;
            break;
        case 'S':
            server_mode = 1;
            break;
        case 'G':
            debug_mode = 1;
            break;
        case 'c':
            output_format = OUTPUT_SHORT;
            break;
        case 'e': {
            int size = get_float_data_from_parameter(optarg, read_buffer, BUFFERLENGTH);

            if (size < 0) {
                fprintf(stderr, "Equalizer bands values size larger than supported %d\n", BUFFERLENGTH);
                return 1;
            }

            if (size == 0) {
                fprintf(stderr, "No bands values specified to --equalizer\n");
                return 1;
            }

            equalizer               = malloc(sizeof(struct equalizer_settings));
            equalizer->size         = size;
            equalizer->bands_values = malloc(sizeof(float) * size);
            for (int i = 0; i < size; i++) {
                equalizer->bands_values[i] = read_buffer[i];
            }
            break;
        }
        case 'f':
            follow = 1;
            if (OPTIONAL_ARGUMENT_IS_PRESENT) {
                follow_sec = strtol(optarg, &endptr, 10);
                if (follow_sec == 0) {
                    fprintf(stderr, "Usage: %s -f [secs timeout]\n", argv[0]);
                    return 1;
                }
            }
            break;
        case 'i':
            inactive_time = strtol(optarg, &endptr, 10);

            if (*endptr != '\0' || endptr == optarg || inactive_time > 90 || inactive_time < 0) {
                fprintf(stderr, "Usage: %s -i 0-90, 0 is off\n", argv[0]);
                return 1;
            }
            break;
        case 'l':
            lights = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || endptr == optarg || lights < 0 || lights > 1) {
                fprintf(stderr, "Usage: %s -l 0|1\n", argv[0]);
                return 1;
            }
            break;
        case 'm':
            request_chatmix = 1;
            break;
        case 'n':
            notification_sound = strtol(optarg, &endptr, 10);

            if (*endptr != '\0' || endptr == optarg || notification_sound < 0 || notification_sound > 1) {
                fprintf(stderr, "Usage: %s -n 0|1\n", argv[0]);
                return 1;
            }
            break;
        case 'o': {
            bool output_specified = true;

            if (OPTIONAL_ARGUMENT_IS_PRESENT) {
                if (strcasecmp(optarg, "JSON") == 0)
                    output_format = OUTPUT_JSON;
                else if (strcasecmp(optarg, "YAML") == 0)
                    output_format = OUTPUT_YAML;
                else if (strcasecmp(optarg, "ENV") == 0)
                    output_format = OUTPUT_ENV;
                else if (strcasecmp(optarg, "STANDARD") == 0)
                    output_format = OUTPUT_STANDARD;
                else if (strcasecmp(optarg, "SHORT") == 0)
                    output_format = OUTPUT_SHORT;
                else
                    output_specified = false;
            } else {
                output_specified = false;
            }

            if (output_specified == false) {
                // short not listed because deprecated
                fprintf(stderr, "Usage: %s -o JSON|YAML|ENV|STANDARD\n", argv[0]);
                return 1;
            }

            break;
        }
        case 'p':
            equalizer_preset = strtol(optarg, &endptr, 10);

            if (*endptr != '\0' || endptr == optarg || equalizer_preset < 0 || equalizer_preset > 3) {
                fprintf(stderr, "Usage: %s -p 0-3, 0 is default\n", argv[0]);
                return 1;
            }
            break;
        case 'r':
            /* Accept optional argument; if absent, print usage message
             * instead of letting getopt_long emit "option requires an argument". */
            if (OPTIONAL_ARGUMENT_IS_PRESENT) {
                rotate_to_mute = strtol(optarg, &endptr, 10);
                if (*endptr != '\0' || endptr == optarg || rotate_to_mute < 0 || rotate_to_mute > 1) {
                    fprintf(stderr, "Usage: %s -r 0|1\n", argv[0]);
                    return 1;
                }
            } else {
                fprintf(stderr, "Usage: %s -r 0|1  (provide 0 to disable, 1 to enable)\n", argv[0]);
                return 1;
            }
            break;
        case 's':
            sidetone_loudness = strtol(optarg, &endptr, 10);

            if (*endptr != '\0' || endptr == optarg || sidetone_loudness > 128 || sidetone_loudness < 0) {
                fprintf(stderr, "Usage: %s -s 0-128\n", argv[0]);
                return 1;
            }
            break;
        case 'u':
            print_udev_rules = 1;
            break;
        case 'v':
            voice_prompts = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || endptr == optarg || voice_prompts < 0 || voice_prompts > 1) {
                fprintf(stderr, "Usage: %s -v 0|1\n", argv[0]);
                return 1;
            }
            break;
        case '?':
            if (optopt == '?' || optopt == 0) {
                print_capabilities = 1;
            } else {
                // User issued an invalid option (stdlib will make an error message automatically)
                free(read_buffer);
                return 1;
            }
            break;
        case 'h':
            should_print_help = 1;
            break;
        case 0:
            if (strcmp(opts[option_index].name, "dev") == 0) {
                dev_mode = 1;
                break;
            } else if (strcmp(opts[option_index].name, "timeout") == 0) {
                hsc_device_timeout = strtol(optarg, &endptr, 10);

                if (*endptr != '\0' || endptr == optarg || hsc_device_timeout < 0 || hsc_device_timeout > 100000) {
                    fprintf(stderr, "Usage: %s --timeout 0-100000\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "microphone-mute-led-brightness") == 0) {
                microphone_mute_led_brightness = strtol(optarg, &endptr, 10);

                if (*endptr != '\0' || endptr == optarg || microphone_mute_led_brightness < 0 || microphone_mute_led_brightness > 3) {
                    fprintf(stderr, "Usage: %s --microphone-mute-led-brightness 0-3\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "mic-status") == 0) {
                request_mic_status = 1;
                break;
            } else if (strcmp(opts[option_index].name, "microphone-volume") == 0) {
                microphone_volume = strtol(optarg, &endptr, 10);

                if (*endptr != '\0' || endptr == optarg || microphone_volume < 0 || microphone_volume > 128) {
                    fprintf(stderr, "Usage: %s --microphone-volume 0-128\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "volume-limiter") == 0) {
                volume_limiter = strtol(optarg, &endptr, 10);

                if (*endptr != '\0' || endptr == optarg || volume_limiter < 0 || volume_limiter > 1) {
                    fprintf(stderr, "Usage: %s --volume-limiter 0-1\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "bt-when-powered-on") == 0) {
                bt_when_powered_on = strtol(optarg, &endptr, 10);

                if (*endptr != '\0' || endptr == optarg || bt_when_powered_on < 0 || bt_when_powered_on > 1) {
                    fprintf(stderr, "Usage: %s --bt-when-powered-on 0-1\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "bt-call-volume") == 0) {
                bt_call_volume = strtol(optarg, &endptr, 10);

                if (*endptr != '\0' || endptr == optarg || bt_call_volume < 0 || bt_call_volume > 2) {
                    fprintf(stderr, "Usage: %s --bt-call-volume 0-2\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "rgb-logo") == 0) {
                if (parse_rgb_string(optarg, rgb_logo) < 0) {
                    fprintf(stderr, "Usage: %s --rgb-logo R,G,B (values 0-255)\n", argv[0]);
                    fprintf(stderr, "Example: %s --rgb-logo 255,0,0 (red)\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "rgb-logo-brightness") == 0) {
                rgb_logo_brightness = strtol(optarg, &endptr, 10);
                if (*endptr != '\0' || endptr == optarg || rgb_logo_brightness < 0 || rgb_logo_brightness > 100) {
                    fprintf(stderr, "Usage: %s --rgb-logo-brightness 0-100\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "rgb-power") == 0) {
                if (parse_rgb_string(optarg, rgb_power) < 0) {
                    fprintf(stderr, "Usage: %s --rgb-power R,G,B (values 0-255)\n", argv[0]);
                    fprintf(stderr, "Example: %s --rgb-power 0,255,0 (green)\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "rgb-power-brightness") == 0) {
                rgb_power_brightness = strtol(optarg, &endptr, 10);
                if (*endptr != '\0' || endptr == optarg || rgb_power_brightness < 0 || rgb_power_brightness > 100) {
                    fprintf(stderr, "Usage: %s --rgb-power-brightness 0-100\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "rgb-mic") == 0) {
                if (parse_rgb_string(optarg, rgb_mic) < 0) {
                    fprintf(stderr, "Usage: %s --rgb-mic R,G,B (values 0-255)\n", argv[0]);
                    fprintf(stderr, "Example: %s --rgb-mic 0,0,255 (blue)\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "rgb-mic-brightness") == 0) {
                rgb_mic_brightness_percent = strtol(optarg, &endptr, 10);
                if (*endptr != '\0' || endptr == optarg || rgb_mic_brightness_percent < 0 || rgb_mic_brightness_percent > 100) {
                    fprintf(stderr, "Usage: %s --rgb-mic-brightness 0-100\n", argv[0]);
                    return 1;
                }
                // Map percentage to 0-3 for microphone mute LED capability
                if (rgb_mic_brightness_percent >= 0) {
                    int mapped = (rgb_mic_brightness_percent * 3 + 50) / 100; // rounded
                    microphone_mute_led_brightness = mapped;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "rgb-all") == 0) {
                if (parse_rgb_string(optarg, rgb_all) < 0) {
                    fprintf(stderr, "Usage: %s --rgb-all R,G,B (values 0-255)\n", argv[0]);
                    fprintf(stderr, "Example: %s --rgb-all 255,255,0 (yellow)\n", argv[0]);
                    return 1;
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "connected") == 0) {
                request_connected = 1;
                break;
            } else if (strcmp(opts[option_index].name, "test-device") == 0) {
                test_device = 1;

                if (OPTIONAL_ARGUMENT_IS_PRESENT) {
                    test_profile = strtol(optarg, &endptr, 10);
                    if (test_profile < 0) {
                        fprintf(stderr, "Usage: %s --test-device [testprofile]\n", argv[0]);
                        return 1;
                    }
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "parametric-equalizer") == 0) {
                parametric_equalizer = parse_parametric_equalizer_settings(optarg);

                for (int i = 0; i < parametric_equalizer->size; i++) {
                    if ((int)parametric_equalizer->bands[i].type == HSC_INVALID_ARG) {
                        fprintf(stderr, "Unknown filter type specified to --parametric-equalizer\n");
                        return 1;
                    }
                }
                // fall through
            } else if (strcmp(opts[option_index].name, "readme-helper") == 0) {
                // We need to initialize it at this point
                init_devices();
                print_readmetable();
                return 0;
            } else if (strcmp(opts[option_index].name, "help-all") == 0) {
                should_print_help_all = 1;
            }
            break;
        default:
            fprintf(stderr, "Invalid argument %c\n", c);
            free(read_buffer);
            return 1;
        }
    }

    // Init all information of supported devices
    // Below getopt_long so that the testdevice has a chance to adjust parameters
    init_devices();

    free(read_buffer);

    if (print_udev_rules == 1) {
        fprintf(stderr, "Generating udev rules..\n\n");
        print_udevrules();
        return 0;
    }

    if (dev_mode) {
        // use +1 to make sure the first parameter is some previous argument (which normally would be the name of the program)
        return dev_main(argc - optind + 1, &argv[optind - 1]);
    } else {
        for (int index = optind; index < argc; index++)
            fprintf(stderr, "Non-option argument %s\n", argv[index]);
    }

    // The array list of compatible devices
    DeviceList* devices_found = NULL;
    // Describes the headsetcontrol device, when a headset was found
    DeviceList* selected_device = NULL;
    // Search parameters for the devices
    SearchParameters search_parameters = { selected_vendor_id, selected_product_id, test_device };

    // Look for a supported device
    int headset_available = find_devices(&devices_found, search_parameters);

    if (headset_available == 1) {
        selected_device     = devices_found;
        selected_product_id = selected_device->device->idProduct;
        selected_vendor_id  = selected_device->device->idVendor;
    } else if (selected_vendor_id != 0 && selected_product_id != 0) {
        for (int i = 0; i < headset_available; i++) {
            if (device_check_ids(devices_found[i].device, selected_vendor_id, selected_product_id)) {
                selected_device = devices_found + i;
                break;
            }
        }
    }

    if (should_print_help || should_print_help_all) {
        if (selected_device == NULL) {
            print_help(argv[0], NULL, should_print_help_all);
        } else {
            print_help(argv[0], selected_device->device, should_print_help_all);
        }
        return 0;
    } else if (headset_available == 0) {
        output(NULL, false, output_format);
        return 1;
    }

    // We open connection to HID devices on demand
    hid_device* device_handles[headset_available];
    char* hid_paths[headset_available];

    /* Wire globals to local arrays and allocate keep-alive buffers */
    g_device_handles = device_handles; /* safe: main() lifetime covers threads */
    g_hid_paths = hid_paths;
    g_headset_available = headset_available;

    g_last_rgb = calloc(headset_available * 3, sizeof(uint8_t));
    g_keepalive_running = calloc(headset_available, sizeof(bool));
#ifdef _WIN32
    g_keepalive_threads = calloc(headset_available, sizeof(HANDLE));
#else
    g_keepalive_threads = calloc(headset_available, sizeof(pthread_t));
#endif

    // Initialize signal handler for CTRL + C
#ifdef _WIN32
    signal(SIGINT, interruptHandler);
#else
    struct sigaction act;
    act.sa_handler = interruptHandler;
    sigaction(SIGINT, &act, NULL);
#endif

    FeatureRequest featureRequests[] = {
        { CAP_SIDETONE, CAPABILITYTYPE_ACTION, &sidetone_loudness, sidetone_loudness != -1, {} },
        { CAP_LIGHTS, CAPABILITYTYPE_ACTION, &lights, lights != -1, {} },
    { CAP_NOTIFICATION_SOUND, CAPABILITYTYPE_ACTION, &notification_sound, notification_sound != -1, {} },
    /* CAP_RGB_ZONES: per-device/zone RGB control (drivers may expose this via
     * CAP_RGB_ZONES and device->set_zone_rgb / set_all_rgb). We don't route
     * RGB commands through handle_feature (HS80 handled separately), but
     * include the capability here so the featureRequests array size stays
     * in sync with NUM_CAPABILITIES and CLI-driven should_process flags can
     * be wired if needed in future. */
    { CAP_RGB_ZONES, CAPABILITYTYPE_ACTION, NULL, 0, {} },
    { CAP_BATTERY_STATUS, CAPABILITYTYPE_INFO, &request_battery, request_battery == 1, {} },
        { CAP_INACTIVE_TIME, CAPABILITYTYPE_ACTION, &inactive_time, inactive_time != -1, {} },
        { CAP_CHATMIX_STATUS, CAPABILITYTYPE_INFO, &request_chatmix, request_chatmix == 1, {} },
        { CAP_VOICE_PROMPTS, CAPABILITYTYPE_ACTION, &voice_prompts, voice_prompts != -1, {} },
        { CAP_ROTATE_TO_MUTE, CAPABILITYTYPE_ACTION, &rotate_to_mute, rotate_to_mute != -1, {} },
    { CAP_BUTTON_HOLD, CAPABILITYTYPE_ACTION, NULL, 0, {} },
    { CAP_BUTTON_RELEASE, CAPABILITYTYPE_ACTION, NULL, 0, {} },
        { CAP_EQUALIZER_PRESET, CAPABILITYTYPE_ACTION, &equalizer_preset, equalizer_preset != -1, {} },
        { CAP_MICROPHONE_MUTE_LED_BRIGHTNESS, CAPABILITYTYPE_ACTION, &microphone_mute_led_brightness, microphone_mute_led_brightness != -1, {} },
        { CAP_MICROPHONE_VOLUME, CAPABILITYTYPE_ACTION, &microphone_volume, microphone_volume != -1, {} },
        { CAP_EQUALIZER, CAPABILITYTYPE_ACTION, equalizer, equalizer != NULL, {} },
        { CAP_PARAMETRIC_EQUALIZER, CAPABILITYTYPE_ACTION, parametric_equalizer, parametric_equalizer != NULL, {} },
        { CAP_VOLUME_LIMITER, CAPABILITYTYPE_ACTION, &volume_limiter, volume_limiter != -1, {} },
        { CAP_BT_WHEN_POWERED_ON, CAPABILITYTYPE_ACTION, &bt_when_powered_on, bt_when_powered_on != -1, {} },
        { CAP_BT_CALL_VOLUME, CAPABILITYTYPE_ACTION, &bt_call_volume, bt_call_volume != -1, {} }
    };
    int numFeatures = sizeof(featureRequests) / sizeof(featureRequests[0]);
    assert(numFeatures == NUM_CAPABILITIES);

    /*
     * Simple stdin REPL "server" mode: when started with --server and a
     * selected device (-d vendor:product) the program will stay alive and
     * accept simple line commands on stdin. This is intentionally not a
     * network server — it's a lightweight way to keep HID open and interact
     * with the device without restarting the binary each time.
     *
     * Commands (one per line):
     *  help
     *  ping
     *  exit|quit
     *  set_light 0|1
     *  set_zone_rgb <zone> <r> <g> <b>
     *  set_all_rgb <r> <g> <b>
     *  set_zone_brightness <zone> <percent>
     *  get_battery
     */
    if (server_mode) {
        if (selected_device == NULL) {
            fprintf(stderr, "Server mode requires a selected device via -d vendor:product\n");
            return 1;
        }

        struct device* dev = selected_device->device;
    hid_device* server_handle = NULL;
    char* server_hid_path = NULL;
    char line[1024];
    /* Local keep-alive for server-mode (uses server_handle directly) */
    bool local_keepalive_running = false;
#ifdef _WIN32
    HANDLE local_keepalive_thread = NULL;
#else
    pthread_t local_keepalive_thread;
#endif
    uint8_t local_last_rgb[3] = {0,0,0};

        fprintf(stderr, "Starting interactive server mode for device %s (%04x:%04x)\n", dev->device_name, dev->idVendor, dev->idProduct);

        /* If selected device is HS80: open handle, init software mode and start local keep-alive */
        if (dev->idVendor == VENDOR_CORSAIR && (dev->idProduct == 0x0a6b || dev->idProduct == 0x0a69 || dev->idProduct == 0x0a71 || dev->idProduct == 0x0a73)) {
            server_handle = dynamic_connect(&server_hid_path, server_handle, dev, CAP_LIGHTS);
            if (!server_handle) {
                fprintf(stderr, "[HS80] Server-mode: could not open HS80 device (CAP_LIGHTS)\n");
            } else {
                int initret = hs80_keep_alive(server_handle);
                fprintf(stderr, "[HS80] Server-mode: hs80_keep_alive init returned %d\n", initret);
                /* Print the HS80 packet dump after the init log so ordering matches user expectation */
                hs80_dump_last_packet();
                /* start local keep-alive using heap-allocated control struct */
                struct LocalKeepAlive* ka = malloc(sizeof(*ka));
                ka->handle = server_handle;
                ka->last_rgb[0] = local_last_rgb[0]; ka->last_rgb[1] = local_last_rgb[1]; ka->last_rgb[2] = local_last_rgb[2];
                ka->running = 1;
                local_keepalive_running = true;
#ifdef _WIN32
                local_keepalive_thread = CreateThread(NULL, 0, local_keepalive_thread_win, ka, 0, NULL);
#else
                pthread_create(&local_keepalive_thread, NULL, local_keepalive_thread_posix, ka);
#endif
            }
        }
        while (fgets(line, sizeof(line), stdin) != NULL) {
            // trim newline
            size_t len = strlen(line);
            if (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = '\0';

            if (len == 0)
                continue;

            if (debug_mode)
                fprintf(stderr, "[DEBUG] command: %s\n", line);

            // tokenise
            char* saveptr = NULL;
            char* cmd = strtok_r(line, " ", &saveptr);
            if (!cmd)
                continue;

            if (strcmp(cmd, "help") == 0) {
                printf("{\"status\":\"ok\",\"help\":\"help,ping,exit,set_light,set_zone_rgb,set_all_rgb,set_zone_brightness,get_battery\"}\n");
                continue;
            }
            if (strcmp(cmd, "ping") == 0) {
                printf("{\"status\":\"ok\",\"message\":\"pong\"}\n");
                continue;
            }
            if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
                break;
            }

            // Ensure we have an open handle for device control (use CAP_LIGHTS interface by default)
            server_handle = dynamic_connect(&server_hid_path, server_handle, dev, CAP_LIGHTS);
            if (!server_handle) {
                printf("{\"status\":\"error\",\"message\":\"Could not open device handle\"}\n");
                continue;
            }

            if (strcmp(cmd, "set_light") == 0) {
                char* arg = strtok_r(NULL, " ", &saveptr);
                if (!arg) {
                    printf("{\"status\":\"error\",\"message\":\"missing argument\"}\n");
                    continue;
                }
                int val = atoi(arg);
                if (dev->switch_lights) {
                    int ret = dev->switch_lights(server_handle, (uint8_t)val);
                    if (ret >= 0)
                        printf("{\"status\":\"ok\",\"value\":%d}\n", ret);
                    else
                        printf("{\"status\":\"error\",\"value\":%d}\n", ret);
                } else {
                    printf("{\"status\":\"error\",\"message\":\"device does not support lights\"}\n");
                }
                continue;
            }

            if (strcmp(cmd, "set_zone_rgb") == 0) {
                char* szone = strtok_r(NULL, " ", &saveptr);
                char* sr = strtok_r(NULL, " ", &saveptr);
                char* sg = strtok_r(NULL, " ", &saveptr);
                char* sb = strtok_r(NULL, " ", &saveptr);
                if (!szone || !sr || !sg || !sb) {
                    printf("{\"status\":\"error\",\"message\":\"usage: set_zone_rgb <zone> <r> <g> <b>\"}\n");
                    continue;
                }
                int zone = atoi(szone);
                int r = atoi(sr), g = atoi(sg), b = atoi(sb);
                // prefer device-specific function pointer
                if (dev->set_zone_rgb) {
                    int ret = dev->set_zone_rgb(server_handle, (uint8_t)zone, (uint8_t)r, (uint8_t)g, (uint8_t)b);
                    if (ret >= 0)
                        printf("{\"status\":\"ok\"}\n");
                    else
                        printf("{\"status\":\"error\",\"value\":%d}\n", ret);
                } else if (dev->idVendor == VENDOR_CORSAIR) {
                    // HS80 helpers exported in header
                    if (hs80_set_zone_rgb(server_handle, (uint8_t)zone, (uint8_t)r, (uint8_t)g, (uint8_t)b) > 0)
                        printf("{\"status\":\"ok\"}\n");
                    else
                        printf("{\"status\":\"error\",\"message\":\"hs80 set_zone_rgb failed\"}\n");
                } else {
                    printf("{\"status\":\"error\",\"message\":\"set_zone_rgb not supported\"}\n");
                }
                continue;
            }

            if (strcmp(cmd, "set_all_rgb") == 0) {
                char* sr = strtok_r(NULL, " ", &saveptr);
                char* sg = strtok_r(NULL, " ", &saveptr);
                char* sb = strtok_r(NULL, " ", &saveptr);
                if (!sr || !sg || !sb) {
                    printf("{\"status\":\"error\",\"message\":\"usage: set_all_rgb <r> <g> <b>\"}\n");
                    continue;
                }
                int r = atoi(sr), g = atoi(sg), b = atoi(sb);
                if (dev->set_all_rgb) {
                    int ret = dev->set_all_rgb(server_handle, (uint8_t)r, (uint8_t)g, (uint8_t)b);
                    if (ret >= 0)
                        printf("{\"status\":\"ok\"}\n");
                    else
                        printf("{\"status\":\"error\",\"value\":%d}\n", ret);
                } else if (dev->idVendor == VENDOR_CORSAIR) {
                        int hret = hs80_set_all_rgb(server_handle, (uint8_t)r, (uint8_t)g, (uint8_t)b);
                        if (hret > 0) {
                            printf("{\"status\":\"ok\"}\n");
                            /* update local keep-alive (server-mode) or global last_rgb */
                            local_last_rgb[0] = (uint8_t)r;
                            local_last_rgb[1] = (uint8_t)g;
                            local_last_rgb[2] = (uint8_t)b;
                            int idx = selected_device - devices_found;
                            if (idx >= 0 && idx < g_headset_available) {
                                g_last_rgb[idx * 3 + 0] = (uint8_t)r;
                                g_last_rgb[idx * 3 + 1] = (uint8_t)g;
                                g_last_rgb[idx * 3 + 2] = (uint8_t)b;
                            }
                        } else {
                            printf("{\"status\":\"error\",\"message\":\"hs80 set_all_rgb failed (ret=%d)\"}\n", hret);
                        }
                } else {
                    printf("{\"status\":\"error\",\"message\":\"set_all_rgb not supported\"}\n");
                }
                continue;
            }

            if (strcmp(cmd, "set_zone_brightness") == 0) {
                char* szone = strtok_r(NULL, " ", &saveptr);
                char* sper = strtok_r(NULL, " ", &saveptr);
                if (!szone || !sper) {
                    printf("{\"status\":\"error\",\"message\":\"usage: set_zone_brightness <zone> <percent>\"}\n");
                    continue;
                }
                int zone = atoi(szone);
                int percent = atoi(sper);
                if (dev->set_zone_brightness) {
                    int ret = dev->set_zone_brightness(server_handle, (uint8_t)zone, (uint8_t)percent);
                    if (ret >= 0)
                        printf("{\"status\":\"ok\"}\n");
                    else
                        printf("{\"status\":\"error\",\"value\":%d}\n", ret);
                } else if (dev->idVendor == VENDOR_CORSAIR) {
                    if (hs80_set_zone_brightness(server_handle, (uint8_t)zone, (uint8_t)percent) >= 0)
                        printf("{\"status\":\"ok\"}\n");
                    else
                        printf("{\"status\":\"error\",\"message\":\"hs80 set_zone_brightness failed\"}\n");
                } else {
                    printf("{\"status\":\"error\",\"message\":\"set_zone_brightness not supported\"}\n");
                }
                continue;
            }

            if (strcmp(cmd, "get_battery") == 0) {
                if (dev->request_battery) {
                    BatteryInfo bi = dev->request_battery(server_handle);
                    printf("{\"status\":\"ok\",\"battery_level\":%d,\"battery_status\":%d}\n", bi.level, bi.status);
                } else {
                    printf("{\"status\":\"error\",\"message\":\"battery not supported\"}\n");
                }
                continue;
            }

            printf("{\"status\":\"error\",\"message\":\"unknown command\"}\n");
        }

            if (server_handle) {
                hid_close(server_handle);
                free(server_hid_path);
            }

            /* stop local keep-alive and free control struct */
            if (local_keepalive_running) {
                local_keepalive_running = false;
#ifdef _WIN32
                if (local_keepalive_thread) {
                    WaitForSingleObject(local_keepalive_thread, 2000);
                    CloseHandle(local_keepalive_thread);
                }
#else
                pthread_join(local_keepalive_thread, NULL);
#endif
                /* the thread frees its own LocalKeepAlive struct when it exits */
            }
        return 0;
    }

    // Initialize all handles, hid_paths and feature requests for all devices
    FeatureRequest* feature_requests[headset_available];
    /* Stop keep-alive threads first */
    for (int i = 0; i < headset_available; i++) {
        if (g_keepalive_running && g_keepalive_running[i]) {
            g_keepalive_running[i] = false;
#ifdef _WIN32
            if (g_keepalive_threads && g_keepalive_threads[i]) {
                WaitForSingleObject(g_keepalive_threads[i], 2000);
                CloseHandle(g_keepalive_threads[i]);
            }
#else
            if (g_keepalive_threads) {
                pthread_join(g_keepalive_threads[i], NULL);
            }
#endif
        }
    }

    for (int i = 0; i < headset_available; i++) {
        device_handles[i]                = NULL;
        hid_paths[i]                     = NULL;
        feature_requests[i]              = memcpy(malloc(sizeof(featureRequests)), featureRequests, sizeof(featureRequests));
        devices_found[i].featureRequests = feature_requests[i];
        devices_found[i].size            = numFeatures;
    }

    bool isExtendedOutput = output_format == OUTPUT_YAML || output_format == OUTPUT_JSON || output_format == OUTPUT_ENV;
    // If JSON output is selected, suppress all stderr to avoid contaminating stdout
    if (output_format == OUTPUT_JSON) {
#ifdef _WIN32
    freopen("NUL", "w", stderr);
#else
    freopen("/dev/null", "w", stderr);
#endif
    }
    for (int i = 0; i < headset_available; i++) {
        for (int j = 0; j < numFeatures; j++) {
            // For specific output types, like YAML, we will do all actions - even when not specified - to aggregate all information
            if (isExtendedOutput && feature_requests[i][j].type == CAPABILITYTYPE_INFO && !feature_requests[i][j].should_process) {
                if (device_has_capability(devices_found[i].device, feature_requests[i][j].cap)) {
                    feature_requests[i][j].should_process = true;
                }
            } else if (feature_requests[i][j].type == CAPABILITYTYPE_ACTION && feature_requests[i][j].should_process) {
                if (headset_available > 1) {
                    if (selected_vendor_id == 0 && selected_product_id == 0) {
                        feature_requests[i][j].result.status  = FEATURE_NOT_PROCESSED;
                        feature_requests[i][j].result.message = strdup("Not processed, multiple devices detected,you need to specify a device with -d");
                        feature_requests[i][j].result.value   = -1;
                    } else if (!device_check_ids(devices_found[i].device, selected_vendor_id, selected_product_id)) {
                        feature_requests[i][j].should_process = false;
                    }
                }
            }
        }
    }

    /* Auto-initialize HS80 devices: open HID and enable software mode + start keep-alive */
    for (int i = 0; i < headset_available; i++) {
        struct device* d = devices_found[i].device;
        if (d->idVendor == VENDOR_CORSAIR) {
            uint16_t pid = d->idProduct;
            if (pid == 0x0a6b || pid == 0x0a69 || pid == 0x0a71 || pid == 0x0a73) {
                /* open using CAP_LIGHTS interface */
                device_handles[i] = dynamic_connect(&hid_paths[i], device_handles[i], d, CAP_LIGHTS);
                if (!device_handles[i]) {
                    fprintf(stderr, "[HS80] Warning: could not open HS80 device index %d for software-mode init\n", i);
                    continue;
                }

                /* Trigger software-mode initialization inside driver */
                hs80_set_all_rgb(device_handles[i], 0, 0, 0);

                /* Initialize last_rgb for keep-alive */
                g_last_rgb[i * 3 + 0] = 0;
                g_last_rgb[i * 3 + 1] = 0;
                g_last_rgb[i * 3 + 2] = 0;

                /* start keep-alive thread */
                g_keepalive_running[i] = true;
#ifdef _WIN32
                int* arg = malloc(sizeof(int)); *arg = i;
                g_keepalive_threads[i] = CreateThread(NULL, 0, hs80_keepalive_thread_win, arg, 0, NULL);
#else
                int* arg = malloc(sizeof(int)); *arg = i;
                pthread_create(&g_keepalive_threads[i], NULL, hs80_keepalive_thread_posix, arg);
#endif
                fprintf(stderr, "[HS80] Software-mode initialized and keep-alive started for device index %d\n", i);
            }
        }
    }

    /* Handle explicit --mic-status request: query current microphone mute state
     * and exit. This mirrors the style used for --battery but keeps the option
     * explicit and separate from other feature processing. */
    if (request_mic_status) {
        if (selected_device == NULL) {
            fprintf(stderr, "Error: No device has been selected.\n");
            return 1;
        }
        int selected_device_index = selected_device - devices_found;
        hid_device* device_handle = device_handles[selected_device_index];
        char* hid_path            = hid_paths[selected_device_index];
        struct device* device     = selected_device->device;
        int is_test_device        = test_device && device->idVendor == VENDOR_TESTDEVICE && device->idProduct == PRODUCT_TESTDEVICE;

        if (!device->request_mic_status) {
            fprintf(stderr, "Microphone status request not supported by this device.\n");
            return 1;
        }

        if (!is_test_device) {
            device_handle = dynamic_connect(&hid_path, device_handle, device, CAP_LIGHTS);
            if (!device_handle) {
                fprintf(stderr, "Error while getting device handle.\n");
                return 1;
            }
        }

        int status = device->request_mic_status(device_handle);
        if (status >= 0) {
            printf("%d\n", status);
            return 0;
        } else {
            fprintf(stderr, "Failed to request microphone status (err=%d)\n", status);
            return 1;
        }
    }

    if (request_connected) {
        if (selected_device == NULL) {
            fprintf(stderr, "Error: No device has been selected.\n");
            return 1;
        }
        int selected_device_index = selected_device - devices_found;
        hid_device* device_handle = device_handles[selected_device_index];
        char* hid_path            = hid_paths[selected_device_index];
        struct device* device     = selected_device->device;
        int is_test_device        = test_device && device->idVendor == VENDOR_TESTDEVICE && device->idProduct == PRODUCT_TESTDEVICE;
        // Check if battery status can be read
        // If it isn't supported, the device is
        // probably wired meaning it is connected
        int battery_error = 0;
        BatteryInfo info;

        if (device_has_capability(device, CAP_BATTERY_STATUS)) {
            if (!is_test_device) {
                device_handle = dynamic_connect(&hid_path, device_handle, device, CAP_BATTERY_STATUS);
                if (!device_handle) {
                    fprintf(stderr, "Error while getting device handle.\n");
                    return 1;
                }
                info = device->request_battery(device_handle);
            } else {
                info = device->request_battery(device_handle);
            }

            if (info.status != BATTERY_AVAILABLE) {
                battery_error = 1;
            }
        }

        if (battery_error != 0) {
            printf("false\n");
        } else {
            printf("true\n");
        }
    } else {
        do {
            // Process all features for all devices
            for (int i = 0; i < headset_available; i++) {
                if (!device_check_ids(devices_found[i].device, selected_vendor_id, selected_product_id))
                    continue;
                FeatureRequest* deviceFeatureRequests = devices_found[i].featureRequests;
                for (int j = 0; j < numFeatures; j++) {
                    if (deviceFeatureRequests[j].should_process && deviceFeatureRequests[j].result.status != FEATURE_NOT_PROCESSED) {
                        // Assuming handle_feature now returns FeatureResult
                        deviceFeatureRequests[j].result = handle_feature(devices_found[i].device, &device_handles[i], &hid_paths[i], deviceFeatureRequests[j].cap, deviceFeatureRequests[j].param);
                    }
                }
                
                // HS80-specific RGB zone control (device-specific features, not standard capabilities)
                if (devices_found[i].device->idVendor == 0x1b1c && devices_found[i].device->idProduct == 0x0a6b) {
                    // Open device handle using CAP_LIGHTS interface
                    device_handles[i] = dynamic_connect(&hid_paths[i], device_handles[i], devices_found[i].device, CAP_LIGHTS);
                    
                    if (!device_handles[i]) {
                        fprintf(stderr, "[HS80] Fehler beim Öffnen des HID-Geräts für RGB-Zonen-Steuerung\n");
                        continue;
                    }
                    
                    // Process RGB zone commands
                    if (rgb_all[0] >= 0) {
                        if (hs80_set_all_rgb(device_handles[i], rgb_all[0], rgb_all[1], rgb_all[2]) > 0) {
                                fprintf(stderr, "[HS80] Alle RGB-Zonen erfolgreich gesetzt: R=%d, G=%d, B=%d\n", rgb_all[0], rgb_all[1], rgb_all[2]);
                                /* update keep-alive color for this device */
                                g_last_rgb[i * 3 + 0] = (uint8_t)rgb_all[0];
                                g_last_rgb[i * 3 + 1] = (uint8_t)rgb_all[1];
                                g_last_rgb[i * 3 + 2] = (uint8_t)rgb_all[2];
                            } else {
                                fprintf(stderr, "[HS80] Fehler beim Setzen aller RGB-Zonen\n");
                            }
                    }
                    
                    if (rgb_logo[0] >= 0) {
                        if (hs80_set_zone_rgb(device_handles[i], 0, rgb_logo[0], rgb_logo[1], rgb_logo[2]) > 0) {
                            fprintf(stderr, "[HS80] Logo RGB erfolgreich gesetzt: R=%d, G=%d, B=%d\n", rgb_logo[0], rgb_logo[1], rgb_logo[2]);
                        } else {
                            fprintf(stderr, "[HS80] Fehler beim Setzen der Logo RGB\n");
                        }
                    }
                    
                    if (rgb_power[0] >= 0) {
                        if (hs80_set_zone_rgb(device_handles[i], 1, rgb_power[0], rgb_power[1], rgb_power[2]) > 0) {
                            fprintf(stderr, "[HS80] Power LED RGB erfolgreich gesetzt: R=%d, G=%d, B=%d\n", rgb_power[0], rgb_power[1], rgb_power[2]);
                        } else {
                            fprintf(stderr, "[HS80] Fehler beim Setzen der Power LED RGB\n");
                        }
                    }
                    
                    if (rgb_mic[0] >= 0) {
                        if (hs80_set_zone_rgb(device_handles[i], 2, rgb_mic[0], rgb_mic[1], rgb_mic[2]) > 0) {
                            fprintf(stderr, "[HS80] Mic LED RGB erfolgreich gesetzt: R=%d, G=%d, B=%d\n", rgb_mic[0], rgb_mic[1], rgb_mic[2]);
                        } else {
                            fprintf(stderr, "[HS80] Fehler beim Setzen der Mic LED RGB\n");
                        }
                    }

                    // Per-zone brightness commands
                    if (rgb_logo_brightness != -1) {
                        if (hs80_set_zone_brightness(device_handles[i], 0, (uint8_t)rgb_logo_brightness) == 0) {
                            fprintf(stderr, "[HS80] Logo Helligkeit gesetzt: %d%%\n", rgb_logo_brightness);
                        } else {
                            fprintf(stderr, "[HS80] Fehler beim Setzen der Logo Helligkeit\n");
                        }
                    }

                    if (rgb_power_brightness != -1) {
                        if (hs80_set_zone_brightness(device_handles[i], 1, (uint8_t)rgb_power_brightness) == 0) {
                            fprintf(stderr, "[HS80] Power Helligkeit gesetzt: %d%%\n", rgb_power_brightness);
                        } else {
                            fprintf(stderr, "[HS80] Fehler beim Setzen der Power Helligkeit\n");
                        }
                    }

                    if (rgb_mic_brightness_percent != -1) {
                        if (hs80_set_zone_brightness(device_handles[i], 2, (uint8_t)rgb_mic_brightness_percent) == 0) {
                            fprintf(stderr, "[HS80] Mic Helligkeit gesetzt: %d%%\n", rgb_mic_brightness_percent);
                        } else {
                            fprintf(stderr, "[HS80] Fehler beim Setzen der Mic Helligkeit\n");
                        }
                    }
                }
            }

            output(devices_found, print_capabilities != -1, output_format);

            if (follow)
                sleep(follow_sec);

        } while (follow);
    }

    if (equalizer != NULL) {
        free(equalizer->bands_values);
        free(equalizer);
    }

    if (parametric_equalizer != NULL) {
        free(parametric_equalizer->bands);
        free(parametric_equalizer);
    }

    for (int i = 0; i < headset_available; i++) {
        if (output_format != OUTPUT_STANDARD) {
            // Free memory from features
            for (int j = 0; j < numFeatures; j++) {
                free(devices_found[i].featureRequests[j].result.message);
            }
        }
        terminate_device_hid(&device_handles[i], &hid_paths[i]);
        free(devices_found[i].featureRequests);
        free(devices_found[i].device);
    }

    if (g_last_rgb) { free(g_last_rgb); g_last_rgb = NULL; }
    if (g_keepalive_running) { free(g_keepalive_running); g_keepalive_running = NULL; }
    if (g_keepalive_threads) { free(g_keepalive_threads); g_keepalive_threads = NULL; }

    hid_exit();
    return 0;
}
