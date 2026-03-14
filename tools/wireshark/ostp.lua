-- ostp.lua — Wireshark Lua dissector for OSTP (Open Sonic Transport Protocol)
--
-- Installation:
--   1. Copy this file to your Wireshark plugins directory:
--        macOS/Linux: ~/.config/wireshark/plugins/
--        Windows:     %APPDATA%\Wireshark\plugins\
--   2. Restart Wireshark (or Analyze → Reload Lua Plugins)
--   3. Capture UDP traffic on port 5100 (or 5200 for OSTP-over-WebRTC)
--      The dissector auto-detects by port and by the 0x4F53 extension profile.
--
-- Display Filter Examples:
--   ostp                                  -- all OSTP packets
--   ostp.stream_id == 1                   -- specific stream
--   ostp.crc_valid == false               -- packets with CRC errors
--   ostp.payload_type == 96               -- Opus audio
--   ostp.media_ts > 48000                 -- after 1 second
--
-- Spec: https://github.com/yukihamada/opensonic/blob/main/OSTP-SPEC.md
-- Version: 0.1.0 (OSTP-1 DRAFT)

local ostp_proto = Proto("ostp", "Open Sonic Transport Protocol")

-- ── Field definitions ──────────────────────────────────────────────────────

-- RTP fields (re-parsed here for context)
local f_rtp_version  = ProtoField.uint8 ("ostp.rtp_version",  "RTP Version",        base.DEC)
local f_rtp_padding  = ProtoField.bool  ("ostp.rtp_padding",  "RTP Padding")
local f_rtp_ext      = ProtoField.bool  ("ostp.rtp_ext",      "RTP Extension Bit")
local f_rtp_cc       = ProtoField.uint8 ("ostp.rtp_cc",       "CSRC Count",         base.DEC)
local f_rtp_marker   = ProtoField.bool  ("ostp.rtp_marker",   "Marker Bit")
local f_rtp_pt       = ProtoField.uint8 ("ostp.payload_type", "Payload Type",       base.DEC)
local f_rtp_seq      = ProtoField.uint16("ostp.seq",          "RTP Sequence",       base.DEC)
local f_rtp_ts       = ProtoField.uint32("ostp.rtp_ts",       "RTP Timestamp",      base.DEC)
local f_rtp_ssrc     = ProtoField.uint32("ostp.ssrc",         "SSRC",               base.HEX)

-- RTP extension header
local f_ext_profile  = ProtoField.uint16("ostp.ext_profile",  "Extension Profile",  base.HEX)
local f_ext_len      = ProtoField.uint16("ostp.ext_len",      "Extension Length (words)", base.DEC)

-- OSTP extension fields
local f_stream_id    = ProtoField.uint16("ostp.stream_id",    "Stream ID",          base.DEC)
local f_seq_ext      = ProtoField.uint16("ostp.seq_ext",      "Sequence Ext",       base.DEC)
local f_full_seq     = ProtoField.uint32("ostp.full_seq",     "Full Sequence (32b)",base.DEC)
local f_media_ts     = ProtoField.uint32("ostp.media_ts",     "Media Timestamp",    base.DEC)

-- Payload
local f_payload      = ProtoField.bytes ("ostp.payload",      "Audio Payload")
local f_payload_len  = ProtoField.uint32("ostp.payload_len",  "Payload Length",     base.DEC)

-- CRC
local f_crc          = ProtoField.uint32("ostp.crc",          "CRC-32",             base.HEX)
local f_crc_valid    = ProtoField.bool  ("ostp.crc_valid",    "CRC Valid")

-- Derived / info
local f_codec        = ProtoField.string("ostp.codec",        "Codec")
local f_channel_bits = ProtoField.uint8 ("ostp.channel_bits", "Channel Count (from stream_id high nibble)", base.DEC)

