#include "baseline.h"
#include "native_control.h"
#include "native_source.h"
#include "prototype_tract.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ssi_host;

struct Event {
    int64_t tick;
    int socket, reg, value;
};

template<class T> void read_number(std::istream& input, T& value) {
    if (!(input >> value)) throw std::runtime_error("truncated or invalid numeric input");
}

Tables read_tables(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open coefficient data");
    Tables t;
    for (auto& p : t.phones) {
        for (int* value : {&p.f1, &p.va, &p.f2, &p.fc, &p.f2q, &p.f3,
                           &p.fa, &p.cld, &p.vd, &p.closure, &p.duration, &p.pause})
            read_number(in, *value);
    }
    for (auto& n : t.native_rom) read_number(in, n);
    for (auto* map : {&t.native_f1, &t.native_f2, &t.native_f2q,
                      &t.native_f3, &t.native_va, &t.native_fa})
        for (auto& n : *map) read_number(in, n);
    for (auto& n : t.sc01_map) read_number(in, n);
    for (auto& row : t.coeff_f1) for (auto& n : row) read_number(in, n);
    for (auto& bank : t.coeff_f2) for (auto& row : bank)
        for (auto& n : row) read_number(in, n);
    for (auto& row : t.coeff_f3) for (auto& n : row) read_number(in, n);
    for (auto& n : t.coeff_f4) read_number(in, n);
    for (auto& n : t.coeff_fn) read_number(in, n);
    for (auto& n : t.coeff_fx) read_number(in, n);
    std::string extra;
    if (in >> extra) throw std::runtime_error("unexpected trailing coefficient data");
    return t;
}

int64_t floor_div(int64_t a, int64_t b) {
    return a / b - (a % b < 0);
}

int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }

void wav_header(std::ostream& out, uint32_t frames) {
    auto word = [&](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (8*i)) & 255));
    };
    out.write("RIFF", 4); word(36 + 4*frames, 4); out.write("WAVEfmt ", 8);
    word(16, 4); word(1, 2); word(2, 2); word(sample_rate, 4);
    word(sample_rate * 4, 4); word(4, 2); word(16, 2);
    out.write("data", 4); word(4*frames, 4);
}

