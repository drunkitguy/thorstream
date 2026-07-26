package com.thorstream.client

import android.content.Intent
import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import com.thorstream.client.databinding.ActivityMainBinding

/**
 * First screen: which PC to stream from, and the quality to ask for.
 *
 * It deliberately does not connect. The host serves one client at a time, so
 * holding a connection here only to hand it over would mean disconnecting and
 * reconnecting anyway; the library screen owns its own connection instead.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        binding.hostInput.setText(prefs.getString(KEY_HOST, ""))
        binding.bitrateInput.setText(prefs.getInt(KEY_BITRATE, 30000).toString())
        binding.fpsInput.setText(prefs.getInt(KEY_FPS, 60).toString())

        binding.connectButton.setOnClickListener { openLibrary() }
    }

    private fun openLibrary() {
        val host = binding.hostInput.text.toString().trim()
        if (host.isEmpty()) {
            binding.statusText.text = getString(R.string.status_idle)
            return
        }

        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString(KEY_HOST, host)
            .putInt(KEY_BITRATE, bitrateKbps())
            .putInt(KEY_FPS, fps())
            .apply()

        startActivity(
            Intent(this, LibraryActivity::class.java).apply {
                putExtra(LibraryActivity.EXTRA_HOST, host)
                putExtra(LibraryActivity.EXTRA_BITRATE, bitrateKbps())
                putExtra(LibraryActivity.EXTRA_FPS, fps())
            }
        )
    }

    private fun bitrateKbps(): Int =
        binding.bitrateInput.text.toString().toIntOrNull()?.coerceIn(1000, 200_000) ?: 30000

    private fun fps(): Int = binding.fpsInput.text.toString().toIntOrNull()?.coerceIn(24, 144) ?: 60

    companion object {
        private const val PREFS = "thorstream"
        private const val KEY_HOST = "host"
        private const val KEY_BITRATE = "bitrate"
        private const val KEY_FPS = "fps"
    }
}
