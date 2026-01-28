/**
 * solctl — Soluna CLI control tool
 *
 * Phase 3: Full routing, stream, and device control.
 *
 * Usage:
 *   solctl devices                              - オーディオデバイス一覧
 *   solctl route add <src> <dst> [gain_db]      - ルート追加
 *   solctl route remove <src> <dst>             - ルート削除
 *   solctl route list                           - ルート一覧
 *   solctl route gain <src> <dst> <gain_db>     - ゲイン設定
 *   solctl route mute <src> <dst> [on|off]      - ミュート設定
 *   solctl stream create <src> <dst> [channels] - ストリーム作成
 *   solctl stream destroy <id>                  - ストリーム削除
 *   solctl stream list                          - ストリーム一覧
 *   solctl meter <channel>                      - メーター表示
 *   solctl status                               - システム状態
 *   solctl version                              - バージョン表示
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/soluna.h>
#include <soluna/pal/audio.h>
#include <soluna/control/discovery.h>
#include <soluna/control/session.h>
#include <soluna/control/routing.h>
#include <soluna/control/protocol.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace soluna;
using namespace soluna::control;

// Global control plane instances (in a real daemon, these live in solunad)
static Discovery g_discovery;
static SessionManager g_sessions;
static RoutingMatrix g_routing;

// Forward declarations for handle_command
namespace soluna::control {
ControlResponse handle_command(
    const ControlRequest& req,
    Discovery& discovery,
    SessionManager& sessions,
    RoutingMatrix& routing);
}

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s <command> [args...]\n"
        "\nデバイス:\n"
        "  devices                              オーディオデバイス一覧\n"
        "\nルーティング:\n"
        "  route add <src> <dst> [gain_db]      ルート追加 (例: route add devA:1 devB:1)\n"
        "  route remove <src> <dst>             ルート削除\n"
        "  route list                           全ルート一覧\n"
        "  route gain <src> <dst> <gain_db>     ゲイン設定 (dB)\n"
        "  route mute <src> <dst> [on|off]      ミュート切替\n"
        "\nストリーム:\n"
        "  stream create <src> <dst> [channels] ストリーム作成\n"
        "  stream destroy <id>                  ストリーム削除\n"
        "  stream list                          全ストリーム一覧\n"
        "\nモニタリング:\n"
        "  meter <channel>                      メーターレベル表示\n"
        "  status                               システム状態\n"
        "\nその他:\n"
        "  version                              バージョン表示\n"
        "  help                                 このヘルプ\n",
        prog);
}

static int cmd_devices() {
    auto devices = pal::AudioDevice::enumerate();
    printf("%-4s %-30s %6s %7s  %s\n", "IDX", "NAME", "IN_CH", "OUT_CH", "DEVICE_ID");
    printf("%-4s %-30s %6s %7s  %s\n", "---", "-----", "-----", "------", "---------");
    int idx = 0;
    for (const auto& d : devices) {
        printf("%-4d %-30s %6u %7u  %s\n",
            idx++, d.name.c_str(), d.max_input_channels, d.max_output_channels, d.id.c_str());
    }
    printf("\n合計: %zu デバイス\n", devices.size());
    return 0;
}

static int cmd_route(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: solctl route <add|remove|list|gain|mute> ...\n");
        return 1;
    }

    std::string sub = argv[2];

    if (sub == "add") {
        if (argc < 5) {
            fprintf(stderr, "Usage: solctl route add <source> <sink> [gain_db]\n");
            return 1;
        }
        float gain = (argc >= 6) ? std::stof(argv[5]) : 0.0f;
        auto src = ChannelId::parse(argv[3]);
        auto dst = ChannelId::parse(argv[4]);

        if (g_routing.add_route(src, dst, gain)) {
            printf("ルート追加: %s -> %s (%.1f dB)\n",
                src.to_string().c_str(), dst.to_string().c_str(), gain);
        } else {
            fprintf(stderr, "エラー: ルートは既に存在します\n");
            return 1;
        }
    } else if (sub == "remove") {
        if (argc < 5) {
            fprintf(stderr, "Usage: solctl route remove <source> <sink>\n");
            return 1;
        }
        auto src = ChannelId::parse(argv[3]);
        auto dst = ChannelId::parse(argv[4]);

        if (g_routing.remove_route(src, dst)) {
            printf("ルート削除: %s -> %s\n",
                src.to_string().c_str(), dst.to_string().c_str());
        } else {
            fprintf(stderr, "エラー: ルートが見つかりません\n");
            return 1;
        }
    } else if (sub == "list") {
        auto routes = g_routing.list_routes();
        if (routes.empty()) {
            printf("ルートなし\n");
            return 0;
        }
        printf("%-20s %-20s %8s %6s\n", "SOURCE", "SINK", "GAIN(dB)", "MUTE");
        printf("%-20s %-20s %8s %6s\n", "------", "----", "--------", "----");
        for (const auto& r : routes) {
            printf("%-20s %-20s %8.1f %6s\n",
                r.source.to_string().c_str(),
                r.sink.to_string().c_str(),
                r.gain_db,
                r.muted ? "ON" : "OFF");
        }
        printf("\n合計: %zu ルート\n", routes.size());
    } else if (sub == "gain") {
        if (argc < 6) {
            fprintf(stderr, "Usage: solctl route gain <source> <sink> <gain_db>\n");
            return 1;
        }
        auto src = ChannelId::parse(argv[3]);
        auto dst = ChannelId::parse(argv[4]);
        float gain = std::stof(argv[5]);

        if (g_routing.set_gain(src, dst, gain)) {
            printf("ゲイン変更: %s -> %s = %.1f dB\n",
                src.to_string().c_str(), dst.to_string().c_str(), gain);
        } else {
            fprintf(stderr, "エラー: ルートが見つかりません\n");
            return 1;
        }
    } else if (sub == "mute") {
        if (argc < 5) {
            fprintf(stderr, "Usage: solctl route mute <source> <sink> [on|off]\n");
            return 1;
        }
        auto src = ChannelId::parse(argv[3]);
        auto dst = ChannelId::parse(argv[4]);
        bool muted = true;
        if (argc >= 6) {
            muted = (std::string(argv[5]) != "off");
        }

        if (g_routing.set_mute(src, dst, muted)) {
            printf("ミュート %s: %s -> %s\n",
                muted ? "ON" : "OFF",
                src.to_string().c_str(), dst.to_string().c_str());
        } else {
            fprintf(stderr, "エラー: ルートが見つかりません\n");
            return 1;
        }
    } else {
        fprintf(stderr, "不明なサブコマンド: %s\n", sub.c_str());
        return 1;
    }

    return 0;
}

static int cmd_stream(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: solctl stream <create|destroy|list> ...\n");
        return 1;
    }

    std::string sub = argv[2];

    if (sub == "create") {
        if (argc < 5) {
            fprintf(stderr, "Usage: solctl stream create <source> <sink> [channels]\n");
            return 1;
        }
        uint32_t ch = (argc >= 6) ? static_cast<uint32_t>(std::stoi(argv[5])) : 1;
        uint16_t id = g_sessions.create_stream(argv[3], argv[4], ch);
        printf("ストリーム作成: ID=%u (%s -> %s, %uch)\n", id, argv[3], argv[4], ch);
    } else if (sub == "destroy") {
        if (argc < 4) {
            fprintf(stderr, "Usage: solctl stream destroy <id>\n");
            return 1;
        }
        uint16_t id = static_cast<uint16_t>(std::stoi(argv[3]));
        if (g_sessions.destroy_stream(id)) {
            printf("ストリーム削除: ID=%u\n", id);
        } else {
            fprintf(stderr, "エラー: ストリームが見つかりません\n");
            return 1;
        }
    } else if (sub == "list") {
        auto streams = g_sessions.list_streams();
        if (streams.empty()) {
            printf("ストリームなし\n");
            return 0;
        }
        printf("%-4s %-15s %-15s %4s %6s %s\n",
            "ID", "SOURCE", "SINK", "CH", "PORT", "STATE");
        printf("%-4s %-15s %-15s %4s %6s %s\n",
            "--", "------", "----", "--", "----", "-----");
        for (const auto& s : streams) {
            const char* state = (s.state == StreamState::Active) ? "active" :
                                (s.state == StreamState::Inactive) ? "inactive" : "error";
            printf("%-4u %-15s %-15s %4u %6u %s\n",
                s.stream_id, s.source_device.c_str(), s.sink_device.c_str(),
                s.channels, s.rtp_port, state);
        }
        printf("\n合計: %zu ストリーム\n", streams.size());
    } else {
        fprintf(stderr, "不明なサブコマンド: %s\n", sub.c_str());
        return 1;
    }

    return 0;
}

static int cmd_meter(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: solctl meter <device:channel>\n");
        return 1;
    }
    auto ch = ChannelId::parse(argv[2]);
    auto meter = g_routing.get_meter(ch);
    printf("チャンネル: %s\n", ch.to_string().c_str());
    printf("  ピーク:  %.1f dBFS\n", meter.peak_db);
    printf("  RMS:     %.1f dBFS\n", meter.rms_db);
    printf("  クリップ: %llu\n", static_cast<unsigned long long>(meter.clip_count));
    return 0;
}

static int cmd_status() {
    ControlRequest req;
    req.command = CommandType::Status;
    auto resp = handle_command(req, g_discovery, g_sessions, g_routing);

    auto devices = g_discovery.devices();
    auto streams = g_sessions.list_streams();
    auto routes = g_routing.list_routes();

    printf("Soluna v%d.%d.%d\n",
        SOLUNA_VERSION_MAJOR, SOLUNA_VERSION_MINOR, SOLUNA_VERSION_PATCH);
    printf("  デバイス:     %zu\n", devices.size());
    printf("  ストリーム:   %zu\n", streams.size());
    printf("  ルート:       %zu\n", routes.size());
    return 0;
}

static int cmd_version() {
    printf("Soluna v%d.%d.%d (Phase 3)\n",
        SOLUNA_VERSION_MAJOR, SOLUNA_VERSION_MINOR, SOLUNA_VERSION_PATCH);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "devices") {
        return cmd_devices();
    } else if (cmd == "route") {
        return cmd_route(argc, argv);
    } else if (cmd == "stream") {
        return cmd_stream(argc, argv);
    } else if (cmd == "meter") {
        return cmd_meter(argc, argv);
    } else if (cmd == "status") {
        return cmd_status();
    } else if (cmd == "version") {
        return cmd_version();
    } else if (cmd == "help" || cmd == "--help") {
        print_usage(argv[0]);
        return 0;
    } else {
        fprintf(stderr, "不明なコマンド: %s\n", cmd.c_str());
        print_usage(argv[0]);
        return 1;
    }
}
