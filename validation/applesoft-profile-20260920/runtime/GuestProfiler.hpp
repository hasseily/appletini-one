#pragma once
// Temporary measurement instrumentation; does not change guest CPU state.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace gs2guest {
class Profiler {
    struct Counter { uint64_t instructions = 0; uint64_t cycles = 0; };
    std::array<Counter, 65536> counts_{};
    std::string directory_;
    std::string run_id_;
    uint64_t last_sequence_ = 0;
    uint64_t active_sequence_ = 0;
    uint64_t instruction_limit_ = 0;
    uint64_t instructions_ = 0;
    uint64_t instruction_cycles_ = 0;
    uint64_t irq_events_ = 0;
    uint64_t irq_cycles_ = 0;
    uint64_t start_cycle_ = 0;
    uint64_t end_cycle_ = 0;
    int start_clock_mode_ = -1;
    bool active_ = false;
    bool pending_dump_ = false;
    std::string reason_;
    using Clock = std::chrono::steady_clock;
    Clock::time_point last_poll_{};
    Clock::time_point started_{};
    Clock::time_point ended_{};

    static bool safe_name(const std::string& value) {
        if (value.empty()) return false;
        for (const unsigned char c : value) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        }
        return true;
    }
    bool replace_file(const std::string& temporary, const std::string& output) {
        std::remove(output.c_str());
        return std::rename(temporary.c_str(), output.c_str()) == 0;
    }
    void acknowledge(const char* state, const std::string& reason = "") {
        const auto path = directory_ + "/ack.json";
        const auto tmp = path + ".tmp";
        std::ofstream out(tmp, std::ios::trunc);
        out << "{\"seq\":" << last_sequence_ << ",\"active_seq\":" << active_sequence_
            << ",\"state\":\"" << state << "\",\"runid\":\"" << run_id_
            << "\",\"reason\":\"" << reason << "\",\"instructions\":"
            << instructions_ << "}\n";
        out.close();
        if (out) replace_file(tmp, path);
    }
    void finish(const std::string& reason) {
        active_ = false;
        pending_dump_ = true;
        ended_ = Clock::now();
        reason_ = reason;
    }
    void dump() {
        pending_dump_ = false;
        const auto base = directory_ + "/" + run_id_;
        std::ofstream csv(base + ".csv.tmp", std::ios::trunc);
        csv << "pc,instructions,cycles\n";
        uint64_t histogram_instructions = 0;
        uint64_t histogram_cycles = 0;
        for (size_t pc = 0; pc < counts_.size(); ++pc) {
            const auto& c = counts_[pc];
            if (!c.instructions) continue;
            csv << pc << ',' << c.instructions << ',' << c.cycles << '\n';
            histogram_instructions += c.instructions;
            histogram_cycles += c.cycles;
        }
        csv.close();
        const double seconds = std::chrono::duration<double>(ended_ - started_).count();
        std::ofstream meta(base + ".json.tmp", std::ios::trunc);
        meta << "{\n"
             << "  \"format\": \"gs2_guest_pc_profile_v1\",\n"
             << "  \"runid\": \"" << run_id_ << "\",\n"
             << "  \"start_seq\": " << active_sequence_ << ",\n"
             << "  \"stop_seq\": " << last_sequence_ << ",\n"
             << "  \"reason\": \"" << reason_ << "\",\n"
             << "  \"instruction_limit\": " << instruction_limit_ << ",\n"
             << "  \"instructions\": " << instructions_ << ",\n"
             << "  \"instruction_cycles\": " << instruction_cycles_ << ",\n"
             << "  \"irq_events\": " << irq_events_ << ",\n"
             << "  \"irq_cycles\": " << irq_cycles_ << ",\n"
             << "  \"accounted_cycles\": " << instruction_cycles_ + irq_cycles_ << ",\n"
             << "  \"start_guest_cycle\": " << start_cycle_ << ",\n"
             << "  \"end_guest_cycle\": " << end_cycle_ << ",\n"
             << "  \"guest_cycle_span\": " << end_cycle_ - start_cycle_ << ",\n"
             << "  \"histogram_instructions\": " << histogram_instructions << ",\n"
             << "  \"histogram_cycles\": " << histogram_cycles << ",\n"
             << "  \"clock_mode_at_start\": " << start_clock_mode_ << ",\n"
             << "  \"capture_wall_seconds\": " << seconds << ",\n"
             << "  \"scope\": \"65C02 PC histogram; IRQ entry accounted separately\",\n"
             << "  \"wall_time_note\": \"Includes host control and pauses; not a clean throughput benchmark\"\n"
             << "}\n";
        meta.close();
        if (!csv || !meta ||
            !replace_file(base + ".csv.tmp", base + ".csv") ||
            !replace_file(base + ".json.tmp", base + ".json")) {
            acknowledge("error", "output_write_failed");
            return;
        }
        acknowledge("complete", reason_);
    }
public:
    Profiler() {
        if (const char* value = std::getenv("GS2_GUEST_PROFILE_DIR")) directory_ = value;
    }
    inline void record(uint16_t pc, uint64_t start, uint64_t end, bool irq) {
        if (!active_) return;
        const auto delta = end - start;
        end_cycle_ = end;
        if (irq) {
            ++irq_events_;
            irq_cycles_ += delta;
            return;
        }
        ++instructions_;
        instruction_cycles_ += delta;
        ++counts_[pc].instructions;
        counts_[pc].cycles += delta;
        if (instruction_limit_ && instructions_ >= instruction_limit_) finish("instruction_budget");
    }
    void poll_control(uint64_t guest_cycle, int clock_mode) {
        if (directory_.empty()) return;
        const auto now = Clock::now();
        if (pending_dump_) dump();
        if (now - last_poll_ < std::chrono::milliseconds(10)) return;
        last_poll_ = now;
        std::ifstream input(directory_ + "/control.txt");
        uint64_t sequence = 0;
        std::string command, id;
        uint64_t limit = 0;
        if (!(input >> sequence >> command >> id) || sequence <= last_sequence_) return;
        last_sequence_ = sequence;
        if (!safe_name(id)) { acknowledge("error", "invalid_runid"); return; }
        if (command == "START") {
            if (active_ || pending_dump_) {
                acknowledge("error", "capture_already_active");
                return;
            }
            input >> limit;
            counts_.fill(Counter{});
            run_id_ = id;
            active_sequence_ = sequence;
            instruction_limit_ = limit;
            instructions_ = instruction_cycles_ = irq_events_ = irq_cycles_ = 0;
            start_cycle_ = end_cycle_ = guest_cycle;
            start_clock_mode_ = clock_mode;
            started_ = now;
            reason_.clear();
            active_ = true;
            acknowledge("active");
        } else if (command == "STOP") {
            if (id != run_id_) { acknowledge("error", "runid_mismatch"); return; }
            if (active_) {
                finish("host_stop");
                dump();
            } else acknowledge("complete", reason_);
        } else acknowledge("error", "unknown_command");
    }
};
inline Profiler profiler;
}

