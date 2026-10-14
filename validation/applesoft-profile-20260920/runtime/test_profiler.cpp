#include "GuestProfiler.hpp"
#include <thread>
static void command(const std::string &text) {
    std::ofstream f(std::string(std::getenv("GS2_GUEST_PROFILE_DIR")) + "/control.txt");
    f << text << "\n";
    f.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
}
int main() {
    command("1 START budget 3");
    gs2guest::profiler.poll_control(100, 0);
    gs2guest::profiler.record(0xD000, 100, 102, false);
    gs2guest::profiler.record(0xD000, 102, 109, true);
    gs2guest::profiler.record(0xE000, 109, 112, false);
    gs2guest::profiler.record(0xD000, 112, 116, false);
    gs2guest::profiler.record(0xF000, 116, 120, false);
    gs2guest::profiler.poll_control(120, 0);
    command("2 START stopped 0");
    gs2guest::profiler.poll_control(200, 0);
    gs2guest::profiler.record(0xF000, 200, 205, false);
    command("3 STOP stopped");
    gs2guest::profiler.poll_control(205, 0);
    return 0;
}

