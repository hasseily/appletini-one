#include "prototype_tract.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace ssi_host {
namespace {
constexpr std::array<int, 4> voice_caps{220, 430, 870, 1800};
constexpr std::array<int, 4> fric1_caps{270, 512, 1068, 2160};
constexpr std::array<int, 4> fric2_caps{270, 530, 1082, 2160};
constexpr std::array<int, 4> f1_caps{160, 330, 660, 1300};
constexpr std::array<int, 4> f2_caps{280, 560, 1120, 2300};
constexpr std::array<int, 4> f3_caps{210, 420, 820, 1640};
constexpr std::array<int, 4> f4_caps{200, 400, 820, 1620};
constexpr std::array<int, 4> amp_caps{76, 150, 300, 600};
constexpr std::array<int, 4> zero_caps{};
}

PrototypeTract::PrototypeTract(int output_gain, int initial_fric_drive)
    : output_gain_(output_gain) {
    if (output_gain < 0 || output_gain > 1024)
        throw std::invalid_argument("prototype output gain must be 0..1024");
    if (initial_fric_drive < -131072 || initial_fric_drive > 131071)
        throw std::invalid_argument("prototype fricative drive must fit signed 18 bits");
    applied_.fric_drive = initial_fric_drive;
}

int PrototypeTract::saturate(int64_t value) {
    if (value < -8388608 || value > 8388607) ++metrics_.state_saturations;
    return static_cast<int>(std::clamp<int64_t>(value, -8388608, 8388607));
}

int PrototypeTract::divide(int64_t numerator, int denominator) {
    // One nearest-away rounding after summing raw pF * Q16 plate charge.
    const int64_t rounded = numerator < 0 ? numerator - denominator / 2 : numerator + denominator / 2;
    return saturate(rounded / denominator);
}

bool PrototypeTract::same_codes(const AnalogCodes& a, const AnalogCodes& b) {
    return a.f1 == b.f1 && a.f2 == b.f2 && a.f2q == b.f2q && a.f3 == b.f3 &&
           a.f4 == b.f4 && a.filter_amp == b.filter_amp &&
           a.voice_amp == b.voice_amp && a.fric_amp == b.fric_amp;
}

int PrototypeTract::cap_sum(int mask, const std::array<int, 4>& caps) {
    int result = 0;
    for (int bit = 0; bit < 4; ++bit) if (mask & (1 << bit)) result += caps[bit];
    return result;
}

int64_t PrototypeTract::charge(int mask, int target, const std::array<int, 4>& plates,
                              const std::array<int, 4>& caps) {
    int64_t result = 0;
    for (int bit = 0; bit < 4; ++bit)
        if (mask & (1 << bit)) result += int64_t(caps[bit]) * (target - plates[bit]);
    return result;
}

int64_t PrototypeTract::weighted(int mask, const std::array<int, 4>& plates,
                                const std::array<int, 4>& caps) {
    int64_t result = 0;
    for (int bit = 0; bit < 4; ++bit)
        if (mask & (1 << bit)) result += int64_t(caps[bit]) * plates[bit];
    return result;
}

void PrototypeTract::set_plates(std::array<int, 4>& plates, int mask, int target) {
    for (int bit = 0; bit < 4; ++bit) if (mask & (1 << bit)) plates[bit] = target;
}

void PrototypeTract::move_formant(int index, int mask, int fixed_cap,
                                 const std::array<int, 4>& caps, int denominator,
                                 int64_t extra_charge) {
    auto& f = state_.formants[index];
    const int delta = divide(int64_t(fixed_cap) * (f.history - f.fixed_plate) +
                             charge(mask, f.history, f.plates, caps) + extra_charge, denominator);
    f.output = saturate(int64_t(f.output) - delta);
    f.fixed_plate = f.history;
    set_plates(f.plates, mask, f.history);
}

