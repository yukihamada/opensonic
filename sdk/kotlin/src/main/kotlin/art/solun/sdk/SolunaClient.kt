package art.solun.sdk

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Connection state.
 */
enum class ConnectionState {
    DISCONNECTED,
    CONNECTED,
    ERROR
}

/**
 * Main entry point for the Soluna audio relay SDK.
 *
 * ```kotlin
 * val client = SolunaClient()
 * client.onAudio { samples, channels -> /* process */ }
 * client.connect("my-channel")
 * // Observe state
 * client.state.collect { state -> /* ... */ }
 * // ...
 * client.disconnect()
 * ```
 */
class SolunaClient {

    private var connection: RelayConnection? = null
    private val _state = MutableStateFlow(ConnectionState.DISCONNECTED)
    private val _channel = MutableStateFlow("")

    /** Current connection state as a StateFlow. */
    val state: StateFlow<ConnectionState> = _state.asStateFlow()

    /** Current channel name as a StateFlow. */
    val channel: StateFlow<String> = _channel.asStateFlow()

    val isConnected: Boolean get() = _state.value == ConnectionState.CONNECTED

    private var onAudioCallback: ((FloatArray, Int) -> Unit)? = null
    private var onPacketCallback: ((OSTPacket) -> Unit)? = null

    /**
     * Register a callback for decoded float32 PCM audio.
     * Called on the receive thread.
     */
    fun onAudio(callback: (samples: FloatArray, channels: Int) -> Unit) {
        onAudioCallback = callback
    }

    /**
     * Register a callback for parsed OSTP packets.
     * Called on the receive thread.
     */
    fun onPacket(callback: (OSTPacket) -> Unit) {
        onPacketCallback = callback
    }

    /**
     * Connect to the Soluna relay and start receiving audio.
     */
    fun connect(
        channel: String,
        host: String = OSTConstants.DEFAULT_HOST,
        port: Int = OSTConstants.DEFAULT_PORT
    ): Boolean {
        if (isConnected) return true

        _channel.value = channel

        val conn = RelayConnection(
            channel = channel,
            host = host,
            port = port
        )

        conn.onPacket = { data, length ->
            handlePacket(data, length)
        }

        return if (conn.connect()) {
            connection = conn
            _state.value = ConnectionState.CONNECTED
            true
        } else {
            _state.value = ConnectionState.ERROR
            false
        }
    }

    /**
     * Disconnect from the relay.
     */
    fun disconnect() {
        connection?.disconnect()
        connection = null
        _state.value = ConnectionState.DISCONNECTED
        _channel.value = ""
    }

    /**
     * Switch to a different channel. Reconnects automatically.
     */
    fun setChannel(name: String) {
        val wasConnected = isConnected
        if (wasConnected) disconnect()
        if (wasConnected) connect(name)
    }

    private fun handlePacket(data: ByteArray, length: Int) {
        val packet = OSTPacketParser.parse(data, length) ?: return

        onPacketCallback?.invoke(packet)

        val samples = AudioDecoder.decodePacketToFloat(packet) ?: return
        onAudioCallback?.invoke(samples, packet.channels)
    }
}
