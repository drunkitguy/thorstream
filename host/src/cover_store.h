#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace thorstream {

// Cover art the user supplied themselves, which wins over whatever Playnite has.
//
// Playnite's artwork is missing for some entries and wrong for others - emulator
// and store imports especially - and it is not this program's business to write
// into another application's library. So overrides live beside the host's own
// data and simply take precedence when present.
class CoverStore {
public:
    // Absolute path to the override for this game, or empty if there is none.
    static std::wstring Find(const std::string& gameId);

    // Replaces any existing override. `bytes` is the raw image file exactly as
    // uploaded; the format is whatever WIC can decode, sniffed from the content
    // rather than trusted from a name the browser supplied.
    static bool Save(const std::string& gameId, const std::vector<uint8_t>& bytes,
                     std::string* error);

    // Deletes the override, so Playnite's art applies again.
    static bool Remove(const std::string& gameId);

    // %LOCALAPPDATA%\thorstream\covers
    static std::wstring Directory();

    // Game ids reach this class straight from an HTTP query string, so they are
    // untrusted input that is about to become part of a file path. Only Playnite
    // GUID characters are allowed through; anything else - a dot, a slash, a
    // colon - is rejected outright rather than escaped, because there is no
    // legitimate id that needs them.
    static bool IsValidId(const std::string& gameId);
};

}  // namespace thorstream
