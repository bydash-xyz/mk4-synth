#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdio>

// ESP-IDF error/log compatibility
#ifndef ESP_ERR_T_DEFINED
#define ESP_ERR_T_DEFINED
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
#endif
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;

#ifndef ESP_INTR_FLAG_LEVEL1
#define ESP_INTR_FLAG_LEVEL1 (1 << 1)
#endif

// ESP-IDF logging macros (no-op in mock)
#ifndef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); } while(0)
#endif
#ifndef ESP_LOGE
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); } while(0)
#endif

// USB descriptor type constants (match ESP-IDF usb/usb_types_ch9.h)
#define USB_W_VALUE_DT_INTERFACE 0x04
#define USB_W_VALUE_DT_ENDPOINT  0x05

// USB endpoint attribute masks
constexpr uint8_t USB_CLASS_AUDIO = 0x01;
constexpr uint8_t USB_SUBCLASS_MIDISTREAMING = 0x03;
constexpr uint8_t USB_BM_ATTRIBUTES_XFER_BULK = 0x02;
constexpr uint8_t USB_BM_ATTRIBUTES_XFERTYPE_MASK = 0x03;
constexpr uint8_t USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK = 0x80;

// USB speeds
enum usb_speed_t {
    USB_SPEED_LOW = 0,
    USB_SPEED_FULL = 1,
    USB_SPEED_HIGH = 2
};

// Transfer status
enum usb_transfer_status_t {
    USB_TRANSFER_STATUS_COMPLETED = 0,
    USB_TRANSFER_STATUS_ERROR,
    USB_TRANSFER_STATUS_TIMED_OUT,
    USB_TRANSFER_STATUS_CANCELED,
    USB_TRANSFER_STATUS_STALL,
    USB_TRANSFER_STATUS_NO_DEVICE
};

// Opaque handles
typedef void* usb_device_handle_t;
typedef void* usb_host_client_handle_t;

// ---- Config structs ----

struct usb_host_config_t {
    bool skip_phy_setup;
    int intr_flags;
};

// Client event enum and message
enum usb_host_client_event_t {
    USB_HOST_CLIENT_EVENT_NEW_DEV,
    USB_HOST_CLIENT_EVENT_DEV_GONE
};

struct usb_host_client_event_msg_t {
    usb_host_client_event_t event;
    union {
        struct { uint8_t address; } new_dev;
    };
};

using usb_host_client_event_cb_t = void (*)(const usb_host_client_event_msg_t *event_msg, void *arg);

// Matches ESP-IDF layout with nested async struct
struct usb_host_client_config_t {
    bool is_synchronous;
    int max_num_event_msg;
    struct {
        usb_host_client_event_cb_t client_event_callback;
        void *callback_arg;
    } async;
};

// ---- Device info ----

struct usb_device_info_t {
    usb_speed_t speed;
    uint8_t dev_addr;
    uint8_t vMaxPacketSize0;
    uint8_t bConfigurationValue;
};

// ---- Transfer ----

struct usb_transfer_t {
    uint8_t* data_buffer;
    size_t data_buffer_size;
    int num_bytes;
    int actual_num_bytes;
    uint32_t flags;
    usb_device_handle_t device_handle;
    uint8_t bEndpointAddress;
    usb_transfer_status_t status;
    uint32_t timeout_ms;
    void *context;
    void (*callback)(usb_transfer_t *transfer);
};

// ---- USB Descriptors (packed, match ESP-IDF) ----

struct usb_device_desc_t {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

// Config descriptor with flexible array member for raw descriptor walk
struct usb_config_desc_t {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
    uint8_t  val[];  // Flexible array member — the driver walks raw bytes via val
} __attribute__((packed));

struct usb_intf_desc_t {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

struct usb_ep_desc_t {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));


// =========================================================================
// Mock USB Host instrumentation state
// =========================================================================
struct MockUsbHost {
    static inline bool installed = false;
    static inline bool clientRegistered = false;
    static inline bool deviceOpened = false;
    static inline bool interfaceClaimed = false;
    static inline std::vector<uint8_t> lastOutTransferData;
    static inline std::vector<uint8_t> pendingInData;

    static inline usb_host_client_event_cb_t registeredCallback = nullptr;
    static inline void* callbackArg = nullptr;

