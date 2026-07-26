package com.thorstream.client

import android.content.Intent
import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.GridLayoutManager
import com.thorstream.client.databinding.ActivityLibraryBinding
import kotlinx.coroutines.launch

/**
 * The library screen: your Playnite games as a cover grid. Connects to the host
 * chosen on the previous screen, shows what it has, and hands over to
 * StreamActivity when you pick something.
 */
class LibraryActivity : AppCompatActivity() {

    private lateinit var binding: ActivityLibraryBinding
    private lateinit var adapter: GameGridAdapter
    private var control: ControlConnection? = null

    private lateinit var addresses: List<String>
    private var host: String = ""
    private var bitrate = 30000
    private var fps = 60

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityLibraryBinding.inflate(layoutInflater)
        setContentView(binding.root)

        addresses = intent.getStringArrayListExtra(EXTRA_ADDRESSES)?.toList() ?: emptyList()
        bitrate = intent.getIntExtra(EXTRA_BITRATE, 30000)
        fps = intent.getIntExtra(EXTRA_FPS, 60)

        adapter = GameGridAdapter(
            onRequestCover = { gameId -> control?.requestCover(gameId) },
            onSelect = { game -> launchGame(game) },
        )

        binding.gameGrid.layoutManager = GridLayoutManager(this, columnsForWidth())
        binding.gameGrid.adapter = adapter
        binding.libraryTitle.text = getString(R.string.library_title)

        connect()
    }

    /** Tile size should stay roughly constant, so the column count follows width. */
    private fun columnsForWidth(): Int {
        val widthDp = resources.configuration.screenWidthDp
        return (widthDp / 180).coerceIn(2, 8)
    }

    private fun connect() {
        setStatus(getString(R.string.connecting))

        val connection = ControlConnection(lifecycleScope)
        control = connection

        connection.onGameList = { games ->
            runOnUiThread {
                adapter.submit(games)
                setStatus(
                    if (games.isEmpty()) "No games found — is Playnite installed on the PC?"
                    else "${games.size} games"
                )
            }
        }
        connection.onCover = { gameId, jpeg ->
            runOnUiThread { adapter.setCover(gameId, jpeg) }
        }
        connection.onError = { message -> runOnUiThread { setStatus("Host: $message") } }
        connection.onDisconnected = { runOnUiThread { setStatus("Disconnected.") } }

        lifecycleScope.launch {
            // Addresses are already in preference order, local first, so at home
            // this succeeds on the first try and never touches the external one.
            val reachable = HostDiscovery.firstReachable(addresses, Protocol.DEFAULT_CONTROL_PORT)
            if (reachable == null) {
                setStatus(getString(R.string.unreachable))
                return@launch
            }

            host = reachable
            if (addresses.size > 1 && reachable != addresses.first()) {
                setStatus(getString(R.string.connected_external, reachable))
            }

            try {
                connection.connect(host, Protocol.DEFAULT_CONTROL_PORT)
            } catch (e: Exception) {
                runOnUiThread { setStatus("Could not connect: ${e.message}") }
            }
        }
    }

    private fun launchGame(game: GameInfo) {
        // The host serves one client at a time, so release this connection before
        // the streaming screen opens its own.
        control?.close()
        control = null

        startActivity(
            Intent(this, StreamActivity::class.java).apply {
                putExtra(StreamActivity.EXTRA_HOST, host)
                putExtra(StreamActivity.EXTRA_GAME_ID, game.id)
                putExtra(StreamActivity.EXTRA_TITLE, game.name)
                putExtra(StreamActivity.EXTRA_BITRATE, bitrate)
                putExtra(StreamActivity.EXTRA_FPS, fps)
            }
        )
    }

    override fun onRestart() {
        super.onRestart()
        // Coming back from a stream, the old connection is gone; get the library
        // again so the grid is not left stale and unresponsive.
        if (control == null || control?.isConnected != true) connect()
    }

    private fun setStatus(message: String) {
        binding.libraryStatus.text = message
    }

    override fun onDestroy() {
        super.onDestroy()
        control?.close()
    }

    companion object {
        const val EXTRA_ADDRESSES = "addresses"
        const val EXTRA_BITRATE = "bitrate"
        const val EXTRA_FPS = "fps"
    }
}
