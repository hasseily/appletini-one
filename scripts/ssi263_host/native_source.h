#pragma once

#include <array>
#include <cstdint>

namespace ssi_host {
class NativeControl;

// Prototype sheet-6 U75/CD4006 state. These seeds reproduce the prior branch
// deterministically; the physical chip's power-up contents are not known.
struct NativeNoise {
    int d1{1}, d2{}, d3{}, d4{}, count{15};
    void rising_edge();
    void falling_edge();
};

struct NativeSourceInputs {
    std::array<int, 8> codes{}; // Native selector order, before phase latches.
    int selector{}, pw3{-1}, inflection{};
    int fric1{-1}, fric2{-1};
    bool phase{}, phase_edge{}, powered_down{true};
};

struct NativeSourceOutput {
    bool phase{}, phase_edge{}, output_open{};
    int f1{}, f2{}, f2q{}, f3{}, f4{};
    int filter_amp{}, voice_amp{}, fric_amp{};
    int voice_target_q16{}, fric_drive_q16{};
    // Provisional SSI startup policy: use FRIC1 until a held route is known.
    // This avoids the first-H hiss reported with the archived FRIC2 seed;
    // it does not assert that the prototype or final chip resets U20 to 1.
    bool fric1{true}, fric2{};
};

struct NativeSourceMetrics {
    std::uint64_t xck_ticks{}, voice_clock_edges{}, voice_toggle_edges{};
    std::uint64_t envelope_edges{}, noise_clock_edges{}, noise_shift_edges{};
    std::uint64_t glottal_loads{}, phi0_edges{}, phi1_edges{};
};

class NativeSource {
public:
    // Accepted voice/noise balance; POT3's physical wiper setting is unknown.
    // This is a model calibration value, not a measured source voltage.
    explicit NativeSource(int voice_trim_q16 = 16384);
    void tick(const NativeSourceInputs& inputs);
    // The renderer supplies Baseline::active_inflection() between output
    // samples: native XCK divider, retained approximate pitch-glide cadence.
    // This does not import the prototype's speech-RATE-dependent glide.
    void tick(const NativeControl& control, int active_inflection);
    const NativeSourceOutput& output() const { return output_; }
    const NativeSourceMetrics& metrics() const { return metrics_; }
    const NativeNoise& noise_state() const { return noise_; }
    int ampct_zero() const { return (ampct_ & 14) == 0 ? 1 : 0; }
    int ampct() const { return ampct_; }
    int glottal_count() const { return voice_count_; }
    bool voice_toggle() const { return u62_; }
    bool noise_bit() const { return noise_bit_; }
    bool glottal_load_pending() const { return load_pending_; }
    int voice_ticks_left() const { return voice_left_; }

private:
    void settle(const NativeSourceInputs& inputs);
    bool u62_reset(const NativeSourceInputs& inputs) const;

    NativeSourceOutput output_{};
    NativeSourceMetrics metrics_{};
    NativeNoise noise_{};
    int voice_trim_, voice_left_{16384}, voice_count_{15}, ampct_{};
    bool u62_{}, u68_clock_{}, noise_clock_{}, pitch_sync1_{}, pitch_sync2_{};
    bool load_pending_{}, noise_bit_{};
};

} // namespace ssi_host
