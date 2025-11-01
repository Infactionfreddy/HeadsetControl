#pragma once

#include <hidapi.h>
#include <stdbool.h>
#include <stdint.h>

#define VENDOR_CORSAIR     0x1b1c
#define VENDOR_LOGITECH    0x046d
#define VENDOR_STEELSERIES 0x1038
#define VENDOR_ROCCAT      0x1e7d
#define VENDOR_AUDEZE      0x3329

#define VENDOR_TESTDEVICE  0xF00B
#define PRODUCT_TESTDEVICE 0xA00C

/// Convert given number to bitmask
#define B(X) (1 << X)

/// global read timeout in millisecounds
extern int hsc_device_timeout;

/** @brief A list of all features settable/queryable
 *         for headsets
 *
 *  B(CAP_X) converts them to a bitmask value
 */
enum capabilities {
    CAP_SIDETONE = 0,
    CAP_BATTERY_STATUS,
    CAP_NOTIFICATION_SOUND,
    CAP_LIGHTS,
    CAP_RGB_ZONES,
    CAP_INACTIVE_TIME,
    CAP_CHATMIX_STATUS,
    CAP_VOICE_PROMPTS,
    CAP_ROTATE_TO_MUTE,
    CAP_BUTTON_HOLD,
    CAP_BUTTON_RELEASE,
    CAP_EQUALIZER_PRESET,
    CAP_EQUALIZER,
    CAP_PARAMETRIC_EQUALIZER,
    CAP_MICROPHONE_MUTE_LED_BRIGHTNESS,
    CAP_MICROPHONE_VOLUME,
    CAP_VOLUME_LIMITER,
    CAP_BT_WHEN_POWERED_ON,
    CAP_BT_CALL_VOLUME,
    NUM_CAPABILITIES
};

enum capabilitytype {
    CAPABILITYTYPE_ACTION,
    CAPABILITYTYPE_INFO
};

/// Long name of every capability
extern const char* const capabilities_str[NUM_CAPABILITIES];
/// Short name of every capability (deprecated)
extern const char capabilities_str_short[NUM_CAPABILITIES];
/// Enum name of every capability
extern const char* const capabilities_str_enum[NUM_CAPABILITIES];

struct capability_detail {
    // Usage page, only used when usageid is not 0; HID Protocol specific
    uint16_t usagepage;
    // Used instead of interface when not 0, and only used on Windows currently; HID Protocol specific
    uint16_t usageid;
    /// Interface ID - zero means first enumerated interface!
    int interface;
};

/** @brief Flags for battery status
 */
enum battery_status {
    BATTERY_UNAVAILABLE,
    BATTERY_CHARGING,
    BATTERY_AVAILABLE,
    BATTERY_HIDERROR,
    BATTERY_TIMEOUT,
};

enum microphone_status {
    MICROPHONE_UNKNOWN = 0,
    MICROPHONE_UP,
};

typedef struct {
    int level;
    enum battery_status status;
    // Often in battery status reports, the microphone status is also included
    enum microphone_status microphone_status;
} BatteryInfo;

typedef struct {
    int bands_count;
    int bands_baseline;
    float bands_step;
    int bands_min;
    int bands_max;
} EqualizerInfo;

typedef struct {
    char* name;
    float* values;
} EqualizerPreset;

typedef struct {
    int bands_count;
    float gain_base; // default/base gain
    float gain_step;
    float gain_min;
    float gain_max;
    float q_factor_min; // q factor
    float q_factor_max;
    int freq_min; // frequency
    int freq_max;
    int filter_types; // bitmap containing available filter types
} ParametricEqualizerInfo;

typedef struct {
    int count;
    EqualizerPreset presets[];
} EqualizerPresets;

enum headsetcontrol_errors {
    HSC_ERROR         = -100,
    HSC_READ_TIMEOUT  = -101,
    HSC_OUT_OF_BOUNDS = -102,
    HSC_INVALID_ARG   = -103,
};

