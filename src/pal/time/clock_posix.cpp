#include <soluna/pal/time.h>

#include <time.h>
#include <cerrno>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

namespace soluna::pal {

class ClockPosix : public Clock {
public:
    ClockPosix() {
#ifdef __APPLE__
        mach_timebase_info(&timebase_info_);
#endif
    }

    Timestamp monotonic_now() override {
#ifdef __APPLE__
        uint64_t ticks = mach_absolute_time();
        uint64_t ns = ticks * timebase_info_.numer / timebase_info_.denom;
        return Timestamp::from_ns(static_cast<int64_t>(ns));
#else
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        Timestamp t;
        t.seconds = ts.tv_sec;
        t.nanoseconds = static_cast<int32_t>(ts.tv_nsec);
        return t;
#endif
    }

    Timestamp realtime_now() override {
        struct timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        Timestamp t;
        t.seconds = ts.tv_sec;
        t.nanoseconds = static_cast<int32_t>(ts.tv_nsec);
        return t;
    }

    void sleep_ns(int64_t ns) override {
        if (ns <= 0) return;
        struct timespec ts{};
        ts.tv_sec = ns / 1'000'000'000LL;
        ts.tv_nsec = ns % 1'000'000'000LL;
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
            // retry on interrupt
        }
    }

    void sleep_until(const Timestamp& target) override {
#ifdef __APPLE__
        // macOS doesn't have clock_nanosleep, use relative sleep
        int64_t now_ns = monotonic_now().to_ns();
        int64_t target_ns = target.to_ns();
        int64_t diff = target_ns - now_ns;
        if (diff > 0) {
            sleep_ns(diff);
        }
#else
        struct timespec ts{};
        ts.tv_sec = target.seconds;
        ts.tv_nsec = target.nanoseconds;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
            // retry on interrupt
        }
#endif
    }

private:
#ifdef __APPLE__
    mach_timebase_info_data_t timebase_info_{};
#endif
};

static ClockPosix g_clock;

Clock& Clock::instance() {
    return g_clock;
}

} // namespace soluna::pal
