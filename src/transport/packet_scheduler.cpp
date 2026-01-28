#include <soluna/transport/packet_scheduler.h>

namespace soluna::transport {

PacketScheduler::PacketScheduler(PacketTier tier, uint32_t sample_rate)
    : samples_per_packet_(soluna::samples_per_packet(tier))
{
    interval_ns_ = (static_cast<int64_t>(samples_per_packet_) * 1'000'000'000LL) / sample_rate;
}

void PacketScheduler::reset() {
    next_send_time_ = pal::Clock::instance().monotonic_now();
}

pal::Timestamp PacketScheduler::wait_next() {
    auto& clock = pal::Clock::instance();

    // Advance target time
    int64_t next_ns = next_send_time_.to_ns() + interval_ns_;
    next_send_time_ = pal::Timestamp::from_ns(next_ns);

    clock.sleep_until(next_send_time_);
    return next_send_time_;
}

} // namespace soluna::transport
