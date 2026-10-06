#pragma once

// Sample-event port of the running SSI backend. Tables come from the checked-in
// HDL package, never from the old generator. This model intentionally retains
// its current SC-01-derived filters, waveform, interpolation and sound shaping.
// It does not simulate fabric-cycle pipeline latency, bus synchronizers or IRQs.
#include "shared.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cmath>

namespace ssi_host {

class Baseline {
public:
    explicit Baseline(const Tables& tables, int effective_hz = 1015625)
        : t_(tables), effective_hz_(effective_hz) {}

    void write(int reg, int byte) {
        byte &= 255;
        switch (reg) {
        case 0:
            dur_ = byte;
            if (!(control_ & 128)) start_phone();
            break;
        case 1: inflect_ = byte; break;
        case 2: rate_ = byte; break;
        case 3: {
            const bool restart = (control_ & 128) && !(byte & 128);
            control_ = byte;
            if (restart) {
                if (dur_ >> 6) function_ = dur_ >> 6;
                start_phone();
            }
            break;
        }
        default: ff_ = byte; break;
        }
    }

    // Optional adapter for a native parameter scanner. The source waveform,
    // closure traits and filter arithmetic remain the current running engine.
    // Readiness is a source gate, not a claim about native envelope circuitry.
    void override_parameters(const std::array<int, 8>& codes,
                             bool voice_ready, bool fric_ready) {
        override_ = true;
        f1_ = t_.native_f1[codes[0] & 15];
        f2_ = t_.native_f2[codes[1] & 15] * 2;
        f2q_ = t_.native_f2q[codes[2] & 15];
        f3_ = t_.native_f3[codes[3] & 15];
        va_ = voice_ready ? t_.native_va[codes[5] & 15] : 0;
        fa_ = fric_ready ? t_.native_fa[codes[6] & 15] : 0;
        fc_ = 15;
        override_amp_ = codes[4] & 15;
    }

    // Used only by explicit A/B experiments. Default false preserves the
    // current backend's pitch approximation at its 20 kHz control cadence.
    void use_exact_pitch(bool enabled) { exact_pitch_ = enabled; }
    void set_native_transition_mode(bool enabled) { native_transition_mode_ = enabled; }
    void source_override(int voice, int noise, bool bypass_closure = false) {
        source_override_ = true;
        external_voice_ = sat16(voice);
        external_noise_ = sat16(noise);
        bypass_closure_ = bypass_closure;
    }

    int16_t sample() {
        int output = render_audio();
        advance_time();
        return static_cast<int16_t>(output);
    }

    int duration_phase() const { return ticks_; }
    int pitch_phase() const { return pitch_; }
    int active_inflection() const { return active_inflection_; }
    int current_function() const { return function_; }
    int register_value(int reg) const {
        switch (reg) {
        case 0: return dur_;
        case 1: return inflect_;
        case 2: return rate_;
        case 3: return control_;
        default: return ff_;
        }
    }
    bool running() const { return active_ && !(control_ & 128); }
    const std::array<int, 7>& parameter_state() const { return cur_; }

private:
    static int sat24(int64_t x) {
        return static_cast<int>(std::clamp<int64_t>(x, -8388608, 8388607));
    }
    static int sat16(int x) { return std::clamp(x, -32768, 32767); }
    static int scale4(int x, int gain) {
        if (gain == 15) return x;
        int y = 0;
        for (int bit = 0; bit != 4; ++bit)
            if (gain & (1 << bit)) y += x >> (4 - bit);
        return y;
    }
    static int scale7(int x, int gain) {
        switch (gain & 7) {
        case 0: return 0;
        case 1: return (x >> 3) + (x >> 6) + (x >> 9);
        case 2: return (x >> 2) + (x >> 5) + (x >> 8);
        case 3: return (x >> 2) + (x >> 3) + (x >> 5) + (x >> 6);
        case 4: return (x >> 1) + (x >> 4);
        case 5: return (x >> 1) + (x >> 3) + (x >> 4) + (x >> 6);
        case 6: return x - (x >> 3) - (x >> 6);
        default: return x;
        }
    }
    static int scale20(int x, int gain) {
        switch (gain) {
        case 5: return x >> 2;
        case 6: return (x >> 2) + (x >> 4) - (x >> 6);
        case 7: return (x >> 2) + (x >> 3) - (x >> 5);
        case 8: return (x >> 1) - (x >> 3) + (x >> 5);
        case 9: return (x >> 1) - (x >> 4);
        case 10: return x >> 1;
        case 11: return (x >> 1) + (x >> 4);
        case 12: return (x >> 1) + (x >> 3) - (x >> 5);
        case 13: return (x >> 1) + (x >> 3) + (x >> 5);
        case 14: return (x >> 1) + (x >> 2) - (x >> 4);
        case 15: return (x >> 1) + (x >> 2);
        case 16: return x - (x >> 3) - (x >> 4);
        case 17: return x - (x >> 3) - (x >> 5);
        case 18: return x - (x >> 3) + (x >> 5);
        case 19: return x - (x >> 4);
        default: return x;
        }
    }
    struct Filter {
        std::array<int, 3> x{}, y{};
        template <typename Coefficients>
        int apply(int input, const Coefficients& c, int order = 3) {
            int64_t sum = int64_t(input) * c[0];
            for (int i = 0; i != order; ++i) {
                sum += int64_t(x[i]) * c[1 + i];
                sum += int64_t(y[i]) * c[1 + order + i];
            }
            int output = sat24(sum >> 15);
            for (int i = order - 1; i > 0; --i) {
                x[i] = x[i - 1]; y[i] = y[i - 1];
            }
            x[0] = input; y[0] = output;
            return output;
        }
    } f1_filter_, f2_filter_, fn_filter_, f2n_filter_, f3_filter_, f4_filter_;

