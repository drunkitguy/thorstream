package com.thorstream.client

import android.content.Intent
import android.os.Bundle
import android.widget.ArrayAdapter
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.thorstream.client.databinding.ActivityMainBinding
import kotlinx.coroutines.launch

/** Connection screen: pick a host, then pick a window on it. */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private var control: ControlConnection? = null
    private var windows: List<WindowInfo> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        binding.hostInput.setText(prefs.getString(KEY_HOST, ""))
        binding.bitrateInput.setText(prefs.getInt(KEY_BITRATE, 30000).toString())
        binding.fpsInput.setText(prefs.getInt(KEY_FPS, 60).toString())

        binding.connectButton.setOnClickListener { connect() }

        binding.windowList.setOnItemClickListener { _, _, position, _ ->
            windows.getOrNull(position)?.let { startStream(it) }
        }
    }

    private fun connect() {
        val host = binding.hostInput.text.toString().trim()
        if (host.isEmpty()) {
            setStatus("Enter your PC's IP address first.")
            return
        }

        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString(KEY_HOST, host)
            .putInt(KEY_BITRATE, bitrateKbps())
            .putInt(KEY_FPS, fps())
            .apply()

        control?.close()
        setStatus("Connecting to $host...")

        val connection = ControlConnection(lifecycleScope)
        control = connection

        connection.onWindowList = { list ->
            runOnUiThread {
                windows = list
                binding.windowList.adapter = ArrayAdapter(
                    this,
                    android.R.layout.simple_list_item_1,
                    list.map { "${it.process}  —  ${it.title}  (${it.width}x${it.height})" },
                )
                setStatus(
                    if (list.isEmpty()) "No capturable windows on that PC."
                    else "Pick a window to stream."
                )
            }
        }
        connection.onError = { message -> runOnUiThread { setStatus("Host error: $message") } }
        connection.onDisconnected = { runOnUiThread { setStatus("Disconnected.") } }

        lifecycleScope.launch {
            try {
                connection.connect(host, Protocol.DEFAULT_CONTROL_PORT)
            } catch (e: Exception) {
                runOnUiThread {
                    setStatus("Could not connect: ${e.message}\nIs the host running with --serve?")
                }
            }
        }
    }

    private fun startStream(window: WindowInfo) {
        // The host serves one client at a time, so hand the connection over
        // rather than holding a second one open from this screen.
        control?.close()
        control = null

        startActivity(
            Intent(this, StreamActivity::class.java).apply {
                putExtra(StreamActivity.EXTRA_HOST, binding.hostInput.text.toString().trim())
                putExtra(StreamActivity.EXTRA_WINDOW_ID, window.id)
                putExtra(StreamActivity.EXTRA_TITLE, window.title)
                putExtra(StreamActivity.EXTRA_BITRATE, bitrateKbps())
                putExtra(StreamActivity.EXTRA_FPS, fps())
            }
        )
    }

    private fun bitrateKbps(): Int =
        binding.bitrateInput.text.toString().toIntOrNull()?.coerceIn(1000, 200_000) ?: 30000

    private fun fps(): Int = binding.fpsInput.text.toString().toIntOrNull()?.coerceIn(24, 144) ?: 60

    private fun setStatus(message: String) {
        binding.statusText.text = message
    }

    override fun onDestroy() {
        super.onDestroy()
        control?.close()
    }

    companion object {
        private const val PREFS = "thorstream"
        private const val KEY_HOST = "host"
        private const val KEY_BITRATE = "bitrate"
        private const val KEY_FPS = "fps"
    }
}
