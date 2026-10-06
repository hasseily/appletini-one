// Pitch-only fabric adapter versus the frozen Baseline public interface.
#include "baseline.h"
#include "Vssi263_native_pitch.h"
#include "verilated.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace ssi_host;

namespace {
uint64_t observations = 0, checks = 0, samples = 0, writes = 0;
uint64_t coincident = 0, stalled = 0;

template<class T> void number(std::istream& in, T& value) {
    if (!(in >> value)) throw std::runtime_error("truncated tables");
}
Tables read_tables(const char* path) {
    std::ifstream in(path);
    Tables t;
    for (auto& p : t.phones)
        for (int* v : {&p.f1, &p.va, &p.f2, &p.fc, &p.f2q, &p.f3,
                       &p.fa, &p.cld, &p.vd, &p.closure, &p.duration, &p.pause}) number(in, *v);
    for (auto& v : t.native_rom) number(in, v);
    for (auto* map : {&t.native_f1, &t.native_f2, &t.native_f2q, &t.native_f3, &t.native_va, &t.native_fa})
        for (auto& v : *map) number(in, v);
    for (auto& v : t.sc01_map) number(in, v);
    for (auto& row : t.coeff_f1) for (auto& v : row) number(in, v);
    for (auto& bank : t.coeff_f2) for (auto& row : bank) for (auto& v : row) number(in, v);
    for (auto& row : t.coeff_f3) for (auto& v : row) number(in, v);
    for (auto& v : t.coeff_f4) number(in, v);
    for (auto& v : t.coeff_fn) number(in, v);
    for (auto& v : t.coeff_fx) number(in, v);
    std::string extra;
    if (in >> extra) throw std::runtime_error("trailing tables");
    return t;
}

void equal(int actual, int expected, const char* signal) {
    ++checks;
    if (actual != expected) {
        std::ostringstream message;
        message << "observation=" << observations << " sample=" << samples << ' ' << signal
                << " RTL=" << actual << " host=" << expected;
        throw std::runtime_error(message.str());
    }
}

class Fixture {
public:
    explicit Fixture(const Tables& tables) : tables_(tables) { reset(); }
    void reset() {
        ref = std::make_unique<Baseline>(tables_);
        dut.rstn = 0; dut.write_strobe = 1; dut.write_reg = 3;
        dut.write_data = 0x5c; dut.audio_tick = 1;
        dut.warm_reset = 0; dut.mode_latch = 0;
        dut.clk = 0; dut.eval(); dut.clk = 1; dut.eval();
        dut.rstn = 1; dut.write_strobe = 0; dut.audio_tick = 0; dut.eval();
        equal(dut.active_inflection, ref->active_inflection(), "reset active");
        equal(dut.current_function, ref->current_function(), "reset function");
    }
    void cycle(bool audio = true, int reg = -1, int value = 0) {
        ++observations;
        dut.audio_tick = audio; dut.write_strobe = reg >= 0;
        dut.write_reg = reg >= 0 ? reg : 0; dut.write_data = value;
        if (reg >= 0) { ref->write(reg, value); ++writes; if (audio) ++coincident; }
        dut.warm_reset = 0; dut.mode_latch = 0;
        dut.clk = 0; dut.eval();
        const int live = ((ref->register_value(2) & 8) << 8) |
                         (ref->register_value(1) << 3) | (ref->register_value(2) & 7);
        const int expected = ref->current_function() == 3 ? ref->active_inflection() : live;
        equal(dut.inflection_for_tick, expected, "pre-sample source inflection");
        if (audio) { ref->sample(); ++samples; }
        else ++stalled;
        dut.clk = 1; dut.eval();
        equal(dut.active_inflection, ref->active_inflection(), "post-sample active inflection");
        equal(dut.current_function, ref->current_function(), "function");
    }
    void run(int count) { for (int n = 0; n < count; ++n) cycle(); }
    void word(int value, bool audio = false, int rate = 8) {
        cycle(audio, 1, (value >> 3) & 255);
        cycle(audio, 2, (rate << 4) | ((value >> 8) & 8) | (value & 7));
    }
    void mode(int function, bool audio = false) {
        cycle(audio, 3, 0xdc);
        cycle(audio, 0, function << 6);
        cycle(audio, 3, 0x5c);
    }
private:
    const Tables& tables_;
    std::unique_ptr<Baseline> ref;
    Vssi263_native_pitch dut;
    // Standalone parity excludes the separately tested AP bus reset.

};
uint32_t rng = 0x263125d1U;
uint32_t random_word() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
}

int main(int argc, char** argv) {
    try {
        Verilated::commandArgs(argc, argv);
        if (argc != 2) throw std::runtime_error("table file required");
        const auto tables = read_tables(argv[1]);
        Fixture f(tables);

        // Function 0 is the cold default; DUR=0 release must retain it.
        f.mode(0); f.run(96);
        for (int function = 0; function < 4; ++function) {
            f.reset(); f.mode(function);
            // Every 12-bit word, including both extremes and all eight
            // glide steps. Every write+sample cadence collision appears.
            for (int value = 0; value < 4096; ++value) {
                f.word(value, value & 1, value >> 8);
                f.cycle(value & 2, 0, value & 63);
                f.run(13);
                f.cycle(false);
            }
        }

        // Every step size, direction and active/target field pair. Include
        // transitions that finish on one cadence edge and full-range glides.
        for (int step = 0; step < 8; ++step)
            for (int from = 0; from < 32; ++from)
                for (int to = 0; to < 32; ++to) {
                    f.reset();
                    f.word((from << 6) | (step << 3) | 5);
                    f.mode(3);
                    f.word(0x800 | (to << 6) | (step << 3) | 2);
                    f.cycle(true, 0, 0x2c);
                    f.run(84);
                }

        // Repeated CTL and function changes must not reseed a used glide;
        // FF aliases must have no effect on the pitch-only state.
        for (int n = 0; n < 150000; ++n) {
            const uint32_t value = random_word();
            const int reg = (value & 15) < 8 ? (value & 7) : -1;
            f.cycle((value & 32) != 0, reg, (value >> 8) & 255);
            if (n && n % 29989 == 0) f.reset();
        }
        std::cout << "{\"status\":\"passed\",\"observations\":" << observations
                  << ",\"checks\":" << checks << ",\"audio_samples\":" << samples
                  << ",\"writes\":" << writes << ",\"coincident_write_samples\":" << coincident
                  << ",\"no_sample_observations\":" << stalled << "}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
