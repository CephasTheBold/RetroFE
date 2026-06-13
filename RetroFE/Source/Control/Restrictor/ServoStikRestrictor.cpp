#include "ServoStikRestrictor.h"
#include "../../Utility/Log.h"

#include <chrono>
#include <string>
#include <thread>

static constexpr char COMPONENT[] = "ServoStik";

namespace {
    constexpr int SERVO_INTERFACE = 0;

    constexpr uint8_t  UM_REQUEST_TYPE = 0x21;   // Host-to-device | Class | Interface
    constexpr uint8_t  UM_REQUEST = 9;      // HID SET_REPORT
    constexpr uint16_t UM_VALUE = 0x0200; // Output report
    constexpr uint16_t UM_INDEX = 0;      // Interface 0
    constexpr unsigned int TIMEOUT_MS = 2000;

    void closeServoStik(libusb_context*& ctx,
        libusb_device_handle*& handle,
        bool& claimed) {
        if (handle) {
            if (claimed) {
                libusb_release_interface(handle, SERVO_INTERFACE);
                claimed = false;
            }

            libusb_close(handle);
            handle = nullptr;
        }

        if (ctx) {
            libusb_exit(ctx);
            ctx = nullptr;
        }
    }
}

ServoStikRestrictor::ServoStikRestrictor(uint16_t vid, uint16_t pid)
    : vid_(vid),
    pid_(pid),
    ctx_(nullptr),
    handle_(nullptr),
    claimed_(false) {
}

ServoStikRestrictor::~ServoStikRestrictor() {
    closeServoStik(ctx_, handle_, claimed_);
}

bool ServoStikRestrictor::initialize() {
    LOG_INFO(COMPONENT, "Attempting to initialize ServoStik restrictor...");

    closeServoStik(ctx_, handle_, claimed_);

    int result = libusb_init(&ctx_);
    if (result != LIBUSB_SUCCESS) {
        LOG_ERROR(COMPONENT, "libusb_init failed: " + std::string(libusb_error_name(result)));
        ctx_ = nullptr;
        return false;
    }

#if defined(_WIN32)
    libusb_set_option(ctx_, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);
#endif

    handle_ = libusb_open_device_with_vid_pid(ctx_, vid_, pid_);
    if (!handle_) {
        LOG_INFO(COMPONENT, "No ServoStik device found.");
        closeServoStik(ctx_, handle_, claimed_);
        return false;
    }

#if defined(__linux__)
    if (libusb_kernel_driver_active(handle_, SERVO_INTERFACE) == 1) {
        result = libusb_detach_kernel_driver(handle_, SERVO_INTERFACE);
        if (result != LIBUSB_SUCCESS) {
            LOG_ERROR(COMPONENT, "Failed to detach kernel driver: " + std::string(libusb_error_name(result)));
            closeServoStik(ctx_, handle_, claimed_);
            return false;
        }
    }
#endif

    result = libusb_claim_interface(handle_, SERVO_INTERFACE);
    if (result == LIBUSB_SUCCESS) {
        claimed_ = true;
        LOG_INFO(COMPONENT, "ServoStik interface claimed.");
    }
    else {
#if defined(_WIN32)
        // On Windows, the control transfer may still work even if claim fails.
        LOG_INFO(COMPONENT, "Could not claim ServoStik interface: " +
            std::string(libusb_error_name(result)) +
            "; continuing anyway.");
#else
        LOG_ERROR(COMPONENT, "libusb_claim_interface failed: " + std::string(libusb_error_name(result)));
        closeServoStik(ctx_, handle_, claimed_);
        return false;
#endif
    }

    LOG_INFO(COMPONENT, "ServoStik restrictor detected and initialized.");
    return true;
}

bool ServoStikRestrictor::setWay(int way) {
    if (!handle_ || (way != 4 && way != 8)) {
        LOG_WARNING(COMPONENT, "Invalid handle or mode in setWay(" + std::to_string(way) + ")");
        return false;
    }

    unsigned char msg[4] = {
        0x00,
        0xDD,
        0x00,
        static_cast<unsigned char>(way == 4 ? 0x00 : 0x01)
    };

    LOG_INFO(COMPONENT, "Sending ServoStik command: {0x00, 0xDD, 0x00, " +
        std::to_string(msg[3]) + "}");

    bool success = false;

    // The known ServoStik implementations send this report twice.
    for (int i = 0; i < 2; ++i) {
        int ret = libusb_control_transfer(
            handle_,
            UM_REQUEST_TYPE,
            UM_REQUEST,
            UM_VALUE,
            UM_INDEX,
            msg,
            sizeof(msg),
            TIMEOUT_MS
        );

        if (ret == static_cast<int>(sizeof(msg))) {
            LOG_INFO(COMPONENT, "ServoStik control transfer successful on attempt " + std::to_string(i + 1));
            success = true;
        }
        else {
            LOG_ERROR(COMPONENT, "ServoStik control transfer failed on attempt " +
                std::to_string(i + 1) + ": " +
                std::to_string(ret) + " (" +
                std::string(libusb_error_name(ret)) + ")");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return success;
}

std::optional<int> ServoStikRestrictor::getWay() {
    return std::nullopt;
}

bool ServoStikRestrictor::isPresent() {
    libusb_context* ctx = nullptr;

    int result = libusb_init(&ctx);
    if (result != LIBUSB_SUCCESS) {
        return false;
    }

    libusb_device_handle* handle =
        libusb_open_device_with_vid_pid(ctx, 0xD209, 0x1700);

    bool present = (handle != nullptr);

    if (handle) {
        libusb_close(handle);
    }

    libusb_exit(ctx);
    return present;
}