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

    // Set when a front end named a game to start; consumed once.
    private var autoLaunch: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityLibraryBinding.inflate(layoutInflater)
        setContentView(binding.root)

        addresses = intent.getStringArrayListExtra(EXTRA_ADDRESSES)?.toList() ?: emptyList()
        bitrate = intent.getIntExtra(EXTRA_BITRATE, 30000)
        fps = intent.getIntExtra(EXTRA_FPS, 60)
        autoLaunch = intent.getStringExtra(EXTRA_AUTOLAUNCH)

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

                // Cleared before use, so returning from the stream lands on the
                // library rather than immediately relaunching the same game.
                val wanted = autoLaunch
                autoLaunch = null
                if (wanted != null) {
                    val match = findGame(games, wanted)
                    if (match != null) launchGame(match)
                    else setStatus("\"$wanted\" is not in the Playnite library")
                }
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

    /**
     * Matches a game by name, tolerating the fact that the name arrived as a
     * filename. Windows cannot put a colon or a question mark in one, so an
     * exact match is tried first and then a comparison that ignores everything
     * except letters and digits.
     */
    private fun findGame(games: List<GameInfo>, wanted: String): GameInfo? {
        games.firstOrNull { it.name.equals(wanted, ignoreCase = true) }?.let { return it }
        val loose = wanted.filter { it.isLetterOrDigit() }.lowercase()
        if (loose.isEmpty()) return null
        return games.firstOrNull { it.name.filter { c -> c.isLetterOrDigit() }.lowercase() == loose }
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
        const val EXTRA_AUTOLAUNCH = "autolaunch"
    }
}
