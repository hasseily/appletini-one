// Compare every fabric observation against the frozen C++ controller.
// Private counters/windows are visible only in this verification harness.
#include <array>
#include <cstdint>
#include <stdexcept>
#define private public
#include "native_control.h"
#undef private
#include "native_source.h"
#include "Vssi263_native_controller.h"
#include "verilated.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace ssi_host;

namespace {
constexpr int art_reference = TEST_ART_REFERENCE_RATE;
uint64_t observations = 0, checks = 0, ticks = 0, writes = 0, no_tick_writes = 0;
uint64_t same_edge_writes = 0, known_routes = 0, unknown_routes = 0;
std::array<bool, 64> seen_phones{};
std::array<bool, 16> seen_rate{};
std::array<bool, 8> seen_art{}, seen_reg{};
std::array<bool, 4> seen_duration{};

Tables read_rom(const char* path) {
    Tables t;
    std::ifstream in(path);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find("//"));
        std::istringstream fields(line);
        int value;
        while (fields >> std::hex >> value) {
            if (n >= 512) throw std::runtime_error("ROM overflow");
            t.native_rom[n++] = value;
        }
    }
    if (n != 512) throw std::runtime_error("ROM truncated");
    return t;
}

uint32_t pack(const std::array<int, 8>& values) {
    uint32_t result = 0;
    for (int n = 0; n < 8; ++n) result |= uint32_t(values[n]) << (4 * n);
    return result;
}

void equal(uint64_t actual, uint64_t expected, const char* signal) {
    ++checks;
    if (actual != expected) {
        std::ostringstream message;
        message << "ART_REF=" << art_reference << " observation=" << observations
                << " tick=" << ticks << " " << signal << ": RTL=0x" << std::hex
                << actual << " host=0x" << expected;
        throw std::runtime_error(message.str());
    }
}

class Fixture {
public:
    explicit Fixture(const Tables& tables) : tables_(tables) { reset(); }

    void reset() {
        ref = std::make_unique<NativeControl>(tables_, 1015625, NativeTiming{art_reference});
        source = NativeSource();
        dut.rstn = 0;
        dut.write_strobe = 1;
        dut.write_reg = 3;
        dut.write_data = 0;
        dut.xck_ce = 1;
        dut.ampct_zero = 1;
        edge();
        dut.rstn = 1;
        compare();
    }

    void cycle(bool ce = true, int reg = -1, int value = 0, int feedback = -1) {
        const int amp_zero = feedback < 0 ? source.ampct_zero() : feedback;
        dut.ampct_zero = amp_zero;
        dut.xck_ce = ce;
        dut.write_strobe = reg >= 0;
        dut.write_reg = reg >= 0 ? reg : 0;
        dut.write_data = value;
        if (reg >= 0) {
            ref->write(reg, value);
            ++writes;
            seen_reg[reg] = true;
            if (ce) ++same_edge_writes;
            else ++no_tick_writes;
            if (reg == 0) { seen_phones[value & 63] = true; seen_duration[value >> 6] = true; }
            if (reg == 2) seen_rate[value >> 4] = true;
            if (reg == 3) seen_art[(value >> 4) & 7] = true;
        }
        ref->set_ampct_zero(amp_zero);
        if (ce) { ref->advance_xck(1); ++ticks; }
        edge();
        compare();
        if (ce) {
            const auto& r = ref->regs_;
            const int pitch = ((r[2] & 8) << 8) | (r[1] << 3) | (r[2] & 7);
            source.tick(*ref, pitch);
        }
    }

    void run(int count, int feedback = -1) { for (int n = 0; n < count; ++n) cycle(true, -1, 0, feedback); }
    NativeControl& host() { return *ref; }

private:
    void edge() {
        dut.warm_reset = 0;
        dut.clk = 0; dut.eval();
        dut.clk = 1; dut.eval();
        ++observations;
    }

