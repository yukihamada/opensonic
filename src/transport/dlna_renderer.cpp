/**
 * DLNA/UPnP Media Renderer implementation
 *
 * Self-contained UPnP AV MediaRenderer with:
 * - SSDP discovery responder + periodic NOTIFY
 * - HTTP server for device description XML + SOAP control
 * - AVTransport, RenderingControl, ConnectionManager services
 * - HTTP audio stream fetcher with PCM/FLAC/MP3 content type detection
 *
 * No external XML library — all XML is string templates.
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef SOLUNA_HAS_DLNA

#include <soluna/transport/dlna.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// minimp3 for MP3 decoding (header-only, already fetched by CMake)
// When built as part of soluna_core, file_source.cpp provides MINIMP3_IMPLEMENTATION.
// For standalone builds (linux-rx), define it here.
#ifdef SOLUNA_HAS_MINIMP3
#ifdef SOLUNA_DLNA_MINIMP3_IMPL
#define MINIMP3_IMPLEMENTATION
#endif
#include <minimp3.h>
#endif

namespace soluna::transport {

// ── Utility ──────────────────────────────────────────────────────────────────

const char* dlna_transport_state_str(DlnaTransportState state) {
    switch (state) {
        case DlnaTransportState::NoMediaPresent:  return "NO_MEDIA_PRESENT";
        case DlnaTransportState::Stopped:         return "STOPPED";
        case DlnaTransportState::Playing:         return "PLAYING";
        case DlnaTransportState::PausedPlayback:  return "PAUSED_PLAYBACK";
        case DlnaTransportState::Transitioning:   return "TRANSITIONING";
    }
    return "STOPPED";
}

static std::string generate_uuid() {
    // Simple UUID v4 using random bytes
    unsigned char buf[16];
    std::srand(static_cast<unsigned>(std::time(nullptr)) ^
               static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (int i = 0; i < 16; i++) buf[i] = static_cast<unsigned char>(std::rand() & 0xFF);
    buf[6] = (buf[6] & 0x0F) | 0x40;  // version 4
    buf[8] = (buf[8] & 0x3F) | 0x80;  // variant 1
    char uuid[40];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             buf[0], buf[1], buf[2], buf[3],
             buf[4], buf[5], buf[6], buf[7],
             buf[8], buf[9], buf[10], buf[11],
             buf[12], buf[13], buf[14], buf[15]);
    return uuid;
}

static std::string get_local_ip() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "127.0.0.1";
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
    connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    struct sockaddr_in local{};
    socklen_t len = sizeof(local);
    getsockname(sock, reinterpret_cast<struct sockaddr*>(&local), &len);
    close(sock);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
    return ip;
}

// ── Minimal SOAP XML parsing ─────────────────────────────────────────────────

static std::string extract_xml_tag(const std::string& xml, const std::string& tag) {
    // Find <tag> or <ns:tag> content
    // Try exact tag first
    std::string open1 = "<" + tag + ">";
    std::string close1 = "</" + tag + ">";
    auto pos = xml.find(open1);
    if (pos != std::string::npos) {
        pos += open1.size();
        auto end = xml.find(close1, pos);
        if (end != std::string::npos) return xml.substr(pos, end - pos);
    }
    // Try with any namespace prefix
    for (size_t i = 0; i < xml.size(); i++) {
        if (xml[i] == '<') {
            auto gt = xml.find('>', i);
            if (gt == std::string::npos) break;
            std::string tagname = xml.substr(i + 1, gt - i - 1);
            // Strip attributes
            auto sp = tagname.find(' ');
            if (sp != std::string::npos) tagname = tagname.substr(0, sp);
            // Check if tag ends with :desiredTag
            auto colon = tagname.find(':');
            std::string local_tag = (colon != std::string::npos) ?
                tagname.substr(colon + 1) : tagname;
            if (local_tag == tag) {
                size_t start = gt + 1;
                // Find closing tag
                std::string close_prefix = "</" + tagname + ">";
                // Also try without ns
                std::string close_local = "</" + tag + ">";
                auto end1 = xml.find(close_prefix, start);
                auto end2 = xml.find(close_local, start);
                size_t end = std::string::npos;
                if (end1 != std::string::npos && end2 != std::string::npos)
                    end = std::min(end1, end2);
                else if (end1 != std::string::npos)
                    end = end1;
                else
                    end = end2;
                if (end != std::string::npos) return xml.substr(start, end - start);
            }
        }
    }
    return "";
}

static std::string extract_soap_action(const std::string& headers) {
    // SOAPAction header: "urn:schemas-upnp-org:service:AVTransport:1#Play"
    auto pos = headers.find("SOAPAction:");
    if (pos == std::string::npos) pos = headers.find("SOAPACTION:");
    if (pos == std::string::npos) pos = headers.find("soapaction:");
    if (pos == std::string::npos) return "";
    pos += 11; // skip "SOAPAction:"
    while (pos < headers.size() && headers[pos] == ' ') pos++;
    auto end = headers.find("\r\n", pos);
    if (end == std::string::npos) end = headers.size();
    std::string val = headers.substr(pos, end - pos);
    // Strip quotes
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
        val = val.substr(1, val.size() - 2);
    // Extract action name after #
    auto hash = val.rfind('#');
    if (hash != std::string::npos) return val.substr(hash + 1);
    return val;
}

// ── XML Templates ────────────────────────────────────────────────────────────

static std::string build_device_description(const std::string& friendly_name,
                                             const std::string& manufacturer,
                                             const std::string& model_name,
                                             const std::string& udn,
                                             const std::string& base_url) {
    return
        "<?xml version=\"1.0\"?>\r\n"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
        "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
        "  <URLBase>" + base_url + "</URLBase>\r\n"
        "  <device>\r\n"
        "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\r\n"
        "    <friendlyName>" + friendly_name + "</friendlyName>\r\n"
        "    <manufacturer>" + manufacturer + "</manufacturer>\r\n"
        "    <modelName>" + model_name + "</modelName>\r\n"
        "    <modelDescription>Soluna Open Network Audio Renderer</modelDescription>\r\n"
        "    <UDN>uuid:" + udn + "</UDN>\r\n"
        "    <serviceList>\r\n"
        "      <service>\r\n"
        "        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>\r\n"
        "        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>\r\n"
        "        <SCPDURL>/AVTransport.xml</SCPDURL>\r\n"
        "        <controlURL>/control/AVTransport</controlURL>\r\n"
        "        <eventSubURL>/event/AVTransport</eventSubURL>\r\n"
        "      </service>\r\n"
        "      <service>\r\n"
        "        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>\r\n"
        "        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>\r\n"
        "        <SCPDURL>/RenderingControl.xml</SCPDURL>\r\n"
        "        <controlURL>/control/RenderingControl</controlURL>\r\n"
        "        <eventSubURL>/event/RenderingControl</eventSubURL>\r\n"
        "      </service>\r\n"
        "      <service>\r\n"
        "        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>\r\n"
        "        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>\r\n"
        "        <SCPDURL>/ConnectionManager.xml</SCPDURL>\r\n"
        "        <controlURL>/control/ConnectionManager</controlURL>\r\n"
        "        <eventSubURL>/event/ConnectionManager</eventSubURL>\r\n"
        "      </service>\r\n"
        "    </serviceList>\r\n"
        "  </device>\r\n"
        "</root>\r\n";
}

static const char* kAvTransportScpd =
    "<?xml version=\"1.0\"?>\r\n"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
    "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
    "  <actionList>\r\n"
    "    <action><name>SetAVTransportURI</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action><name>Play</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action><name>Stop</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action><name>Pause</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action><name>GetTransportInfo</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action><name>GetPositionInfo</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Track</name><direction>out</direction><relatedStateVariable>CurrentTrack</relatedStateVariable></argument>\r\n"
    "        <argument><name>TrackDuration</name><direction>out</direction><relatedStateVariable>CurrentTrackDuration</relatedStateVariable></argument>\r\n"
    "        <argument><name>TrackMetaData</name><direction>out</direction><relatedStateVariable>CurrentTrackMetaData</relatedStateVariable></argument>\r\n"
    "        <argument><name>TrackURI</name><direction>out</direction><relatedStateVariable>CurrentTrackURI</relatedStateVariable></argument>\r\n"
    "        <argument><name>RelTime</name><direction>out</direction><relatedStateVariable>RelativeTimePosition</relatedStateVariable></argument>\r\n"
    "        <argument><name>AbsTime</name><direction>out</direction><relatedStateVariable>AbsoluteTimePosition</relatedStateVariable></argument>\r\n"
    "        <argument><name>RelCount</name><direction>out</direction><relatedStateVariable>RelativeCounterPosition</relatedStateVariable></argument>\r\n"
    "        <argument><name>AbsCount</name><direction>out</direction><relatedStateVariable>AbsoluteCounterPosition</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "  </actionList>\r\n"
    "  <serviceStateTable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"yes\"><name>TransportState</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"yes\"><name>TransportStatus</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>TransportPlaySpeed</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>CurrentTrack</name><dataType>ui4</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>CurrentTrackDuration</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>CurrentTrackMetaData</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>CurrentTrackURI</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>RelativeTimePosition</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>AbsoluteTimePosition</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>RelativeCounterPosition</name><dataType>i4</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>AbsoluteCounterPosition</name><dataType>i4</dataType></stateVariable>\r\n"
    "  </serviceStateTable>\r\n"
    "</scpd>\r\n";

static const char* kRenderingControlScpd =
    "<?xml version=\"1.0\"?>\r\n"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
    "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
    "  <actionList>\r\n"
    "    <action><name>GetVolume</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action><name>SetVolume</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>\r\n"
    "        <argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action><name>GetMute</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>\r\n"
    "        <argument><name>CurrentMute</name><direction>out</direction><relatedStateVariable>Mute</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "    <action><name>SetMute</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>\r\n"
    "        <argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>\r\n"
    "        <argument><name>DesiredMute</name><direction>in</direction><relatedStateVariable>Mute</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "  </actionList>\r\n"
    "  <serviceStateTable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Channel</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"yes\"><name>Volume</name><dataType>ui2</dataType><allowedValueRange><minimum>0</minimum><maximum>100</maximum></allowedValueRange></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"yes\"><name>Mute</name><dataType>boolean</dataType></stateVariable>\r\n"
    "  </serviceStateTable>\r\n"
    "</scpd>\r\n";

static const char* kConnectionManagerScpd =
    "<?xml version=\"1.0\"?>\r\n"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
    "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
    "  <actionList>\r\n"
    "    <action><name>GetProtocolInfo</name>\r\n"
    "      <argumentList>\r\n"
    "        <argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>\r\n"
    "        <argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>\r\n"
    "      </argumentList>\r\n"
    "    </action>\r\n"
    "  </actionList>\r\n"
    "  <serviceStateTable>\r\n"
    "    <stateVariable sendEvents=\"yes\"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>\r\n"
    "    <stateVariable sendEvents=\"yes\"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>\r\n"
    "  </serviceStateTable>\r\n"
    "</scpd>\r\n";

static const char* kSinkProtocolInfo =
    "http-get:*:audio/L16:*,"
    "http-get:*:audio/L24:*,"
    "http-get:*:audio/x-flac:*,"
    "http-get:*:audio/mpeg:*,"
    "http-get:*:audio/wav:*,"
    "http-get:*:audio/x-wav:*";

// ── SOAP response helpers ────────────────────────────────────────────────────

static std::string soap_response(const std::string& service_type,
                                  const std::string& action,
                                  const std::string& body) {
    return
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
        "  <s:Body>\r\n"
        "    <u:" + action + "Response xmlns:u=\"" + service_type + "\">\r\n"
        + body +
        "    </u:" + action + "Response>\r\n"
        "  </s:Body>\r\n"
        "</s:Envelope>\r\n";
}

static std::string http_response(int code, const std::string& content_type,
                                  const std::string& body) {
    const char* reason = (code == 200) ? "OK" : (code == 404) ? "Not Found" : "Internal Server Error";
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << " " << reason << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

// ── Audio content type detection ─────────────────────────────────────────────

enum class AudioContentType {
    Unknown,
    PCM_L16,    // audio/L16;rate=48000;channels=2
    PCM_L24,    // audio/L24;rate=48000;channels=2
    FLAC,       // audio/x-flac
    MP3,        // audio/mpeg
    WAV,        // audio/wav, audio/x-wav
};

static AudioContentType detect_content_type(const std::string& content_type,
                                              uint32_t& rate, uint32_t& channels) {
    // Default
    rate = 48000;
    channels = 2;

    std::string ct = content_type;
    std::transform(ct.begin(), ct.end(), ct.begin(), ::tolower);

    if (ct.find("audio/l16") != std::string::npos) {
        // Parse rate and channels from content-type params
        auto rpos = ct.find("rate=");
        if (rpos != std::string::npos) rate = static_cast<uint32_t>(std::atoi(ct.c_str() + rpos + 5));
        auto cpos = ct.find("channels=");
        if (cpos != std::string::npos) channels = static_cast<uint32_t>(std::atoi(ct.c_str() + cpos + 9));
        return AudioContentType::PCM_L16;
    }
    if (ct.find("audio/l24") != std::string::npos) {
        auto rpos = ct.find("rate=");
        if (rpos != std::string::npos) rate = static_cast<uint32_t>(std::atoi(ct.c_str() + rpos + 5));
        auto cpos = ct.find("channels=");
        if (cpos != std::string::npos) channels = static_cast<uint32_t>(std::atoi(ct.c_str() + cpos + 9));
        return AudioContentType::PCM_L24;
    }
    if (ct.find("audio/x-flac") != std::string::npos || ct.find("audio/flac") != std::string::npos)
        return AudioContentType::FLAC;
    if (ct.find("audio/mpeg") != std::string::npos || ct.find("audio/mp3") != std::string::npos)
        return AudioContentType::MP3;
    if (ct.find("audio/wav") != std::string::npos || ct.find("audio/x-wav") != std::string::npos)
        return AudioContentType::WAV;

    return AudioContentType::Unknown;
}

// ── URL parsing ──────────────────────────────────────────────────────────────

struct ParsedUrl {
    std::string host;
    uint16_t    port = 80;
    std::string path;
};

static bool parse_url(const std::string& url, ParsedUrl& out) {
    // http://host:port/path
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    size_t host_start = scheme_end + 3;
    auto path_start = url.find('/', host_start);
    std::string hostport;
    if (path_start != std::string::npos) {
        hostport = url.substr(host_start, path_start - host_start);
        out.path = url.substr(path_start);
    } else {
        hostport = url.substr(host_start);
        out.path = "/";
    }
    auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = static_cast<uint16_t>(std::atoi(hostport.c_str() + colon + 1));
    } else {
        out.host = hostport;
        out.port = (url.substr(0, scheme_end) == "https") ? 443 : 80;
    }
    return !out.host.empty();
}

// ── WAV header parsing ───────────────────────────────────────────────────────

struct WavHeader {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_offset = 0;
    bool valid = false;
};

static WavHeader parse_wav_header(const uint8_t* data, size_t len) {
    WavHeader hdr;
    if (len < 44) return hdr;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return hdr;

    // Find "fmt " chunk
    size_t pos = 12;
    while (pos + 8 < len) {
        uint32_t chunk_size;
        memcpy(&chunk_size, data + pos + 4, 4);
        if (memcmp(data + pos, "fmt ", 4) == 0 && pos + 8 + chunk_size <= len) {
            uint16_t audio_format;
            memcpy(&audio_format, data + pos + 8, 2);
            memcpy(&hdr.channels, data + pos + 10, 2);
            memcpy(&hdr.sample_rate, data + pos + 12, 4);
            memcpy(&hdr.bits_per_sample, data + pos + 22, 2);
            if (audio_format == 1) { // PCM
                hdr.valid = true;
            }
        }
        if (memcmp(data + pos, "data", 4) == 0) {
            hdr.data_offset = static_cast<uint32_t>(pos + 8);
            break;
        }
        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++; // padding
    }
    return hdr;
}

// ── Implementation ───────────────────────────────────────────────────────────

struct DlnaRenderer::Impl {
    Config               config;
    DlnaAudioCallback    audio_cb;
    std::string          uuid;
    std::string          local_ip;
    std::atomic<bool>    running{false};
    std::atomic<bool>    stop_requested{false};

    // Transport state
    std::mutex           state_mutex;
    DlnaTransportState   transport_state = DlnaTransportState::NoMediaPresent;
    DlnaTransportStatus  transport_status = DlnaTransportStatus::OK;
    std::string          current_uri;
    std::string          current_uri_metadata;
    std::atomic<int>     volume{100};
    std::atomic<bool>    muted{false};
    std::atomic<bool>    paused{false};

    // Position tracking
    std::atomic<uint64_t> play_start_ms{0};
    std::atomic<uint64_t> pause_offset_ms{0};

    // Threads
    std::thread          ssdp_thread;
    std::thread          http_thread;
    std::thread          stream_thread;
    int                  http_listen_fd = -1;
    uint16_t             actual_http_port = 0;

    // ── SSDP ────────────────────────────────────────────────────────────

    void ssdp_loop() {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            fprintf(stderr, "[dlna] SSDP socket failed: %s\n", strerror(errno));
            return;
        }

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

        struct sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = htons(kSsdpPort);

        if (bind(sock, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            fprintf(stderr, "[dlna] SSDP bind failed: %s\n", strerror(errno));
            close(sock);
            return;
        }

        // Join SSDP multicast group
        struct ip_mreq mreq{};
        inet_pton(AF_INET, kSsdpMulticastAddr, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            fprintf(stderr, "[dlna] SSDP multicast join failed: %s\n", strerror(errno));
            close(sock);
            return;
        }

        // Set receive timeout for periodic announce
        struct timeval tv{};
        tv.tv_sec = 30;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Send initial NOTIFY alive
        send_ssdp_alive(sock);

        uint8_t buf[2048];
        while (!stop_requested.load()) {
            struct sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                                 reinterpret_cast<struct sockaddr*>(&from), &from_len);
            if (n <= 0) {
                // Timeout — send periodic alive
                if (!stop_requested.load()) send_ssdp_alive(sock);
                continue;
            }
            buf[n] = 0;
            std::string msg(reinterpret_cast<char*>(buf), static_cast<size_t>(n));

            if (msg.find("M-SEARCH") != std::string::npos) {
                handle_msearch(sock, msg, from);
            }
        }

        // Send byebye before exit
        send_ssdp_byebye(sock);
        close(sock);
    }

    void handle_msearch(int sock, const std::string& msg, const struct sockaddr_in& from) {
        // Check if the search target matches our device
        bool match = false;
        if (msg.find("ssdp:all") != std::string::npos) match = true;
        if (msg.find("upnp:rootdevice") != std::string::npos) match = true;
        if (msg.find("urn:schemas-upnp-org:device:MediaRenderer:1") != std::string::npos) match = true;
        if (msg.find("urn:schemas-upnp-org:service:AVTransport:1") != std::string::npos) match = true;
        if (msg.find("urn:schemas-upnp-org:service:RenderingControl:1") != std::string::npos) match = true;
        if (msg.find("urn:schemas-upnp-org:service:ConnectionManager:1") != std::string::npos) match = true;
        if (!match) return;

        std::string location = "http://" + local_ip + ":" + std::to_string(actual_http_port) + "/description.xml";

        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "CACHE-CONTROL: max-age=1800\r\n"
            "LOCATION: " + location + "\r\n"
            "SERVER: Soluna/1.0 UPnP/1.0 DLNA/1.50\r\n"
            "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
            "USN: uuid:" + uuid + "::urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
            "EXT:\r\n"
            "\r\n";

        // Small random delay (0-100ms) to avoid response storms
        std::this_thread::sleep_for(std::chrono::milliseconds(std::rand() % 100));

        sendto(sock, response.c_str(), response.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&from), sizeof(from));
    }

    void send_ssdp_alive(int sock) {
        std::string location = "http://" + local_ip + ":" + std::to_string(actual_http_port) + "/description.xml";

        std::string notify =
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "CACHE-CONTROL: max-age=1800\r\n"
            "LOCATION: " + location + "\r\n"
            "NT: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
            "NTS: ssdp:alive\r\n"
            "SERVER: Soluna/1.0 UPnP/1.0 DLNA/1.50\r\n"
            "USN: uuid:" + uuid + "::urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
            "\r\n";

        struct sockaddr_in mcast{};
        mcast.sin_family = AF_INET;
        mcast.sin_port = htons(kSsdpPort);
        inet_pton(AF_INET, kSsdpMulticastAddr, &mcast.sin_addr);

        sendto(sock, notify.c_str(), notify.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&mcast), sizeof(mcast));
    }

    void send_ssdp_byebye(int sock) {
        std::string notify =
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "NT: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
            "NTS: ssdp:byebye\r\n"
            "USN: uuid:" + uuid + "::urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
            "\r\n";

        struct sockaddr_in mcast{};
        mcast.sin_family = AF_INET;
        mcast.sin_port = htons(kSsdpPort);
        inet_pton(AF_INET, kSsdpMulticastAddr, &mcast.sin_addr);

        sendto(sock, notify.c_str(), notify.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&mcast), sizeof(mcast));
    }

    // ── HTTP Server ─────────────────────────────────────────────────────

    bool start_http_server() {
        http_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (http_listen_fd < 0) {
            fprintf(stderr, "[dlna] HTTP socket failed: %s\n", strerror(errno));
            return false;
        }

        int reuse = 1;
        setsockopt(http_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(config.http_port);

        if (bind(http_listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            fprintf(stderr, "[dlna] HTTP bind failed: %s\n", strerror(errno));
            close(http_listen_fd);
            http_listen_fd = -1;
            return false;
        }

        // Get actual port if auto-assigned
        struct sockaddr_in bound{};
        socklen_t bound_len = sizeof(bound);
        getsockname(http_listen_fd, reinterpret_cast<struct sockaddr*>(&bound), &bound_len);
        actual_http_port = ntohs(bound.sin_port);

        if (listen(http_listen_fd, 8) < 0) {
            fprintf(stderr, "[dlna] HTTP listen failed: %s\n", strerror(errno));
            close(http_listen_fd);
            http_listen_fd = -1;
            return false;
        }

        return true;
    }

    void http_loop() {
        while (!stop_requested.load()) {
            // Use select with timeout so we can check stop_requested
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(http_listen_fd, &fds);
            struct timeval tv{};
            tv.tv_sec = 1;
            int sel = select(http_listen_fd + 1, &fds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;

            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(http_listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) continue;

            // Read HTTP request (simple: read until double CRLF)
            std::string request;
            char buf[4096];
            // Set read timeout
            struct timeval rtv{};
            rtv.tv_sec = 5;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
                ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                request.append(buf, static_cast<size_t>(n));
            }

            // Read body if Content-Length present
            std::string body;
            auto cl_pos = request.find("Content-Length:");
            if (cl_pos == std::string::npos) cl_pos = request.find("content-length:");
            if (cl_pos != std::string::npos) {
                size_t cl_val_start = cl_pos + 15;
                while (cl_val_start < request.size() && request[cl_val_start] == ' ') cl_val_start++;
                int content_length = std::atoi(request.c_str() + cl_val_start);
                auto header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    size_t body_start = header_end + 4;
                    body = request.substr(body_start);
                    while (static_cast<int>(body.size()) < content_length) {
                        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
                        if (n <= 0) break;
                        body.append(buf, static_cast<size_t>(n));
                    }
                }
            }

            std::string response = handle_http_request(request, body);
            // Send response (ignore partial write for simplicity)
            size_t sent = 0;
            while (sent < response.size()) {
                ssize_t n = send(client_fd, response.c_str() + sent, response.size() - sent, 0);
                if (n <= 0) break;
                sent += static_cast<size_t>(n);
            }
            close(client_fd);
        }

        if (http_listen_fd >= 0) {
            close(http_listen_fd);
            http_listen_fd = -1;
        }
    }

    std::string handle_http_request(const std::string& request, const std::string& body) {
        // Parse method and path
        auto sp1 = request.find(' ');
        auto sp2 = request.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos)
            return http_response(400, "text/plain", "Bad Request");

        std::string method = request.substr(0, sp1);
        std::string path   = request.substr(sp1 + 1, sp2 - sp1 - 1);

        if (method == "GET") {
            if (path == "/description.xml") {
                std::string base_url = "http://" + local_ip + ":" + std::to_string(actual_http_port);
                std::string xml = build_device_description(
                    config.friendly_name, config.manufacturer,
                    config.model_name, uuid, base_url);
                return http_response(200, "text/xml; charset=\"utf-8\"", xml);
            }
            if (path == "/AVTransport.xml") {
                return http_response(200, "text/xml; charset=\"utf-8\"", kAvTransportScpd);
            }
            if (path == "/RenderingControl.xml") {
                return http_response(200, "text/xml; charset=\"utf-8\"", kRenderingControlScpd);
            }
            if (path == "/ConnectionManager.xml") {
                return http_response(200, "text/xml; charset=\"utf-8\"", kConnectionManagerScpd);
            }
            return http_response(404, "text/plain", "Not Found");
        }

        if (method == "POST") {
            if (path == "/control/AVTransport") {
                return handle_av_transport(request, body);
            }
            if (path == "/control/RenderingControl") {
                return handle_rendering_control(request, body);
            }
            if (path == "/control/ConnectionManager") {
                return handle_connection_manager(request, body);
            }
            return http_response(404, "text/plain", "Not Found");
        }

        // SUBSCRIBE for eventing (minimal — just accept)
        if (method == "SUBSCRIBE") {
            return "HTTP/1.1 200 OK\r\nSID: uuid:" + uuid + "\r\nTIMEOUT: Second-1800\r\nContent-Length: 0\r\n\r\n";
        }

        return http_response(404, "text/plain", "Not Found");
    }

    // ── AVTransport SOAP handling ───────────────────────────────────────

    std::string handle_av_transport(const std::string& headers, const std::string& body) {
        std::string action = extract_soap_action(headers);
        const std::string svc = "urn:schemas-upnp-org:service:AVTransport:1";

        if (action == "SetAVTransportURI") {
            std::string uri = extract_xml_tag(body, "CurrentURI");
            std::string meta = extract_xml_tag(body, "CurrentURIMetaData");
            return handle_set_uri(uri, meta, svc);
        }
        if (action == "Play") {
            return handle_play(svc);
        }
        if (action == "Stop") {
            return handle_stop(svc);
        }
        if (action == "Pause") {
            return handle_pause(svc);
        }
        if (action == "GetTransportInfo") {
            return handle_get_transport_info(svc);
        }
        if (action == "GetPositionInfo") {
            return handle_get_position_info(svc);
        }

        // Unknown action — return empty success
        return http_response(200, "text/xml; charset=\"utf-8\"",
                             soap_response(svc, action, ""));
    }

    std::string handle_set_uri(const std::string& uri, const std::string& meta,
                                const std::string& svc) {
        fprintf(stderr, "[dlna] SetAVTransportURI: %s\n", uri.c_str());
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            // Stop any existing stream
            if (transport_state == DlnaTransportState::Playing ||
                transport_state == DlnaTransportState::PausedPlayback) {
                stop_stream();
            }
            current_uri = uri;
            current_uri_metadata = meta;
            transport_state = DlnaTransportState::Stopped;
            transport_status = DlnaTransportStatus::OK;
        }
        return http_response(200, "text/xml; charset=\"utf-8\"",
                             soap_response(svc, "SetAVTransportURI", ""));
    }

    std::string handle_play(const std::string& svc) {
        fprintf(stderr, "[dlna] Play\n");
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (current_uri.empty()) {
                return http_response(200, "text/xml; charset=\"utf-8\"",
                                     soap_response(svc, "Play", ""));
            }
            if (transport_state == DlnaTransportState::PausedPlayback) {
                paused.store(false);
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                play_start_ms.store(static_cast<uint64_t>(now_ms) - pause_offset_ms.load());
                transport_state = DlnaTransportState::Playing;
            } else if (transport_state == DlnaTransportState::Stopped ||
                       transport_state == DlnaTransportState::NoMediaPresent) {
                transport_state = DlnaTransportState::Transitioning;
                start_stream(current_uri);
            }
            // Already playing — ignore
        }
        return http_response(200, "text/xml; charset=\"utf-8\"",
                             soap_response(svc, "Play", ""));
    }

    std::string handle_stop(const std::string& svc) {
        fprintf(stderr, "[dlna] Stop\n");
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            stop_stream();
            transport_state = DlnaTransportState::Stopped;
            pause_offset_ms.store(0);
        }
        return http_response(200, "text/xml; charset=\"utf-8\"",
                             soap_response(svc, "Stop", ""));
    }

    std::string handle_pause(const std::string& svc) {
        fprintf(stderr, "[dlna] Pause\n");
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (transport_state == DlnaTransportState::Playing) {
                paused.store(true);
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                pause_offset_ms.store(static_cast<uint64_t>(now_ms) - play_start_ms.load());
                transport_state = DlnaTransportState::PausedPlayback;
            }
        }
        return http_response(200, "text/xml; charset=\"utf-8\"",
                             soap_response(svc, "Pause", ""));
    }

    std::string handle_get_transport_info(const std::string& svc) {
        DlnaTransportState st;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            st = transport_state;
        }
        std::string body =
            "      <CurrentTransportState>" + std::string(dlna_transport_state_str(st)) + "</CurrentTransportState>\r\n"
            "      <CurrentTransportStatus>OK</CurrentTransportStatus>\r\n"
            "      <CurrentSpeed>1</CurrentSpeed>\r\n";
        return http_response(200, "text/xml; charset=\"utf-8\"",
                             soap_response(svc, "GetTransportInfo", body));
    }

    std::string format_time(uint64_t ms) {
        uint64_t secs = ms / 1000;
        uint64_t h = secs / 3600;
        uint64_t m = (secs % 3600) / 60;
        uint64_t s = secs % 60;
        char buf[16];
        snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                 (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
        return buf;
    }

    std::string handle_get_position_info(const std::string& svc) {
        uint64_t elapsed = 0;
        std::string uri;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            uri = current_uri;
            if (transport_state == DlnaTransportState::Playing) {
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                elapsed = static_cast<uint64_t>(now_ms) - play_start_ms.load();
            } else if (transport_state == DlnaTransportState::PausedPlayback) {
                elapsed = pause_offset_ms.load();
            }
        }
        std::string time_str = format_time(elapsed);
        std::string body =
            "      <Track>1</Track>\r\n"
            "      <TrackDuration>0:00:00</TrackDuration>\r\n"
            "      <TrackMetaData></TrackMetaData>\r\n"
            "      <TrackURI>" + uri + "</TrackURI>\r\n"
            "      <RelTime>" + time_str + "</RelTime>\r\n"
            "      <AbsTime>" + time_str + "</AbsTime>\r\n"
            "      <RelCount>0</RelCount>\r\n"
            "      <AbsCount>0</AbsCount>\r\n";
        return http_response(200, "text/xml; charset=\"utf-8\"",
                             soap_response(svc, "GetPositionInfo", body));
    }

    // ── RenderingControl SOAP handling ──────────────────────────────────

    std::string handle_rendering_control(const std::string& headers, const std::string& body) {
        std::string action = extract_soap_action(headers);
        const std::string svc = "urn:schemas-upnp-org:service:RenderingControl:1";

        if (action == "GetVolume") {
            std::string resp_body =
                "      <CurrentVolume>" + std::to_string(volume.load()) + "</CurrentVolume>\r\n";
            return http_response(200, "text/xml; charset=\"utf-8\"",
                                 soap_response(svc, "GetVolume", resp_body));
        }
        if (action == "SetVolume") {
            std::string vol_str = extract_xml_tag(body, "DesiredVolume");
            if (!vol_str.empty()) {
                int v = std::atoi(vol_str.c_str());
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                volume.store(v);
                fprintf(stderr, "[dlna] SetVolume: %d\n", v);
            }
            return http_response(200, "text/xml; charset=\"utf-8\"",
                                 soap_response(svc, "SetVolume", ""));
        }
        if (action == "GetMute") {
            std::string resp_body =
                "      <CurrentMute>" + std::string(muted.load() ? "1" : "0") + "</CurrentMute>\r\n";
            return http_response(200, "text/xml; charset=\"utf-8\"",
                                 soap_response(svc, "GetMute", resp_body));
        }
        if (action == "SetMute") {
            std::string mute_str = extract_xml_tag(body, "DesiredMute");
            muted.store(mute_str == "1" || mute_str == "true" || mute_str == "True");
            fprintf(stderr, "[dlna] SetMute: %s\n", muted.load() ? "on" : "off");
            return http_response(200, "text/xml; charset=\"utf-8\"",
                                 soap_response(svc, "SetMute", ""));
        }

        return http_response(200, "text/xml; charset=\"utf-8\"",
                             soap_response(svc, action, ""));
    }

    // ── ConnectionManager SOAP handling ─────────────────────────────────

    std::string handle_connection_manager(const std::string& headers, const std::string& /*body*/) {
        std::string action = extract_soap_action(headers);
        const std::string svc = "urn:schemas-upnp-org:service:ConnectionManager:1";

        if (action == "GetProtocolInfo") {
            std::string resp_body =
                "      <Source></Source>\r\n"
                "      <Sink>" + std::string(kSinkProtocolInfo) + "</Sink>\r\n";
            return http_response(200, "text/xml; charset=\"utf-8\"",
                                 soap_response(svc, "GetProtocolInfo", resp_body));
        }

        return http_response(200, "text/xml; charset=\"utf-8\"",
                             soap_response(svc, action, ""));
    }

    // ── Audio stream fetch & decode ─────────────────────────────────────

    void start_stream(const std::string& uri) {
        // Stop existing stream thread if any
        stop_stream_thread();

        paused.store(false);
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        play_start_ms.store(static_cast<uint64_t>(now_ms));
        pause_offset_ms.store(0);

        stream_thread = std::thread([this, uri]() {
            stream_audio(uri);
        });
    }

    void stop_stream() {
        paused.store(false);
        stop_stream_thread();
    }

    void stop_stream_thread() {
        // Signal stream thread to stop by setting state
        // (stream_audio checks transport_state and stop_requested)
        if (stream_thread.joinable()) {
            // We set transport_state before calling this, the stream loop will exit
            stream_thread.detach(); // Don't block the SOAP response
        }
    }

    void stream_audio(const std::string& uri) {
        ParsedUrl parsed;
        if (!parse_url(uri, parsed)) {
            fprintf(stderr, "[dlna] Invalid URI: %s\n", uri.c_str());
            std::lock_guard<std::mutex> lock(state_mutex);
            transport_state = DlnaTransportState::Stopped;
            transport_status = DlnaTransportStatus::ErrorOccurred;
            return;
        }

        // Resolve host
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(parsed.host.c_str(), std::to_string(parsed.port).c_str(), &hints, &res) != 0 || !res) {
            fprintf(stderr, "[dlna] DNS resolution failed for %s\n", parsed.host.c_str());
            std::lock_guard<std::mutex> lock(state_mutex);
            transport_state = DlnaTransportState::Stopped;
            transport_status = DlnaTransportStatus::ErrorOccurred;
            return;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            freeaddrinfo(res);
            std::lock_guard<std::mutex> lock(state_mutex);
            transport_state = DlnaTransportState::Stopped;
            return;
        }

        // Connect timeout (5s)
        struct timeval tv{};
        tv.tv_sec = 5;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
            fprintf(stderr, "[dlna] Connect failed to %s:%d: %s\n",
                    parsed.host.c_str(), parsed.port, strerror(errno));
            close(sock);
            freeaddrinfo(res);
            std::lock_guard<std::mutex> lock(state_mutex);
            transport_state = DlnaTransportState::Stopped;
            transport_status = DlnaTransportStatus::ErrorOccurred;
            return;
        }
        freeaddrinfo(res);

        // Send HTTP GET
        std::string req = "GET " + parsed.path + " HTTP/1.1\r\n"
                          "Host: " + parsed.host + "\r\n"
                          "Accept: audio/*\r\n"
                          "Connection: close\r\n"
                          "\r\n";
        send(sock, req.c_str(), req.size(), 0);

        // Read HTTP response headers
        std::string headers;
        char buf[8192];
        while (headers.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                close(sock);
                std::lock_guard<std::mutex> lock(state_mutex);
                transport_state = DlnaTransportState::Stopped;
                return;
            }
            headers.append(buf, static_cast<size_t>(n));
        }

        // Extract content-type
        std::string content_type;
        auto ct_pos = headers.find("Content-Type:");
        if (ct_pos == std::string::npos) ct_pos = headers.find("content-type:");
        if (ct_pos != std::string::npos) {
            size_t start = ct_pos + 13;
            while (start < headers.size() && headers[start] == ' ') start++;
            auto end = headers.find("\r\n", start);
            content_type = headers.substr(start, end - start);
        }

        // Get any body data already received after headers
        auto header_end = headers.find("\r\n\r\n");
        std::string initial_body;
        if (header_end + 4 < headers.size()) {
            initial_body = headers.substr(header_end + 4);
        }

        // Set transport to playing
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            transport_state = DlnaTransportState::Playing;
        }

        uint32_t stream_rate = config.sample_rate;
        uint32_t stream_channels = config.channels;
        AudioContentType ct = detect_content_type(content_type, stream_rate, stream_channels);

        fprintf(stderr, "[dlna] Streaming: type=%s rate=%u ch=%u\n",
                content_type.c_str(), stream_rate, stream_channels);

        // Read and decode audio data
        switch (ct) {
            case AudioContentType::PCM_L16:
                stream_pcm_l16(sock, initial_body, stream_rate, stream_channels);
                break;
            case AudioContentType::PCM_L24:
                stream_pcm_l24(sock, initial_body, stream_rate, stream_channels);
                break;
            case AudioContentType::WAV:
                stream_wav(sock, initial_body, stream_rate, stream_channels);
                break;
            case AudioContentType::MP3:
                stream_mp3(sock, initial_body, stream_rate, stream_channels);
                break;
            case AudioContentType::FLAC:
                // FLAC requires a decoder; for now treat as raw PCM fallback
                fprintf(stderr, "[dlna] FLAC streaming not yet implemented, treating as L16\n");
                stream_pcm_l16(sock, initial_body, stream_rate, stream_channels);
                break;
            case AudioContentType::Unknown:
                fprintf(stderr, "[dlna] Unknown content type: %s\n", content_type.c_str());
                break;
        }

        close(sock);

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (transport_state == DlnaTransportState::Playing) {
                transport_state = DlnaTransportState::Stopped;
            }
        }
        fprintf(stderr, "[dlna] Stream ended\n");
    }

    bool should_stop_stream() {
        if (stop_requested.load()) return true;
        std::lock_guard<std::mutex> lock(state_mutex);
        return transport_state != DlnaTransportState::Playing &&
               transport_state != DlnaTransportState::PausedPlayback;
    }

    // Apply volume scaling to samples
    void apply_volume(int32_t* samples, size_t count) {
        if (muted.load()) {
            std::memset(samples, 0, count * sizeof(int32_t));
            return;
        }
        int vol = volume.load();
        if (vol == 100) return;
        float scale = static_cast<float>(vol) / 100.0f;
        for (size_t i = 0; i < count; i++) {
            samples[i] = static_cast<int32_t>(static_cast<float>(samples[i]) * scale);
        }
    }

    // ── PCM L16 (big-endian 16-bit) stream ──────────────────────────────

    void stream_pcm_l16(int sock, const std::string& initial,
                         uint32_t rate, uint32_t channels) {
        std::vector<uint8_t> raw_buf;
        raw_buf.insert(raw_buf.end(), initial.begin(), initial.end());

        const size_t frame_bytes = 2 * channels;  // 16-bit per channel
        const size_t chunk_frames = 480;  // 10ms at 48kHz
        std::vector<int32_t> pcm(chunk_frames * channels);

        char net_buf[8192];
        while (!should_stop_stream()) {
            if (paused.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            ssize_t n = recv(sock, net_buf, sizeof(net_buf), 0);
            if (n <= 0) break;
            raw_buf.insert(raw_buf.end(), net_buf, net_buf + n);

            while (raw_buf.size() >= frame_bytes * chunk_frames) {
                // Convert big-endian S16 to S24 in S32 container
                const uint8_t* src = raw_buf.data();
                for (size_t i = 0; i < chunk_frames * channels; i++) {
                    int16_t s16 = static_cast<int16_t>((src[i * 2] << 8) | src[i * 2 + 1]);
                    pcm[i] = static_cast<int32_t>(s16) << 8; // Scale to 24-bit range
                }
                apply_volume(pcm.data(), chunk_frames * channels);
                if (audio_cb) audio_cb(pcm.data(), chunk_frames, channels, rate);
                raw_buf.erase(raw_buf.begin(), raw_buf.begin() + static_cast<ptrdiff_t>(frame_bytes * chunk_frames));
            }
        }
    }

    // ── PCM L24 (big-endian 24-bit) stream ──────────────────────────────

    void stream_pcm_l24(int sock, const std::string& initial,
                         uint32_t rate, uint32_t channels) {
        std::vector<uint8_t> raw_buf;
        raw_buf.insert(raw_buf.end(), initial.begin(), initial.end());

        const size_t frame_bytes = 3 * channels;  // 24-bit per channel
        const size_t chunk_frames = 480;
        std::vector<int32_t> pcm(chunk_frames * channels);

        char net_buf[8192];
        while (!should_stop_stream()) {
            if (paused.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            ssize_t n = recv(sock, net_buf, sizeof(net_buf), 0);
            if (n <= 0) break;
            raw_buf.insert(raw_buf.end(), net_buf, net_buf + n);

            while (raw_buf.size() >= frame_bytes * chunk_frames) {
                const uint8_t* src = raw_buf.data();
                for (size_t i = 0; i < chunk_frames * channels; i++) {
                    // Big-endian 24-bit to signed 32-bit
                    int32_t s = (static_cast<int32_t>(src[i * 3]) << 16) |
                                (static_cast<int32_t>(src[i * 3 + 1]) << 8) |
                                static_cast<int32_t>(src[i * 3 + 2]);
                    // Sign-extend from 24-bit
                    if (s & 0x800000) s |= static_cast<int32_t>(0xFF000000);
                    pcm[i] = s;
                }
                apply_volume(pcm.data(), chunk_frames * channels);
                if (audio_cb) audio_cb(pcm.data(), chunk_frames, channels, rate);
                raw_buf.erase(raw_buf.begin(), raw_buf.begin() + static_cast<ptrdiff_t>(frame_bytes * chunk_frames));
            }
        }
    }

    // ── WAV stream ──────────────────────────────────────────────────────

    void stream_wav(int sock, const std::string& initial,
                     uint32_t /*rate*/, uint32_t /*channels*/) {
        // Accumulate enough for WAV header
        std::vector<uint8_t> buf;
        buf.insert(buf.end(), initial.begin(), initial.end());

        char net_buf[8192];
        while (buf.size() < 128 && !should_stop_stream()) {
            ssize_t n = recv(sock, net_buf, sizeof(net_buf), 0);
            if (n <= 0) return;
            buf.insert(buf.end(), net_buf, net_buf + n);
        }

        WavHeader hdr = parse_wav_header(buf.data(), buf.size());
        if (!hdr.valid) {
            fprintf(stderr, "[dlna] Invalid WAV header\n");
            return;
        }

        fprintf(stderr, "[dlna] WAV: %uHz %uch %ubit\n",
                hdr.sample_rate, hdr.channels, hdr.bits_per_sample);

        // Skip to data and route to appropriate PCM decoder
        std::string remaining(buf.begin() + hdr.data_offset, buf.end());
        if (hdr.bits_per_sample == 16) {
            // WAV PCM is little-endian, not big-endian like L16
            stream_wav_pcm16_le(sock, remaining, hdr.sample_rate, hdr.channels);
        } else if (hdr.bits_per_sample == 24) {
            stream_wav_pcm24_le(sock, remaining, hdr.sample_rate, hdr.channels);
        } else {
            fprintf(stderr, "[dlna] Unsupported WAV bit depth: %u\n", hdr.bits_per_sample);
        }
    }

    void stream_wav_pcm16_le(int sock, const std::string& initial,
                              uint32_t rate, uint32_t channels) {
        std::vector<uint8_t> raw_buf;
        raw_buf.insert(raw_buf.end(), initial.begin(), initial.end());

        const size_t frame_bytes = 2 * channels;
        const size_t chunk_frames = 480;
        std::vector<int32_t> pcm(chunk_frames * channels);

        char net_buf[8192];
        while (!should_stop_stream()) {
            if (paused.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            ssize_t n = recv(sock, net_buf, sizeof(net_buf), 0);
            if (n <= 0) break;
            raw_buf.insert(raw_buf.end(), net_buf, net_buf + n);

            while (raw_buf.size() >= frame_bytes * chunk_frames) {
                const uint8_t* src = raw_buf.data();
                for (size_t i = 0; i < chunk_frames * channels; i++) {
                    // Little-endian S16
                    int16_t s16 = static_cast<int16_t>(src[i * 2] | (src[i * 2 + 1] << 8));
                    pcm[i] = static_cast<int32_t>(s16) << 8;
                }
                apply_volume(pcm.data(), chunk_frames * channels);
                if (audio_cb) audio_cb(pcm.data(), chunk_frames, channels, rate);
                raw_buf.erase(raw_buf.begin(), raw_buf.begin() + static_cast<ptrdiff_t>(frame_bytes * chunk_frames));
            }
        }
    }

    void stream_wav_pcm24_le(int sock, const std::string& initial,
                              uint32_t rate, uint32_t channels) {
        std::vector<uint8_t> raw_buf;
        raw_buf.insert(raw_buf.end(), initial.begin(), initial.end());

        const size_t frame_bytes = 3 * channels;
        const size_t chunk_frames = 480;
        std::vector<int32_t> pcm(chunk_frames * channels);

        char net_buf[8192];
        while (!should_stop_stream()) {
            if (paused.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            ssize_t n = recv(sock, net_buf, sizeof(net_buf), 0);
            if (n <= 0) break;
            raw_buf.insert(raw_buf.end(), net_buf, net_buf + n);

            while (raw_buf.size() >= frame_bytes * chunk_frames) {
                const uint8_t* src = raw_buf.data();
                for (size_t i = 0; i < chunk_frames * channels; i++) {
                    // Little-endian 24-bit
                    int32_t s = static_cast<int32_t>(src[i * 3]) |
                                (static_cast<int32_t>(src[i * 3 + 1]) << 8) |
                                (static_cast<int32_t>(src[i * 3 + 2]) << 16);
                    if (s & 0x800000) s |= static_cast<int32_t>(0xFF000000);
                    pcm[i] = s;
                }
                apply_volume(pcm.data(), chunk_frames * channels);
                if (audio_cb) audio_cb(pcm.data(), chunk_frames, channels, rate);
                raw_buf.erase(raw_buf.begin(), raw_buf.begin() + static_cast<ptrdiff_t>(frame_bytes * chunk_frames));
            }
        }
    }

    // ── MP3 stream (via minimp3) ────────────────────────────────────────

    void stream_mp3(int sock, const std::string& initial,
                     uint32_t /*rate*/, uint32_t /*channels*/) {
#ifdef SOLUNA_HAS_MINIMP3
        mp3dec_t mp3d;
        mp3dec_init(&mp3d);

        std::vector<uint8_t> raw_buf;
        raw_buf.insert(raw_buf.end(), initial.begin(), initial.end());

        mp3dec_frame_info_t info;
        mp3d_sample_t mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        std::vector<int32_t> out_pcm;

        char net_buf[8192];
        while (!should_stop_stream()) {
            if (paused.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            ssize_t n = recv(sock, net_buf, sizeof(net_buf), 0);
            if (n <= 0) break;
            raw_buf.insert(raw_buf.end(), net_buf, net_buf + n);

            // Decode frames from buffer
            while (raw_buf.size() > MINIMP3_MAX_SAMPLES_PER_FRAME) {
                int samples = mp3dec_decode_frame(&mp3d, raw_buf.data(),
                                                   static_cast<int>(raw_buf.size()),
                                                   mp3_pcm, &info);
                if (info.frame_bytes == 0) break; // Need more data
                raw_buf.erase(raw_buf.begin(), raw_buf.begin() + info.frame_bytes);

                if (samples > 0 && info.channels > 0) {
                    size_t total = static_cast<size_t>(samples) * static_cast<size_t>(info.channels);
                    out_pcm.resize(total);
                    // minimp3 outputs S16 — scale to S24 range
                    for (size_t i = 0; i < total; i++) {
                        out_pcm[i] = static_cast<int32_t>(mp3_pcm[i]) << 8;
                    }
                    apply_volume(out_pcm.data(), total);
                    if (audio_cb) {
                        audio_cb(out_pcm.data(), static_cast<size_t>(samples),
                                 static_cast<uint32_t>(info.channels),
                                 static_cast<uint32_t>(info.hz));
                    }
                }
            }
        }
#else
        (void)sock; (void)initial;
        fprintf(stderr, "[dlna] MP3 decoding requires SOLUNA_HAS_MINIMP3\n");
        std::lock_guard<std::mutex> lock(state_mutex);
        transport_state = DlnaTransportState::Stopped;
        transport_status = DlnaTransportStatus::ErrorOccurred;
#endif
    }
};

// ── DlnaRenderer public API ──────────────────────────────────────────────────

DlnaRenderer::DlnaRenderer() : impl_(std::make_unique<Impl>()) {}
DlnaRenderer::~DlnaRenderer() { stop(); }

bool DlnaRenderer::start(const Config& config, DlnaAudioCallback audio_callback) {
    if (impl_->running.load()) return false;

    impl_->config = config;
    impl_->audio_cb = std::move(audio_callback);
    impl_->uuid = generate_uuid();
    impl_->local_ip = get_local_ip();
    impl_->stop_requested.store(false);

    // Start HTTP server first (need port for SSDP responses)
    if (!impl_->start_http_server()) return false;

    impl_->running.store(true);

    fprintf(stderr, "[dlna] Starting DLNA renderer \"%s\" at http://%s:%u\n",
            config.friendly_name.c_str(),
            impl_->local_ip.c_str(),
            impl_->actual_http_port);

    // Start SSDP discovery thread
    impl_->ssdp_thread = std::thread([this]() { impl_->ssdp_loop(); });

    // Start HTTP control server thread
    impl_->http_thread = std::thread([this]() { impl_->http_loop(); });

    return true;
}

void DlnaRenderer::stop() {
    if (!impl_->running.load()) return;

    impl_->stop_requested.store(true);

    // Close HTTP listen socket to unblock accept()
    if (impl_->http_listen_fd >= 0) {
        shutdown(impl_->http_listen_fd, SHUT_RDWR);
    }

    if (impl_->ssdp_thread.joinable()) impl_->ssdp_thread.join();
    if (impl_->http_thread.joinable()) impl_->http_thread.join();
    if (impl_->stream_thread.joinable()) impl_->stream_thread.join();

    impl_->running.store(false);
    fprintf(stderr, "[dlna] Renderer stopped\n");
}

bool DlnaRenderer::is_running() const { return impl_->running.load(); }
uint16_t DlnaRenderer::http_port() const { return impl_->actual_http_port; }
DlnaTransportState DlnaRenderer::transport_state() const { return impl_->transport_state; }
int DlnaRenderer::volume() const { return impl_->volume.load(); }
bool DlnaRenderer::muted() const { return impl_->muted.load(); }

} // namespace soluna::transport

#endif // SOLUNA_HAS_DLNA