typedef enum {
    FEATURE_SUCCESS,
    FEATURE_ERROR,
    FEATURE_DEVICE_FAILED_OPEN,
    FEATURE_INFO, // For non-error, informational states like "charging"
    FEATURE_NOT_PROCESSED
} FeatureStatus;

typedef struct {
    FeatureStatus status;
    /// Can hold battery level, error codes, or special status codes
    int value;
    /// Status depending on the feature (not used by all)
    int status2;
    /// For error messages, "Charging", "Unavailable", etc. Should be free()d after use
    char* message;
} FeatureResult;

typedef struct {
    enum capabilities cap;
    enum capabilitytype type;
    void* param;
    bool should_process;
    FeatureResult result;
} FeatureRequest;

/** @brief Defines equalizer custom settings
 */
struct equalizer_settings {
    /// The size of the bands array
    int size;
    /// The equalizer frequency bands values
    float* bands_values;
};

typedef enum {
    EQ_FILTER_LOWSHELF,
    EQ_FILTER_LOWPASS,
    EQ_FILTER_PEAKING,
    EQ_FILTER_HIGHPASS,
    EQ_FILTER_HIGHSHELF,
    NUM_EQ_FILTER_TYPES
} EqualizerFilterType;

/// Enum name of every parametric equalizer filter type
extern const char* const equalizer_filter_type_str[NUM_EQ_FILTER_TYPES];

/** @brief Defines parametric equalizer custom settings
 */
struct parametric_equalizer_settings {
    /// The size of the bands array
    int size;
    /// The equalizer bands
    struct parametric_equalizer_band* bands;
};

/** @brief Defines parameteric equalizer band
 */
struct parametric_equalizer_band {
    float frequency;
    float gain;
    float q_factor;
    EqualizerFilterType type;
};

/** @brief Defines the basic data of a device
 *
 *  Also defines function pointers for using supported features
 */
struct device {
    /// USB Vendor id
    uint16_t idVendor;
    /// USB Product id used (and found as connected), set by device_registry.c
    uint16_t idProduct;
    /// The USB Product ids this instance of "struct device" supports
    const uint16_t* idProductsSupported;
    /// Size of idProducts
    int numIdProducts;

    /// Name of device, used as information for the user
    char device_name[64];

    // Equalizer Infos
    EqualizerInfo* equalizer;
    EqualizerPresets* equalizer_presets;
    ParametricEqualizerInfo* parametric_equalizer;

    wchar_t device_hid_vendorname[64];
    wchar_t device_hid_productname[64];

    /// Bitmask of currently supported features the software can currently handle
    int capabilities;
    /// Details of all capabilities (e.g. to which interface to connect)
    struct capability_detail capability_details[NUM_CAPABILITIES];

    /** @brief Function pointer for setting headset sidetone
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  num             Level of the sidetone, between 0 - 128
     *
     *  @returns    > 0         on success
     *              HSC_ERROR   on error specific to this software
     *              -1          HIDAPI error
     */
    int (*send_sidetone)(hid_device* hid_device, uint8_t num);

    /** @brief Function pointer for retrieving the headsets battery status
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *
     *  @returns    >= 0            battery level (in %)
     *              BATTERY_LOADING when the battery is currently being loaded
     *              -1              HIDAPI error
     */
    BatteryInfo (*request_battery)(hid_device* hid_device);

    /** @brief Function pointer for sending a notification sound to the headset
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  soundid         The soundid which should be used
     *
     *  @returns    > 0         success
     *              -1          HIDAPI error
     */
    int (*notification_sound)(hid_device* hid_device, uint8_t soundid);

    /** @brief Function pointer for turning light on or off of the headset
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  on              1 if it should be turned on; 0 otherwise
     *
     *  @returns    > 0         success
     *              -1          HIDAPI error
     */
    int (*switch_lights)(hid_device* hid_device, uint8_t on);

