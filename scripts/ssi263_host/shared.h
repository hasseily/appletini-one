#pragma once

#include <array>
#include <cstdint>

namespace ssi_host {

constexpr int sample_rate = 48000;

struct Phone {
    int f1{}, va{}, f2{}, fc{}, f2q{}, f3{}, fa{};
    int cld{}, vd{}, closure{}, duration{}, pause{};
};

// These are read from the committed table package. The host renderer never
// runs the historical ROM/coefficient generator or changes FPGA data.
struct Tables {
    std::array<Phone, 64> phones{};
    std::array<int, 512> native_rom{};
    std::array<int, 16> native_f1{}, native_f2{}, native_f2q{};
    std::array<int, 16> native_f3{}, native_va{}, native_fa{};
    std::array<int, 64> sc01_map{};
    int coeff_f1[16][7]{};
    int coeff_f2[32][16][7]{};
    int coeff_f3[16][7]{};
    int coeff_f4[7]{};
    int coeff_fn[5]{};
    int coeff_fx[2]{};
};

} // namespace ssi_host
