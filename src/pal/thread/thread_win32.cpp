/**
 * Win32 Thread Implementation
 * SPDX-License-Identifier: MIT
 */

#include <soluna/pal/thread.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <avrt.h>
#include <process.h>

#include <atomic>
#include <cstring>

#pragma comment(lib, "avrt.lib")

namespace soluna::pal {

class ThreadWin32 : public Thread {
public:
    ThreadWin32(const std::string& name, ThreadPriority priority)
        : name_(name), priority_(priority) {}

    ~ThreadWin32() override {
        if (running_.load()) {
            join();
        }
    }

    bool start(std::function<void()> func) override {
        if (running_.load()) return false;
        func_ = std::move(func);
        running_.store(true);

        handle_ = reinterpret_cast<HANDLE>(_beginthreadex(
            nullptr, 0, &ThreadWin32::thread_entry, this, 0, nullptr));

        if (!handle_) {
            running_.store(false);
            return false;
        }

        // Set priority
        if (priority_ == ThreadPriority::Realtime) {
            SetThreadPriority(handle_, THREAD_PRIORITY_TIME_CRITICAL);
        } else if (priority_ == ThreadPriority::High) {
            SetThreadPriority(handle_, THREAD_PRIORITY_HIGHEST);
        }

        // Set thread name (Windows 10 1607+)
        if (!name_.empty()) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, name_.c_str(), -1, nullptr, 0);
            std::vector<wchar_t> wname(wlen);
            MultiByteToWideChar(CP_UTF8, 0, name_.c_str(), -1, wname.data(), wlen);

            using SetThreadDescriptionFunc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
            auto fn = reinterpret_cast<SetThreadDescriptionFunc>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
            if (fn) {
                fn(handle_, wname.data());
            }
        }

        return true;
    }

    void join() override {
        if (running_.load() && handle_) {
            WaitForSingleObject(handle_, INFINITE);
            CloseHandle(handle_);
            handle_ = nullptr;
            running_.store(false);
        }
    }

    bool is_running() const override {
        return running_.load();
    }

private:
    static unsigned __stdcall thread_entry(void* arg) {
        auto* self = static_cast<ThreadWin32*>(arg);

        // For realtime priority, use MMCSS (Multimedia Class Scheduler Service)
        DWORD task_index = 0;
        HANDLE task_handle = nullptr;
        if (self->priority_ == ThreadPriority::Realtime) {
            task_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        }

        if (self->func_) {
            self->func_();
        }

        if (task_handle) {
            AvRevertMmThreadCharacteristics(task_handle);
        }

        self->running_.store(false);
        return 0;
    }

    std::string name_;
    ThreadPriority priority_;
    HANDLE handle_ = nullptr;
    std::function<void()> func_;
    std::atomic<bool> running_{false};
};

std::unique_ptr<Thread> Thread::create(const std::string& name, ThreadPriority priority) {
    return std::make_unique<ThreadWin32>(name, priority);
}

bool Thread::set_realtime_priority() {
    // Use MMCSS for pro audio scheduling
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    return task != nullptr;
}

} // namespace soluna::pal

#endif // _WIN32