    // The native transition adapter exposes long filter tails that the old
    // closure envelope hid. Keep their fractions until PCM conversion: Q15
    // feedback truncation otherwise sustains audible zero-input limit cycles.
    // This is host arithmetic, not a claim about the SC-02 analog circuit.
    struct PreciseFilter {
        std::array<double, 3> x{}, y{};
        template <typename Coefficients>
        double apply(double input, const Coefficients& c, int order = 3) {
            double sum = input * c[0];
            for (int i = 0; i != order; ++i)
                sum += x[i] * c[1 + i] + y[i] * c[1 + order + i];
            const double output = std::clamp(sum / 32768.0, -8388608.0, 8388607.0);
            for (int i = order - 1; i > 0; --i) { x[i] = x[i-1]; y[i] = y[i-1]; }
            x[0] = input; y[0] = output;
            return output;
        }
    } precise_f1_, precise_f2_, precise_fn_, precise_f2n_, precise_f3_, precise_f4_;
    double precise_closed_ = 0, precise_fx_ = 0, precise_presence_ = 0, precise_audio_ = 0;

    int live_inflection() const {
        return ((rate_ & 8) << 8) | (inflect_ << 3) | (rate_ & 7);
    }
    int duration_slot() const { return (4 - (dur_ >> 6)) * 256 * (16 - (rate_ >> 4)); }
    static int pitch_limit(int inflection) { return std::max(1, ((4096 - inflection) * 5) >> 5); }

    void start_phone() {
        active_ = true;
        const int phone = dur_ & 63;
        legacy_phone_ = t_.sc01_map[phone];
        p_ = t_.phones[legacy_phone_];
        auto target = [&](int selector) { return (t_.native_rom[phone * 8 + selector] >> 4) & 15; };
        p_.f1 = t_.native_f1[target(0)];
        p_.f2 = t_.native_f2[target(1)];
        p_.f2q = t_.native_f2q[target(2)];
        p_.f3 = t_.native_f3[target(3)];
        p_.va = t_.native_va[target(5)];
        p_.fa = t_.native_fa[target(6)];
        p_.fc = 15;
        if (legacy_phone_ == 63) p_.cld = p_.vd = p_.closure = 0;
        p_.pause = !p_.va && !p_.fa;
        ticks_ = 0;
        duration_left_ = duration_slot();
        int next = live_inflection();
        if (function_ == 3) {
            if (inflection_seeded_) next = (next & ~0x7C0) | (active_inflection_ & 0x7C0);
            inflection_seeded_ = true;
        }
        active_inflection_ = next;
        pitch_limit_ = pitch_limit(next);
        if (!p_.cld) closure_active_ = p_.closure != 0;
        // Equivalent to the backend's per-tap valid masks. No old taps are
        // exposed until overwritten, and its pipeline values start at zero.
        f1_filter_ = {}; f2_filter_ = {}; fn_filter_ = {}; f2n_filter_ = {};
        f3_filter_ = {}; f4_filter_ = {};
        fx_ = closed_ = presence_low_ = filter_phase_ = 0;
        precise_f1_ = {}; precise_f2_ = {}; precise_fn_ = {}; precise_f2n_ = {};
        precise_f3_ = {}; precise_f4_ = {};
        precise_closed_ = precise_fx_ = precise_presence_ = 0;
    }

