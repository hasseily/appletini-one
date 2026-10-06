#pragma once

// Optional ideal switched-capacitor tract from the archived SC-02 prototype.
// This is an experimental circuit candidate, not proof of final SSI silicon.
// Arithmetic follows the archived ssi263_sc02_audio.sv charge events; fabric
// scheduling, FIFO capacity and divider pipeline latency are not modeled.
// Source branch commit: 502ae04f68f04e23ce04abaaf0d44990c71e763a.
#include <array>
#include <cstdint>

namespace ssi_host {

struct AnalogCodes {
    int f1{}, f2{}, f2q{}, f3{}, f4{}, filter_amp{}, voice_amp{}, fric_amp{};
};

struct FilterEvent {
    bool phase{};       // false = Phi0, true = Phi1
    bool phase_edge{};  // one actual edge into phase; false for held-phase edits
    AnalogCodes codes{};
    int voice_drive{};  // signed Q16 voltage at the U116 input, before VA bank
    int fric_drive{};   // signed Q16 voltage at the fricative source input
    bool fric1{}, fric2{};
    bool output_open{true}; // U148 reconstruction switch follows output when true
};

class PrototypeTract {
public:
    struct FormantState {
        int output{}, history{}, fixed_plate{};
        std::array<int, 4> plates{};
    };
    struct State {
        int voice{}, fric1{}, fric2_source{}, fric2_shape{};
        std::array<int, 4> voice_plates{}, fric1_plates{}, f2q_plates{}, filter_plates{};
        std::array<FormantState, 5> formants{};
        int c143_plate{}, c151_plate{};
        int c150_delta{}, c151_delta{};
        int output{}, reconstruction{};
    };
    struct Metrics {
        uint64_t events{}, phase_edges{}, state_saturations{}, output_clips{}, samples{};
        int peak_output_q16{};
    };

    // Output gain and source levels are explicit experiment settings. The old
    // branch used voice drive -2048, fricative +/-301 and socket gain 32; no
    // potentiometer setting or chip output gain is established by those values.
    explicit PrototypeTract(int output_gain = 1, int initial_fric_drive = 0);
    void process(const FilterEvent& event);
    int16_t sample();
    const State& state() const { return state_; }
    const Metrics& metrics() const { return metrics_; }

private:
    int saturate(int64_t value);
    int divide(int64_t numerator, int denominator);
    static bool same_codes(const AnalogCodes& a, const AnalogCodes& b);
    static int cap_sum(int mask, const std::array<int, 4>& caps);
    static int64_t charge(int mask, int target, const std::array<int, 4>& plates,
                          const std::array<int, 4>& caps);
    static int64_t weighted(int mask, const std::array<int, 4>& plates,
                            const std::array<int, 4>& caps);
    static void set_plates(std::array<int, 4>& plates, int mask, int target);
    void move_formant(int index, int mask, int fixed_cap,
                     const std::array<int, 4>& caps, int denominator,
                     int64_t extra_charge = 0);

    State state_{};
    Metrics metrics_{};
    FilterEvent applied_{};
    int output_gain_;
};

} // namespace ssi_host
