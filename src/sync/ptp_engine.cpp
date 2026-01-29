/**
 * PTP Engine — Full PTPv2 state machine
 * SPDX-License-Identifier: MIT
 */

#include <soluna/sync/ptp_engine.h>
#include <soluna/soluna.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <chrono>

#ifdef __linux__
#include <net/if.h>
#include <ifaddrs.h>
#endif

namespace soluna::sync {

PtpEngine::PtpEngine(const PtpConfig& config)
    : config_(config)
    , servo_(config.servo_config)
{
    generate_clock_id();
}

PtpEngine::~PtpEngine() {
    stop();
}

void PtpEngine::generate_clock_id() {
    // Generate a pseudo-unique clock ID from random bytes
    // In production, derive from MAC address (EUI-48 → EUI-64)
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    for (auto& b : local_port_id_.clock_id) {
        b = static_cast<uint8_t>(std::rand() & 0xFF);
    }
    // Set locally-administered bit
    local_port_id_.clock_id[0] |= 0x02;
    local_port_id_.port_number = 1;
}

bool PtpEngine::start() {
    if (running_.load()) return false;

    // Create sockets
    event_socket_ = pal::UdpSocket::create();
    general_socket_ = pal::UdpSocket::create();
    if (!event_socket_ || !general_socket_) {
        fprintf(stderr, "PTP: failed to create sockets\n");
        return false;
    }

    // Bind
    if (!event_socket_->bind(kPortPTPEvent)) {
        fprintf(stderr, "PTP: failed to bind event port %u\n", kPortPTPEvent);
        return false;
    }
    if (!general_socket_->bind(kPortPTPGeneral)) {
        fprintf(stderr, "PTP: failed to bind general port %u\n", kPortPTPGeneral);
        return false;
    }

    // Join PTP multicast
    event_socket_->join_multicast(kMulticastPTP, config_.interface);
    general_socket_->join_multicast(kMulticastPTP, config_.interface);

    // Set DSCP: EF for event, CS7 for general
    event_socket_->set_dscp(46);
    general_socket_->set_dscp(56);

    // Set timeouts for non-blocking receive loops
    event_socket_->set_recv_timeout_ms(100);
    general_socket_->set_recv_timeout_ms(100);

    running_.store(true);
    role_.store(PtpRole::Listening);

    // Start threads
    event_thread_ = pal::Thread::create("ptp-event", pal::ThreadPriority::High);
    general_thread_ = pal::Thread::create("ptp-general", pal::ThreadPriority::Normal);
    task_thread_ = pal::Thread::create("ptp-task", pal::ThreadPriority::Normal);

    event_thread_->start([this]() { event_loop(); });
    general_thread_->start([this]() { general_loop(); });
    task_thread_->start([this]() {
        if (role_.load() == PtpRole::Master) {
            master_loop();
        } else {
            slave_loop();
        }
    });

    return true;
}

void PtpEngine::stop() {
    running_.store(false);
    if (event_thread_) { event_thread_->join(); event_thread_.reset(); }
    if (general_thread_) { general_thread_->join(); general_thread_.reset(); }
    if (task_thread_) { task_thread_->join(); task_thread_.reset(); }

    if (event_socket_) {
        event_socket_->leave_multicast(kMulticastPTP);
        event_socket_.reset();
    }
    if (general_socket_) {
        general_socket_->leave_multicast(kMulticastPTP);
        general_socket_.reset();
    }
}

PtpSyncInfo PtpEngine::sync_info() const {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    return sync_info_;
}

pal::Timestamp PtpEngine::ptp_now() const {
    auto local = pal::Clock::instance().realtime_now();
    std::lock_guard<std::mutex> lock(sync_mutex_);
    // Apply offset correction
    int64_t corrected = local.to_ns() - sync_info_.offset_ns;
    return pal::Timestamp::from_ns(corrected);
}

int64_t PtpEngine::get_media_clock_ns() const {
    return ptp_now().to_ns();
}

uint32_t PtpEngine::media_clock_to_rtp_timestamp(int64_t media_clock_ns, uint32_t sample_rate) {
    // Convert nanoseconds to samples, then truncate to 32-bit
    // RTP timestamp = (media_clock_ns * sample_rate) / 1,000,000,000
    // Use 64-bit arithmetic to avoid overflow
    int64_t samples = (media_clock_ns * static_cast<int64_t>(sample_rate)) / 1'000'000'000LL;
    return static_cast<uint32_t>(samples & 0xFFFFFFFFLL);
}

void PtpEngine::set_sync_callback(PtpSyncCallback cb) {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    sync_callback_ = std::move(cb);
}

// ---- Event loop (port 319): Sync and Delay_Req ----

void PtpEngine::event_loop() {
    uint8_t buf[256];
    pal::SocketAddress src;

    while (running_.load()) {
        int n = event_socket_->recv_from(buf, sizeof(buf), src);
        if (n <= 0) continue;

        auto recv_ts = pal::Clock::instance().realtime_now();

        PtpHeader hdr;
        if (!ptp_parse_header(buf, static_cast<size_t>(n), hdr)) continue;
        if (hdr.domain_number != config_.domain) continue;

        switch (hdr.message_type) {
            case PtpMessageType::Sync:
                if (role_.load() == PtpRole::Slave) {
                    handle_sync(buf, static_cast<size_t>(n), recv_ts);
                }
                break;
            case PtpMessageType::DelayReq:
                if (role_.load() == PtpRole::Master) {
                    // Master responds with Delay_Resp
                    PtpTimestamp recv_ptp = PtpTimestamp::from_pal(recv_ts);
                    send_delay_resp(hdr.sequence_id, recv_ptp, hdr.source_port_id);
                }
                break;
            default:
                break;
        }
    }
}

// ---- General loop (port 320): Follow_Up, Delay_Resp, Announce ----

void PtpEngine::general_loop() {
    uint8_t buf[256];
    pal::SocketAddress src;

    while (running_.load()) {
        int n = general_socket_->recv_from(buf, sizeof(buf), src);
        if (n <= 0) continue;

        PtpHeader hdr;
        if (!ptp_parse_header(buf, static_cast<size_t>(n), hdr)) continue;
        if (hdr.domain_number != config_.domain) continue;

        switch (hdr.message_type) {
            case PtpMessageType::FollowUp:
                if (role_.load() == PtpRole::Slave) {
                    handle_follow_up(buf, static_cast<size_t>(n));
                }
                break;
            case PtpMessageType::DelayResp:
                if (role_.load() == PtpRole::Slave) {
                    handle_delay_resp(buf, static_cast<size_t>(n));
                }
                break;
            case PtpMessageType::Announce:
                handle_announce(buf, static_cast<size_t>(n));
                break;
            default:
                break;
        }
    }
}

// ---- Master loop ----

void PtpEngine::master_loop() {
    auto& clock = pal::Clock::instance();

    // Compute intervals from log values
    int64_t sync_interval_ms = static_cast<int64_t>(
        1000.0 * std::pow(2.0, config_.log_sync_interval));
    if (sync_interval_ms < 10) sync_interval_ms = 10;

    int64_t announce_interval_ms = static_cast<int64_t>(
        1000.0 * std::pow(2.0, config_.log_announce_interval));
    if (announce_interval_ms < 100) announce_interval_ms = 100;

    int64_t last_sync_ms = 0;
    int64_t last_announce_ms = 0;

    while (running_.load()) {
        if (role_.load() != PtpRole::Master) {
            // Switched to slave or listening, exit master loop
            clock.sleep_ns(100'000'000); // 100ms
            continue;
        }

        int64_t now_ms = clock.monotonic_now().to_ns() / 1'000'000;

        if (now_ms - last_sync_ms >= sync_interval_ms) {
            send_sync();
            last_sync_ms = now_ms;
        }

        if (now_ms - last_announce_ms >= announce_interval_ms) {
            send_announce();
            last_announce_ms = now_ms;
        }

        clock.sleep_ns(1'000'000); // 1ms tick
    }
}

// ---- Slave loop ----

void PtpEngine::slave_loop() {
    auto& clock = pal::Clock::instance();

    int64_t delay_req_interval_ms = static_cast<int64_t>(
        1000.0 * std::pow(2.0, config_.log_delay_req_interval));
    if (delay_req_interval_ms < 10) delay_req_interval_ms = 10;

    int64_t last_delay_req_ms = 0;

    while (running_.load()) {
        if (role_.load() != PtpRole::Slave) {
            clock.sleep_ns(100'000'000);
            continue;
        }

        int64_t now_ms = clock.monotonic_now().to_ns() / 1'000'000;

        if (now_ms - last_delay_req_ms >= delay_req_interval_ms) {
            send_delay_req();
            last_delay_req_ms = now_ms;
        }

        // Check for announce timeout
        announce_timeout_count_++;
        int timeout_ticks = static_cast<int>(
            config_.announce_receipt_timeout *
            std::pow(2.0, config_.log_announce_interval) * 1000.0 /
            delay_req_interval_ms);
        if (announce_timeout_count_ > timeout_ticks) {
            // Master lost — run BMCA, potentially become master
            run_bmca();
            announce_timeout_count_ = 0;
        }

        clock.sleep_ns(delay_req_interval_ms * 1'000'000);
    }
}

// ---- Message handlers ----

void PtpEngine::handle_sync(const uint8_t* buf, size_t len,
                              const pal::Timestamp& recv_ts) {
    PtpHeader hdr;
    if (!ptp_parse_header(buf, len, hdr)) return;

    // Store t2 (receive time) and sync sequence
    t2_ = recv_ts;
    sync_seq_ = hdr.sequence_id;
    have_t2_ = true;

    // Two-step: t1 comes in Follow_Up
    // One-step: t1 is in Sync origin timestamp
    if (!(hdr.flags[0] & 0x02)) { // TWO_STEP flag not set → one-step
        PtpTimestamp ts;
        if (ptp_parse_timestamp_body(buf, len, ts)) {
            t1_ = ts;
            have_t1_ = true;

            // Calculate offset with available data
            if (have_t1_ && have_t2_) {
                // offset = t2 - t1 - delay
                double offset = static_cast<double>(t2_.to_ns() - t1_.to_ns())
                                - servo_.state().path_delay_ns;
                servo_.feed_offset(offset);
                update_sync_info();
                have_t1_ = false;
                have_t2_ = false;
            }
        }
    }
}

void PtpEngine::handle_follow_up(const uint8_t* buf, size_t len) {
    PtpHeader hdr;
    if (!ptp_parse_header(buf, len, hdr)) return;

    if (hdr.sequence_id != sync_seq_) return; // mismatch

    PtpTimestamp ts;
    if (!ptp_parse_timestamp_body(buf, len, ts)) return;

    t1_ = ts;
    have_t1_ = true;

    if (have_t1_ && have_t2_) {
        double offset = static_cast<double>(t2_.to_ns() - t1_.to_ns())
                        - servo_.state().path_delay_ns;
        servo_.feed_offset(offset);
        update_sync_info();
        have_t1_ = false;
        have_t2_ = false;
    }
}

void PtpEngine::handle_delay_resp(const uint8_t* buf, size_t len) {
    PtpHeader hdr;
    if (!ptp_parse_header(buf, len, hdr)) return;

    if (hdr.sequence_id != delay_req_seq_) return;

    PtpDelayRespBody body;
    if (!ptp_parse_delay_resp(buf, len, body)) return;

    // Verify it's for us
    if (body.requesting_port_id != local_port_id_) return;

    t4_ = body.receive_timestamp;

    // Calculate path delay: delay = ((t2-t1) + (t4-t3)) / 2
    // Using offset formulation: delay = ((t4-t3) - offset) / 2 if offset known
    // Or simpler: delay = (t4 - t3 - (t2 - t1)) / 2 + (t2 - t1) / 2
    // Standard: delay = ((t2-t1) + (t4-t3)) / 2
    double t2_t1 = static_cast<double>(t2_.to_ns() - t1_.to_ns());
    double t4_t3 = static_cast<double>(t4_.to_ns() - t3_.to_ns());
    double delay = (t2_t1 + t4_t3) / 2.0;

    if (delay >= 0) {
        servo_.feed_delay(delay);
    }
}

void PtpEngine::handle_announce(const uint8_t* buf, size_t len) {
    PtpHeader hdr;
    if (!ptp_parse_header(buf, len, hdr)) return;

    PtpAnnounceBody body;
    if (!ptp_parse_announce(buf, len, body)) return;

    // Reset announce timeout
    announce_timeout_count_ = 0;

    // BMCA: is this better than current best?
    if (!best_master_id_.clock_id[0] && !best_master_id_.clock_id[1]) {
        // No current master
        best_announce_ = body;
        best_master_id_ = hdr.source_port_id;
    } else if (bmca_compare(body, hdr.source_port_id,
                              best_announce_, best_master_id_)) {
        best_announce_ = body;
        best_master_id_ = hdr.source_port_id;
    }

    // Determine our role
    PtpAnnounceBody local_announce;
    local_announce.grandmaster_priority1 = config_.priority1;
    local_announce.grandmaster_priority2 = config_.priority2;
    local_announce.grandmaster_clock_quality = config_.clock_quality;
    std::memcpy(local_announce.grandmaster_identity.data(),
                local_port_id_.clock_id.data(), 8);

    if (bmca_compare(local_announce, local_port_id_,
                      best_announce_, best_master_id_)) {
        // We are the best — become master
        role_.store(PtpRole::Master);
    } else {
        // Someone else is better — become slave
        role_.store(PtpRole::Slave);
    }

    update_sync_info();
}

// ---- Message senders ----

void PtpEngine::send_sync() {
    uint8_t buf[kPtpSyncSize];

    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = config_.domain;
    hdr.source_port_id = local_port_id_;
    hdr.sequence_id = master_sync_seq_;
    hdr.log_message_interval = config_.log_sync_interval;
    hdr.flags[0] = 0x02; // TWO_STEP

    auto now = pal::Clock::instance().realtime_now();
    PtpTimestamp origin = PtpTimestamp::from_pal(now);

    size_t len = ptp_serialize_sync(buf, sizeof(buf), hdr, origin);
    if (len > 0) {
        pal::SocketAddress dest{kMulticastPTP, kPortPTPEvent};
        event_socket_->send_to(buf, len, dest);

        // Send Follow_Up with precise timestamp
        auto precise_ts = PtpTimestamp::from_pal(
            pal::Clock::instance().realtime_now());
        send_follow_up(master_sync_seq_, precise_ts);

        master_sync_seq_++;
    }
}

void PtpEngine::send_follow_up(uint16_t seq_id, const PtpTimestamp& precise_ts) {
    uint8_t buf[kPtpFollowUpSize];

    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = config_.domain;
    hdr.source_port_id = local_port_id_;
    hdr.sequence_id = seq_id;
    hdr.log_message_interval = config_.log_sync_interval;

    size_t len = ptp_serialize_follow_up(buf, sizeof(buf), hdr, precise_ts);
    if (len > 0) {
        pal::SocketAddress dest{kMulticastPTP, kPortPTPGeneral};
        general_socket_->send_to(buf, len, dest);
    }
}

void PtpEngine::send_delay_req() {
    uint8_t buf[kPtpDelayReqSize];

    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = config_.domain;
    hdr.source_port_id = local_port_id_;
    hdr.sequence_id = delay_req_seq_++;
    hdr.log_message_interval = config_.log_delay_req_interval;

    t3_ = pal::Clock::instance().realtime_now();
    PtpTimestamp origin = PtpTimestamp::from_pal(t3_);

    size_t len = ptp_serialize_delay_req(buf, sizeof(buf), hdr, origin);
    if (len > 0) {
        pal::SocketAddress dest{kMulticastPTP, kPortPTPEvent};
        event_socket_->send_to(buf, len, dest);
    }
}

void PtpEngine::send_delay_resp(uint16_t seq_id, const PtpTimestamp& recv_ts,
                                  const PtpPortIdentity& requester) {
    uint8_t buf[kPtpDelayRespSize];

    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = config_.domain;
    hdr.source_port_id = local_port_id_;
    hdr.sequence_id = seq_id;
    hdr.log_message_interval = config_.log_delay_req_interval;

    PtpDelayRespBody body;
    body.receive_timestamp = recv_ts;
    body.requesting_port_id = requester;

    size_t len = ptp_serialize_delay_resp(buf, sizeof(buf), hdr, body);
    if (len > 0) {
        pal::SocketAddress dest{kMulticastPTP, kPortPTPGeneral};
        general_socket_->send_to(buf, len, dest);
    }
}

void PtpEngine::send_announce() {
    uint8_t buf[kPtpAnnounceSize];

    PtpHeader hdr;
    hdr.version = 2;
    hdr.domain_number = config_.domain;
    hdr.source_port_id = local_port_id_;
    hdr.sequence_id = master_announce_seq_++;
    hdr.log_message_interval = config_.log_announce_interval;

    PtpAnnounceBody body;
    body.grandmaster_priority1 = config_.priority1;
    body.grandmaster_priority2 = config_.priority2;
    body.grandmaster_clock_quality = config_.clock_quality;
    std::memcpy(body.grandmaster_identity.data(),
                local_port_id_.clock_id.data(), 8);

    size_t len = ptp_serialize_announce(buf, sizeof(buf), hdr, body);
    if (len > 0) {
        pal::SocketAddress dest{kMulticastPTP, kPortPTPGeneral};
        general_socket_->send_to(buf, len, dest);
    }
}

void PtpEngine::run_bmca() {
    // No announces received — check if we should become master
    PtpAnnounceBody local_announce;
    local_announce.grandmaster_priority1 = config_.priority1;
    local_announce.grandmaster_priority2 = config_.priority2;
    local_announce.grandmaster_clock_quality = config_.clock_quality;
    std::memcpy(local_announce.grandmaster_identity.data(),
                local_port_id_.clock_id.data(), 8);

    if (best_master_id_.clock_id == local_port_id_.clock_id ||
        !best_master_id_.clock_id[0]) {
        // No other master — become master
        role_.store(PtpRole::Master);
    }
    update_sync_info();
}

void PtpEngine::update_sync_info() {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    sync_info_.offset_ns = static_cast<int64_t>(servo_.state().offset_ns);
    sync_info_.path_delay_ns = static_cast<int64_t>(servo_.state().path_delay_ns);
    sync_info_.freq_adj_ppb = servo_.state().freq_adj_ppb;
    sync_info_.synchronized = servo_.state().converged;
    sync_info_.role = role_.load();
    sync_info_.master_id = best_master_id_;
    sync_info_.sync_count = static_cast<uint64_t>(servo_.state().sample_count);

    if (sync_callback_) {
        sync_callback_(sync_info_);
    }
}

} // namespace soluna::sync
