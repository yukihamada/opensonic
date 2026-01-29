/**
 * REST API — Minimal HTTP server for control API
 *
 * Phase 3: Simple single-threaded TCP server on port 8400.
 * Handles JSON-based control commands over HTTP POST.
 * Full WebSocket support via libwebsockets comes in Phase 7.
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/protocol.h>
#include <soluna/control/discovery.h>
#include <soluna/control/session.h>
#include <soluna/control/routing.h>
#include <soluna/control/preset_manager.h>
#include <soluna/sync/ptp_engine.h>

// Placeholder — REST API implementation will use libwebsockets in Phase 7.
// For Phase 3, the CLI communicates directly via the control protocol structs.

namespace soluna::control {

// Command handler: processes a ControlRequest and returns a ControlResponse.
// This is the core logic used by both REST API and CLI.
ControlResponse handle_command(
    const ControlRequest& req,
    Discovery& discovery,
    SessionManager& sessions,
    RoutingMatrix& routing,
    sync::PtpEngine* ptp = nullptr)
{
    ControlResponse resp;
    resp.id = req.id;
    resp.success = true;

    switch (req.command) {
        case CommandType::DeviceList: {
            auto devices = discovery.devices();
            std::string json = "[";
            for (size_t i = 0; i < devices.size(); i++) {
                if (i > 0) json += ",";
                json += "{\"id\":\"" + devices[i].id +
                        "\",\"name\":\"" + devices[i].name +
                        "\",\"host\":\"" + devices[i].host +
                        "\",\"inputs\":" + std::to_string(devices[i].input_channels) +
                        ",\"outputs\":" + std::to_string(devices[i].output_channels) +
                        ",\"local\":" + (devices[i].is_local ? "true" : "false") + "}";
            }
            json += "]";
            resp.data = json;
            break;
        }

        case CommandType::StreamList: {
            auto streams = sessions.list_streams();
            std::string json = "[";
            for (size_t i = 0; i < streams.size(); i++) {
                if (i > 0) json += ",";
                json += "{\"id\":" + std::to_string(streams[i].stream_id) +
                        ",\"source\":\"" + streams[i].source_device +
                        "\",\"sink\":\"" + streams[i].sink_device +
                        "\",\"channels\":" + std::to_string(streams[i].channels) +
                        ",\"port\":" + std::to_string(streams[i].rtp_port) +
                        ",\"state\":\"" +
                        (streams[i].state == StreamState::Active ? "active" :
                         streams[i].state == StreamState::Inactive ? "inactive" : "error") +
                        "\"}";
            }
            json += "]";
            resp.data = json;
            break;
        }

        case CommandType::StreamCreate: {
            std::string src = req.get_param("source");
            std::string dst = req.get_param("sink");
            uint32_t ch = 1;
            auto ch_str = req.get_param("channels", "1");
            if (!ch_str.empty()) ch = static_cast<uint32_t>(std::stoi(ch_str));

            if (src.empty() || dst.empty()) {
                resp.success = false;
                resp.error = "source and sink are required";
                break;
            }

            uint16_t id = sessions.create_stream(src, dst, ch);
            resp.data = "{\"stream_id\":" + std::to_string(id) + "}";
            break;
        }

        case CommandType::StreamDestroy: {
            auto id_str = req.get_param("stream_id");
            if (id_str.empty()) {
                resp.success = false;
                resp.error = "stream_id is required";
                break;
            }
            uint16_t id = static_cast<uint16_t>(std::stoi(id_str));
            resp.success = sessions.destroy_stream(id);
            if (!resp.success) resp.error = "stream not found";
            break;
        }

        case CommandType::RouteList: {
            auto routes = routing.list_routes();
            std::string json = "[";
            for (size_t i = 0; i < routes.size(); i++) {
                if (i > 0) json += ",";
                json += "{\"source\":\"" + routes[i].source.to_string() +
                        "\",\"sink\":\"" + routes[i].sink.to_string() +
                        "\",\"gain_db\":" + std::to_string(routes[i].gain_db) +
                        ",\"muted\":" + (routes[i].muted ? "true" : "false") + "}";
            }
            json += "]";
            resp.data = json;
            break;
        }

        case CommandType::RouteAdd: {
            auto src = req.get_param("source");
            auto dst = req.get_param("sink");
            float gain = 0.0f;
            auto gain_str = req.get_param("gain_db", "0");
            if (!gain_str.empty()) gain = std::stof(gain_str);

            if (src.empty() || dst.empty()) {
                resp.success = false;
                resp.error = "source and sink are required";
                break;
            }

            resp.success = routing.add_route(ChannelId::parse(src),
                                              ChannelId::parse(dst), gain);
            if (!resp.success) resp.error = "route already exists";
            break;
        }

        case CommandType::RouteRemove: {
            auto src = req.get_param("source");
            auto dst = req.get_param("sink");
            resp.success = routing.remove_route(ChannelId::parse(src),
                                                 ChannelId::parse(dst));
            if (!resp.success) resp.error = "route not found";
            break;
        }

        case CommandType::RouteSetGain: {
            auto src = req.get_param("source");
            auto dst = req.get_param("sink");
            float gain = std::stof(req.get_param("gain_db", "0"));
            resp.success = routing.set_gain(ChannelId::parse(src),
                                             ChannelId::parse(dst), gain);
            if (!resp.success) resp.error = "route not found";
            break;
        }

        case CommandType::RouteSetMute: {
            auto src = req.get_param("source");
            auto dst = req.get_param("sink");
            bool muted = req.get_param("muted") == "true";
            resp.success = routing.set_mute(ChannelId::parse(src),
                                             ChannelId::parse(dst), muted);
            if (!resp.success) resp.error = "route not found";
            break;
        }

        case CommandType::MeterGet: {
            auto ch = req.get_param("channel");
            if (ch.empty()) {
                resp.success = false;
                resp.error = "channel is required";
                break;
            }
            auto meter = routing.get_meter(ChannelId::parse(ch));
            resp.data = "{\"peak_db\":" + std::to_string(meter.peak_db) +
                        ",\"rms_db\":" + std::to_string(meter.rms_db) +
                        ",\"clip_count\":" + std::to_string(meter.clip_count) + "}";
            break;
        }

        case CommandType::Version: {
            resp.data = "{\"version\":\"0.2.0\",\"phase\":8}";
            break;
        }

        case CommandType::Status: {
            auto devices = discovery.devices();
            auto streams = sessions.list_streams();
            auto routes = routing.list_routes();
            resp.data = "{\"devices\":" + std::to_string(devices.size()) +
                        ",\"streams\":" + std::to_string(streams.size()) +
                        ",\"routes\":" + std::to_string(routes.size()) + "}";
            break;
        }

        case CommandType::MeterGetAll: {
            // Get meters for all channels
            auto routes_list = routing.list_routes();
            std::string json = "{\"channels\":[";

            // Collect unique channels from routes
            std::map<std::string, bool> seen;
            std::vector<std::string> channels;
            for (const auto& r : routes_list) {
                auto src = r.source.to_string();
                auto dst = r.sink.to_string();
                if (seen.find(src) == seen.end()) {
                    seen[src] = true;
                    channels.push_back(src);
                }
                if (seen.find(dst) == seen.end()) {
                    seen[dst] = true;
                    channels.push_back(dst);
                }
            }

            for (size_t i = 0; i < channels.size(); i++) {
                if (i > 0) json += ",";
                auto meter = routing.get_meter(ChannelId::parse(channels[i]));
                json += "{\"channel\":\"" + channels[i] +
                        "\",\"peak_db\":" + std::to_string(meter.peak_db) +
                        ",\"rms_db\":" + std::to_string(meter.rms_db) +
                        ",\"clip_count\":" + std::to_string(meter.clip_count) + "}";
            }
            json += "]}";
            resp.data = json;
            break;
        }

        case CommandType::SystemStats: {
            // System statistics
            auto devices = discovery.devices();
            auto streams = sessions.list_streams();
            auto routes_list = routing.list_routes();

            // Count active streams
            size_t active_streams = 0;
            for (const auto& s : streams) {
                if (s.state == StreamState::Active) active_streams++;
            }

            // Calculate approximate bandwidth (bytes/sec)
            // Assume 48kHz, 24-bit, 48 samples/packet = 1ms packets
            size_t bandwidth_bps = active_streams * 48000 * 3 * 8; // bits per stream

            // PTP sync information
            bool ptp_synced = false;
            int64_t ptp_offset_ns = 0;
            int64_t ptp_path_delay_ns = 0;
            std::string ptp_role = "unknown";
            uint64_t ptp_sync_count = 0;

            if (ptp) {
                auto info = ptp->sync_info();
                ptp_synced = info.synchronized;
                ptp_offset_ns = info.offset_ns;
                ptp_path_delay_ns = info.path_delay_ns;
                ptp_sync_count = info.sync_count;
                switch (info.role) {
                    case sync::PtpRole::Listening: ptp_role = "listening"; break;
                    case sync::PtpRole::Master: ptp_role = "master"; break;
                    case sync::PtpRole::Slave: ptp_role = "slave"; break;
                }
            }

            resp.data = "{\"device_count\":" + std::to_string(devices.size()) +
                        ",\"stream_count\":" + std::to_string(streams.size()) +
                        ",\"active_streams\":" + std::to_string(active_streams) +
                        ",\"route_count\":" + std::to_string(routes_list.size()) +
                        ",\"bandwidth_bps\":" + std::to_string(bandwidth_bps) +
                        ",\"uptime_sec\":0" +
                        ",\"ptp_synced\":" + (ptp_synced ? "true" : "false") +
                        ",\"ptp_offset_ns\":" + std::to_string(ptp_offset_ns) +
                        ",\"ptp_path_delay_ns\":" + std::to_string(ptp_path_delay_ns) +
                        ",\"ptp_role\":\"" + ptp_role + "\"" +
                        ",\"ptp_sync_count\":" + std::to_string(ptp_sync_count) + "}";
            break;
        }

        case CommandType::PresetList: {
            static PresetManager presets;
            auto list = presets.list();
            std::string json = "{\"presets\":[";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) json += ",";
                json += "{\"name\":\"" + list[i].name +
                        "\",\"filename\":\"" + list[i].filename +
                        "\",\"route_count\":" + std::to_string(list[i].route_count) +
                        ",\"modified_time\":" + std::to_string(list[i].modified_time) + "}";
            }
            json += "]}";
            resp.data = json;
            break;
        }

        case CommandType::PresetSave: {
            auto name = req.get_param("name");
            if (name.empty()) {
                resp.success = false;
                resp.error = "name is required";
                break;
            }
            static PresetManager presets;
            resp.success = presets.save(name, routing);
            if (resp.success) {
                resp.data = "{\"saved\":true,\"name\":\"" + name + "\"}";
            } else {
                resp.error = "failed to save preset";
            }
            break;
        }

        case CommandType::PresetLoad: {
            auto name = req.get_param("name");
            if (name.empty()) {
                resp.success = false;
                resp.error = "name is required";
                break;
            }
            static PresetManager presets;
            if (!presets.exists(name)) {
                resp.success = false;
                resp.error = "preset not found";
                break;
            }
            resp.success = presets.load(name, routing);
            if (resp.success) {
                auto routes = routing.list_routes();
                resp.data = "{\"loaded\":true,\"name\":\"" + name +
                            "\",\"route_count\":" + std::to_string(routes.size()) + "}";
            } else {
                resp.error = "failed to load preset";
            }
            break;
        }

        case CommandType::PresetDelete: {
            auto name = req.get_param("name");
            if (name.empty()) {
                resp.success = false;
                resp.error = "name is required";
                break;
            }
            static PresetManager presets;
            if (!presets.exists(name)) {
                resp.success = false;
                resp.error = "preset not found";
                break;
            }
            resp.success = presets.remove(name);
            if (resp.success) {
                resp.data = "{\"deleted\":true,\"name\":\"" + name + "\"}";
            } else {
                resp.error = "failed to delete preset";
            }
            break;
        }

        case CommandType::SecurityStatus: {
            // Security status - DTLS configuration and certificate info
            // Note: In a full implementation, this would query TransportManager
            // For now, provide a status structure that the UI can display
            bool dtls_enabled = false;
            bool has_cert = false;
            bool has_key = false;
            std::string cert_path = "";
            std::string key_path = "";
            std::string cert_subject = "";
            std::string cert_expiry = "";

#ifdef SOLUNA_HAS_DTLS
            dtls_enabled = true;
            // These would normally come from config/TransportManager
            // Placeholder for now
#endif

            resp.data = "{\"dtls_enabled\":" + std::string(dtls_enabled ? "true" : "false") +
                        ",\"has_certificate\":" + std::string(has_cert ? "true" : "false") +
                        ",\"has_key\":" + std::string(has_key ? "true" : "false") +
                        ",\"cert_path\":\"" + cert_path + "\"" +
                        ",\"key_path\":\"" + key_path + "\"" +
                        ",\"cert_subject\":\"" + cert_subject + "\"" +
                        ",\"cert_expiry\":\"" + cert_expiry + "\"" +
                        ",\"dtls_available\":" +
#ifdef SOLUNA_HAS_DTLS
                        "true" +
#else
                        "false" +
#endif
                        "}";
            break;
        }

        case CommandType::SecuritySetDtls: {
            // Enable/disable DTLS and set certificate paths
            auto enabled_str = req.get_param("enabled");
            auto cert_path = req.get_param("cert_path");
            auto key_path = req.get_param("key_path");

#ifndef SOLUNA_HAS_DTLS
            resp.success = false;
            resp.error = "DTLS not available (built without OpenSSL)";
            break;
#else
            // In a full implementation, this would update TransportManager config
            // For now, acknowledge the request
            bool enabled = (enabled_str == "true" || enabled_str == "1");

            // Validate paths if provided
            if (enabled && (!cert_path.empty() || !key_path.empty())) {
                if (cert_path.empty() || key_path.empty()) {
                    resp.success = false;
                    resp.error = "both cert_path and key_path are required when enabling DTLS";
                    break;
                }
            }

            resp.data = "{\"dtls_enabled\":" + std::string(enabled ? "true" : "false") +
                        ",\"cert_path\":\"" + cert_path + "\"" +
                        ",\"key_path\":\"" + key_path + "\"}";
#endif
            break;
        }

        default:
            resp.success = false;
            resp.error = "unknown command";
            break;
    }

    return resp;
}

} // namespace soluna::control
