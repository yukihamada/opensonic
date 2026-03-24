package art.solun.sdk

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Low-level UDP socket connection to the Soluna relay server.
 * Uses java.net.DatagramSocket, matching the Swift/C++ implementation.
 */
class RelayConnection(
    private val channel: String,
    private val host: String = OSTConstants.DEFAULT_HOST,
    private val port: Int = OSTConstants.DEFAULT_PORT,
    private val deviceName: String = "SolunaSDK-Kotlin"
) {
    private var socket: DatagramSocket? = null
    private var recvThread: Thread? = null
    private var heartbeatThread: Thread? = null
    private val running = AtomicBoolean(false)
    private var address: InetAddress? = null

    /** Called on the recv thread when an audio packet arrives. */
    var onPacket: ((ByteArray, Int) -> Unit)? = null

    /** Called on the recv thread when a text control message arrives. */
    var onControlMessage: ((String) -> Unit)? = null

    /**
     * Open the UDP socket, send HELLO/JOIN, and start the receive loop.
     * Returns false if DNS resolution or socket creation fails.
     */
    fun connect(): Boolean {
        if (running.get()) return true

        return try {
            // DNS resolve
            address = InetAddress.getByName(host)

            // Create UDP socket
            val sock = DatagramSocket()
            sock.soTimeout = 1000
            socket = sock

            // Send HELLO x3 (100ms apart)
            for (i in 0..2) {
                sendMessage("HELLO\n")
                if (i < 2) Thread.sleep(100)
            }

            // JOIN
            sendMessage("JOIN:$channel::$deviceName\n")

            running.set(true)

            // Start receive thread
            recvThread = Thread({
                recvLoop()
            }, "SolunaSDK-Recv").apply {
                isDaemon = true
                start()
            }

            // Start heartbeat thread
            heartbeatThread = Thread({
                heartbeatLoop()
            }, "SolunaSDK-Heartbeat").apply {
                isDaemon = true
                start()
            }

            true
        } catch (e: Exception) {
            false
        }
    }

    /** Close the connection. */
    fun disconnect() {
        running.set(false)

        socket?.close()
        socket = null

        recvThread?.join(2000)
        recvThread = null

        heartbeatThread?.join(2000)
        heartbeatThread = null
    }

    val isConnected: Boolean get() = running.get()

    private fun sendMessage(message: String) {
        val sock = socket ?: return
        val addr = address ?: return
        try {
            val bytes = message.toByteArray(Charsets.UTF_8)
            val packet = DatagramPacket(bytes, bytes.size, addr, port)
            sock.send(packet)
        } catch (_: Exception) {
        }
    }

    private fun recvLoop() {
        val buf = ByteArray(OSTConstants.RECV_BUFFER_SIZE)

        while (running.get()) {
            try {
                val sock = socket ?: break
                val packet = DatagramPacket(buf, buf.size)
                sock.receive(packet)
                val n = packet.length

                if (n < OSTConstants.RTP_HEADER_SIZE) continue

                // RTP/OSTP audio packet: (byte[0] & 0xC0) == 0x80
                if ((buf[0].toInt() and 0xC0) == 0x80) {
                    onPacket?.invoke(buf, n)
                } else {
                    // Text control message
                    try {
                        val msg = String(buf, 0, n, Charsets.UTF_8)
                        onControlMessage?.invoke(msg)
                    } catch (_: Exception) {
                    }
                }
            } catch (_: java.net.SocketTimeoutException) {
                continue
            } catch (_: Exception) {
                break
            }
        }
    }

    private fun heartbeatLoop() {
        while (running.get()) {
            try {
                Thread.sleep(OSTConstants.HEARTBEAT_INTERVAL_MS)
            } catch (_: InterruptedException) {
                break
            }
            if (!running.get()) break
            sendMessage("HELLO\n")
            sendMessage("JOIN:$channel::$deviceName\n")
        }
    }
}
