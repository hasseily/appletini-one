// Read-only instrumentation of the host candidate. Kept outside the host
// source directory so it cannot become part of the listening executable.
#include "ssi263_host/baseline.h"
#include "ssi263_host/native_control.h"
#include "ssi263_host/native_source.h"
#include "ssi263_host/prototype_tract.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ssi_host;

namespace {
struct Event { int64_t tick; int socket, reg, value; };

template<class T> void number(std::istream& in, T& value) {
    if (!(in >> value)) throw std::runtime_error("invalid numeric input");
}

Tables tables_from(const char* path) {
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
    if (in >> extra) throw std::runtime_error("trailing table data");
    return t;
}

int64_t floor_div(int64_t a, int64_t b) { return a / b - (a % b < 0); }
int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }
}

int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::runtime_error("usage: attack_trace TABLES EVENTS STATE_CSV MONO_PCM");
        const auto tables = tables_from(argv[1]);
        std::ifstream in(argv[2]);
        std::string magic;
        int hz;
        int64_t start_tick, end_tick;
        size_t count;
        in >> magic >> hz >> start_tick >> end_tick >> count;
        if (!in || magic != "SSIHOST1" || hz <= 0 || start_tick != 0 || end_tick <= 0)
            throw std::runtime_error("invalid trace header");
        std::vector<Event> events;
        for (size_t n = 0; n < count; ++n) {
            Event e;
            in >> e.tick >> e.socket >> e.reg >> e.value;
            if (!in || e.socket < 0 || e.socket > 1 || e.reg < 0 || e.reg > 7 ||
                e.value < 0 || e.value > 255 || (!events.empty() && e.tick < events.back().tick))
                throw std::runtime_error("invalid trace event");
            if (e.socket == 0) events.push_back(e);
        }
        Baseline pitch(tables, hz);
        NativeControl control(tables, hz);
        NativeSource source;
        PrototypeTract tract(1);
        std::ofstream csv(argv[3]), pcm(argv[4], std::ios::binary);
        if (!csv || !pcm) throw std::runtime_error("cannot open diagnostic output");
        csv << "tick,reason,write_reg,write_value,phone,ctl,duration_phase,pw0,pw1,pw2,pw3,pw5,u20,"
               "amp_code,va_code,fa_code,amp_a,amp_b,amp_c,amp_target,amp_up,va_a,va_b,va_c,va_target,va_up,"
               "fa_a,fa_b,fa_c,fa_target,fa_up,ampct,ampct_zero,u62,voice_amp,fric_amp,filter_amp,fric1,fric2,"
               "selector,scan_phase,filter_phase,filter_edge,glottal_count,voice_drive,fric_drive,"
               "tract_output,reconstruction\n";
        std::vector<int> previous;
        uint64_t records = 0;
        auto snapshot = [&](int64_t tick, const char* reason, int reg = -1, int value = -1) {
            const auto& c = control.parameter_codes();
            const auto& s = source.output();
            std::vector<int> state{control.phone(), int(control.latched_ctrl()), control.duration_phase(),
                control.pw0(), control.pw1(), control.pw2(), control.pw3(), control.pw5(), control.u20(),
                c[4], c[5], c[6]};
            for (int selector : {4, 5, 6}) {
                const auto& d = control.parameter_state(selector);
                state.insert(state.end(), {d.a, d.b, d.c, d.target, int(d.upward)});
            }
            state.insert(state.end(), {source.ampct(), source.ampct_zero(), int(source.voice_toggle()),
                s.voice_amp, s.fric_amp, s.filter_amp, int(s.fric1), int(s.fric2)});
            if (reg < 0 && state == previous) return;
            previous = state;
            csv << tick << ',' << reason << ',' << reg << ',' << value;
            for (int v : state) csv << ',' << v;
            csv << ',' << control.selector() << ',' << control.selector_phase() << ','
                << int(s.phase) << ',' << int(s.phase_edge) << ',' << source.glottal_count() << ','
                << s.voice_target_q16 << ',' << s.fric_drive_q16 << ',' << tract.state().output << ','
                << tract.state().reconstruction << '\n';
            ++records;
        };
        const int64_t first_tick = events.empty() ? 0 : std::min<int64_t>(0, events.front().tick);
        const int64_t first_frame = floor_div(first_tick * sample_rate, hz);
        const int64_t last_frame = ceil_div(end_tick * sample_rate, hz);
        int64_t native_tick = ceil_div(first_frame * hz, sample_rate);
        auto advance = [&](int64_t tick) {
            for (; native_tick < tick; ++native_tick) {
                control.set_ampct_zero(source.ampct_zero());
                control.advance_xck(1);
                const int rate = pitch.register_value(2);
                const int live_i = ((rate & 8) << 8) | (pitch.register_value(1) << 3) | (rate & 7);
                source.tick(control, pitch.current_function() == 3 ? pitch.active_inflection() : live_i);
                const auto& s = source.output();
                tract.process({s.phase, s.phase_edge,
                    {s.f1, s.f2, s.f2q, s.f3, s.f4, s.filter_amp, s.voice_amp, s.fric_amp},
                    s.voice_target_q16, s.fric_drive_q16, s.fric1, s.fric2, s.output_open});
                snapshot(native_tick + 1, "state");
            }
        };
        size_t next = 0;
        for (int64_t frame = first_frame; frame < last_frame; ++frame) {
            const int64_t tick = ceil_div(frame * hz, sample_rate);
            while (next < events.size() && events[next].tick <= tick) {
                const auto& e = events[next++];
                advance(e.tick);
                pitch.write(e.reg, e.value);
                control.write(e.reg, e.value);
                snapshot(e.tick, "write", e.reg, e.value);
            }
            advance(tick);
            pitch.sample();
            const uint16_t value = static_cast<uint16_t>(tract.sample());
            if (frame >= 0) {
                pcm.put(static_cast<char>(value & 255));
                pcm.put(static_cast<char>(value >> 8));
            }
        }
        while (next < events.size()) {
            const auto& e = events[next++];
            advance(e.tick);
            pitch.write(e.reg, e.value);
            control.write(e.reg, e.value);
            snapshot(e.tick, "write", e.reg, e.value);
        }
        advance(end_tick);
        csv.close(); pcm.close();
        if (!csv || !pcm) throw std::runtime_error("diagnostic output write failed");
        std::cout << "{\"state_records\":" << records << ",\"frames\":" << last_frame
                  << ",\"writes\":" << events.size() << ",\"state_saturations\":"
                  << tract.metrics().state_saturations << ",\"output_clips\":"
                  << tract.metrics().output_clips << "}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