ostp_proto.fields = {
    f_rtp_version, f_rtp_padding, f_rtp_ext, f_rtp_cc,
    f_rtp_marker, f_rtp_pt, f_rtp_seq, f_rtp_ts, f_rtp_ssrc,
    f_ext_profile, f_ext_len,
    f_stream_id, f_seq_ext, f_full_seq, f_media_ts,
    f_payload, f_payload_len,
    f_crc, f_crc_valid,
    f_codec, f_channel_bits,
}

-- ── CRC-32 implementation ──────────────────────────────────────────────────

local crc32_table = {}
do
    for i = 0, 255 do
        local c = i
        for _ = 1, 8 do
            if c % 2 == 1 then
                c = 0xEDB88320 ~ (c >> 1)
            else
                c = c >> 1
            end
        end
        crc32_table[i] = c
    end
end

local function crc32(tvb, offset, length)
    local crc = 0xFFFFFFFF
    for i = offset, offset + length - 1 do
        local byte = tvb:get_index(i)
        crc = crc32_table[(crc ~ byte) & 0xFF] ~ (crc >> 8)
    end
    return crc ~ 0xFFFFFFFF
end

-- ── Codec name ─────────────────────────────────────────────────────────────

local CODEC_NAMES = {
    [96] = "Opus",
    [97] = "PCM-S16LE",
    [98] = "AAC-LC",
}

-- ── Dissector ──────────────────────────────────────────────────────────────