    int interpolate(int value, int target) const {
        static constexpr int shifts[8] = {5, 5, 4, 4, 3, 3, 2, 1};
        const int delta = target * 16 - value;
        if (!delta) return value;
        const int step = std::max(1, std::abs(delta) >> shifts[(control_ >> 4) & 7]);
        return std::clamp(value + (delta > 0 ? step : -step), 0, 255);
    }
    void advance_control() {
        update_counter_ = (update_counter_ + 1) % 48;
        if (!override_) {
            if (update_counter_ == 40 && (!p_.pause || (!fa_ && !va_))) {
                cur_[0] = interpolate(cur_[0], p_.f1);
                cur_[1] = interpolate(cur_[1], p_.f2);
                cur_[2] = interpolate(cur_[2], p_.f2q);
                cur_[3] = interpolate(cur_[3], p_.f3);
                cur_[4] = interpolate(cur_[4], p_.fc);
            }
            if (!(update_counter_ & 15)) {
                if ((ticks_ & 15) >= p_.vd) cur_[6] = interpolate(cur_[6], p_.fa);
                if ((ticks_ & 15) >= p_.cld) cur_[5] = interpolate(cur_[5], p_.va);
            }
        }
        if (!closure_active_ && (fa_ || va_)) closure_age_ = 0;
        else if (closure_age_ != 28) ++closure_age_;
    }
    void commit_filters() {
        if (override_) return;
        f1_ = cur_[0] >> 4; f2_ = cur_[1] >> 3; f2q_ = cur_[2] >> 4;
        f3_ = cur_[3] >> 4; fc_ = cur_[4] >> 4;
        va_ = cur_[5] >> 4; fa_ = cur_[6] >> 4;
    }
    void advance_inflection() {
        int next = live_inflection();
        if (function_ == 3) {
            static constexpr int steps[8] = {1, 2, 3, 4, 6, 8, 12, 16};
            const int active = (active_inflection_ >> 6) & 31;
            const int target = (next >> 6) & 31;
            const int step = steps[(next >> 3) & 7];
            const int transitioned = active + std::clamp(target - active, -step, step);
            next = (next & ~0x7C0) | (transitioned << 6);
        }
        active_inflection_ = next;
    }
    void advance_time() {
        xck_accum_ += effective_hz_;
        const int edges = int(xck_accum_ / 48000);
        xck_accum_ %= 48000;
        if (active_) {
            duration_left_ -= edges;
            while (duration_left_ <= 0) {
                duration_left_ += duration_slot();
                ticks_ = (ticks_ + 1) & 15;
                if (ticks_ == p_.cld && ticks_ != 0) closure_active_ = p_.closure != 0;
            }
        }
        if (exact_pitch_) {
            // The adapter preserves the nine-entry waveform's 400 us entries.
            // It only replaces the period; high pitches truncate the pulse.
            exact_pitch_edges_ += edges;
            const int period = 8 * (4096 - active_inflection_);
            while (exact_pitch_edges_ >= period) exact_pitch_edges_ -= period;
            pitch_ = int((int64_t(exact_pitch_edges_) * 20000) / effective_hz_);
            pitch_gate_ = exact_pitch_edges_ >= period / 2;
        }
        control_accum_ += 20000;
        if (control_accum_ < 48000) return;
        control_accum_ -= 48000;
        pitch_limit_ = pitch_limit(active_inflection_);
        advance_inflection();
        advance_control();
        if (!exact_pitch_) {
            int next = pitch_ + 1;
            if (next >= pitch_limit_) next -= pitch_limit_;
            pitch_ = next & 1023;
            pitch_gate_ = pitch_ >= (pitch_limit_ >> 1);
        }
        if ((pitch_ & 0x3F9) == 8) commit_filters();
        noise_state_ = ((noise_state_ << 1) & 0x7FFE) | (noise_bit_ && noise_state_ != 0x7FFF);
        noise_bit_ = !(((noise_state_ >> 14) ^ (noise_state_ >> 13)) & 1);
    }
    bool ch_fricative() const { return !native_transition_mode_ && legacy_phone_ == 0x10 && fa_ && !va_ && !p_.closure; }
    int closure_gain() const {
        if (native_transition_mode_ || bypass_closure_) return 7;
        if (p_.closure && !p_.fa && p_.va) {
            static constexpr int gain[6] = {7, 7, 6, 4, 2, 1};
            const int age = ticks_ - p_.cld;
            return age >= 0 && age < 6 ? gain[age] : 0;
        }
        if (fa_ && !va_) {
            if (!p_.closure) return 7;
            static constexpr int gain[5] = {7, 7, 5, 3, 2};
            const int age = ticks_ - p_.vd - 1;
            return age >= 0 && age < 5 ? gain[age] : 0;
        }
        return 7 ^ (closure_age_ >> 2);
    }
    int attack_level() const {
        if (ch_fricative() || (!p_.fa && !p_.va)) return 0;
        const int start = p_.closure && p_.cld <= p_.vd ? p_.cld : p_.fa ? p_.vd : p_.cld;
        const int age = ticks_ - start;
        return age >= 0 && age < 3 ? 3 - age : 0;
    }
    // Preserve the current waveform, coefficients and shaping, with fractional
    // state throughout. VA/FA zero stops excitation but does not cut filter tails.
    int render_precise_audio() {
        static constexpr int glottal[9] = {0, -4681, 8192, 7022, 5851, 4681, 3511, 2340, 1170};
        auto gain = [](double x, int a) { return a == 15 ? x : x * a / 16.0; };
        auto bounded = [](double x) { return std::clamp(x, -8388608.0, 8388607.0); };
        static constexpr int scale20_numerator[15] = {
            16, 19, 22, 26, 28, 32, 36, 38, 42, 44, 48, 52, 54, 58, 60};
        const int noise_gain = 20 - fc_;
        const double noise_mix = noise_gain >= 5 && noise_gain <= 19 ?
            scale20_numerator[noise_gain - 5] / 64.0 : 1.0;
        const bool muted = !active_ || (control_ & 128) || !(control_ & 15);
        const double raw_voice = source_override_ ? external_voice_ : pitch_ < 72 ? glottal[pitch_ >> 3] : 0;
        const double raw_noise = source_override_ ? external_noise_ : pitch_gate_ && noise_bit_ ? 8192 : -8192;
        const double voice = muted ? 0 : gain(raw_voice, va_);
        const double noise = muted ? 0 : bounded(gain(raw_noise, fa_) * 64);
        int passes = 1;
        if (ff_ == 128) filter_phase_ = 0;
        else { filter_phase_ += 128 + ff_; passes = filter_phase_ >> 8; filter_phase_ &= 255; }
        for (int i = 0; i < passes; ++i) {
            const double v1 = precise_f1_.apply(voice, t_.coeff_f1[f1_]);
            const double v2 = precise_f2_.apply(v1, t_.coeff_f2[f2_][f2q_]);
            const double n = precise_fn_.apply(noise, t_.coeff_fn, 2);
            const double n2 = precise_f2n_.apply(bounded(gain(n, fc_) * 8), t_.coeff_f2[f2_][f2q_]);
            const double v3 = precise_f3_.apply(bounded(v2 + n2), t_.coeff_f3[f3_]);
            precise_closed_ = precise_f4_.apply(bounded(v3 + n * noise_mix), t_.coeff_f4);
            precise_fx_ = bounded((precise_closed_ * t_.coeff_fx[0] + precise_fx_ * t_.coeff_fx[1]) / 32768.0);
        }
        const double high = precise_closed_ - precise_fx_;
        double shaped = precise_fx_;
        if (va_ && !fa_) shaped = bounded(precise_fx_ + high * .75);
        else if (fa_) shaped = bounded(precise_fx_ + high * 1.25);
        const int amplitude = muted ? 0 : override_ ? override_amp_ : control_ & 15;
        const double scaled = gain(shaped, amplitude);
        const double delta = scaled - precise_presence_;
        const double presence = bounded(scaled + delta / 2.0);
        precise_presence_ = bounded(precise_presence_ + delta / 8.0);
        double output = bounded(gain(presence, 10) * 2);
        if (output > 8192) output = 8192 + (output - 8192) / 4.0;
        else if (output < -8192) output = -8192 + (output + 8192) / 4.0;
        output = std::clamp(output, -32768.0, 32767.0);
        precise_audio_ += std::clamp(output - precise_audio_, -3000.0, 3000.0);
        return static_cast<int>(std::lround(precise_audio_));
    }

