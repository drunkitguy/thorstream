package com.thorstream.client

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

/**
 * Console-style cover grid.
 *
 * Artwork is requested lazily as tiles come into view and cached in memory, so
 * opening the library does not stall on downloading every cover at once.
 */
class GameGridAdapter(
    private val onRequestCover: (String) -> Unit,
    private val onSelect: (GameInfo) -> Unit,
) : RecyclerView.Adapter<GameGridAdapter.Tile>() {

    private var games: List<GameInfo> = emptyList()
    private val covers = HashMap<String, Bitmap?>()
    private val requested = HashSet<String>()

    fun submit(list: List<GameInfo>) {
        games = list
        notifyDataSetChanged()
    }

    /** null bytes means the host has no artwork for this game. */
    fun setCover(gameId: String, jpeg: ByteArray?) {
        covers[gameId] = if (jpeg == null || jpeg.isEmpty()) {
            null
        } else {
            BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size)
        }
        val index = games.indexOfFirst { it.id == gameId }
        if (index >= 0) notifyItemChanged(index)
    }

    override fun getItemCount() = games.size

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Tile {
        val view = LayoutInflater.from(parent.context).inflate(R.layout.item_game, parent, false)
        return Tile(view)
    }

    override fun onBindViewHolder(holder: Tile, position: Int) {
        val game = games[position]
        holder.name.text = game.name
        holder.subtitle.text = game.subtitle

        // Box art is roughly 2:3, so size the image from the tile width to keep
        // the grid even regardless of what the host sends back.
        holder.cover.post {
            val width = holder.itemView.width
            if (width > 0) {
                holder.cover.layoutParams.height = (width * 3) / 2
                holder.cover.requestLayout()
            }
        }

        val cover = covers[game.id]
        if (cover != null) {
            holder.cover.setImageBitmap(cover)
            holder.cover.visibility = View.VISIBLE
            holder.placeholder.visibility = View.GONE
        } else {
            holder.cover.setImageDrawable(null)
            holder.cover.visibility = View.GONE
            holder.placeholder.visibility = View.VISIBLE
            // Distinguish "still loading" from "this game has no art".
            holder.placeholder.text = if (covers.containsKey(game.id)) game.name else "…"
        }

        if (requested.add(game.id)) onRequestCover(game.id)

        holder.itemView.setOnClickListener { onSelect(game) }
    }

    class Tile(view: View) : RecyclerView.ViewHolder(view) {
        val cover: ImageView = view.findViewById(R.id.cover)
        val placeholder: TextView = view.findViewById(R.id.placeholder)
        val name: TextView = view.findViewById(R.id.gameName)
        val subtitle: TextView = view.findViewById(R.id.gameSubtitle)
    }
}
