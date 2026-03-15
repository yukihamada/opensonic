package com.soluna.receiver

import android.os.Build
import android.util.Log
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress

/**
 * OSTP/RTP UDP client for the Soluna relay server.
 *
 * Protocol:
 *   Client -> Relay:  "JOIN:<channel>:<password>:<device_name>\n"
 *   Relay  -> Client: "OK:joined\n", then audio packets + control messages
 *   Keepalive:        "HELLO\n" every 5s
 *
 * Received packet formats:
 *   Modern OSTP (iOS/relay-native):
 *     RTP(12B, X=1) + ext_hdr(4B: profile=0x4F53, len=2) + OSTP_ext(8B) + PCM + CRC(4B)
 *   Legacy OSTP (Windows-compatible):
 *     RTP(12B, X=0) + OstpHeader(20B: magic 'OSTP' + fields) + PCM + CRC(4B)
 */
class SolunaClient(private val audioPlayer: AudioPlayer) {

    companion object {
        private const val TAG = "SolunaClient"
        private const val RTP_HEADER_SIZE = 12
        // Modern OSTP: RTP(12) + RTP ext header(4) + OSTP ext(8) = 24
        private const val MODERN_HEADER_SIZE = 24
        // Legacy OSTP: RTP(12) + OstpHeader(20) = 32
        private const val LEGACY_HEADER_SIZE = 32
        private const val CRC_SIZE = 4
        private const val KEEPALIVE_INTERVAL_MS = 5_000L
        private const val RECV_BUF_SIZE = 65536

        // FEC / NACK constants
        private const val FEC_GROUP_SIZE = 5
        private const val PT_FEC = 127
    }

    data class ConnectionState(
        val status: Status = Status.DISCONNECTED,
        val channel: String = "",
        val memberCount: Int = 0,
        val walletBalance: String = "",
        val packetsReceived: Long = 0,
        val rxLevel: Float = 0f,
        val errorMessage: String? = null
    )

    enum class Status { DISCONNECTED, CONNECTING, CONNECTED, ERROR }

    private val _state = MutableStateFlow(ConnectionState())
    val state: StateFlow<ConnectionState> = _state.asStateFlow()

    private var socket: DatagramSocket? = null
    private var scope: CoroutineScope? = null
    private var relayAddr: InetAddress? = null
    private var relayPort: Int = 5100
    private var joinMessage: ByteArray = ByteArray(0)

    // FEC state
    private data class FecGroup(
        val packets: Array<ByteArray?> = arrayOfNulls(FEC_GROUP_SIZE),
        val sizes: IntArray = IntArray(FEC_GROUP_SIZE),
        var parity: ByteArray? = null,
        var paritySize: Int = 0,
        var received: Int = 0
    )
    private val fecGroups = HashMap<Int, FecGroup>()
    private val nackSent = HashSet<Int>()

    init {
        audioPlayer.onLevelUpdate = { level ->
            _state.value = _state.value.copy(rxLevel = level)
        }
    }

    fun connect(host: String, port: Int, channel: String, password: String = "") {
        disconnect()

        _state.value = ConnectionState(status = Status.CONNECTING, channel = channel)

        scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
        scope?.launch {
            try {
                relayAddr = InetAddress.getByName(host)
                relayPort = port
                socket = DatagramSocket().also { it.soTimeout = 0 }

                val deviceName = "${Build.MANUFACTURER} ${Build.MODEL}"
                joinMessage = "JOIN:$channel:$password:$deviceName\n".toByteArray(Charsets.UTF_8)
                sendBytes(joinMessage)
                Log.i(TAG, "Sent JOIN to $host:$port channel=$channel")

                launch { keepaliveLoop() }
                launch { receiverReportLoop() }
                audioPlayer.start()
                receiveLoop()
            } catch (e: Exception) {
                Log.e(TAG, "Connection error", e)
                _state.value = _state.value.copy(status = Status.ERROR, errorMessage = e.message)
            }
        }
    }

