#include "Vsource_tb.h"
#include "native_source.h"
#include "native_control.h"
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using namespace ssi_host;
std::uint64_t checks = 0, ticks = 0;
Vsource_tb rtl;
NativeSource model;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(std::string(message) + " at tick " + std::to_string(ticks));
}
int sign_extend(std::uint32_t value, int bits) {
    return static_cast<int>(value << (32 - bits)) >> (32 - bits);
}
void clock_rtl() {
    rtl.clk = 0; rtl.eval();
    rtl.clk = 1; rtl.eval();
}
void reset() {
    rtl.rstn = 0; rtl.xck_ce = 0; clock_rtl();
    rtl.rstn = 1;
    model = NativeSource();
    check(rtl.event_fric1 && !rtl.event_fric2, "cold FRIC1 seed");
    check(rtl.debug_ampct == 0 && rtl.debug_voice_count == 15, "cold counters");
    check(rtl.debug_voice_left == 16384 && rtl.debug_noise == (15u << 18 | 1u), "cold pitch/noise");
}
void compare(const NativeSourceInputs& in, bool enable = true) {
    std::uint32_t packed = 0;
    for (int i = 0; i < 8; ++i) packed |= unsigned(in.codes[i]) << (4 * i);
    rtl.codes = packed;
    rtl.selector = in.selector;
    rtl.pw3 = in.pw3 == 1; rtl.pw3_known = in.pw3 >= 0;
    rtl.inflection = in.inflection;
    rtl.fric1 = in.fric1 == 1; rtl.fric1_known = in.fric1 >= 0;
    rtl.fric2 = in.fric2 == 1; rtl.fric2_known = in.fric2 >= 0;
    rtl.filter_phase = in.phase; rtl.filter_phase_edge = in.phase_edge;
    rtl.powered_down = in.powered_down; rtl.xck_ce = enable;
    if (enable) { model.tick(in); ++ticks; }
    clock_rtl();
    if (enable) {
        check(rtl.source_busy && !rtl.source_valid, "accepted XCK starts transaction");
        const auto previous_codes = rtl.event_codes;
        const auto previous_ampct_zero = rtl.ampct_zero;
        for (int cycle = 1; cycle <= 8; ++cycle) {
            rtl.xck_ce = 0;
            // Live bus/scanner signals can change during the transaction.
            // Only the values captured with XCK may affect this event.
            rtl.codes ^= 0xaaaaaaaa;
            rtl.selector ^= 7;
            rtl.inflection ^= 0xfff;
            rtl.pw3 ^= 1; rtl.pw3_known ^= 1;
            rtl.filter_phase ^= 1; rtl.filter_phase_edge ^= 1;
            rtl.powered_down ^= 1;
            clock_rtl();
            check(bool(rtl.source_valid) == (cycle == 8), "eight-cycle source latency");
            check(bool(rtl.source_busy) == (cycle != 8), "source busy window");
            if (cycle != 8) {
                check(rtl.event_codes == previous_codes, "uncommitted codes remain hidden");
                check(rtl.ampct_zero == previous_ampct_zero, "coherent envelope feedback");
            }
        }
    } else {
        check(!rtl.source_valid && !rtl.source_busy, "idle has no event");
    }
    const auto& out = model.output();
    const std::array<int, 8> fields{out.f1, out.f2, out.f2q, out.f3, out.f4,
                                  out.filter_amp, out.voice_amp, out.fric_amp};
    for (int i = 0; i < 8; ++i)
        check(((rtl.event_codes >> (i * 4)) & 15) == unsigned(fields[i]), "analog latch codes");
    check(sign_extend(rtl.voice_drive, 24) == out.voice_target_q16, "voice drive");
    check(sign_extend(rtl.fric_drive, 18) == out.fric_drive_q16, "fricative drive");
    check(bool(rtl.event_phase) == out.phase, "phase hold");
    check(bool(rtl.event_phase_edge) == (enable && out.phase_edge), "phase edge enable");
    check(bool(rtl.event_output_open) == (enable && out.output_open), "output open enable");
    check(bool(rtl.event_fric1) == out.fric1 && bool(rtl.event_fric2) == out.fric2, "held routes");
    check(rtl.debug_ampct == model.ampct(), "U68 counter");
    check(rtl.ampct_zero == model.ampct_zero(), "upper-bit zero detection");
    check(rtl.debug_voice_count == model.glottal_count(), "glottal count");
    check(rtl.debug_voice_left == model.voice_ticks_left(), "pitch countdown");
    const auto& n = model.noise_state();
    const unsigned noise = n.d1 | (n.d2 << 4) | (n.d3 << 9) | (n.d4 << 13) | (n.count << 18);
    check(rtl.debug_noise == noise, "CD4006 and noise prescaler");
    const unsigned flags = model.voice_toggle() | (model.noise_bit() << 1) |
                           (model.glottal_load_pending() << 2);
    check(rtl.debug_flags == flags, "pitch/load/noise state");
    check(!rtl.source_fault, "bounded gate settling converges");
}
void hello_four(const char* path) {
    std::ifstream input(path);
    int start, end, remaining;
    check(bool(input >> start >> end >> remaining), "hello header");
    Tables tables;
    for (int& b : tables.native_rom) check(bool(input >> b), "native ROM byte");
    NativeControl control(tables);
    reset();
    int event_tick, event_reg, event_value;
    check(bool(input >> event_tick >> event_reg >> event_value), "first hello write");
    std::array<int, 4> audible_h{};
    int h_index = -1;
    for (int tick = start; tick < end; ++tick) {
        while (remaining && event_tick == tick) {
            control.write(event_reg, event_value);
            if (event_reg == 0 && (event_value & 63) == 0x2c) ++h_index;
            if (--remaining) check(bool(input >> event_tick >> event_reg >> event_value), "hello write");
        }
        control.set_ampct_zero(model.ampct_zero());
        control.advance_xck(1);
        NativeSourceInputs in;
        in.codes = control.parameter_codes();
        in.selector = control.selector();
        in.pw3 = control.pw3(); in.inflection = 2942;
        in.fric1 = control.fric1_sw(); in.fric2 = control.fric2_sw();
        in.phase = control.filter_phase(); in.phase_edge = control.filter_phase_edge();
        in.powered_down = control.latched_ctrl();
        compare(in);
        const auto& out = model.output();
        if (control.phone() == 0x2c && out.filter_amp && out.fric_amp) {
            check(h_index >= 0 && h_index < 4, "H index");
            ++audible_h[h_index];
            check(rtl.event_fric1 && !rtl.event_fric2, "all four H routes");
        }
    }
    check(remaining == 0 && h_index == 3, "all hello writes consumed");
    for (int count : audible_h) check(count > 1000, "audible H interval");
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "startup trace argument");
        reset();
        NativeSourceInputs in;
        in.powered_down = false;
        in.pw3 = 0;
        // All 16 host masks through charge/discharge, including gates that
        // open while SEL2 is already high, not only on scanner transitions.
        for (int mask = 0; mask < 16; ++mask) {
            in.codes[4] = mask;
            for (int pw : {0, 1, -1}) {
                in.pw3 = pw;
                for (int a = 0; a < 16; ++a) {
                    in.codes[5] = a; in.codes[6] = 15 - a;
                    for (int t = 0; t < 96; ++t) {
                        in.selector = (t / 2) & 7;
                        in.phase = t & 1; in.phase_edge = true;
                        compare(in);
                    }
                }
            }
        }
        // A long run at each boundary allows divider reloads and glottal
        // synchronization to occur, rather than checking only arithmetic.
        for (int pitch : {0, 1, 2048, 3072, 4094, 4095}) {
            reset(); in = NativeSourceInputs{};
            in.inflection = pitch; in.pw3 = 0; in.powered_down = false;
            in.codes[4] = in.codes[5] = in.codes[6] = 15;
            for (int t = 0; t < 80000; ++t) {
                in.selector = (t / 16) & 7;
                in.phase = (t / 25) & 1; in.phase_edge = t % 25 == 0;
                compare(in);
            }
        }
        std::mt19937 random(0x263);
        reset();
        for (int t = 0; t < 200000; ++t) {
            for (int& code : in.codes) code = random() & 15;
            in.selector = random() & 7; in.inflection = random() & 4095;
            in.pw3 = int(random() % 3) - 1;
            in.fric1 = int(random() % 3) - 1;
            in.fric2 = int(random() % 3) - 1;
            in.phase = random() & 1; in.phase_edge = random() & 1;
            in.powered_down = (random() & 31) == 0;
            compare(in);
            if ((random() & 7) == 0) compare(in, false);
        }
        hello_four(argv[1]);
        reset();
        rtl.xck_ce = 1; clock_rtl();
        check(rtl.source_busy && !rtl.source_fault, "overrun setup");
        clock_rtl();
        check(rtl.source_fault, "overrun becomes sticky fault");
        rtl.xck_ce = 0;
        for (int t = 0; t < 10; ++t) clock_rtl();
        check(rtl.source_fault && !rtl.source_busy, "overrun fault survives transaction");
        reset();
        check(!rtl.source_fault, "reset clears source fault");
        std::cout << "PASS " << checks << " source RTL/host checks across " << ticks << " XCK ticks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
