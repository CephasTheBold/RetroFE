#pragma once

#include "Restrictor.h"

#include <cstdint>
#include <optional>
#include <libusb.h>

class ServoStikRestrictor : public IRestrictor {
public:
    ServoStikRestrictor(uint16_t vid = 0xD209, uint16_t pid = 0x1700);
    ~ServoStikRestrictor() override;

    bool initialize() override;
    bool setWay(int way) override;
    std::optional<int> getWay() override;

    static bool isPresent();

private:
    uint16_t vid_;
    uint16_t pid_;

    libusb_context* ctx_;
    libusb_device_handle* handle_;
    bool claimed_;
};