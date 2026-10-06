#pragma once

#include "shared.h"

#include <array>
#include <cstdint>

namespace ssi_host {

// Sheet-4 transition RAM arithmetic. The clock that permits step() is a
// separate concern; this class does not inherit the prototype RATE coupling.
struct NativeDda {
    int a{}, b{}, c{8}, target{};
    bool upward{true};
    void retarget(int value);
    bool step();
};

struct NativeTiming {
    // Experiment: freeze the prototype articulation clock's RATE factor at
    // R=8, rather than driving it with the speech RATE register. At ART=5,
    // sixteen steps take 98304 XCK ticks (96.8 ms on the PAL Phasor).
    // The datasheet specifies RATE independence but gives no absolute time.
    int articulation_reference_rate{8};
};

struct NativeControlMetrics {
    std::uint64_t xck_ticks{}, scans{}, write_phases{}, latch_phases{};
    std::uint64_t setups{}, transition_steps{}, parameter_changes{};
    std::uint64_t duration_edges{}, articulation_edges{}, amplitude_edges{};
};

class NativeControl {
public:
    explicit NativeControl(const Tables& tables, int effective_hz = 1015625,
                           NativeTiming timing = {});
    void write(int reg, int value);
    void advance_xck(std::uint64_t ticks);
    void advance_sample();

    // Selector order: F1, F2, F2Q, shared F3/F4, host amplitude, VA, FA, unused.
    // These are the first parameter latches, before analog phase-latch masking.
    const std::array<int, 8>& parameter_codes() const { return codes_; }
    const NativeControlMetrics& metrics() const { return metrics_; }
    const NativeDda& parameter_state(int selector) const;
    int selector() const { return selector_; }
    int selector_phase() const { return scan_phase_; }
    int duration_phase() const { return duration_phase_; }
    int phone() const { return regs_[0] & 63; }
    int pw0() const { return pw0_; }
    int pw1() const { return pw1_; }
    int pw2() const { return pw2_; }
    int pw3() const { return pw3_; }
    int pw5() const { return pw5_; }
    int u20() const { return u20_; }
    int fric1_sw() const { return fric1_; }
    int fric2_sw() const { return fric2_; }
    bool active() const { return active_; }
    bool filter_phase() const { return filter_phase_; }
    bool filter_phase_edge() const { return filter_phase_edge_; }
    int filter_frequency() const { return regs_[4]; }
    bool latched_ctrl() const { return (regs_[3] & 0x80) != 0; }
    int articulation_period_ticks() const;
    int duration_period_ticks() const;
    void set_articulation_reference_rate(int value);

    // -1 means unknown. Never infer U68 from the retained waveform's counter.
    // A later native source implementation may supply this real state.
    void set_ampct_zero(int value);

private:
    void tick();
    void restart_duration();
    void parameter_write();
    void parameter_latch(bool rising);
    int target(int selector) const;
    int amplitude_period_ticks() const;
    bool transition_permit(int selector) const;

    const Tables& tables_;
    int effective_hz_;
    NativeTiming timing_;
    std::array<int, 5> regs_{{0xC0, 0, 0, 0x80, 0xFF}};
    std::array<NativeDda, 8> dda_{};
    std::array<int, 8> codes_{};
    NativeControlMetrics metrics_{};
    std::uint64_t sample_fraction_{};
    int selector_{}, scan_phase_{}, duration_phase_{};
    int duration_left_{}, articulation_left_{}, amplitude_left_{};
    bool active_{}, phone_valid_{};
    bool phone_setup_pending_{}, phone_setup_window_{};
    bool control_setup_pending_{}, control_setup_window_{};
    bool articulation_pending_{}, articulation_window_{};
    bool amplitude_pending_{}, amplitude_window_{};
    bool duration_pending_{}, duration_window_{};
    int pw0_{-1}, pw1_{-1}, pw2_{-1}, pw3_{-1}, pw5_{-1};
    int ampct_zero_{-1}, u20_{-1}, fric1_{-1}, fric2_{-1};
    int filter_left_{1};
    bool filter_phase_{};
    bool filter_phase_edge_{};
};

} // namespace ssi_host
