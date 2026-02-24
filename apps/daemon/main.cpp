/**
 * solunad — Soluna Network Audio Daemon
 *
 * Network audio daemon with YAML configuration support.
 * Usage:
 *   solunad --config /etc/soluna/config.yaml
 *   solunad --tx --device hw:0 --dest 239.69.0.1:5004
 *   solunad --rx --device hw:0 --port 5004
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/soluna.h>
#include <soluna/pal/audio.h>
#include <soluna/pal/net.h>
#include <soluna/pal/time.h>
#include <soluna/pal/thread.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/pipeline/pipeline.h>
#include <soluna/transport/ostp.h>
#include <soluna/transport/packet_scheduler.h>
#include <soluna/transport/transport_manager.h>
#include <soluna/transport/aes67.h>
#include <soluna/config/config.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#ifdef __linux__
#include <arpa/inet.h>
#endif

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false);
}

struct DaemonConfig {
    bool tx_mode = false;
    bool rx_mode = false;
    bool aes67_mode = false;
    std::string audio_device;
    std::string dest_ip = soluna::kMulticastAudio;
    uint16_t dest_port = soluna::kPortRTPBase;
    uint16_t listen_port = soluna::kPortRTPBase;
    uint32_t sample_rate = soluna::kDefaultSampleRate;
    uint32_t channels = 1;
    uint32_t ssrc = 0x4F534E43; // "OSNC"
    uint16_t stream_id = 1;
    std::string config_file;

    // Security settings from config
    soluna::config::SecurityConfig security;

    // Apply settings from YAML config
    void apply_yaml_config(const soluna::config::Config& cfg) {
        if (!cfg.device.audio_device.empty() && cfg.device.audio_device != "default") {
            audio_device = cfg.device.audio_device;
        }
        if (cfg.network.rtp_base_port != 5004) {
            dest_port = cfg.network.rtp_base_port;
            listen_port = cfg.network.rtp_base_port;
        }
        if (!cfg.network.multicast_audio.empty()) {
            dest_ip = cfg.network.multicast_audio;
        }
        if (cfg.audio.sample_rate != 48000) {
            sample_rate = cfg.audio.sample_rate;
        }
        if (cfg.audio.channels != 2) {
            channels = cfg.audio.channels;
        }
        // Apply security settings
        security = cfg.security;
    }
};

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --config FILE     Load configuration from YAML file\n"
        "  --tx              Transmit mode (capture → network)\n"
        "  --rx              Receive mode (network → playback)\n"
        "  --aes67-mode      Use AES67-compatible RTP (no OSTP extensions)\n"
        "  --device DEV      Audio device (e.g., hw:0, default)\n"
        "  --dest IP:PORT    Destination (TX mode, default: 239.69.0.1:5004)\n"
        "  --port PORT       Listen port (RX mode, default: 5004)\n"
        "  --rate RATE       Sample rate (default: 48000)\n"
        "  --channels N      Channel count (default: 1)\n"
        "  --dtls            Enable DTLS encryption\n"
        "  --cert FILE       DTLS certificate file (PEM)\n"
        "  --key FILE        DTLS private key file (PEM)\n"
        "  --list-devices    List available audio devices\n"
        "  --generate-config Print default configuration to stdout\n"
        "  --help            Show this help\n",
        prog);
}

static bool parse_args(int argc, char** argv, DaemonConfig& cfg) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--tx") {
            cfg.tx_mode = true;
        } else if (arg == "--rx") {
            cfg.rx_mode = true;
        } else if (arg == "--aes67-mode") {
            cfg.aes67_mode = true;
        } else if (arg == "--dtls") {
            cfg.security.dtls_enabled = true;
        } else if (arg == "--cert" && i + 1 < argc) {
            cfg.security.certificate_path = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            cfg.security.private_key_path = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            cfg.config_file = argv[++i];
        } else if (arg == "--device" && i + 1 < argc) {
            cfg.audio_device = argv[++i];
        } else if (arg == "--dest" && i + 1 < argc) {
            std::string dest = argv[++i];
            auto colon = dest.rfind(':');
            if (colon != std::string::npos) {
                cfg.dest_ip = dest.substr(0, colon);
                cfg.dest_port = static_cast<uint16_t>(std::stoi(dest.substr(colon + 1)));
            } else {
                cfg.dest_ip = dest;
            }
        } else if (arg == "--port" && i + 1 < argc) {
            cfg.listen_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--rate" && i + 1 < argc) {
            cfg.sample_rate = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--channels" && i + 1 < argc) {
            cfg.channels = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--list-devices") {
            auto devices = soluna::pal::AudioDevice::enumerate();
            printf("Available audio devices:\n");
            for (const auto& d : devices) {
                printf("  %-30s  in:%u out:%u  [%s]\n",
                    d.name.c_str(), d.max_input_channels, d.max_output_channels, d.id.c_str());
            }
            std::exit(0);
        } else if (arg == "--generate-config") {
            auto default_cfg = soluna::config::Config::defaults();
            printf("%s", default_cfg.to_yaml().c_str());
            std::exit(0);
        } else if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return false;
        }
    }

    // Load config file if specified or search default paths
    if (!cfg.config_file.empty()) {
        auto result = soluna::config::ConfigLoader::load(cfg.config_file);
        if (!result.ok()) {
            fprintf(stderr, "Error loading config: %s\n", result.error().to_string().c_str());
            return false;
        }
        cfg.apply_yaml_config(result.value());
    } else {
        // Try default paths
        auto result = soluna::config::ConfigLoader::load_with_fallbacks(
            soluna::config::ConfigLoader::default_paths());
        if (result.ok()) {
            cfg.apply_yaml_config(result.value());
        }
    }

    if (!cfg.tx_mode && !cfg.rx_mode) {
        fprintf(stderr, "Error: specify --tx or --rx\n");
        return false;
    }

    return true;
}

static int run_tx(const DaemonConfig& cfg) {
    using namespace soluna;
    using namespace soluna::pal;
    using namespace soluna::pipeline;
    using namespace soluna::transport;

    constexpr uint32_t kFramesPerPacket = 480; // 10ms at 48kHz — robust for home LAN/WiFi
    const size_t frame_size = sizeof(int32_t) * cfg.channels; // S24 in 32-bit container

    // Ring buffer: 8 packets worth
    RingBuffer ring(kFramesPerPacket * 8, frame_size);

    // Audio device
    auto audio = AudioDevice::create();
    if (!audio) {
        fprintf(stderr, "Error: cannot create audio device\n");
        return 1;
    }

    AudioStreamConfig audio_cfg;
    audio_cfg.sample_rate = cfg.sample_rate;
    audio_cfg.channels = cfg.channels;
    audio_cfg.frames_per_buffer = kFramesPerPacket;
    audio_cfg.format = SampleFormat::S24_LE;

    if (!audio->open_input(cfg.audio_device, audio_cfg)) {
        fprintf(stderr, "Error: cannot open audio input device '%s'\n", cfg.audio_device.c_str());
        return 1;
    }

    // Conversion buffer (float from ALSA → S24 for network)
    std::vector<int32_t> conv_buf(kFramesPerPacket * cfg.channels);

    // Audio callback: capture → convert → ring buffer
    audio->start([&](float* buffer, uint32_t frame_count) {
        size_t samples = frame_count * cfg.channels;
        float_to_s24(buffer, conv_buf.data(), samples);
        ring.write(conv_buf.data(), frame_count);
    });

    // Create transport manager for optional DTLS
    TransportManager transport_mgr(cfg.security);

    // Create transport socket
    std::unique_ptr<TransportSocket> socket;
    if (cfg.security.dtls_enabled) {
        printf("solunad: DTLS encryption enabled\n");
        SocketAddress dest{cfg.dest_ip, cfg.dest_port};
        socket = transport_mgr.establish_secure_channel(dest);
        if (!socket) {
            fprintf(stderr, "Error: DTLS handshake failed\n");
            return 1;
        }
    } else {
        socket = transport_mgr.create_tx_socket();
        if (!socket) {
            fprintf(stderr, "Error: cannot create transport socket\n");
            return 1;
        }
    }
    socket->set_dscp(46);

    SocketAddress dest{cfg.dest_ip, cfg.dest_port};

    // TX loop
    PacketScheduler scheduler(PacketTier::LAN, cfg.sample_rate);
    scheduler.reset();

    std::vector<uint8_t> packet_buf(kMaxPacketSize);
    std::vector<int32_t> audio_buf(kFramesPerPacket * cfg.channels);
    uint64_t sequence = 0;
    uint32_t rtp_timestamp = 0;
    uint32_t media_ts = 0;

    const char* mode_str = cfg.aes67_mode ? "AES67" : "OSTP";
    const char* security_str = cfg.security.dtls_enabled ? " [DTLS]" : "";
    printf("solunad TX (%s%s): %s → %s:%u (%uHz, %uch)\n",
        mode_str, security_str,
        cfg.audio_device.c_str(), cfg.dest_ip.c_str(), cfg.dest_port,
        cfg.sample_rate, cfg.channels);

    while (g_running.load()) {
        scheduler.wait_next();

        if (ring.available_read() < kFramesPerPacket) {
            continue; // underrun, skip
        }

        ring.read(audio_buf.data(), kFramesPerPacket);

        size_t pkt_size = 0;

        if (cfg.aes67_mode) {
            // Build AES67-compatible RTP packet
            uint8_t payload_type = kPayloadTypeAES67_L24;
            size_t bytes_per_sample = 3; // 24-bit packed

            // Convert to big-endian 24-bit packed
            std::vector<uint8_t> payload(kFramesPerPacket * cfg.channels * bytes_per_sample);
            uint8_t* out = payload.data();
            for (size_t i = 0; i < kFramesPerPacket * cfg.channels; i++) {
                int32_t sample = audio_buf[i];
                *out++ = static_cast<uint8_t>((sample >> 16) & 0xFF);
                *out++ = static_cast<uint8_t>((sample >> 8) & 0xFF);
                *out++ = static_cast<uint8_t>(sample & 0xFF);
            }

            pkt_size = aes67_build_rtp_packet(
                packet_buf.data(), packet_buf.size(),
                cfg.ssrc, static_cast<uint16_t>(sequence & 0xFFFF),
                rtp_timestamp, payload_type,
                payload.data(), payload.size()
            );
        } else {
            // Build OSTP packet
            size_t payload_size = kFramesPerPacket * frame_size;
            uint16_t seq_lo = static_cast<uint16_t>(sequence & 0xFFFF);
            uint16_t seq_hi = static_cast<uint16_t>((sequence >> 16) & 0xFFFF);

            pkt_size = ostp_build_packet(
                packet_buf.data(), packet_buf.size(),
                cfg.ssrc, seq_lo, rtp_timestamp,
                kPayloadTypePCM24,
                cfg.stream_id, seq_hi, media_ts,
                audio_buf.data(), payload_size
            );
        }

        if (pkt_size > 0) {
            socket->send_to(packet_buf.data(), pkt_size, dest);
        }

        sequence++;
        rtp_timestamp += kFramesPerPacket;
        media_ts += static_cast<uint32_t>(
            (static_cast<uint64_t>(kFramesPerPacket) * 1'000'000'000ULL) / cfg.sample_rate);

        if (sequence % 1000 == 0) {
            printf("\rTX: %lu packets sent", static_cast<unsigned long>(sequence));
            fflush(stdout);
        }
    }

    audio->stop();
    printf("\nTX stopped. Total packets: %lu\n", static_cast<unsigned long>(sequence));
    return 0;
}

static int run_rx(const DaemonConfig& cfg) {
    using namespace soluna;
    using namespace soluna::pal;
    using namespace soluna::pipeline;
    using namespace soluna::transport;

    constexpr uint32_t kFramesPerPacket = 480; // 10ms packets
    const size_t frame_size = sizeof(int32_t) * cfg.channels;

    // Ring buffer: 30 packets = 300ms for jitter absorption
    constexpr uint32_t kRingPackets = 30;
    constexpr uint32_t kPrefillPackets = 8;  // pre-fill 80ms before starting playback
    constexpr uint32_t kRefillThreshold = 3; // re-prefill if buffer drops below 30ms
    RingBuffer ring(kFramesPerPacket * kRingPackets, frame_size);
    std::atomic<bool> prefilled{false};

    // Audio device
    auto audio = AudioDevice::create();
    if (!audio) {
        fprintf(stderr, "Error: cannot create audio device\n");
        return 1;
    }

    AudioStreamConfig audio_cfg;
    audio_cfg.sample_rate = cfg.sample_rate;
    audio_cfg.channels = cfg.channels;
    audio_cfg.frames_per_buffer = kFramesPerPacket;

    if (!audio->open_output(cfg.audio_device, audio_cfg)) {
        fprintf(stderr, "Error: cannot open audio output device '%s'\n", cfg.audio_device.c_str());
        return 1;
    }

    // Conversion buffer (S24 from network → float for playback)
    std::vector<float> conv_buf(kFramesPerPacket * cfg.channels);

    // Audio callback: ring buffer → convert → playback
    static uint64_t sine_phase = 0;
    bool sine_test = (std::getenv("SOLUNA_SINE_TEST") != nullptr);
    audio->start([&](float* buffer, uint32_t frame_count) {
        size_t samples = frame_count * cfg.channels;

        if (sine_test) {
            // Debug: generate 440Hz sine wave to test ALSA output
            for (uint32_t i = 0; i < frame_count; i++) {
                float s = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * sine_phase / 48000.0f);
                for (uint32_t ch = 0; ch < cfg.channels; ch++) {
                    buffer[i * cfg.channels + ch] = s;
                }
                sine_phase++;
            }
            return;
        }

        // Wait for prefill before starting playback to absorb jitter
        if (!prefilled.load()) {
            if (ring.available_read() < kFramesPerPacket * kPrefillPackets) {
                std::memset(buffer, 0, samples * sizeof(float));
                return;
            }
            prefilled.store(true);
        }

        std::vector<int32_t> s24_buf(samples);

        size_t read = ring.read(s24_buf.data(), frame_count);
        if (read < frame_count) {
            // Underrun: zero-fill
            std::memset(s24_buf.data() + read * cfg.channels, 0,
                (frame_count - read) * frame_size);
        }
        s24_to_float(s24_buf.data(), buffer, samples);
    });

    // Create transport manager for optional DTLS
    TransportManager transport_mgr(cfg.security);

    // Create transport socket
    auto socket = transport_mgr.create_rx_socket();
    if (!socket) {
        fprintf(stderr, "Error: cannot create transport socket\n");
        return 1;
    }

    if (!socket->bind(cfg.listen_port)) {
        fprintf(stderr, "Error: cannot bind to port %u\n", cfg.listen_port);
        return 1;
    }
    socket->join_multicast(cfg.dest_ip);
    socket->set_recv_timeout_ms(10);

    const char* mode_str = cfg.aes67_mode ? "AES67" : "Auto";
    const char* security_str = cfg.security.dtls_enabled ? " [DTLS]" : "";
    printf("solunad RX (%s%s): %s:%u → %s (%uHz, %uch)\n",
        mode_str, security_str,
        cfg.dest_ip.c_str(), cfg.listen_port, cfg.audio_device.c_str(),
        cfg.sample_rate, cfg.channels);

    std::vector<uint8_t> recv_buf(kMaxPacketSize);
    std::vector<int32_t> audio_buf(kMaxPayloadSize / sizeof(int32_t));
    uint64_t packets_received = 0;
    uint64_t aes67_packets = 0;
    uint64_t ostp_packets = 0;
    uint64_t sequence_errors = 0;
    int32_t last_seq = -1;

    while (g_running.load()) {
        SocketAddress src;
        int n = socket->recv_from(recv_buf.data(), recv_buf.size(), src);
        if (n <= 0) continue;

        // Check packet type
        if (static_cast<size_t>(n) < sizeof(RtpHeader)) continue;

        const RtpHeader* rtp_ptr = reinterpret_cast<const RtpHeader*>(recv_buf.data());
        bool is_aes67 = aes67_is_standard_packet(*rtp_ptr);

        size_t frames = 0;
        uint32_t full_seq = 0;

        if (is_aes67) {
            // AES67 packet
            RtpHeader rtp;
            std::memcpy(&rtp, recv_buf.data(), sizeof(RtpHeader));

            uint16_t sequence = ntohs(rtp.sequence);
            full_seq = sequence;

            const uint8_t* payload = recv_buf.data() + sizeof(RtpHeader);
            size_t payload_size = static_cast<size_t>(n) - sizeof(RtpHeader);

            // Convert payload
            if (rtp.pt == kPayloadTypeAES67_L24) {
                size_t samples = payload_size / 3;
                for (size_t i = 0; i < samples && i < audio_buf.size(); i++) {
                    int32_t sample = (static_cast<int32_t>(payload[i * 3]) << 16)
                                   | (static_cast<int32_t>(payload[i * 3 + 1]) << 8)
                                   | static_cast<int32_t>(payload[i * 3 + 2]);
                    if (sample & 0x800000) sample |= 0xFF000000;
                    audio_buf[i] = sample;
                }
                frames = samples / cfg.channels;
            } else if (rtp.pt == kPayloadTypeAES67_L16) {
                size_t samples = payload_size / 2;
                const int16_t* src_samples = reinterpret_cast<const int16_t*>(payload);
                for (size_t i = 0; i < samples && i < audio_buf.size(); i++) {
                    int16_t be_sample = src_samples[i];
                    int16_t sample = static_cast<int16_t>((be_sample >> 8) | (be_sample << 8));
                    audio_buf[i] = static_cast<int32_t>(sample) << 8;
                }
                frames = samples / cfg.channels;
            }

            ring.write(audio_buf.data(), frames);
            aes67_packets++;
        } else {
            // OSTP packet
            RtpHeader rtp;
            OstpHeader ostp;
            const uint8_t* payload = nullptr;
            size_t payload_size = 0;

            if (!ostp_parse_packet(recv_buf.data(), static_cast<size_t>(n),
                                   rtp, ostp, payload, payload_size)) {
                continue;
            }

            full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;
            frames = payload_size / frame_size;
            ring.write(payload, frames);
            ostp_packets++;
        }

        // Sequence check
        if (last_seq >= 0) {
            int32_t expected = (last_seq + 1) & 0xFFFF;
            if (static_cast<int32_t>(full_seq & 0xFFFF) != expected) {
                sequence_errors++;
            }
        }
        last_seq = static_cast<int32_t>(full_seq);
        packets_received++;

        if (packets_received % 1000 == 0) {
            printf("\rRX: %lu pkts (OSTP:%lu AES67:%lu), %lu seq errors, ring: %zu/%zu",
                static_cast<unsigned long>(packets_received),
                static_cast<unsigned long>(ostp_packets),
                static_cast<unsigned long>(aes67_packets),
                static_cast<unsigned long>(sequence_errors),
                ring.available_read(), ring.capacity());
            fflush(stdout);
        }
    }

    audio->stop();
    printf("\nRX stopped. Packets: %lu (OSTP:%lu, AES67:%lu), Errors: %lu\n",
        static_cast<unsigned long>(packets_received),
        static_cast<unsigned long>(ostp_packets),
        static_cast<unsigned long>(aes67_packets),
        static_cast<unsigned long>(sequence_errors));
    return 0;
}

int main(int argc, char** argv) {
    DaemonConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Soluna Daemon v%d.%d.%d\n",
        SOLUNA_VERSION_MAJOR, SOLUNA_VERSION_MINOR, SOLUNA_VERSION_PATCH);

    if (cfg.tx_mode) {
        return run_tx(cfg);
    } else {
        return run_rx(cfg);
    }
}
