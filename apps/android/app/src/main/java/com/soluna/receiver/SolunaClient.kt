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
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * OSTP/RTP UDP client for the Soluna relay server.
 *
 * Protocol:
 *   Client -> Relay:  "JOIN:<channel>:<password>:<device_name>\n"
 *   Relay  -> Client: "OK:joined\n", then audio packets + control messages
 *
 * Packets:
 *   - Text commands: ASCII lines terminated by \n
 *   - Audio: RTP header (12B) + RTP extension header (4B) + OSTP header (8B) + payload
 *     Total header = 24 bytes. Payload = interleaved int32 PCM samples.
 *
 * Keepalive: re-send JOIN every 10s to avoid 15s stale timeout on relay.
 */
class SolunaClient(private val audioPlayer: AudioPlayer) {

    companion object {
        private const val TAG = "SolunaClient"
        private const val RTP_HEADER_SIZE = 12
        private const val RTP_EXT_HEADER_SIZE = 4
        private const val OSTP_HEADER_SIZE = 8
        private const val TOTAL_HEADER_SIZE = RTP_HEADER_SIZE + RTP_EXT_HEADER_SIZE + OSTP_HEADER_SIZE
        private const val CRC_SIZE = 4
        private const val OSTP_PROFILE = 0x4F53.toShort() // "OS"
        private const val KEEPALIVE_INTERVAL_MS = 10_000L
        private const val RECV_BUF_SIZE = 16384
    }

    data class ConnectionState(
        val status: Status = Status.DISCONNECTED,
        val channel: String = "",
        val memberCount: Int = 0,
        val walletBalance: String = "",
        val packetsReceived: Long = 0,
        val errorMessage: String? = null
    )

    enum class Status { DISCONNECTED, CONNECTING, CONNECTED, ERROR }

    private val _state = MutableStateFlow(ConnectionState())
    val state: StateFlow<ConnectionState> = _state.asStateFlow()

    private var socket: DatagramSocket? = null
    private var scope: CoroutineScope? = null
    private var relayAddress: InetAddress? = null
    private var relayPort: Int = 5100
    private var currentChannel: String = ""
    private var joinMessage: ByteArray = ByteArray(0)

    fun connect(host: String, port: Int, channel: String) {
        disconnect()

        currentChannel = channel
        _state.value = ConnectionState(
            status = Status.CONNECTING,
            channel = channel
        )

        scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
        scope?.launch {
            try {
                relayAddress = InetAddress.getByName(host)
                relayPort = port
                socket = DatagramSocket().also { it.soTimeout = 0 }

                // Build JOIN message
                val deviceName = "${Build.MANUFACTURER} ${Build.MODEL}"
                val joinStr = "JOIN:$channel::$deviceName\n"
                joinMessage = joinStr.toByteArray(Charsets.UTF_8)

                // Send initial JOIN
                sendBytes(joinMessage)
                Log.i(TAG, "Sent JOIN to $host:$port channel=$channel")

                // Start keepalive
                launch { keepaliveLoop() }

                // Start receiving
                audioPlayer.start()
                receiveLoop()
            } catch (e: Exception) {
                Log.e(TAG, "Connection error", e)
                _state.value = _state.value.copy(
                    status = Status.ERROR,
                    errorMessage = e.message
                )
            }
        }
    }

    fun disconnect() {
        scope?.cancel()
        scope = null
        audioPlayer.stop()
        try { socket?.close() } catch (_: Exception) {}
        socket = null
        _state.value = ConnectionState()
        Log.i(TAG, "Disconnected")
    }

    fun sendCommand(command: String) {
        scope?.launch {
            sendBytes("$command\n".toByteArray(Charsets.UTF_8))
        }
    }

    fun requestWallet() = sendCommand("WALLET")

    fun sendTip(amount: String) = sendCommand("TIP:$amount")

    fun requestMembers() = sendCommand("MEMBERS")