function ostp_proto.dissector(tvb, pinfo, tree)
    local pkt_len = tvb:len()

    -- Minimum packet size: 12 (RTP) + 4 (ext hdr) + 8 (OSTP) + 4 (CRC) = 28
    if pkt_len < 28 then return 0 end

    -- Quick profile check before committing
    local b0 = tvb(0, 1):uint()
    local has_ext = (b0 & 0x10) ~= 0
    if not has_ext then return 0 end

    local ext_profile = tvb(12, 2):uint()
    if ext_profile ~= 0x4F53 then return 0 end

    -- We have an OSTP packet
    pinfo.cols.protocol = "OSTP"

    local b1     = tvb(1, 1):uint()
    local pt     = b1 & 0x7F
    local seq    = tvb(2, 2):uint()
    local rtp_ts = tvb(4, 4):uint()

    local ext_len_words = tvb(14, 2):uint()
    if ext_len_words ~= 2 then return 0 end  -- unknown ext format

    local stream_id = tvb(16, 2):uint()
    local seq_ext   = tvb(18, 2):uint()
    local media_ts  = tvb(20, 4):uint()
    local full_seq  = (seq_ext * 65536) + seq

    local payload_start = 24
    local payload_len   = pkt_len - payload_start - 4  -- subtract CRC
    if payload_len < 0 then return 0 end

    local stored_crc = tvb(pkt_len - 4, 4):le_uint()
    local calc_crc   = crc32(tvb, 0, pkt_len - 4)
    local crc_ok     = (stored_crc == calc_crc)

    local codec = CODEC_NAMES[pt] or string.format("PT=%d", pt)
    local chan_count = ((stream_id >> 12) & 0xF)
    if chan_count == 0 then chan_count = 2 end  -- default stereo

    -- Column info
    pinfo.cols.info:set(string.format(
        "OSTP  seq=%u  stream=%u  %s  %dB%s",
        full_seq, stream_id, codec, payload_len,
        crc_ok and "" or "  [CRC ERROR]"
    ))

    -- Tree
    local subtree = tree:add(ostp_proto, tvb(0, pkt_len),
        string.format("OSTP, Seq=%u, Stream=%u, %s, %dB payload",
            full_seq, stream_id, codec, payload_len))

    -- RTP Header subtree
    local rtp_tree = subtree:add(tvb(0, 12), "RTP Header")
    rtp_tree:add(f_rtp_version, tvb(0, 1), (b0 >> 6) & 0x3)
    rtp_tree:add(f_rtp_padding, tvb(0, 1), ((b0 >> 5) & 0x1) ~= 0)
    rtp_tree:add(f_rtp_ext,     tvb(0, 1), has_ext)
    rtp_tree:add(f_rtp_cc,      tvb(0, 1), b0 & 0x0F)
    rtp_tree:add(f_rtp_marker,  tvb(1, 1), ((b1 >> 7) & 0x1) ~= 0)
    rtp_tree:add(f_rtp_pt,      tvb(1, 1), pt)
    rtp_tree:add(f_rtp_seq,     tvb(2, 2))
    rtp_tree:add(f_rtp_ts,      tvb(4, 4))
    rtp_tree:add(f_rtp_ssrc,    tvb(8, 4))

    -- RTP Extension Header subtree
    local ext_hdr_tree = subtree:add(tvb(12, 4), "RTP Extension Header (OSTP Profile 0x4F53)")
    ext_hdr_tree:add(f_ext_profile, tvb(12, 2)):append_text(" ('OS')")
    ext_hdr_tree:add(f_ext_len,     tvb(14, 2)):append_text(" = 8 bytes")

    -- OSTP Extension Data subtree
    local ostp_tree = subtree:add(tvb(16, 8), "OSTP Extension Data")
    ostp_tree:add(f_stream_id,  tvb(16, 2))
    ostp_tree:add(f_seq_ext,    tvb(18, 2))
    local full_seq_item = ostp_tree:add(f_full_seq, tvb(16, 4), full_seq)
    full_seq_item:set_generated()
    ostp_tree:add(f_media_ts,   tvb(20, 4)):append_text(
        string.format(" (%.3f s @ 48 kHz)", media_ts / 48000.0))

    -- Audio Payload
    if payload_len > 0 then
        local pl_tree = subtree:add(tvb(payload_start, payload_len),
            string.format("Audio Payload (%s, %d bytes)", codec, payload_len))
        pl_tree:add(f_codec,       tvb(payload_start, 1), codec):set_generated()
        pl_tree:add(f_payload,     tvb(payload_start, payload_len))
        pl_tree:add(f_payload_len, tvb(payload_start, payload_len), payload_len):set_generated()
        pl_tree:add(f_channel_bits, tvb(16, 2), chan_count):set_generated()
    end

    -- CRC-32 trailer
    local crc_tree = subtree:add(tvb(pkt_len - 4, 4),
        string.format("CRC-32: 0x%08X [%s]", stored_crc, crc_ok and "correct" or "INCORRECT"))
    local crc_item = crc_tree:add(f_crc,       tvb(pkt_len - 4, 4)):set_text(
        string.format("CRC-32: 0x%08X (little-endian)", stored_crc))
    local valid_item = crc_tree:add(f_crc_valid, tvb(pkt_len - 4, 4), crc_ok)
    valid_item:set_generated()
    if not crc_ok then
        valid_item:add_expert_info(PI_CHECKSUM, PI_ERROR,
            string.format("CRC mismatch: stored=0x%08X calc=0x%08X", stored_crc, calc_crc))
    end

    return pkt_len
end

-- ── Port registration ──────────────────────────────────────────────────────

-- Register on default OSTP UDP port (5100)
local udp_table = DissectorTable.get("udp.port")
udp_table:add(5100, ostp_proto)
udp_table:add(5200, ostp_proto)  -- secondary (OSTP-over-WebRTC encap)

-- Heuristic dissector for other ports — detect by 0x4F53 extension profile
local function heuristic_check(tvb, pinfo, tree)
    if tvb:len() < 28 then return false end
    local b0 = tvb(0, 1):uint()
    if (b0 & 0xF0) ~= 0x90 then return false end  -- V=2, X=1
    if tvb(12, 2):uint() ~= 0x4F53 then return false end
    ostp_proto.dissector(tvb, pinfo, tree)
    return true
end

ostp_proto:register_heuristic("udp", heuristic_check)

print("OSTP dissector loaded (port 5100, 5200, heuristic UDP)")