void PrototypeTract::process(const FilterEvent& event) {
    FilterEvent e = event;
    for (int* code : {&e.codes.f1, &e.codes.f2, &e.codes.f2q, &e.codes.f3, &e.codes.f4,
                      &e.codes.filter_amp, &e.codes.voice_amp, &e.codes.fric_amp}) {
        if (*code < 0 || *code > 15) throw std::invalid_argument("prototype capacitor code must be 0..15");
    }
    if (e.voice_drive < -8388608 || e.voice_drive > 8388607 ||
        e.fric_drive < -131072 || e.fric_drive > 131071)
        throw std::invalid_argument("prototype source voltage exceeds RTL input width");
    const bool changed = e.phase_edge || e.phase != applied_.phase ||
        !same_codes(e.codes, applied_.codes) || e.voice_drive != applied_.voice_drive ||
        e.fric_drive != applied_.fric_drive || e.fric1 != applied_.fric1 || e.fric2 != applied_.fric2;
    if (!changed) {
        if (e.output_open) state_.reconstruction = state_.output;
        applied_.output_open = e.output_open;
        return;
    }
    ++metrics_.events;
    if (e.phase_edge) ++metrics_.phase_edges;
    const bool phi0 = e.phase_edge && !e.phase;
    const bool phi1 = e.phase_edge && e.phase;
    const int old_f2q = applied_.codes.f2q;
    const int old_fric = applied_.fric_drive;
    applied_ = e;

    auto& f1 = state_.formants[0]; auto& f2 = state_.formants[1];
    auto& f3 = state_.formants[2]; auto& f4 = state_.formants[3];
    auto& f5 = state_.formants[4];
    // Continuously closed precharge/reset paths use the final mask. Open
    // capacitor plates retain their own voltage across code changes.
    if (!e.phase) {
        state_.voice = 0;
        set_plates(state_.voice_plates, e.codes.voice_amp, 0);
        f1.fixed_plate = f3.fixed_plate = f5.fixed_plate = 0;
        set_plates(f1.plates, e.codes.f1, 0);
        set_plates(f3.plates, e.codes.f3, 0);
        set_plates(state_.filter_plates, e.codes.filter_amp, f5.output);
    } else {
        state_.fric1 = 0;
        set_plates(state_.fric1_plates, e.codes.fric_amp, 0);
        f2.fixed_plate = f4.fixed_plate = 0;
        set_plates(state_.f2q_plates, e.codes.f2q, 0);
        set_plates(f2.plates, e.codes.f2, 0);
        set_plates(f4.plates, e.codes.f4, 0);
        if (e.fric1) state_.c143_plate = 0;
    }

    // U152/U154, including source edges while either phase stays open.
    if (phi0) {
        const int delta = divide(int64_t(3600) * state_.fric2_shape, 3900);
        state_.fric2_source = saturate(int64_t(state_.fric2_source) + delta);
        state_.fric2_shape = saturate(int64_t(state_.fric2_shape) + divide(int64_t(-5700) * delta, 3900));
    } else if (phi1) {
        state_.fric2_shape = saturate(int64_t(state_.fric2_shape) + divide(int64_t(-3600) * state_.fric2_source, 3900));
    }
    const int edge_delta = divide(-int64_t(cap_sum(e.codes.fric_amp, fric2_caps)) * (e.fric_drive - old_fric), 3900);
    state_.fric2_source = saturate(int64_t(state_.fric2_source) + edge_delta);
    state_.fric2_shape = saturate(int64_t(state_.fric2_shape) +
                                 divide(int64_t(e.phase ? -9300 : -5700) * edge_delta, 3900));
    state_.c150_delta = e.phase ? edge_delta : 0;
    state_.c151_delta = e.phase && e.fric2 ? state_.fric2_source - state_.c151_plate : 0;
    if (e.fric2) state_.c151_plate = state_.fric2_source;

    const int old_voice = state_.voice;
    if (e.phase) {
        state_.voice = saturate(int64_t(state_.voice) -
            divide(charge(e.codes.voice_amp, e.voice_drive, state_.voice_plates, voice_caps), 3300));
        set_plates(state_.voice_plates, e.codes.voice_amp, e.voice_drive);
        const int value = phi1 ? divide(int64_t(11500) * f1.history +
            int64_t(2700) * (f1.output - state_.voice) - int64_t(2700) * state_.voice, 11700) :
            divide(int64_t(-5400) * (state_.voice - old_voice), 11700);
        f1.history = phi1 ? value : saturate(int64_t(f1.history) + value);
    } else {
        state_.fric1 = saturate(int64_t(state_.fric1) -
            divide(charge(e.codes.fric_amp, e.fric_drive, state_.fric1_plates, fric1_caps), 3900));
        set_plates(state_.fric1_plates, e.codes.fric_amp, e.fric_drive);
        const int new_mask = (~old_f2q) & e.codes.f2q;
        if (phi0) {
            f2.history = divide(int64_t(6800) * f2.history + int64_t(4700) * (f2.output - f1.output) +
                weighted(e.codes.f2q, state_.f2q_plates, voice_caps), 7000 + cap_sum(e.codes.f2q, voice_caps));
        } else if (new_mask) {
            const int keep = old_f2q & e.codes.f2q;
            const int denominator = 7000 + cap_sum(keep, voice_caps);
            f2.history = divide(int64_t(denominator) * f2.history +
                weighted(new_mask, state_.f2q_plates, voice_caps), denominator + cap_sum(new_mask, voice_caps));
        }
        if (phi0 || new_mask) set_plates(state_.f2q_plates, e.codes.f2q, f2.history);
    }

    const int old_f1 = f1.output;
    if (e.phase) {
        move_formant(0, e.codes.f1, 250, f1_caps, 11500);
        const int value = phi1 ? divide(int64_t(4700) * f3.history +
            int64_t(3900) * (f3.output - f2.output) + int64_t(2000) * (old_f1 - f1.output), 4900) :
            divide(int64_t(-2000) * (f1.output - old_f1), 4900);
        f3.history = phi1 ? value : saturate(int64_t(f3.history) + value);
        move_formant(2, e.codes.f3, 820, f3_caps, 4700);
        const int64_t side_charge = int64_t(-1150) * state_.c150_delta - int64_t(3700) * state_.c151_delta;
        const int last_history = phi1 ? divide(int64_t(3450) * f5.history +
            int64_t(4700) * (f5.output - f4.output) + side_charge, 3730) : divide(side_charge, 3730);
        f5.history = phi1 ? last_history : saturate(int64_t(f5.history) + last_history);
        move_formant(4, 0, 4700, zero_caps, 3450);
        const int64_t output_charge = charge(e.codes.filter_amp, f5.output, state_.filter_plates, amp_caps);
        if (phi1) state_.output = divide(int64_t(2700) * state_.output - output_charge, 2750);
        else state_.output = saturate(int64_t(state_.output) - divide(output_charge, 2750));
        set_plates(state_.filter_plates, e.codes.filter_amp, f5.output);
    } else {
        const int c143_delta = e.fric1 ? state_.c143_plate - state_.fric1 : 0;
        move_formant(1, e.codes.f2, 500, f2_caps, 6800, int64_t(-1000) * c143_delta);
        if (e.fric1) state_.c143_plate = state_.fric1;
        if (phi0) f4.history = divide(int64_t(4300) * f4.history + int64_t(4700) * (f4.output - f3.output), 4500);
        move_formant(3, e.codes.f4, 1670, f4_caps, 4300);
    }
    metrics_.peak_output_q16 = std::max(metrics_.peak_output_q16, std::abs(state_.output));
    if (e.output_open) state_.reconstruction = state_.output;
}

int16_t PrototypeTract::sample() {
    ++metrics_.samples;
    // Signed division here must reproduce arithmetic >>1, including -1 -> -1.
    const int64_t scaled = int64_t(state_.reconstruction) * output_gain_;
    const int64_t output = scaled >= 0 ? scaled / 2 : -((-scaled + 1) / 2);
    if (output < -32768 || output > 32767) ++metrics_.output_clips;
    return static_cast<int16_t>(std::clamp<int64_t>(output, -32768, 32767));
}

} // namespace ssi_host
