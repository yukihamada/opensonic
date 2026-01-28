/**
 * Win32 High-Resolution Clock Implementation
 * Uses QueryPerformanceCounter for monotonic time.
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pal/time.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

namespace soluna::pal {

class ClockWin32 : public Clock {
public:
    ClockWin32() {
        QueryPerformanceFrequency(&freq_);
    }

    Timestamp monotonic_now() override {
        LARGE_INTEGER count;
        QueryPerformanceCounter(&count);

        // Convert to nanoseconds avoiding overflow: split into seconds and remainder
        int64_t seconds = count.QuadPart / freq_.QuadPart;
        int64_t remainder = count.QuadPart % freq_.QuadPart;
        int64_t ns = seconds * 1'000'000'000LL + (remainder * 1'000'000'000LL) / freq_.QuadPart;

        return Timestamp::from_ns(ns);
    }

    Timestamp realtime_now() override {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);

        // FILETIME is 100ns intervals since 1601-01-01
        // Unix epoch offset: 11644473600 seconds
        ULARGE_INTEGER ul;
        ul.LowPart = ft.dwLowDateTime;
        ul.HighPart = ft.dwHighDateTime;

        constexpr int64_t kEpochOffset = 116444736000000000LL;
        int64_t ns100 = static_cast<int64_t>(ul.QuadPart) - kEpochOffset;

        Timestamp t;
        t.seconds = ns100 / 10'000'000LL;
        t.nanoseconds = static_cast<int32_t>((ns100 % 10'000'000LL) * 100);
        return t;
    }

    void sleep_ns(int64_t ns) override {
        if (ns <= 0) return;

        // For short sleeps, use spin-wait for precision
        if (ns < 1'000'000LL) { // < 1ms
            int64_t target = monotonic_now().to_ns() + ns;
            while (monotonic_now().to_ns() < target) {
                YieldProcessor();
            }
            return;
        }

        // For longer sleeps, use waitable timer
        HANDLE timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(ns / 100); // negative = relative, in 100ns units
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
        } else {
            Sleep(static_cast<DWORD>(ns / 1'000'000LL));
        }
    }

    void sleep_until(const Timestamp& target) override {
        int64_t now_ns = monotonic_now().to_ns();
        int64_t target_ns = target.to_ns();
        int64_t diff = target_ns - now_ns;
        if (diff > 0) {
            sleep_ns(diff);
        }
    }

private:
    LARGE_INTEGER freq_{};
};

static ClockWin32 g_clock;

Clock& Clock::instance() {
    return g_clock;
}

} // namespace soluna::pal

#endif // _WIN32