    /** @brief Function pointer for setting RGB zone color (device-specific)
    *
    * @brief Set a single RGB zone to a color.
    *
    * Drivers that support per-zone RGB should set this function pointer
    * to provide zone-level control. Zone numbering is device specific
    * (e.g. 0=Logo,1=Power,2=Mic for HS80). The caller should check
    * `device->capabilities & B(CAP_RGB_ZONES)` before calling.
    *
    * @param hid_device Pointer to an open HID handle for the device.
    * @param zone Zone index (0.. N-1) as defined by the device.
    * @param r Red channel 0-255.
    * @param g Green channel 0-255.
    * @param b Blue channel 0-255.
    * @return >=0 on success, negative error code on failure.
     */
    int (*set_zone_rgb)(hid_device* hid_device, uint8_t zone, uint8_t r, uint8_t g, uint8_t b);

    /**
     * @brief Set all RGB zones to the same color.
     *
     * Convenience function for drivers that can efficiently set every
     * zone to the same RGB value. If not implemented, callers can fall
     * back to calling `set_zone_rgb` for each zone.
     *
     * @param hid_device Open HID handle.
     * @param r Red 0-255.
     * @param g Green 0-255.
     * @param b Blue 0-255.
     * @return >=0 on success, negative on error.
     */
    int (*set_all_rgb)(hid_device* hid_device, uint8_t r, uint8_t g, uint8_t b);

    /**
     * @brief Set per-zone brightness as percentage (0-100).
     *
     * Some devices expose native per-zone brightness. For devices that
     * do not, drivers may implement software-emulation by scaling the
     * currently cached RGB values for the zone and re-sending the RGB
     * packet (HS80 driver currently does this).
     *
     * Note: For backward compatibility, callers that also support
     * `CAP_MICROPHONE_MUTE_LED_BRIGHTNESS` may map percentage values to
     * the 0..3 scale expected by `send_microphone_mute_led_brightness`.
     *
     * @param hid_device Open HID handle.
     * @param zone Zone index.
     * @param brightness_percent Brightness in percent (0-100).
     * @return >=0 on success, negative on error.
     */
    int (*set_zone_brightness)(hid_device* hid_device, uint8_t zone, uint8_t brightness_percent);

    /** @brief Function pointer for setting headset inactive time
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  num             Number of minutes after which the device
     *                          is turned off, between 0 - 90,
     *                          0 disables the automatic shutdown feature
     *
     *  @returns    > 0         on success
     *              HSC_ERROR   on error specific to this software
     *              -1          HIDAPI error
     */
    int (*send_inactive_time)(hid_device* hid_device, uint8_t num);

    /** @brief Function pointer for retrieving the headsets chatmix level
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *
     *  @returns    >= 0            chatmix level
     *              -1              HIDAPI error
     */
    int (*request_chatmix)(hid_device* hid_device);

    /** @brief Function pointer for enabling or disabling voice
     *  prompts on the headset
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  on              1 if it should be turned on; 0 otherwise
     *
     *  @returns    > 0         success
     *              -1          HIDAPI error
     */
    int (*switch_voice_prompts)(hid_device* hid_device, uint8_t on);

    /** @brief Function pointer for enabling or disabling auto-muting
     *  when rotating the headset microphone
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  on              1 if it should be turned on; 0 otherwise
     *
     *  @returns    > 0         success
     *              -1          HIDAPI error
     */
    int (*switch_rotate_to_mute)(hid_device* hid_device, uint8_t on);

    /** @brief Function pointer for requesting microphone (rotate-to-mute) status
     *
     * Some devices (e.g. HS80) provide an event/readable report that indicates
     * whether the microphone is currently muted/unmuted. Drivers that support
     * reporting the microphone status should implement this function pointer.
     *
     * @param hid_device Open HID handle
     * @returns >=0 on success (e.g. 0 = unmuted, 1 = muted), negative on error
     */
    int (*request_mic_status)(hid_device* hid_device);

