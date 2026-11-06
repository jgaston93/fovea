// cigi-test-host: a scripted CIGI 3.3 host for trying and measuring Fovea
// (milestone 2 step 4). It flies entity 0 through a script's segments and
// sends it at the script's rate (asynchronous), or once per Start of Frame
// (--sync), and reports the Start of Frame round trip.
//
//   cigi-test-host [--seconds N] [--sync] [--no-smoothing] <script.toml>
//
// --no-smoothing withholds the IG's leave to extrapolate between samples
// (CIGI's Extrapolation/Interpolation Enable), for comparing.
//
// Fovea's [host] section must match the script: its port is the script's
// [ig] port, and Start of Frame comes back to the script's [host] port
// (where this program sends from).

#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

#include "fovea/core/time.hpp"
#include "fovea/net/udp.hpp"
#include "script.hpp"
#include "session.hpp"

namespace {

int usage() {
    (void)std::fprintf(stderr,
                       "usage: cigi-test-host [--seconds N] [--sync] [--no-smoothing] "
                       "<script.toml>\n");
    return 2;
}

void print_stats(const fovea::test_host::SessionStats& s, double seconds) {
    std::printf("%.1f s: %llu messages sent, %llu Start of Frame received (IG frame %u)", seconds,
                static_cast<unsigned long long>(s.messages_sent),
                static_cast<unsigned long long>(s.start_of_frames), s.last_ig_frame);
    if (s.round_trips > 0) {
        std::printf("; round trip mean %.2f ms, max %.2f ms", s.round_trip_mean_ms,
                    s.round_trip_max_ms);
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = 60.0;
    bool sync = false;
    bool smoothing = true;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--sync") {
            sync = true;
        } else if (a == "--no-smoothing") {
            smoothing = false;
        } else if (a == "--seconds" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), seconds);
            if (ec != std::errc{} || end != v.data() + v.size() || !(seconds > 0.0) ||
                seconds > 86400.0) {
                (void)std::fprintf(stderr,
                                   "cigi-test-host: --seconds needs more than 0, up to 86400\n");
                return 2;
            }
        } else if (!a.empty() && a[0] != '-' && path.empty()) {
            path = std::string(a);
        } else {
            return usage();
        }
    }
    if (path.empty()) {
        return usage();
    }
    const auto loaded = fovea::test_host::load_script(path);
    if (!loaded.script) {
        for (const auto& e : loaded.errors) {
            (void)std::fprintf(stderr, "%s\n", e.c_str());
        }
        return 1;
    }
    const fovea::test_host::Script& script = *loaded.script;
    try {
        const fovea::UdpEndpoint ig{script.ig_address, script.ig_port};
        fovea::test_host::Session session(ig, script.host_port, 0, smoothing);
        fovea::test_host::Flight flight(script.start, script.segments, script.loop);
        std::printf("cigi-test-host: CIGI 3.3 to %s from port %u, %s, %zu segments%s, for %.0f s\n",
                    fovea::format_endpoint(ig).c_str(), unsigned{session.local_port()},
                    sync ? "once per Start of Frame" : "asynchronous", script.segments.size(),
                    script.loop ? " (looping)" : "", seconds);

        const auto start = std::chrono::steady_clock::now();
        // Deadlines from the tick count: no drift, and after a stall (or
        // leaving --sync) the missed ones are skipped, not sent in a burst.
        fovea::PeriodicSchedule schedule(start, script.rate_hz);
        auto last = start;
        auto next_report = start + std::chrono::seconds(5);
        bool told_operating = false;
        for (;;) {
            const auto t = std::chrono::steady_clock::now();
            if (t - start >= std::chrono::duration<double>(seconds)) {
                break;
            }
            if (sync && session.operating()) {
                // The IG's frame paces the host; the timer only keeps it
                // asking while the IG is quiet.
                (void)session.wait_start_of_frame(100);
            } else {
                session.wait_until(schedule.next_deadline());
                (void)schedule.tick(std::chrono::steady_clock::now());
            }
            const auto now = std::chrono::steady_clock::now();
            if (session.operating()) {
                flight.step(std::chrono::duration<double>(now - last).count());
            }
            last = now;
            session.send_frame(flight.state(), now);
            if (session.operating() && !told_operating) {
                std::printf("IG reports Operate: flying\n");
                told_operating = true;
            }
            if (now >= next_report) {
                print_stats(session.stats(), std::chrono::duration<double>(now - start).count());
                next_report += std::chrono::seconds(5);
            }
        }
        print_stats(
            session.stats(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        if (schedule.skipped() > 0) {
            std::printf("%llu send deadlines missed (the host was held up)\n",
                        static_cast<unsigned long long>(schedule.skipped()));
        }
        if (!told_operating) {
            (void)std::fprintf(stderr,
                               "cigi-test-host: the IG never reported Operate (is Fovea running "
                               "with a matching [host] section?)\n");
            return 1;
        }
    } catch (const std::exception& e) {
        (void)std::fprintf(stderr, "cigi-test-host: %s\n", e.what());
        return 1;
    }
    return 0;
}
