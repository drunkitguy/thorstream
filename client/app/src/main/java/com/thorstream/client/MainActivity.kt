package com.thorstream.client

import android.content.Intent
import android.os.Bundle
import android.widget.ArrayAdapter
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.thorstream.client.databinding.ActivityMainBinding
import kotlinx.coroutines.launch

/**
 * First screen: which PC to stream from.
 *
 * Hosts on the same network announce themselves, so the usual path is to tap one
 * of them. The manual fields exist for a PC that broadcast cannot reach - most
 * obviously one you are connecting to from outside the house.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private var discovered: List<DiscoveredHost> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        binding.hostInput.setText(prefs.getString(KEY_HOST, ""))
        binding.externalInput.setText(prefs.getString(KEY_EXTERNAL, ""))
        binding.bitrateInput.setText(prefs.getInt(KEY_BITRATE, 30000).toString())
        binding.fpsInput.setText(prefs.getInt(KEY_FPS, 60).toString())

        binding.hostList.setOnItemClickListener { _, _, position, _ ->
            discovered.getOrNull(position)?.let { open(listOf(it.address)) }
        }
        binding.rescanButton.setOnClickListener { scan() }
        binding.connectButton.setOnClickListener { connectManually() }

        // Launched by a front end such as Cocoon rather than by hand: skip the
        // picker entirely and go straight to the game it named.
        val requested = requestedGameName()
        if (requested != null) {
            launchExternally(requested)
            return
        }

        scan()
    }

    /**
     * The game an external launcher asked for, or null for a normal start.
     *
     * A name can be given directly, but the usual route is a file path: Cocoon
     * names its tag file after the game, so the base name is the title. Parsing
     * it here rather than relying on a front end's own substitution tokens keeps
     * this working whatever those turn out to be called.
     */
    private fun requestedGameName(): String? {
        intent.getStringExtra(EXTRA_GAME)?.trim()?.takeIf { it.isNotEmpty() }?.let { return it }

        val path = intent.getStringExtra(EXTRA_PATH)?.trim() ?: intent.data?.path
        if (path.isNullOrEmpty()) return null
        return path.substringAfterLast('/')
            .substringAfterLast('\\')
            .substringBeforeLast('.')
            .trim()
            .takeIf { it.isNotEmpty() }
    }

    private fun launchExternally(gameName: String) {
        binding.statusText.text = getString(R.string.searching)
        lifecycleScope.launch {
            val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
            val known = listOf(
                intent.getStringExtra(EXTRA_HOST)?.trim().orEmpty(),
                prefs.getString(KEY_HOST, "").orEmpty(),
                prefs.getString(KEY_EXTERNAL, "").orEmpty(),
            ).filter { it.isNotEmpty() }

            // Falling back to a broadcast scan means a launcher entry keeps
            // working after the PC's address changes, which it will.
            val addresses = known.ifEmpty { HostDiscovery.scan().map { it.address } }
            if (addresses.isEmpty()) {
                binding.statusText.text = getString(R.string.none_found)
                return@launch
            }

            startActivity(
                Intent(this@MainActivity, LibraryActivity::class.java).apply {
                    putStringArrayListExtra(LibraryActivity.EXTRA_ADDRESSES, ArrayList(addresses))
                    putExtra(LibraryActivity.EXTRA_BITRATE, bitrateKbps())
                    putExtra(LibraryActivity.EXTRA_FPS, fps())
                    putExtra(LibraryActivity.EXTRA_AUTOLAUNCH, gameName)
                }
            )
            // Finish, so backing out of the stream returns to the front end
            // rather than to a host picker the user never asked for.
            finish()
        }
    }

    private fun scan() {
        binding.statusText.text = getString(R.string.searching)
        lifecycleScope.launch {
            val hosts = HostDiscovery.scan()
            discovered = hosts
            binding.hostList.adapter = ArrayAdapter(
                this@MainActivity,
                android.R.layout.simple_list_item_1,
                hosts.map { "${it.name}\n${it.address}" },
            )
            binding.statusText.text = when {
                hosts.isEmpty() -> getString(R.string.none_found)
                hosts.size == 1 -> getString(R.string.found_one)
                else -> getString(R.string.found_many, hosts.size)
            }
        }
    }

    private fun connectManually() {
        val local = binding.hostInput.text.toString().trim()
        val external = binding.externalInput.text.toString().trim()
        if (local.isEmpty() && external.isEmpty()) {
            binding.statusText.text = getString(R.string.none_found)
            return
        }
        // Local first, always: being at home should never route out over the
        // internet just because an external address is also configured.
        open(listOf(local, external).filter { it.isNotEmpty() })
    }

    private fun open(addresses: List<String>) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString(KEY_HOST, binding.hostInput.text.toString().trim())
            .putString(KEY_EXTERNAL, binding.externalInput.text.toString().trim())
            .putInt(KEY_BITRATE, bitrateKbps())
            .putInt(KEY_FPS, fps())
            .apply()

        startActivity(
            Intent(this, LibraryActivity::class.java).apply {
                putStringArrayListExtra(LibraryActivity.EXTRA_ADDRESSES, ArrayList(addresses))
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
        private const val KEY_EXTERNAL = "external"
        private const val KEY_BITRATE = "bitrate"
        private const val KEY_FPS = "fps"

        // The external launch contract, used by the Cocoon platform definition
        // in cocoon/. Documented in cocoon/README.md - changing these names
        // breaks anyone's existing launcher entries.
        const val EXTRA_GAME = "game"
        const val EXTRA_PATH = "path"
        const val EXTRA_HOST = "host"
    }
}