    void compare() {
        const auto& c = *ref;
        equal(dut.codes, pack(c.parameter_codes()), "codes");
        equal(dut.selector, c.selector(), "selector");
        equal(dut.duration_phase, c.duration_phase(), "duration_phase");
        equal(dut.filter_phase, c.filter_phase(), "filter_phase");
        equal(dut.filter_phase_edge, c.filter_phase_edge(), "filter_phase_edge");
        equal(dut.powered_down, c.latched_ctrl(), "powered_down");
        equal(dut.pw3_known, c.pw3() >= 0, "pw3_known");
        equal(dut.pw3, c.pw3() > 0, "pw3");
        equal(dut.fric1_known, c.fric1_sw() >= 0, "fric1_known");
        equal(dut.fric1, c.fric1_sw() > 0, "fric1");
        equal(dut.fric2_known, c.fric2_sw() >= 0, "fric2_known");
        equal(dut.fric2, c.fric2_sw() > 0, "fric2");
        equal(dut.inflection, ((c.regs_[2] & 8) << 8) | (c.regs_[1] << 3) | (c.regs_[2] & 7), "inflection");
        equal(dut.debug_phone, c.phone(), "phone");
        equal(dut.debug_phone_valid, c.phone_valid_, "phone_valid");
        equal(dut.debug_scan_phase, c.selector_phase(), "scan_phase");
        std::array<int, 8> held{{c.pw0(), c.pw1(), c.pw2(), c.pw3(), c.pw5(), c.u20(), c.fric1_sw(), c.fric2_sw()}};
        unsigned values = 0, known = 0, up = 0;
        std::array<int, 8> a{}, b{}, carry{}, target{};
        for (int n = 0; n < 8; ++n) {
            if (held[n] >= 0) known |= 1U << n;
            if (held[n] > 0) values |= 1U << n;
            const auto& d = c.parameter_state(n);
            a[n] = d.a; b[n] = d.b; carry[n] = d.c; target[n] = d.target;
            if (d.upward) up |= 1U << n;
        }
        equal(dut.debug_held_values, values, "held_values");
        equal(dut.debug_held_known, known, "held_known");
        equal(dut.debug_dda_a, pack(a), "DDA A");
        equal(dut.debug_dda_b, pack(b), "DDA B");
        equal(dut.debug_dda_c, pack(carry), "DDA C");
        equal(dut.debug_dda_target, pack(target), "DDA target");
        equal(dut.debug_dda_up, up, "DDA up");
        equal(dut.debug_duration_left, c.duration_left_, "duration_left");
        equal(dut.debug_articulation_left, c.articulation_left_, "articulation_left");
        equal(dut.debug_amplitude_left, c.amplitude_left_, "amplitude_left");
        equal(dut.debug_filter_left, c.filter_left_, "filter_left");
        equal(dut.debug_filter_frequency, c.filter_frequency(), "filter_frequency");
        uint64_t regs = 0;
        for (int n = 0; n < 5; ++n) regs |= uint64_t(c.regs_[n]) << (8 * n);
        equal(dut.debug_registers, regs, "registers");
        const unsigned windows = c.phone_setup_pending_ | (c.phone_setup_window_ << 1) |
            (c.control_setup_pending_ << 2) | (c.control_setup_window_ << 3) |
            (c.articulation_pending_ << 4) | (c.articulation_window_ << 5) |
            (c.amplitude_pending_ << 6) | (c.amplitude_window_ << 7) |
            (c.duration_pending_ << 8) | (c.duration_window_ << 9);
        equal(dut.debug_windows, windows, "pending/window flags");
        if (c.u20() >= 0) ++known_routes; else ++unknown_routes;
    }

    const Tables& tables_;
    std::unique_ptr<NativeControl> ref;
    NativeSource source;
    Vssi263_native_controller dut;
    // Standalone parity excludes the separately tested AP bus reset.

};

uint32_t rng = 0x26302025U;
uint32_t random_word() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
}

