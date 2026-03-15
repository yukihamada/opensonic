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
 * Packet format:
 *   RTP header (12B) + OstpHeader (20B: magic 'OSTP' + fields) + int32 BE PCM + CRC-32 (4B)
 *   Detection: bytes[12..15] == 'O','S','T','P'
 */
class SolunaClient(private val audioPlayer: AudioPlayer) {

    companion object {
        private const val TAG = "SolunaClient"
        private const val RTP_HEADER_SIZE = 12
        private const val OSTP_HEADER_SIZE = 20
        private const val TOTAL_HEADER_SIZE = RTP_HEADER_SIZE + OSTP_HEADER_SIZE  // 32
        private const val CRC_SIZE = 4
        private const val KEEPALIVE_INTERVAL_MS = 5_000L
        private const val RECV_BUF_SIZE = 65536
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

                // RTP/OSTP audio packet
                if (len >= TOTAL_HEADER_SIZE) {
                    handleAudioPacket(data, len)
                    packetsReceived++
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

        // Detect OSTP by magic bytes at offset 12
        val isOstp = len >= TOTAL_HEADER_SIZE &&
                data[12] == 0x4F.toByte() &&  // 'O'
                data[13] == 0x53.toByte() &&  // 'S'
                data[14] == 0x54.toByte() &&  // 'T'
                data[15] == 0x50.toByte()     // 'P'

        val (payloadOffset, payloadLen) = if (isOstp) {
            val pLen = len - TOTAL_HEADER_SIZE - CRC_SIZE
            Pair(TOTAL_HEADER_SIZE, pLen)
        } else {
            Pair(RTP_HEADER_SIZE, len - RTP_HEADER_SIZE)
        }

        if (payloadLen > 0) {
            audioPlayer.writePayload(data, payloadOffset, payloadLen)
        }
    }

    private fun sendBytes(bytes: ByteArray) {
        val addr = relayAddr ?: return
        socket?.send(DatagramPacket(bytes, bytes.size, addr, relayPort))
    }
}