    // Mock Akai MPK Mini MK4 Device Descriptor
    static inline usb_device_desc_t dev_desc = {
        .bLength = sizeof(usb_device_desc_t),
        .bDescriptorType = 0x01,
        .bcdUSB = 0x0200,
        .bDeviceClass = 0x00,
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = 64,
        .idVendor = 0x09E8,     // Akai Professional
        .idProduct = 0x0028,    // MPK Mini MK4
        .bcdDevice = 0x0100,
        .iManufacturer = 0x01,
        .iProduct = 0x02,
        .iSerialNumber = 0x00,
        .bNumConfigurations = 1
    };

    // Raw config descriptor block: config + interface + 2 endpoints
    // Layout matches what the driver walks via config_desc->val
    static constexpr size_t CONFIG_BLOCK_SIZE =
        sizeof(usb_config_desc_t) + sizeof(usb_intf_desc_t) + sizeof(usb_ep_desc_t) * 2;
    static inline uint8_t config_descriptor_block[CONFIG_BLOCK_SIZE];

    static void reset() {
        installed = false;
        clientRegistered = false;
        deviceOpened = false;
        interfaceClaimed = false;
        lastOutTransferData.clear();
        pendingInData.clear();
        registeredCallback = nullptr;
        callbackArg = nullptr;
    }

    static void simulateDeviceConnect(uint8_t addr = 1) {
        if (registeredCallback) {
            usb_host_client_event_msg_t msg;
            msg.event = USB_HOST_CLIENT_EVENT_NEW_DEV;
            msg.new_dev.address = addr;
            registeredCallback(&msg, callbackArg);
        }
    }

    static void simulateDeviceDisconnect() {
        if (registeredCallback) {
            usb_host_client_event_msg_t msg;
            msg.event = USB_HOST_CLIENT_EVENT_DEV_GONE;
            registeredCallback(&msg, callbackArg);
        }
    }

    static void injectMidiInData(const std::vector<uint8_t>& data) {
        pendingInData.insert(pendingInData.end(), data.begin(), data.end());
    }
};

// =========================================================================
// Mock USB Host Functions
// =========================================================================

inline esp_err_t usb_host_install(const usb_host_config_t*) {
    MockUsbHost::installed = true;
    return ESP_OK;
}

inline esp_err_t usb_host_uninstall() {
    MockUsbHost::installed = false;
    return ESP_OK;
}

inline esp_err_t usb_host_lib_handle_events(uint32_t, uint32_t*) {
    return ESP_OK;
}

inline esp_err_t usb_host_client_register(const usb_host_client_config_t* config, usb_host_client_handle_t* handle) {
    if (config) {
        MockUsbHost::registeredCallback = config->async.client_event_callback;
        MockUsbHost::callbackArg = config->async.callback_arg;
    }
    MockUsbHost::clientRegistered = true;
    if (handle) *handle = reinterpret_cast<void*>(0x1234);
    return ESP_OK;
}

inline esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, uint32_t) {
    return ESP_OK;
}

inline esp_err_t usb_host_client_deregister(usb_host_client_handle_t) {
    MockUsbHost::clientRegistered = false;
    MockUsbHost::registeredCallback = nullptr;
    MockUsbHost::callbackArg = nullptr;
    return ESP_OK;
}

inline esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t* handle) {
    MockUsbHost::deviceOpened = true;
    if (handle) *handle = reinterpret_cast<void*>(0x5678);
    return ESP_OK;
}

inline esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t) {
    MockUsbHost::deviceOpened = false;
    return ESP_OK;
}

inline esp_err_t usb_host_device_info(usb_device_handle_t, usb_device_info_t* info) {
    if (info) {
        info->speed = USB_SPEED_FULL;
        info->dev_addr = 1;
        info->vMaxPacketSize0 = 64;
        info->bConfigurationValue = 1;
    }
    return ESP_OK;
}

inline esp_err_t usb_host_get_device_descriptor(usb_device_handle_t, const usb_device_desc_t** desc) {
    if (desc) *desc = &MockUsbHost::dev_desc;
    return ESP_OK;
}

