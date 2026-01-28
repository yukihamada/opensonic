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

// Placeholder — REST API implementation will use libwebsockets in Phase 7.
// For Phase 3, the CLI communicates directly via the control protocol structs.

namespace soluna::control {

// Command handler: processes a ControlRequest and returns a ControlResponse.
// This is the core logic used by both REST API and CLI.
ControlResponse handle_command(
    const ControlRequest& req,
    Discovery& discovery,
    SessionManager& sessions,
    RoutingMatrix& routing)
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
            resp.data = "{\"version\":\"0.1.0\",\"phase\":7}";
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

        default:
            resp.success = false;
            resp.error = "unknown command";
            break;
    }

    return resp;
}

} // namespace soluna::control
