#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace thorstream {

// Playnite's cover images are full-size artwork - over a megabyte each. Sending
// them untouched would make a grid of 37 tiles crawl, so they are decoded,
// scaled down and re-encoded as JPEG before going over the wire.
class CoverArt {
public:
    // Returns false if the file is missing or not a decodable image.
    static bool LoadThumbnail(const std::wstring& path, int maxWidth,
                              std::vector<uint8_t>* jpegOut);
};

}  // namespace thorstream