    fun disconnect() {
        scope?.cancel()
        scope = null
        audioPlayer.stop()
        try { socket?.close() } catch (_: Exception) {}
        socket = null
        relayAddr = null
        _packetsLost = 0L; _lastSeq = -1; _jitterMs = 0f; _prevArrivalMs = 0L; _prevRtpTs = 0
        _ssrc = (System.nanoTime() and 0xFFFFFFFFL)
        fecGroups.clear()
        nackSent.clear()
        _state.value = ConnectionState()
        Log.i(TAG, "Disconnected")
    }

    val isConnected: Boolean
        get() = _state.value.status == Status.CONNECTED

    /** Called by MicTransmitter to send a pre-built OSTP audio packet. */
    fun sendAudioPacketDirect(bytes: ByteArray) {
        val addr = relayAddr ?: return
        val sock = socket ?: return
        try {
            sock.send(DatagramPacket(bytes, bytes.size, addr, relayPort))
        } catch (e: Exception) {
            Log.w(TAG, "TX send error", e)
        }
    }

    fun sendCommand(command: String) {
        scope?.launch { sendBytes("$command\n".toByteArray(Charsets.UTF_8)) }
    }

    fun requestWallet() = sendCommand("WALLET")
    fun sendTip(amount: String) = sendCommand("TIP:$amount")
    fun requestMembers() = sendCommand("MEMBERS")

    private suspend fun keepaliveLoop() {
        val hello = "HELLO\n".toByteArray(Charsets.UTF_8)
        while (currentCoroutineContext().isActive) {
            delay(KEEPALIVE_INTERVAL_MS)
            try { sendBytes(hello) } catch (e: Exception) { Log.w(TAG, "Keepalive failed", e) }
        }
    }

    // OSTP §10.1 Receiver Report — sent every 5s via relay UDP text channel
    private suspend fun receiverReportLoop() {
        while (currentCoroutineContext().isActive) {
            delay(5_000L)
            val s = _state.value
            if (s.status != Status.CONNECTED) continue
            val total = s.packetsReceived
            val lost  = _packetsLost
            val lossRate = if (total > 0) lost.toFloat() / (total + lost).toFloat() * 100f else 0f
            val report = "REPORT:${_ssrc}:${total}:${lost}:${String.format("%.2f", lossRate)}:" +
                         "${String.format("%.2f", _jitterMs)}:${_lastSeq}\n"
            try { sendBytes(report.toByteArray(Charsets.UTF_8)) } catch (e: Exception) { /* ignore */ }
        }
    }

    // Receiver stats tracked per-session
    private var _ssrc: Long = (System.nanoTime() and 0xFFFFFFFFL)
    private var _packetsLost: Long = 0L
    private var _lastSeq: Int = -1
    private var _jitterMs: Float = 0f
    private var _prevArrivalMs: Long = 0L
    private var _prevRtpTs: Int = 0