int main(int argc, char** argv) {
    try {
        if (argc != 6 && argc != 8) throw std::runtime_error(
            "usage: ssi263_host TABLES EVENTS PCM_OR_WAV baseline|pitch|transitions|prototype ART_REFERENCE_RATE [PROTOTYPE_GAIN VOICE_TRIM]");
        const Tables tables = read_tables(argv[1]);
        std::ifstream input(argv[2]);
        std::string magic;
        int clock_hz, articulation_reference;
        int64_t start_tick, end_tick;
        size_t event_count;
        input >> magic >> clock_hz >> start_tick >> end_tick >> event_count;
        if (!input || magic != "SSIHOST1" || clock_hz < 100000 || clock_hz > 4000000 ||
            start_tick < 0 || end_tick <= start_tick || end_tick > int64_t(clock_hz) * 3600 ||
            event_count > 10000000)
            throw std::runtime_error("invalid trace header");
        const std::string mode(argv[4]);
        if (mode != "baseline" && mode != "transitions" && mode != "pitch" && mode != "prototype")
            throw std::runtime_error("unknown engine profile");
        auto integer = [](const char* value) {
            size_t parsed = 0;
            const int result = std::stoi(value, &parsed);
            if (parsed != std::string(value).size())
                throw std::runtime_error("model settings must be integers");
            return result;
        };
        articulation_reference = integer(argv[5]);
        if (articulation_reference < 0 || articulation_reference > 15)
            throw std::runtime_error("articulation reference rate must be 0..15");
        const int prototype_gain = argc == 8 ? integer(argv[6]) : 1;
        const int voice_trim = argc == 8 ? integer(argv[7]) : 16384;
        if (prototype_gain < 1 || prototype_gain > 1024 || voice_trim < 0 || voice_trim > 131071)
            throw std::runtime_error("prototype gain must be 1..1024 and voice trim 0..131071");
        const bool native = mode == "transitions" || mode == "prototype";
        std::vector<Event> events(event_count);
        for (size_t i = 0; i < events.size(); ++i) {
            auto& e = events[i];
            input >> e.tick >> e.socket >> e.reg >> e.value;
            if (!input || e.tick < -clock_hz || e.tick >= end_tick || e.socket < 0 ||
                e.socket > 1 || e.reg < 0 || e.reg > 7 || e.value < 0 || e.value > 255 ||
                (i && e.tick < events[i-1].tick))
                throw std::runtime_error("invalid or unsorted register event");
        }
        std::string trailing;
        if (input >> trailing) throw std::runtime_error("unexpected trailing event data");
        Baseline audio[2] = {Baseline(tables, clock_hz), Baseline(tables, clock_hz)};
        NativeControl controls[2] = {NativeControl(tables, clock_hz), NativeControl(tables, clock_hz)};
        NativeSource sources[2] = {NativeSource(voice_trim), NativeSource(voice_trim)};
        PrototypeTract tracts[2] = {PrototypeTract(prototype_gain), PrototypeTract(prototype_gain)};
        for (int socket = 0; socket != 2; ++socket) {
            audio[socket].set_native_transition_mode(mode == "transitions");
            audio[socket].use_exact_pitch(mode == "pitch");
            controls[socket].set_articulation_reference_rate(articulation_reference);
        }
        const int64_t first_tick = events.empty() ? 0 : std::min<int64_t>(0, events[0].tick);
        const int64_t first_frame = floor_div(first_tick * sample_rate, clock_hz);
        const int64_t start_frame = ceil_div(start_tick * sample_rate, clock_hz);
        const int64_t end_frame = ceil_div(end_tick * sample_rate, clock_hz);
        if (start_frame == end_frame)
            throw std::runtime_error("range contains no 48 kHz sample");
        int64_t native_tick = ceil_div(first_frame * clock_hz, sample_rate);
        std::ofstream output(argv[3], std::ios::binary);
        if (!output) throw std::runtime_error("cannot open PCM output");
        const std::string output_name(argv[3]);
        if (output_name.size() >= 4 && output_name.substr(output_name.size() - 4) == ".wav")
            wav_header(output, static_cast<uint32_t>(end_frame - start_frame));
        std::vector<unsigned char> buffer;
        buffer.reserve(65536);
        size_t next_event = 0;
        auto advance = [&](int64_t tick) {
            if (mode == "prototype") {
                for (; native_tick < tick; ++native_tick) {
                    for (int socket = 0; socket != 2; ++socket) {
                        auto& control = controls[socket];
                        auto& source = sources[socket];
                        control.set_ampct_zero(source.ampct_zero());
                        control.advance_xck(1);
                        // Only transitioned mode retains the running glide policy.
                        // Immediate writes must reach the next native reload.
                        const auto& a = audio[socket];
                        const int rate = a.register_value(2);
                        const int live_i = ((rate & 8) << 8) | (a.register_value(1) << 3) | (rate & 7);
                        source.tick(control, a.current_function() == 3 ? a.active_inflection() : live_i);
                        const auto& s = source.output();
                        const FilterEvent e{s.phase, s.phase_edge,
                            {s.f1, s.f2, s.f2q, s.f3, s.f4, s.filter_amp, s.voice_amp, s.fric_amp},
                            s.voice_target_q16, s.fric_drive_q16, s.fric1, s.fric2, s.output_open};
                        tracts[socket].process(e);
                    }
                }
            } else if (native) {
                for (auto& c : controls) c.advance_xck(tick - native_tick);
            }
            native_tick = tick;
        };
        for (int64_t frame = first_frame; frame < end_frame; ++frame) {
            const int64_t tick = ceil_div(frame * clock_hz, sample_rate);
            while (next_event < events.size() && events[next_event].tick <= tick) {
                const auto& e = events[next_event++];
                advance(e.tick);
                audio[e.socket].write(e.reg, e.value);
                controls[e.socket].write(e.reg, e.value);
            }
            advance(tick);
            for (int socket = 0; socket != 2; ++socket) {
                if (mode == "transitions")
                    audio[socket].override_parameters(controls[socket].parameter_codes(), true, true);
                const int16_t baseline_sample = audio[socket].sample();
                const uint16_t value = static_cast<uint16_t>(mode == "prototype" ?
                    tracts[socket].sample() : baseline_sample);
                if (frame >= start_frame) {
                    buffer.push_back(static_cast<unsigned char>(value & 255));
                    buffer.push_back(static_cast<unsigned char>(value >> 8));
                }
            }
            if (buffer.size() >= 65536) {
                output.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
                buffer.clear();
            }
        }
        // Complete the trace's final fraction of a sample interval as well.
        while (next_event < events.size()) {
            const auto& e = events[next_event++];
            advance(e.tick);
            audio[e.socket].write(e.reg, e.value);
            controls[e.socket].write(e.reg, e.value);
        }
        advance(end_tick);
        output.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        output.close();
        if (!output) throw std::runtime_error("PCM write failed");
        std::cout << "{\"frames\":" << end_frame - start_frame
                  << ",\"processed_frames\":" << end_frame - first_frame
                  << ",\"applied_writes\":" << next_event;
        if (native) {
            std::cout << ",\"native_metrics\":[";
            for (int socket = 0; socket != 2; ++socket) {
                const auto& c = controls[socket].metrics();
                const auto& s = sources[socket].metrics();
                const auto& t = tracts[socket].metrics();
                if (socket) std::cout << ',';
                std::cout << "{\"xck_ticks\":" << c.xck_ticks << ",\"scans\":" << c.scans
                          << ",\"transition_steps\":" << c.transition_steps
                          << ",\"envelope_edges\":" << s.envelope_edges
                          << ",\"glottal_loads\":" << s.glottal_loads
                          << ",\"noise_shifts\":" << s.noise_shift_edges
                          << ",\"tract_events\":" << t.events
                          << ",\"state_saturations\":" << t.state_saturations
                          << ",\"output_clips\":" << t.output_clips << '}';
            }
            std::cout << ']';
        }
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
