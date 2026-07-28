package com.thorstream.client

import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.os.Build
import android.util.Log
import java.util.Locale

/**
 * What this particular handheld can actually decode, and in what order to try.
 *
 * The codec cannot be a build-time decision. The Thor Pro is a Snapdragon 8 Gen 2
 * with hardware AV1 decode; the Thor LITE is a Snapdragon 865 with none at all,
 * and the same APK runs on both. Anything offered to the host has to be checked
 * against this device first.
 */
object CodecSupport {

    const val MIME_H264 = "video/avc"
    const val MIME_HEVC = "video/hevc"

    // Spelled out rather than MediaFormat.MIMETYPE_VIDEO_AV1, which is API 29
    // while this app ships from 26. The string is the same either way.
    const val MIME_AV1 = "video/av01"

    // Persisted in SharedPreferences, so these numbers are on users' devices:
    // append only, never renumber.
    const val PREF_AUTO = 0
    const val PREF_AV1 = 1
    const val PREF_HEVC = 2
    const val PREF_H264 = 3

    fun mimeFor(codec: Byte): String = when (codec) {
        Protocol.CODEC_AV1 -> MIME_AV1
        Protocol.CODEC_HEVC -> MIME_HEVC
        else -> MIME_H264
    }

    /**
     * The codecs to try, best first.
     *
     * A chain rather than a single answer: the host can refuse what this device
     * can decode - an older GPU has no AV1 encoder - and a decoder that
     * advertises a format can still fail to configure for it. Whatever goes
     * wrong, there has to be something left underneath.
     *
     * H.264 is the floor and is never hardware-tested. Every Android device that
     * plays video at all decodes it, and if it somehow cannot then there is
     * nothing else to fall back to anyway.
     */
    fun chainFor(preference: Int): List<Byte> {
        val automatic = listOfNotNull(
            Protocol.CODEC_AV1.takeIf { hasHardwareDecoder(MIME_AV1) },
            Protocol.CODEC_HEVC.takeIf { hasHardwareDecoder(MIME_HEVC) },
            Protocol.CODEC_H264,
        )
        val forced = when (preference) {
            PREF_AV1 -> Protocol.CODEC_AV1
            PREF_HEVC -> Protocol.CODEC_HEVC
            PREF_H264 -> Protocol.CODEC_H264
            else -> return automatic
        }
        // A forced codec is tried even when no hardware decoder was found - the
        // user asked for it, and this detection can be wrong in both directions
        // - but everything below it stays in the chain so a refusal still has
        // somewhere to land.
        return (listOf(forced) + automatic.filter { quality(it) < quality(forced) }).distinct()
    }

    /**
     * Whether a HARDWARE decoder exists for [mime].
     *
     * Hardware specifically. Android ships a software AV1 decoder on every
     * recent device, so merely finding a `video/av01` decoder proves nothing:
     * software AV1 cannot hold 1080p60, and falling into it would be worse than
     * the H.264 this replaces.
     */
    fun hasHardwareDecoder(mime: String): Boolean = hardwareDecoderName(mime) != null

    /**
     * The name of that hardware decoder, for createByCodecName.
     *
     * Worth the extra step for AV1: createDecoderByType returns whichever
     * decoder is first in the device's list, and on a device carrying both that
     * can be the software one.
     */
    fun hardwareDecoderName(mime: String): String? = synchronized(cache) {
        // containsKey rather than getOrPut: "no hardware decoder" is a real
        // answer worth caching, and getOrPut would re-enumerate on every miss.
        if (cache.containsKey(mime)) return cache.getValue(mime)
        val name = try {
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
                .asSequence()
                .filter { !it.isEncoder && isHardware(it) }
                .filter { info -> info.supportedTypes.any { it.equals(mime, ignoreCase = true) } }
                .map { it.name }
                .firstOrNull()
        } catch (e: Exception) {
            // Some devices throw out of the codec list rather than return an
            // empty one. Treat that as "no hardware decoder" instead of taking
            // the app down over it.
            Log.w(TAG, "could not enumerate decoders for $mime", e)
            null
        }
        Log.i(TAG, "hardware decoder for $mime: ${name ?: "none"}")
        cache[mime] = name
        return name
    }

    private fun isHardware(info: MediaCodecInfo): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) return info.isHardwareAccelerated
        // Before Q there is no API for this, only the naming every vendor
        // follows: Google's own software codecs are OMX.google.* and c2.android.*,
        // and a vendor's software fallback carries .sw. in its name.
        val name = info.name.lowercase(Locale.US)
        return !name.startsWith("omx.google.") &&
            !name.startsWith("c2.android.") &&
            !name.contains(".sw.")
    }

    /** Higher is better picture per bit; also the order of the fallback chain. */
    private fun quality(codec: Byte): Int = when (codec) {
        Protocol.CODEC_AV1 -> 2
        Protocol.CODEC_HEVC -> 1
        else -> 0
    }

    // Enumerating the codec list costs milliseconds and the answer cannot change
    // while the process lives.
    private val cache = HashMap<String, String?>()

    private const val TAG = "CodecSupport"
}
