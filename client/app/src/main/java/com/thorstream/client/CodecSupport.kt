package com.thorstream.client

/**
 * Which codecs this client asks for, and what they mean to MediaCodec.
 *
 * H.265 is the choice, on every device and without asking: it is the one codec
 * both ends do well, and a per-device answer only ever meant more ways for the
 * picture not to arrive.
 *
 * H.264 sits underneath it all the same. It is not a second choice offered to
 * anyone - nothing ever requests it - it is the floor [chain] leaves in place so
 * that an HEVC decoder which refuses to configure has somewhere to land instead
 * of a black screen with no way out. Every Android device that plays video at
 * all decodes H.264, and if one somehow cannot then there is nothing else left
 * to try anyway.
 */
object CodecSupport {

    const val MIME_H264 = "video/avc"
    const val MIME_HEVC = "video/hevc"

    fun mimeFor(codec: Byte): String = when (codec) {
        Protocol.CODEC_HEVC -> MIME_HEVC
        else -> MIME_H264
    }

    /** The codecs to try, best first. Fixed: there is nothing to decide. */
    val chain: List<Byte> = listOf(Protocol.CODEC_HEVC, Protocol.CODEC_H264)
}
