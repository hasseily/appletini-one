#include "native_control.h"

#include <stdexcept>

namespace ssi_host {
namespace {
int bit_and(int a, int b) {
    return a == 0 || b == 0 ? 0 : a < 0 || b < 0 ? -1 : 1;
}
int bit_or(int a, int b) {
    return a == 1 || b == 1 ? 1 : a < 0 || b < 0 ? -1 : 0;
}
int held_load(int old, int enable, int value) {
    return enable == 1 ? value : enable == 0 || old == value ? old : -1;
}
} // namespace

void NativeDda::retarget(int value) {
    if (value < 0 || value > 15) throw std::invalid_argument("DDA target must be 0..15");
    target = value;
    b = (target - a) & 15;
    c = 8;
    upward = target >= a;
}

bool NativeDda::step() {
    if (a == target) return false;
    const int sum = b + c;
    c = sum & 15;
    const bool carry = sum >= 16;
    if (upward && carry) ++a;
    if (!upward && !carry) --a;
    return true;
}

NativeControl::NativeControl(const Tables& tables, int effective_hz, NativeTiming timing)
    : tables_(tables), effective_hz_(effective_hz), timing_(timing) {
    if (effective_hz <= 0) throw std::invalid_argument("Effective XCK must be positive");
    if (timing.articulation_reference_rate < 0 || timing.articulation_reference_rate > 15)
        throw std::invalid_argument("Articulation reference RATE must be 0..15");
    restart_duration();
}

const NativeDda& NativeControl::parameter_state(int selector) const {
    return dda_.at(static_cast<std::size_t>(selector));
}

int NativeControl::articulation_period_ticks() const {
    return 256 * (16 - timing_.articulation_reference_rate) * (8 - ((regs_[3] >> 4) & 7));
}

int NativeControl::duration_period_ticks() const {
    return 256 * (16 - (regs_[2] >> 4)) * (4 - (regs_[0] >> 6));
}

void NativeControl::set_articulation_reference_rate(int value) {
    if (value < 0 || value > 15)
        throw std::invalid_argument("Articulation reference RATE must be 0..15");
    timing_.articulation_reference_rate = value;
    articulation_left_ = articulation_period_ticks();
}

int NativeControl::amplitude_period_ticks() const {
    // Prototype selected RATECLK vs RATECLK/2. This timing affects source
    // amplitude, not articulation. It remains an explicit audio experiment.
    return 128 * (16 - (regs_[2] >> 4)) * ((regs_[0] & 0x80) ? 1 : 2);
}

void NativeControl::restart_duration() {
    duration_phase_ = 0;
    duration_left_ = duration_period_ticks();
    articulation_left_ = articulation_period_ticks();
    amplitude_left_ = amplitude_period_ticks();
    duration_pending_ = duration_window_ = false;
    articulation_pending_ = articulation_window_ = false;
    amplitude_pending_ = amplitude_window_ = false;
}

void NativeControl::write(int reg, int value) {
    if (reg < 0 || reg > 7 || value < 0 || value > 255)
        throw std::invalid_argument("SSI write needs register 0..7 and byte 0..255");
    reg = reg >= 4 ? 4 : reg;
    const int old = regs_[reg];
    regs_[reg] = value;
    if (reg == 0) {
        phone_valid_ = true;
        pw0_ = pw1_ = 0;
        phone_setup_pending_ = true;
        // Compatibility boundary: retain today's full first duration interval,
        // rather than importing the prototype's free-running CTL counters.
        // Parameter and scanner state persist across phone writes.
        duration_phase_ = 0;
        duration_left_ = duration_period_ticks();
        duration_pending_ = duration_window_ = false;
    } else if (reg == 3) {
        control_setup_pending_ = true;
        active_ = (value & 0x80) == 0;
        if ((old & 0x80) && active_) restart_duration();
    }
}

int NativeControl::target(int selector) const {
    if (selector == 4) return regs_[3] & 15;
    if ((selector == 5 || selector == 6) && (regs_[3] & 15) == 0) return 0;
    return (tables_.native_rom[8 * phone() + selector] >> 4) & 15;
}

bool NativeControl::transition_permit(int selector) const {
    switch (selector) {
    case 0:
    case 1:
    case 3: {
        const int blocked = bit_and(pw5_, ((phone() & 32) || codes_[5] || codes_[6]) ? 1 : 0);
        return articulation_window_ && blocked == 0;
    }
    case 2: return articulation_window_;
    case 4:
        // AMP=0 silences VA/FA while preserving the stored filter amplitude.
        // The remaining duration permit still approximates prototype U166B.
        return duration_window_ && (regs_[3] & 15) != 0;
    case 5: return amplitude_window_ && pw0_ == 1;
    case 6: return amplitude_window_ && pw1_ == 1;
    default: return false;
    }
}

void NativeControl::parameter_write() {
    ++metrics_.write_phases;
    if (!phone_valid_ || selector_ == 7) return;
    const bool setup = (phone_setup_window_ && selector_ != 4) ||
                       (control_setup_window_ && selector_ >= 4 && selector_ <= 6 &&
                        (selector_ != 4 || (regs_[3] & 15) != 0));
    if (setup) {
        dda_[selector_].retarget(target(selector_));
        ++metrics_.setups;
    } else if (!phone_setup_pending_ && active_ && transition_permit(selector_)) {
        const int before = dda_[selector_].a;
        if (dda_[selector_].step()) ++metrics_.transition_steps;
        if (dda_[selector_].a != before) ++metrics_.parameter_changes;
    }
}

void NativeControl::parameter_latch(bool rising) {
    if (rising) ++metrics_.latch_phases;
    if (!phone_valid_) return;
    if (selector_ < 7) codes_[selector_] = dda_[selector_].a;
    const int flags = tables_.native_rom[8 * phone() + selector_] & 15;
    if (selector_ < 2 && duration_phase_ == ((flags & 1) ? 2 : 6)) {
        (selector_ == 0 ? pw0_ : pw1_) = 1;
    } else if (selector_ == 2) {
        // U34/PW3 and the first parameter latches remain transparent through
        // r10 and r11. U20 and PW2/PW5 below are edge-qualified separately.
        pw3_ = held_load(pw3_, pw1_, ((regs_[3] & 0x80) || !(flags & 2)) ? 1 : 0);
        if (!rising) return;
        // Keep the old PW2 term: the transparent slot can also open its gate
        // after new TPARM2 rises. Do not substitute a baseline pulse counter
        // for the still-unknown native U68 amplitude counter.
        const int tparm2 = (flags >> 2) & 1;
        const int settled = bit_or(bit_and(bit_and(pw0_, pw1_), ampct_zero_), codes_[6] == 0 ? 1 : 0);
        const int route_enable = bit_and(bit_and(pw1_, bit_or(pw2_, tparm2)), settled);
        u20_ = held_load(u20_, route_enable, (flags >> 3) & 1);
        pw2_ = tparm2;
        pw5_ = 1 - tparm2;
    }
}

void NativeControl::set_ampct_zero(int value) {
    if (value < -1 || value > 1) throw std::invalid_argument("AMPCT_ZERO must be -1, 0 or 1");
    ampct_zero_ = value;
}

void NativeControl::tick() {
    ++metrics_.xck_ticks;
    filter_phase_edge_ = false;
    const int previous_u20 = u20_;
    if (active_ && phone_valid_) {
        if (--duration_left_ == 0) {
            duration_left_ = duration_period_ticks();
            duration_phase_ = (duration_phase_ + 1) & 15;
            duration_pending_ = true;
            ++metrics_.duration_edges;
        }
        if (--articulation_left_ == 0) {
            articulation_left_ = articulation_period_ticks();
            articulation_pending_ = true;
            ++metrics_.articulation_edges;
        }
        if (--amplitude_left_ == 0) {
            amplitude_left_ = amplitude_period_ticks();
            amplitude_pending_ = true;
            ++metrics_.amplitude_edges;
        }
    }

    // Event r names the settled ripple-counter state after this XCK edge.
    // WRITE rises at r=2; LATCH rises at r=10; the selector changes at r=0.
    scan_phase_ = (scan_phase_ + 1) & 15;
    if (scan_phase_ == 0) {
        const int old_selector = selector_;
        selector_ = (selector_ + 1) & 7;
        if (selector_ == 0) ++metrics_.scans;
        if (old_selector == 3) {
            phone_setup_window_ = phone_setup_pending_;
            control_setup_window_ = control_setup_pending_;
            articulation_window_ = articulation_pending_;
            amplitude_window_ = amplitude_pending_;
            duration_window_ = duration_pending_;
            phone_setup_pending_ = control_setup_pending_ = false;
            articulation_pending_ = amplitude_pending_ = duration_pending_ = false;
        }
    }
    if (scan_phase_ == 2) parameter_write();
    if (scan_phase_ == 10 || scan_phase_ == 11)
        parameter_latch(scan_phase_ == 10);

    // Only route latches use this exact divider here. The audio bridge still
    // uses its existing digital filters, so these are observable control
    // states rather than a claim of a native switched-capacitor simulation.
    if (--filter_left_ == 0) {
        filter_left_ = 256 - regs_[4];
        filter_phase_ = !filter_phase_;
        filter_phase_edge_ = true;
        // A simultaneous selector-2 latch and positive Phi0 captures old
        // U20, matching the archived RTL's pre-edge register semantics.
        // This defines host replay ordering, not physical propagation delay.
        if (!filter_phase_) fric2_ = previous_u20 < 0 ? -1 : 1 - previous_u20;
    }
    if (filter_phase_) fric1_ = u20_;
}

void NativeControl::advance_xck(std::uint64_t ticks) {
    while (ticks--) tick();
}

void NativeControl::advance_sample() {
    sample_fraction_ += static_cast<std::uint64_t>(effective_hz_);
    const std::uint64_t ticks = sample_fraction_ / sample_rate;
    sample_fraction_ %= sample_rate;
    advance_xck(ticks);
}

} // namespace ssi_host