    int render_audio() {
        if (native_transition_mode_) return render_precise_audio();
        static constexpr int glottal[9] = {0, -4681, 8192, 7022, 5851, 4681, 3511, 2340, 1170};
        const bool muted = !active_ || (control_ & 128) || !(control_ & 15);
        const int raw_voice = source_override_ ? external_voice_ : pitch_ < 72 ? glottal[pitch_ >> 3] : 0;
        const int raw_noise = source_override_ ? external_noise_ : pitch_gate_ && noise_bit_ ? 8192 : -8192;
        const int voice = muted ? 0 : scale4(raw_voice, va_);
        const int noise = muted ? 0 : sat24(int64_t(scale4(raw_noise, fa_)) * 64);
        int passes = 1;
        if (ff_ == 128) filter_phase_ = 0;
        else {
            filter_phase_ += 128 + ff_;
            passes = filter_phase_ >> 8;
            filter_phase_ &= 255;
        }
        for (int i = 0; i < passes; ++i) {
            const int v1 = f1_filter_.apply(voice, t_.coeff_f1[f1_]);
            const int v2 = f2_filter_.apply(v1, t_.coeff_f2[f2_][f2q_]);
            const int n = fn_filter_.apply(noise, t_.coeff_fn, 2);
            const int n2 = f2n_filter_.apply(sat24(int64_t(scale4(n, fc_)) * 8), t_.coeff_f2[f2_][f2q_]);
            const int v3 = f3_filter_.apply(sat24(int64_t(v2) + n2), t_.coeff_f3[f3_]);
            const int v4 = f4_filter_.apply(sat24(int64_t(v3) + scale20(n, 20 - fc_)), t_.coeff_f4);
            closed_ = scale7(v4, active_ ? closure_gain() : 0);
            fx_ = sat24((int64_t(closed_) * t_.coeff_fx[0] + int64_t(fx_) * t_.coeff_fx[1]) >> 15);
        }
        const int high = closed_ - fx_;
        int shaped = fx_;
        if (ch_fricative() || (va_ && !fa_)) shaped = sat24(int64_t(fx_) + (high >> 1) + (high >> 2));
        else if (fa_ && (native_transition_mode_ || !p_.closure)) shaped = sat24(int64_t(fx_) + high + (high >> 2));
        if (!native_transition_mode_ && (fa_ || p_.closure)) {
            const int level = attack_level();
            if (level) shaped = sat24(int64_t(shaped) + (shaped >> (4 - level)));
        }
        const int amplitude = muted ? 0 : override_ ? override_amp_ : control_ & 15;
        const int scaled = scale4(shaped, amplitude);
        const int delta = scaled - presence_low_;
        const int presence = sat24(int64_t(scaled) + (delta >> 1));
        presence_low_ = sat24(int64_t(presence_low_) + (delta >> 3));
        int output = sat24(int64_t(scale4(presence, 10)) * 2);
        if (output > 8192) output = 8192 + ((output - 8192) >> 2);
        else if (output < -8192) output = -8192 + ((output + 8192) >> 2);
        output = sat16(output);
        audio_ += std::clamp(output - audio_, -3000, 3000);
        return audio_;
    }

