#include <soluna/pal/thread.h>

#include <pthread.h>
#include <sched.h>
#include <atomic>
#include <cstring>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace soluna::pal {

class ThreadPosix : public Thread {
public:
    ThreadPosix(const std::string& name, ThreadPriority priority)
        : name_(name), priority_(priority)
    {
    }

    ~ThreadPosix() override {
        if (running_.load()) {
            join();
        }
    }

    bool start(std::function<void()> func) override {
        if (running_.load()) return false;
        func_ = std::move(func);

        pthread_attr_t attr;
        pthread_attr_init(&attr);

        if (priority_ == ThreadPriority::Realtime) {
            pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
            struct sched_param param{};
            param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;
            pthread_attr_setschedparam(&attr, &param);
            pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        }

        running_.store(true);
        int ret = pthread_create(&thread_, &attr, &ThreadPosix::thread_entry, this);
        pthread_attr_destroy(&attr);

        if (ret != 0) {
            // Retry without RT priority
            if (priority_ == ThreadPriority::Realtime) {
                ret = pthread_create(&thread_, nullptr, &ThreadPosix::thread_entry, this);
            }
            if (ret != 0) {
                running_.store(false);
                return false;
            }
        }

#ifdef __linux__
        if (!name_.empty()) {
            pthread_setname_np(thread_, name_.substr(0, 15).c_str());
        }
#elif defined(__APPLE__)
        // On macOS, pthread_setname_np only works for the current thread
#endif

        return true;
    }

    void join() override {
        if (running_.load()) {
            pthread_join(thread_, nullptr);
            running_.store(false);
        }
    }

    bool is_running() const override {
        return running_.load();
    }

private:
    static void* thread_entry(void* arg) {
        auto* self = static_cast<ThreadPosix*>(arg);

#ifdef __APPLE__
        if (!self->name_.empty()) {
            pthread_setname_np(self->name_.substr(0, 63).c_str());
        }
#endif

        if (self->func_) {
            self->func_();
        }
        self->running_.store(false);
        return nullptr;
    }

    std::string name_;
    ThreadPriority priority_;
    pthread_t thread_{};
    std::function<void()> func_;
    std::atomic<bool> running_{false};
};

std::unique_ptr<Thread> Thread::create(const std::string& name, ThreadPriority priority) {
    return std::make_unique<ThreadPosix>(name, priority);
}

bool Thread::set_realtime_priority() {
#ifdef __linux__
    struct sched_param param{};
    param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#elif defined(__APPLE__)
    thread_time_constraint_policy_data_t policy{};
    // 1ms period at ~24MHz timebase
    policy.period = 24000;
    policy.computation = 6000;
    policy.constraint = 12000;
    policy.preemptible = 0;
    kern_return_t ret = thread_policy_set(
        mach_thread_self(),
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT
    );
    return ret == KERN_SUCCESS;
#else
    return false;
#endif
}

} // namespace soluna::pal