    private fun receiveLoop() {
        val buf = ByteArray(RECV_BUF_SIZE)
        val packet = DatagramPacket(buf, buf.size)
        var packetsReceived = 0L

        while (scope?.isActive == true) {
            try {
                socket?.receive(packet) ?: break
                val data = packet.data
                val len = packet.length
                if (len == 0) continue

                // Text message: starts with printable ASCII
                if (data[0] in 0x20..0x7E) {
                    handleTextMessage(String(data, 0, len, Charsets.UTF_8).trim())
                    continue
                }

                // RTP/OSTP audio packet (minimum: plain RTP = 12B + some payload)
                if (len >= RTP_HEADER_SIZE + 4) {
                    // Sequence gap detection + NACK before handleAudioPacket
                    val seq = ((data[2].toInt() and 0xFF) shl 8) or (data[3].toInt() and 0xFF)
                    if (_lastSeq >= 0) {
                        val expected = (_lastSeq + 1) and 0xFFFF
                        if (seq != expected) {
                            val gap = (seq - expected + 0x10000) and 0xFFFF
                            if (gap in 1..FEC_GROUP_SIZE) {
                                val missing = mutableListOf<Int>()
                                for (i in 0 until gap) {
                                    val ms = (expected + i) and 0xFFFF
                                    if (nackSent.add(ms)) missing.add(ms)
                                }
                                if (missing.isNotEmpty()) sendNack(missing)
                            }
                            _packetsLost += gap.toLong()
                        }
                    }

                    handleAudioPacket(data, len)
                    packetsReceived++

                    // RFC 3550 §A.8 jitter calculation
                    val rtpTs = ((data[4].toInt() and 0xFF) shl 24) or ((data[5].toInt() and 0xFF) shl 16) or
                                ((data[6].toInt() and 0xFF) shl 8) or  (data[7].toInt() and 0xFF)
                    val arrivalMs = System.currentTimeMillis()
                    if (_lastSeq >= 0) {
                        val d = Math.abs((arrivalMs - _prevArrivalMs) -
                                        (((rtpTs - _prevRtpTs) and 0xFFFFFFFFL.toInt()) / 48.0))
                        _jitterMs = _jitterMs + (d.toFloat() - _jitterMs) / 16f
                    }
                    _lastSeq = seq; _prevArrivalMs = arrivalMs; _prevRtpTs = rtpTs

                    if (packetsReceived % 500 == 0L) {
                        _state.value = _state.value.copy(packetsReceived = packetsReceived)
                    }
                }
            } catch (e: Exception) {
                if (scope?.isActive == true) Log.w(TAG, "Receive error", e)
            }
        }
    }

    private fun handleTextMessage(text: String) {
        Log.d(TAG, "Text: $text")
        when {
            text.startsWith("OK:joined") -> {
                _state.value = _state.value.copy(status = Status.CONNECTED)
                sendCommand("MEMBERS")
                sendCommand("WALLET")
            }
            text.startsWith("MEMBERS:") -> parseMembersResponse(text.removePrefix("MEMBERS:"))
            text.startsWith("WALLET:") -> parseWalletResponse(text.removePrefix("WALLET:"))
            text.startsWith("MEMBER_JOINED:") || text.startsWith("MEMBER_LEFT:") -> sendCommand("MEMBERS")
            text.startsWith("ERR:") -> _state.value = _state.value.copy(errorMessage = text)
            else -> Log.i(TAG, "Server: $text")
        }
    }

    private fun parseMembersResponse(json: String) {
        try {
            val arrayStart = json.indexOf('[')
            val arrayEnd = json.indexOf(']', arrayStart)
            if (arrayStart >= 0 && arrayEnd >= 0) {
                val count = if (json.substring(arrayStart + 1, arrayEnd).isBlank()) 0
                            else json.substring(arrayStart + 1, arrayEnd).count { it == '{' }
                _state.value = _state.value.copy(memberCount = count)
            }
        } catch (e: Exception) { Log.w(TAG, "Parse MEMBERS error", e) }
    }

    private fun parseWalletResponse(json: String) {
        try {
            val m = Regex("\"balance\":(\\d+\\.?\\d*)").find(json)
            if (m != null) {
                _state.value = _state.value.copy(walletBalance = "$${m.groupValues[1]}")
            }
        } catch (e: Exception) { Log.w(TAG, "Parse WALLET error", e) }
    }