int main(int argc, char** argv) {
    try {
        Verilated::commandArgs(argc, argv);
        if (argc < 2) throw std::runtime_error("ROM path required");
        const auto tables = read_rom(argv[1]);
        Fixture f(tables);
        f.run(512); // Cold scanner/filter still run while source is stopped.
        for (int reg = 0; reg < 8; ++reg) { f.cycle(false, reg, 0x81 + reg); f.cycle(false); }
        f.reset();

        // Every ROM phone: high RATE allows PW and amplitude events, while
        // enough ticks exercise rising/falling DDA targets and held routes.
        f.cycle(false, 2, 0xf8); f.cycle(false, 4, 0xe7); f.cycle(false, 3, 0x7f);
        for (int phone = 0; phone < 64; ++phone) {
            f.cycle(true, 0, phone | 0xc0);
            f.run(65536);
            f.cycle(false, 3, 0x70); // Preserve provisional AMP=0 fade policy.
            f.run(2048);
            f.cycle(true, 3, 0x7f);
        }

        // All RATE/DUR combinations, every ART code, FF extremes, stop and
        // restart; duration/ART counters must not truncate maximum periods.
        for (int rate = 0; rate < 16; ++rate) for (int duration = 0; duration < 4; ++duration) {
            const int art = (rate + duration) & 7;
            f.cycle(false, 3, 0x80 | (art << 4) | 12);
            f.cycle(false, 2, (rate << 4) | (rate & 15));
            f.cycle(false, 0, (duration << 6) | ((rate * 4 + duration) & 63));
            f.cycle(false, 7, (rate & 1) ? 255 : 0);
            f.run(19);
            f.cycle(true, 3, (art << 4) | 12);
            f.run(2 * f.host().duration_period_ticks() + 129);
        }

        // Deliberately collide writes with every scanner slot/phase,
        // including r10/r11 transparent latch phases and fast FF edges.
        for (int slot = 0; slot < 8; ++slot) for (int phase = 0; phase < 16; ++phase) {
            while (f.host().selector() != slot || f.host().selector_phase() != phase) f.cycle();
            f.cycle(true, (slot + phase) & 7, int(random_word() & 255), phase & 1);
            f.cycle(false, 3, phase & 1 ? 0x80 : 0x5c);
            f.run(129, (phase >> 1) & 1);
        }

        // Reproducible ordered bus traffic with fabric stalls. Feedback
        // toggles independently so all route-enable inputs are exercised.
        for (int n = 0; n < 600000; ++n) {
            const uint32_t value = random_word();
            const bool ce = (value & 7) != 0;
            const int reg = (value & 511) < 8 ? int((value >> 9) & 7) : -1;
            f.cycle(ce, reg, (value >> 16) & 255, (value >> 24) & 1);
            if (n && n % 99991 == 0) f.reset();
        }

        // Optional real listening traces retain exact negative pre-roll,
        // register order and tick spacing. No register snapshot shortcut.
        for (int arg = 2; arg < argc; ++arg) {
            f.reset();
            std::ifstream trace(argv[arg]);
            std::string magic;
            int hz;
            int64_t start, end;
            size_t count;
            trace >> magic >> hz >> start >> end >> count;
            if (!trace || magic != "SSIHOST1") throw std::runtime_error("bad listening trace");
            struct Write { int64_t tick; int reg, value; };
            std::vector<Write> events;
            for (size_t n = 0; n < count; ++n) {
                int64_t tick; int socket, reg, value;
                trace >> tick >> socket >> reg >> value;
                if (socket == 0) events.push_back({tick, reg, value});
            }
            const int64_t first_tick = std::min<int64_t>(0, events.front().tick);
            const int64_t product = first_tick * sample_rate;
            const int64_t first_frame = product / hz - (product % hz < 0);
            const int64_t boundary = first_frame * hz;
            int64_t tick = boundary / sample_rate + (boundary % sample_rate > 0);
            for (const auto& e : events) {
                while (tick < e.tick) { f.cycle(); ++tick; }
                f.cycle(false, e.reg, e.value);
            }
            while (tick++ < end) f.cycle();
        }

        for (bool seen : seen_phones) if (!seen) throw std::runtime_error("phone coverage incomplete");
        for (bool seen : seen_rate) if (!seen) throw std::runtime_error("RATE coverage incomplete");
        for (bool seen : seen_art) if (!seen) throw std::runtime_error("ART coverage incomplete");
        for (bool seen : seen_duration) if (!seen) throw std::runtime_error("DUR coverage incomplete");
        for (bool seen : seen_reg) if (!seen) throw std::runtime_error("register coverage incomplete");
        std::cout << "{\"status\":\"passed\",\"art_reference\":" << art_reference
                  << ",\"observations\":" << observations << ",\"checks\":" << checks
                  << ",\"effective_ticks\":" << ticks << ",\"writes\":" << writes
                  << ",\"no_tick_writes\":" << no_tick_writes
                  << ",\"same_edge_writes\":" << same_edge_writes
                  << ",\"known_route_observations\":" << known_routes
                  << ",\"unknown_route_observations\":" << unknown_routes << "}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
