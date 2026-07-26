package com.thorstream.client

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface

data class DiscoveredHost(
    val name: String,
    val address: String,
    val port: Int,
)

/**
 * Finds thorstream hosts on the local network so nobody has to type an IP.
 *
 * A UDP broadcast probe rather than mDNS: no responder stack to depend on, and
 * the host's reply comes from the address the handheld can actually reach,
 * rather than one it advertises and hopes is routable.
 */
object HostDiscovery {

    private const val PROBE = "THORSTREAM-DISCOVER-1"
    private const val REPLY_PREFIX = "THORSTREAM-HOST-1|"
    private const val PORT = 47809

    suspend fun scan(timeoutMillis: Int = 1500): List<DiscoveredHost> =
        withContext(Dispatchers.IO) {
            val found = LinkedHashMap<String, DiscoveredHost>()

            try {
                DatagramSocket().use { socket ->
                    socket.broadcast = true
                    socket.soTimeout = 250

                    val probe = PROBE.toByteArray()
                    // Send to the global broadcast address and to each interface's
                    // own broadcast address: some networks drop 255.255.255.255
                    // but pass the subnet-directed form.
                    for (target in broadcastTargets()) {
                        try {
                            socket.send(DatagramPacket(probe, probe.size, target, PORT))
                        } catch (e: Exception) {
                            Log.d(TAG, "probe to $target failed: ${e.message}")
                        }
                    }

                    val deadline = System.currentTimeMillis() + timeoutMillis
                    val buffer = ByteArray(512)
                    while (System.currentTimeMillis() < deadline) {
                        val packet = DatagramPacket(buffer, buffer.size)
                        try {
                            socket.receive(packet)
                        } catch (e: Exception) {
                            continue // socket timeout; keep going until the deadline
                        }

                        val text = String(packet.data, 0, packet.length)
                        if (!text.startsWith(REPLY_PREFIX)) continue

                        val parts = text.removePrefix(REPLY_PREFIX).split('|')
                        if (parts.size < 2) continue

                        val address = packet.address.hostAddress ?: continue
                        found[address] = DiscoveredHost(
                            name = parts[0],
                            address = address,
                            port = parts[1].toIntOrNull() ?: Protocol.DEFAULT_CONTROL_PORT,
                        )
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "discovery failed", e)
            }

            found.values.toList()
        }

    private fun broadcastTargets(): List<InetAddress> {
        val targets = mutableListOf<InetAddress>()
        try {
            targets.add(InetAddress.getByName("255.255.255.255"))
        } catch (_: Exception) {
        }

        try {
            for (nic in NetworkInterface.getNetworkInterfaces()) {
                if (!nic.isUp || nic.isLoopback) continue
                for (address in nic.interfaceAddresses) {
                    address.broadcast?.let { targets.add(it) }
                }
            }
        } catch (e: Exception) {
            Log.d(TAG, "could not enumerate interfaces: ${e.message}")
        }
        return targets
    }

    /**
     * Returns the first address that accepts a connection, trying them in order.
     * Callers put the LAN address first so being at home never routes out over
     * the internet just because an external address is also configured.
     */
    suspend fun firstReachable(
        addresses: List<String>,
        port: Int,
        timeoutMillisEach: Int = 2000,
    ): String? = withContext(Dispatchers.IO) {
        for (address in addresses.filter { it.isNotBlank() }) {
            try {
                java.net.Socket().use { probe ->
                    probe.connect(InetSocketAddress(address, port), timeoutMillisEach)
                }
                return@withContext address
            } catch (e: Exception) {
                Log.d(TAG, "$address unreachable: ${e.message}")
            }
        }
        null
    }

    private const val TAG = "HostDiscovery"
}