    /** @brief Function pointer for setting headset equalizer preset
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  num             The preset number, between 0 - 3
     *
     *  @returns    > 0                on success
     *              HSC_OUT_OF_BOUNDS  on preset parameter out of range
     *                                 specific to this hardware
     *              -1                 HIDAPI error
     */
    int (*send_equalizer_preset)(hid_device* hid_device, uint8_t num);

    /** @brief Function pointer for setting headset equalizer
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  settings        The equalizer settings containing
     *                          the frequency bands values
     *
     *  @returns    > 0                on success
     *              HSC_OUT_OF_BOUNDS  on equalizer settings size out of range
     *                                 specific to this hardware
     *              -1                 HIDAPI error
     */
    int (*send_equalizer)(hid_device* hid_device, struct equalizer_settings* settings);

    /** @brief Function pointer for setting headset parametric equalizer
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  settings        The parametric equalizer settings containing
     *                          the filter values
     *
     *  @returns    > 0                on success
     *              HSC_OUT_OF_BOUNDS  on equalizer settings size out of range
     *                                 specific to this hardware
     *              HSC_INVALID_ARG    on equalizer filter type invalid/unsupported
     *                                 specific to this hardware
     *              -1                 HIDAPI error
     */
    int (*send_parametric_equalizer)(hid_device* hid_device, struct parametric_equalizer_settings* settings);

    /** @brief Function pointer for setting headset microphone mute LED brightness
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  num             The level number, between 0 - 3
     *
     *  @returns    > 0                on success
     *              HSC_OUT_OF_BOUNDS  on level parameter out of range
     *                                 specific to this hardware
     *              -1                 HIDAPI error
     */
    int (*send_microphone_mute_led_brightness)(hid_device* hid_device, uint8_t num);

    /** @brief Function pointer for setting headset microphone volume
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  num             The volume number, between 0 - 128
     *
     *  @returns    > 0                on success
     *              HSC_OUT_OF_BOUNDS  on volume parameter out of range
     *                                 specific to this hardware
     *              -1                 HIDAPI error
     */
    int (*send_microphone_volume)(hid_device* hid_device, uint8_t num);

    /** @brief Function pointer for setting headset volume limiter
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  on              1 if it should be turned on; 0 otherwise
     *
     *  @returns    > 0                on success
     *              HSC_OUT_OF_BOUNDS  on preset parameter out of range
     *                                 specific to this hardware
     *              -1                 HIDAPI error
     */
    int (*send_volume_limiter)(hid_device* hid_device, uint8_t on);

    /** @brief Function pointer for setting headset bluetooth when powered on
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  on              1 if it should be turned on; 0 otherwise
     *
     *  @returns    > 0                on success
     *              HSC_OUT_OF_BOUNDS  on preset parameter out of range
     *                                 specific to this hardware
     *              -1                 HIDAPI error
     */
    int (*send_bluetooth_when_powered_on)(hid_device* hid_device, uint8_t num);

    /** @brief Function pointer for setting headset bluetooth call volume
     *
     *  Forwards the request to the device specific implementation
     *
     *  @param  device_handle   The hidapi handle. Must be the same
     *                          device as defined here (same ids)
     *  @param  num             The volume number during a bluetooth
     *                          call, between 0 - 2
     *
     *  @returns    > 0                on success
     *              HSC_OUT_OF_BOUNDS  on volume parameter out of range
     *                                 specific to this hardware
     *              -1                 HIDAPI error
     */
    int (*send_bluetooth_call_volume)(hid_device* hid_device, uint8_t num);
};

/**
 * @brief Node structure for a linked list of devices.
 *
 * This structure represents a node in a linked list where each node contains a pointer to a device
 * and a pointer to the next node in the list.
 */
typedef struct DeviceListNode {
    struct device* element;
    struct DeviceListNode* next;
} DeviceListNode;

bool device_check_ids(struct device* device, uint16_t vid, uint16_t pid);

bool device_has_capability(struct device* device, enum capabilities cap);

bool has_capability(int capabilities, enum capabilities cap);