    private suspend fun keepaliveLoop() {
        while (currentCoroutineContext().isActive) {
            delay(KEEPALIVE_INTERVAL_MS)
            try {
                sendBytes(joinMessage)
            } catch (e: Exception) {
                Log.w(TAG, "Keepalive failed", e)
            }
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

                // Check if this is a text control message (starts with ASCII letter)
                if (len > 0 && data[0] in 0x20..0x7E) {
                    val text = String(data, 0, len, Charsets.UTF_8).trim()
                    handleTextMessage(text)
                    continue
                }

                // Otherwise treat as RTP/OSTP audio packet
                if (len >= TOTAL_HEADER_SIZE) {
                    handleAudioPacket(data, len)
                    packetsReceived++
                    if (packetsReceived % 500 == 0L) {
                        _state.value = _state.value.copy(packetsReceived = packetsReceived)
                    }
                }
            } catch (e: Exception) {
                if (scope?.isActive == true) {
                    Log.w(TAG, "Receive error", e)
                }
            }
        }
    }

    private fun handleTextMessage(text: String) {
        Log.d(TAG, "Text: $text")
        when {
            text.startsWith("OK:joined") -> {
                _state.value = _state.value.copy(status = Status.CONNECTED)
                // Request member list and wallet after joining
                sendCommand("MEMBERS")
                sendCommand("WALLET")
            }
            text.startsWith("MEMBERS:") -> {
                parseMembersResponse(text.removePrefix("MEMBERS:"))
            }
            text.startsWith("WALLET:") -> {
                parseWalletResponse(text.removePrefix("WALLET:"))
            }
            text.startsWith("MEMBER_JOINED:") || text.startsWith("MEMBER_LEFT:") -> {
                // Re-request member count
                sendCommand("MEMBERS")
            }
            text.startsWith("ROLE:") -> {
                Log.i(TAG, "Role assigned: $text")
            }
            text.startsWith("META:") -> {
                Log.i(TAG, "Metadata: $text")
            }
            text.startsWith("TIP_RECEIVED:") -> {
                Log.i(TAG, "Tip received: $text")
            }
            text.startsWith("ERR:") -> {
                _state.value = _state.value.copy(errorMessage = text)
            }
        }
    }

    private fun parseMembersResponse(json: String) {
        // Simple JSON parsing for member count: look for "members" array
        try {
            val membersStart = json.indexOf("\"members\"")
            if (membersStart >= 0) {
                val arrayStart = json.indexOf('[', membersStart)
                val arrayEnd = json.indexOf(']', arrayStart)
                if (arrayStart >= 0 && arrayEnd >= 0) {
                    val membersStr = json.substring(arrayStart + 1, arrayEnd)
                    val count = if (membersStr.isBlank()) 0
                    else membersStr.count { it == '{' }
                    _state.value = _state.value.copy(memberCount = count)
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to parse MEMBERS", e)
        }
    }

    private fun parseWalletResponse(json: String) {
        // Extract balance from WALLET:{...} JSON
        try {
            val balanceMatch = Regex("\"balance\":(\\d+\\.?\\d*)").find(json)
            if (balanceMatch != null) {
                val balance = balanceMatch.groupValues[1]
                _state.value = _state.value.copy(walletBalance = "$$balance")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to parse WALLET", e)
        }
    }

    private fun handleAudioPacket(data: ByteArray, len: Int) {
        // Verify RTP version = 2
        val firstByte = data[0].toInt() and 0xFF
        val version = (firstByte shr 6) and 0x03
        if (version != 2) return

        // Check extension bit
        val hasExtension = ((firstByte shr 4) and 0x01) == 1

        if (!hasExtension) {
            // No extension header - payload starts after RTP header
            val payloadOffset = RTP_HEADER_SIZE
            val payloadLen = len - payloadOffset
            if (payloadLen > 0) {
                audioPlayer.writePayload(data, payloadOffset, payloadLen)
            }
            return
        }

        // With extension: verify OSTP profile
        if (len < TOTAL_HEADER_SIZE) return

        val bb = ByteBuffer.wrap(data, RTP_HEADER_SIZE, RTP_EXT_HEADER_SIZE)
            .order(ByteOrder.BIG_ENDIAN)
        val profile = bb.short

        if (profile != OSTP_PROFILE) return

        // Payload starts after total header, ends before optional CRC trailer
        val payloadOffset = TOTAL_HEADER_SIZE
        val payloadLen = if (len >= TOTAL_HEADER_SIZE + CRC_SIZE + 4) {
            // Assume CRC trailer present if packet is large enough
            len - TOTAL_HEADER_SIZE - CRC_SIZE
        } else {
            len - TOTAL_HEADER_SIZE
        }

        if (payloadLen > 0) {
            audioPlayer.writePayload(data, payloadOffset, payloadLen)
        }
    }

    private fun sendBytes(bytes: ByteArray) {
        val addr = relayAddress ?: return
        val packet = DatagramPacket(bytes, bytes.size, addr, relayPort)
        socket?.send(packet)
    }
}