    const Tables& t_;
    int effective_hz_;
    int dur_ = 0xC0, inflect_ = 0, rate_ = 0, control_ = 0x80, ff_ = 0, function_ = 0;
    bool active_ = false, override_ = false, exact_pitch_ = false, inflection_seeded_ = false;
    bool native_transition_mode_ = false, source_override_ = false, bypass_closure_ = false;
    bool closure_active_ = true, pitch_gate_ = false, noise_bit_ = false;
    Phone p_{7, 0, 9, 0, 4, 12, 0, 1, 1, 1, 15, 0};
    std::array<int, 7> cur_{};
    int f1_ = 0, f2_ = 0, f2q_ = 0, f3_ = 0, fc_ = 0, va_ = 0, fa_ = 0;
    int override_amp_ = 0, legacy_phone_ = 63, ticks_ = 0, duration_left_ = 1;
    int active_inflection_ = 0, pitch_limit_ = 255, pitch_ = 0, closure_age_ = 0;
    int update_counter_ = 0, control_accum_ = 0, noise_state_ = 0;
    int64_t xck_accum_ = 0;
    int exact_pitch_edges_ = 0;
    int external_voice_ = 0, external_noise_ = 0;
    int filter_phase_ = 0, closed_ = 0, fx_ = 0, presence_low_ = 0, audio_ = 0;
};

} // namespace ssi_host