inline esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t, const usb_config_desc_t** desc) {
    static bool init = false;
    if (!init) {
        uint8_t* ptr = MockUsbHost::config_descriptor_block;

        // Config descriptor header
        usb_config_desc_t* cfg = reinterpret_cast<usb_config_desc_t*>(ptr);
        memset(cfg, 0, sizeof(usb_config_desc_t));
        cfg->bLength = sizeof(usb_config_desc_t);
        cfg->bDescriptorType = 0x02;
        cfg->wTotalLength = MockUsbHost::CONFIG_BLOCK_SIZE;
        cfg->bNumInterfaces = 1;
        ptr += sizeof(usb_config_desc_t);

        // MIDI Streaming Interface
        usb_intf_desc_t* intf = reinterpret_cast<usb_intf_desc_t*>(ptr);
        memset(intf, 0, sizeof(usb_intf_desc_t));
        intf->bLength = sizeof(usb_intf_desc_t);
        intf->bDescriptorType = USB_W_VALUE_DT_INTERFACE;
        intf->bInterfaceNumber = 0;
        intf->bAlternateSetting = 0;
        intf->bNumEndpoints = 2;
        intf->bInterfaceClass = USB_CLASS_AUDIO;
        intf->bInterfaceSubClass = USB_SUBCLASS_MIDISTREAMING;
        ptr += sizeof(usb_intf_desc_t);

        // Bulk IN endpoint
        usb_ep_desc_t* ep_in = reinterpret_cast<usb_ep_desc_t*>(ptr);
        memset(ep_in, 0, sizeof(usb_ep_desc_t));
        ep_in->bLength = sizeof(usb_ep_desc_t);
        ep_in->bDescriptorType = USB_W_VALUE_DT_ENDPOINT;
        ep_in->bEndpointAddress = 0x81; // IN 1
        ep_in->bmAttributes = USB_BM_ATTRIBUTES_XFER_BULK;
        ep_in->wMaxPacketSize = 64;
        ptr += sizeof(usb_ep_desc_t);

        // Bulk OUT endpoint
        usb_ep_desc_t* ep_out = reinterpret_cast<usb_ep_desc_t*>(ptr);
        memset(ep_out, 0, sizeof(usb_ep_desc_t));
        ep_out->bLength = sizeof(usb_ep_desc_t);
        ep_out->bDescriptorType = USB_W_VALUE_DT_ENDPOINT;
        ep_out->bEndpointAddress = 0x01; // OUT 1
        ep_out->bmAttributes = USB_BM_ATTRIBUTES_XFER_BULK;
        ep_out->wMaxPacketSize = 64;

        init = true;
    }

    if (desc) *desc = reinterpret_cast<const usb_config_desc_t*>(MockUsbHost::config_descriptor_block);
    return ESP_OK;
}

inline esp_err_t usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t, uint8_t, uint8_t) {
    MockUsbHost::interfaceClaimed = true;
    return ESP_OK;
}

inline esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t) {
    MockUsbHost::interfaceClaimed = false;
    return ESP_OK;
}

inline esp_err_t usb_host_transfer_alloc(size_t data_buffer_size, int, usb_transfer_t** transfer) {
    if (transfer) {
        usb_transfer_t* t = new usb_transfer_t();
        memset(t, 0, sizeof(usb_transfer_t));
        t->data_buffer = new uint8_t[data_buffer_size]();
        t->data_buffer_size = data_buffer_size;
        *transfer = t;
    }
    return ESP_OK;
}

inline esp_err_t usb_host_transfer_free(usb_transfer_t* transfer) {
    if (transfer) {
        delete[] transfer->data_buffer;
        delete transfer;
    }
    return ESP_OK;
}

inline esp_err_t usb_host_transfer_submit(usb_transfer_t* transfer) {
    if (!transfer) return ESP_FAIL;

    bool is_in = (transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;

    if (is_in) {
        if (!MockUsbHost::pendingInData.empty()) {
            size_t copy_len = std::min(transfer->data_buffer_size, MockUsbHost::pendingInData.size());
            std::copy(MockUsbHost::pendingInData.begin(),
                      MockUsbHost::pendingInData.begin() + copy_len,
                      transfer->data_buffer);
            transfer->actual_num_bytes = static_cast<int>(copy_len);
            MockUsbHost::pendingInData.erase(MockUsbHost::pendingInData.begin(),
                                             MockUsbHost::pendingInData.begin() + copy_len);
        } else {
            transfer->actual_num_bytes = 0;
        }
    } else {
        if (transfer->num_bytes > 0 && transfer->data_buffer) {
            MockUsbHost::lastOutTransferData.assign(
                transfer->data_buffer,
                transfer->data_buffer + transfer->num_bytes);
        }
        transfer->actual_num_bytes = transfer->num_bytes;
    }

    transfer->status = USB_TRANSFER_STATUS_COMPLETED;

    if (transfer->callback) {
        transfer->callback(transfer);
    }

    return ESP_OK;
}
