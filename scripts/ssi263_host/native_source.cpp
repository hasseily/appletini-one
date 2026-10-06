#include "native_source.h"
#include "native_control.h"

#include <stdexcept>

namespace ssi_host {

void NativeNoise::rising_edge() {
    count = count == 15 ? 1 : count + 1;
}

void NativeNoise::falling_edge() {
    const int force = (count & 12) == 0 ? 1 : 0;
    const int feedback = force ^ ((d1 >> 3) & 1) ^ ((d2 >> 4) & 1) ^
                         ((d4 >> 3) & 1) ^ ((d4 >> 4) & 1);
    // All inputs name pre-edge state. U73 has two four-stage and two
    // five-stage sections; this is not the old 15-bit SC-01 recurrence.
    const int next_d1 = ((d1 << 1) | ((d3 >> 3) & 1)) & 15;
    const int next_d2 = ((d2 << 1) | ((d4 >> 4) & 1)) & 31;
    const int next_d3 = ((d3 << 1) | ((d2 >> 4) & 1)) & 15;
    const int next_d4 = ((d4 << 1) | feedback) & 31;
    d1 = next_d1;
    d2 = next_d2;
    d3 = next_d3;
    d4 = next_d4;
}

NativeSource::NativeSource(int voice_trim_q16) : voice_trim_(voice_trim_q16) {
    if (voice_trim_q16 < 0 || voice_trim_q16 > 131071)
        throw std::invalid_argument("Voice trim must fit positive signed Q16/18-bit range");
}

bool NativeSource::u62_reset(const NativeSourceInputs& inputs) const {
    // A still-unknown PW3 uses the prior branch's zero cold seed only in
    // this optional prototype experiment. NativeControl keeps its -1 flag.
    const bool u104c = inputs.pw3 == 1 && !u62_;
    const bool up = !u104c && (output_.voice_amp != 0 || output_.fric_amp != 0);
    const bool nco = up ? ampct_ != 15 : ampct_ != 0;
    return u104c || nco;
}

void NativeSource::settle(const NativeSourceInputs& inputs) {
    // Resolve actual drawn gate levels, including a gate that opens while
    // SEL2 is already high. Counting only 3->4 selector edges misses that
    // case. Each pass represents settling between counter/latch events;
    // it is not another XCK edge or a model of analog propagation delays.
    for (int pass = 0; pass < 16; ++pass) {
        bool changed = false;
        if (u62_reset(inputs) && u62_) {
            u62_ = false;
            changed = true;
        }
        const bool u104c = inputs.pw3 == 1 && !u62_;
        const bool up = !u104c && (output_.voice_amp != 0 || output_.fric_amp != 0);
        const bool nco = up ? ampct_ != 15 : ampct_ != 0;
        const bool clock = (inputs.selector & 4) &&
                           !((!nco && up) || (!up && ampct_zero()));
        if (clock && !u68_clock_) {
            ampct_ = (ampct_ + (up ? 1 : 15)) & 15;
            ++metrics_.envelope_edges;
            changed = true;
        }
        if (clock != u68_clock_) changed = true;
        u68_clock_ = clock;

        const bool u41c = !(inputs.pw3 == 1 && !u62_) &&
                          !(inputs.selector & 2) && output_.fric_amp != 0;
        if (u41c && !noise_clock_) {
            noise_.rising_edge();
            ++metrics_.noise_clock_edges;
        } else if (!u41c && noise_clock_) {
            noise_.falling_edge();
            ++metrics_.noise_shift_edges;
        }
        noise_clock_ = u41c;
        if (!changed) return;
    }
    throw std::runtime_error("Native source gate settling did not converge");
}

void NativeSource::tick(const NativeControl& control, int active_inflection) {
    NativeSourceInputs inputs;
    inputs.codes = control.parameter_codes();
    inputs.selector = control.selector();
    inputs.pw3 = control.pw3();
    inputs.inflection = active_inflection;
    inputs.fric1 = control.fric1_sw();
    inputs.fric2 = control.fric2_sw();
    inputs.phase = control.filter_phase();
    inputs.phase_edge = control.filter_phase_edge();
    // Host-policy departure: this optional renderer uses CTL as both mute
    // and the U61-clear input. The archived prototype connects PD/RST to
    // U61 separately. Their equivalence and hard source zero are not proved
    // production-chip behavior; keep this policy isolated for comparison.
    inputs.powered_down = control.latched_ctrl();
    tick(inputs);
}

void NativeSource::tick(const NativeSourceInputs& inputs) {
    if (inputs.selector < 0 || inputs.selector > 7 || inputs.inflection < 0 || inputs.inflection > 4095)
        throw std::invalid_argument("Native source selector or inflection outside chip range");
    ++metrics_.xck_ticks;
    output_.phase = inputs.phase;
    output_.phase_edge = inputs.phase_edge;
    output_.output_open = inputs.phase_edge && !inputs.phase;

    // These are transparent phase latches, not just edge-triggered samples.
    if (!inputs.phase) {
        output_.f1 = inputs.codes[0];
        output_.f3 = inputs.codes[3];
        output_.voice_amp = inputs.codes[5];
    } else {
        output_.f2 = inputs.codes[1];
        output_.f2q = inputs.codes[2];
        output_.f4 = inputs.codes[3];
        output_.fric_amp = inputs.codes[6];
    }
    // Unknown cold routes keep the provisional FRIC1 startup seed until
    // their real held control becomes known. The former FRIC2 seed caused
    // a first-H hiss absent from the user's physical SSI experience.
    // NativeControl retains unknown flags and the prototype latch gating;
    // this source-only fallback is not a measured U20 power-up state.
    if (inputs.fric1 >= 0) output_.fric1 = inputs.fric1 != 0;
    if (inputs.fric2 >= 0) output_.fric2 = inputs.fric2 != 0;

    settle(inputs);
    // The parallel pitch register takes effect on the next divider reload.
    // No host phone write or CTL edge resets this divider.
    if (--voice_left_ == 0) {
        voice_left_ = 4 * (4096 - inputs.inflection);
        ++metrics_.voice_clock_edges;
        if (!u62_reset(inputs)) {
            u62_ = !u62_;
            ++metrics_.voice_toggle_edges;
        }
    }
    settle(inputs);

    if (inputs.powered_down) {
        pitch_sync1_ = pitch_sync2_ = load_pending_ = false;
    } else if (inputs.phase_edge && !inputs.phase) {
        const bool old_sync1 = pitch_sync1_;
        pitch_sync1_ = u62_;
        pitch_sync2_ = old_sync1;
        if (u62_ && !old_sync1) load_pending_ = true;
    }
    if (inputs.phase_edge) {
        if (inputs.phase) {
            ++metrics_.phi1_edges;
            if (!inputs.powered_down && load_pending_) {
                voice_count_ = 11; // U60 P3..P0 = 1011.
                load_pending_ = false;
                ++metrics_.glottal_loads;
            } else if (voice_count_ != 15) {
                ++voice_count_;
            }
        } else {
            ++metrics_.phi0_edges;
            const bool u104c = inputs.pw3 == 1 && !u62_;
            output_.filter_amp = inputs.codes[4] & ((ampct_ & 14) | (u104c ? 0 : 1));
        }
    }
    const bool u104c = inputs.pw3 == 1 && !u62_;
    noise_bit_ = !(((noise_.d3 >> 3) & 1) || u104c) &&
                 (!u62_ || output_.voice_amp == 0);
    // Retained host hard-mute policy, not a reproduced prototype gate:
    // archived audio retains U60/noise excitation state through PD/RST.
    output_.voice_target_q16 = !inputs.powered_down && voice_count_ != 15 ? -voice_trim_ : 0;
    output_.fric_drive_q16 = inputs.powered_down ? 0 : noise_bit_ ? 301 : -301;
}

} // namespace ssi_host
