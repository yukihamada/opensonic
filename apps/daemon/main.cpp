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
#include <soluna/config/config.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false);
}

struct DaemonConfig {
    bool tx_mode = false;
    bool rx_mode = false;
    std::string audio_device;
    std::string dest_ip = soluna::kMulticastAudio;
    uint16_t dest_port = soluna::kPortRTPBase;
    uint16_t listen_port = soluna::kPortRTPBase;
    uint32_t sample_rate = soluna::kDefaultSampleRate;
    uint32_t channels = 1;
    uint32_t ssrc = 0x4F534E43; // "OSNC"
    uint16_t stream_id = 1;
    std::string config_file;

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
    }
};

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --config FILE     Load configuration from YAML file\n"
        "  --tx              Transmit mode (capture → network)\n"
        "  --rx              Receive mode (network → playback)\n"
        "  --device DEV      Audio device (e.g., hw:0, default)\n"
        "  --dest IP:PORT    Destination (TX mode, default: 239.69.0.1:5004)\n"
        "  --port PORT       Listen port (RX mode, default: 5004)\n"
        "  --rate RATE       Sample rate (default: 48000)\n"
        "  --channels N      Channel count (default: 1)\n"
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

    constexpr uint32_t kFramesPerPacket = 48; // 1ms at 48kHz
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

    // Network socket
    auto socket = UdpSocket::create();
    if (!socket) {
        fprintf(stderr, "Error: cannot create UDP socket\n");
        return 1;
    }
    socket->set_dscp(46);

    SocketAddress dest{cfg.dest_ip, cfg.dest_port};

    // TX loop
    PacketScheduler scheduler(PacketTier::Standard, cfg.sample_rate);
    scheduler.reset();

    std::vector<uint8_t> packet_buf(kMaxPacketSize);
    std::vector<int32_t> audio_buf(kFramesPerPacket * cfg.channels);
    uint64_t sequence = 0;
    uint32_t rtp_timestamp = 0;
    uint32_t media_ts = 0;

    printf("solunad TX: %s → %s:%u (%uHz, %uch)\n",
        cfg.audio_device.c_str(), cfg.dest_ip.c_str(), cfg.dest_port,
        cfg.sample_rate, cfg.channels);

    while (g_running.load()) {
        scheduler.wait_next();

        if (ring.available_read() < kFramesPerPacket) {
            continue; // underrun, skip
        }

        ring.read(audio_buf.data(), kFramesPerPacket);

        size_t payload_size = kFramesPerPacket * frame_size;
        uint16_t seq_lo = static_cast<uint16_t>(sequence & 0xFFFF);
        uint16_t seq_hi = static_cast<uint16_t>((sequence >> 16) & 0xFFFF);

        size_t pkt_size = ostp_build_packet(
            packet_buf.data(), packet_buf.size(),
            cfg.ssrc, seq_lo, rtp_timestamp,
            kPayloadTypePCM24,
            cfg.stream_id, seq_hi, media_ts,
            audio_buf.data(), payload_size
        );

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

    constexpr uint32_t kFramesPerPacket = 48;
    const size_t frame_size = sizeof(int32_t) * cfg.channels;

    // Ring buffer: 8 packets for jitter absorption
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

    if (!audio->open_output(cfg.audio_device, audio_cfg)) {
        fprintf(stderr, "Error: cannot open audio output device '%s'\n", cfg.audio_device.c_str());
        return 1;
    }

    // Conversion buffer (S24 from network → float for playback)
    std::vector<float> conv_buf(kFramesPerPacket * cfg.channels);

    // Audio callback: ring buffer → convert → playback
    audio->start([&](float* buffer, uint32_t frame_count) {
        size_t samples = frame_count * cfg.channels;
        std::vector<int32_t> s24_buf(samples);

        size_t read = ring.read(s24_buf.data(), frame_count);
        if (read < frame_count) {
            // Underrun: zero-fill
            std::memset(s24_buf.data() + read * cfg.channels, 0,
                (frame_count - read) * frame_size);
        }
        s24_to_float(s24_buf.data(), buffer, samples);
    });

    // Network socket
    auto socket = UdpSocket::create();
    if (!socket) {
        fprintf(stderr, "Error: cannot create UDP socket\n");
        return 1;
    }

    if (!socket->bind(cfg.listen_port)) {
        fprintf(stderr, "Error: cannot bind to port %u\n", cfg.listen_port);
        return 1;
    }
    socket->join_multicast(cfg.dest_ip);
    socket->set_recv_timeout_ms(10);

    printf("solunad RX: %s:%u → %s (%uHz, %uch)\n",
        cfg.dest_ip.c_str(), cfg.listen_port, cfg.audio_device.c_str(),
        cfg.sample_rate, cfg.channels);

    std::vector<uint8_t> recv_buf(kMaxPacketSize);
    uint64_t packets_received = 0;
    uint64_t sequence_errors = 0;
    int32_t last_seq = -1;

    while (g_running.load()) {
        SocketAddress src;
        int n = socket->recv_from(recv_buf.data(), recv_buf.size(), src);
        if (n <= 0) continue;

        RtpHeader rtp;
        OstpHeader ostp;
        const uint8_t* payload = nullptr;
        size_t payload_size = 0;

        if (!ostp_parse_packet(recv_buf.data(), static_cast<size_t>(n),
                               rtp, ostp, payload, payload_size)) {
            continue;
        }

        // Sequence check
        uint32_t full_seq = (static_cast<uint32_t>(ostp.sequence_ext) << 16) | rtp.sequence;
        if (last_seq >= 0) {
            int32_t expected = last_seq + 1;
            if (static_cast<int32_t>(full_seq) != expected) {
                sequence_errors++;
            }
        }
        last_seq = static_cast<int32_t>(full_seq);
        packets_received++;

        // Write audio to ring buffer
        size_t frames = payload_size / frame_size;
        ring.write(payload, frames);

        if (packets_received % 1000 == 0) {
            printf("\rRX: %lu packets, %lu seq errors, ring: %zu/%zu",
                static_cast<unsigned long>(packets_received),
                static_cast<unsigned long>(sequence_errors),
                ring.available_read(), ring.capacity());
            fflush(stdout);
        }
    }

    audio->stop();
    printf("\nRX stopped. Packets: %lu, Errors: %lu\n",
        static_cast<unsigned long>(packets_received),
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
