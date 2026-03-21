#pragma once
#include "Restrictor.h"
#include "libserialport.h"
#include <optional>
#include <string>
#include <cstdint>

class TOSGRSRestrictor : public IRestrictor {
public:
    // Constants for the GRS hardware (Arduino Leonardo based)
    static constexpr uint16_t GRS_VID = 0x2341;
    static constexpr uint16_t GRS_PID = 0x8036;

    TOSGRSRestrictor();
    ~TOSGRSRestrictor() override;

    bool initialize() override;
    bool setWay(int way) override;
    std::optional<int> getWay() override;

    static bool isPresent();

private:
    sp_port* port_;

    sp_port* findPort();
    std::string sendCmd(const std::string&);
};