    private fun handleAudioPacket(data: ByteArray, len: Int) {
        val version = (data[0].toInt() and 0xFF) shr 6
        if (version != 2) return

        val hasExtension = (data[0].toInt() and 0x10) != 0
        val pt = data[1].toInt() and 0x7F

        when {
            // Modern OSTP: X=1, profile=0x4F53, length=2 (8 bytes OSTP ext)
            hasExtension && len >= MODERN_HEADER_SIZE &&
            data[12] == 0x4F.toByte() && data[13] == 0x53.toByte() &&
            data[14] == 0x00.toByte() && data[15] == 0x02.toByte() -> {
                // sequence_ext is at bytes 18-19 of the packet (OSTP ext offset 2-3)
                val groupId = ((data[18].toInt() and 0xFF) shl 8) or (data[19].toInt() and 0xFF)

                if (pt == PT_FEC) {
                    // FEC parity packet: store parity and attempt recovery
                    val group = fecGroups.getOrPut(groupId) { FecGroup() }
                    val parityData = data.copyOfRange(MODERN_HEADER_SIZE, len - CRC_SIZE)
                    group.parity = parityData
                    group.paritySize = parityData.size
                    // Attempt recovery for any missing slot
                    if (group.received == FEC_GROUP_SIZE - 1) {
                        val missingIndex = group.packets.indexOfFirst { it == null }
                        if (missingIndex >= 0) {
                            val recovered = tryFecRecovery(groupId, missingIndex)
                            if (recovered != null) {
                                Log.d(TAG, "FEC recovered packet index=$missingIndex in group=$groupId")
                                audioPlayer.writePayload(recovered, 0, recovered.size)
                            }
                        }
                    }
                    return
                }

                val pLen = len - MODERN_HEADER_SIZE - CRC_SIZE
                if (pLen <= 0) return

                // Store in FEC group
                val seq = ((data[2].toInt() and 0xFF) shl 8) or (data[3].toInt() and 0xFF)
                val group = fecGroups.getOrPut(groupId) { FecGroup() }
                val index = seq % FEC_GROUP_SIZE
                if (group.packets[index] == null) {
                    group.packets[index] = data.copyOfRange(MODERN_HEADER_SIZE, len - CRC_SIZE)
                    group.sizes[index] = pLen
                    group.received++
                }

                audioPlayer.writePayload(data, MODERN_HEADER_SIZE, pLen)
            }
            // Legacy OSTP: X=0, 'OSTP' magic at offset 12
            !hasExtension && len >= LEGACY_HEADER_SIZE &&
            data[12] == 0x4F.toByte() && data[13] == 0x53.toByte() &&
            data[14] == 0x54.toByte() && data[15] == 0x50.toByte() -> {
                val pLen = len - LEGACY_HEADER_SIZE - CRC_SIZE
                if (pLen <= 0) return
                audioPlayer.writePayload(data, LEGACY_HEADER_SIZE, pLen)
            }
            // Plain RTP (no OSTP header)
            else -> {
                val pLen = len - RTP_HEADER_SIZE
                if (pLen <= 0) return
                audioPlayer.writePayload(data, RTP_HEADER_SIZE, pLen)
            }
        }
    }

    /**
     * XOR all received packets in the FEC group with the parity to recover the missing one.
     * Returns the recovered payload bytes, or null if recovery is not possible.
     */
    private fun tryFecRecovery(groupId: Int, missingIndex: Int): ByteArray? {
        val group = fecGroups[groupId] ?: return null
        val parity = group.parity ?: return null

        val result = parity.copyOf()
        for (i in 0 until FEC_GROUP_SIZE) {
            if (i == missingIndex) continue
            val pkt = group.packets[i] ?: return null  // another packet also missing
            val minLen = minOf(result.size, pkt.size)
            for (j in 0 until minLen) {
                result[j] = (result[j].toInt() xor pkt[j].toInt()).toByte()
            }
        }
        return result
    }

    private fun sendNack(missingSeqs: List<Int>) {
        val addr = relayAddr ?: return
        val sock = socket ?: return
        val buf = ByteArray(2 + missingSeqs.size * 4)
        buf[0] = 0x4E; buf[1] = 0x41  // "NA"
        missingSeqs.forEachIndexed { i, seq ->
            buf[2 + i * 4] = (seq shr 24).toByte()
            buf[2 + i * 4 + 1] = (seq shr 16).toByte()
            buf[2 + i * 4 + 2] = (seq shr 8).toByte()
            buf[2 + i * 4 + 3] = seq.toByte()
        }
        try { sock.send(DatagramPacket(buf, buf.size, addr, relayPort + 1)) }
        catch (e: Exception) { Log.w(TAG, "NACK send error", e) }
    }

    private fun sendBytes(bytes: ByteArray) {
        val addr = relayAddr ?: return
        socket?.send(DatagramPacket(bytes, bytes.size, addr, relayPort))
    }
}
