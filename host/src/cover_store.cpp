#include "cover_store.h"

#include <Windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>

namespace thorstream {
namespace {

// One extension for everything. The image format is determined by decoding the
// content, so recording the uploaded name would add a second, less trustworthy
// source of truth about what the file actually is.
constexpr wchar_t kExtension[] = L".cover";

}  // namespace

bool CoverStore::IsValidId(const std::string& gameId) {
    if (gameId.empty() || gameId.size() > 128) return false;
    for (const char c : gameId) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!allowed) return false;
    }
    return true;
}

std::wstring CoverStore::Directory() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        return {};
    }
    std::wstring path = localAppData;
    CoTaskMemFree(localAppData);
    return path + L"\\thorstream\\covers";
}

std::wstring CoverStore::Find(const std::string& gameId) {
    if (!IsValidId(gameId)) return {};

    const std::wstring directory = Directory();
    if (directory.empty()) return {};

    const std::wstring path =
        directory + L"\\" + std::wstring(gameId.begin(), gameId.end()) + kExtension;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    return path;
}

bool CoverStore::Save(const std::string& gameId, const std::vector<uint8_t>& bytes,
                      std::string* error) {
    if (!IsValidId(gameId)) {
        *error = "that game id is not valid";
        return false;
    }
    if (bytes.empty()) {
        *error = "the uploaded image was empty";
        return false;
    }

    const std::wstring directory = Directory();
    if (directory.empty()) {
        *error = "could not locate the local app data folder";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        *error = "could not create the covers folder";
        return false;
    }

    const std::wstring path =
        directory + L"\\" + std::wstring(gameId.begin(), gameId.end()) + kExtension;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        *error = "could not open the cover file for writing";
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        *error = "could not write the cover file";
        return false;
    }
    return true;
}

bool CoverStore::Remove(const std::string& gameId) {
    const std::wstring path = Find(gameId);
    if (path.empty()) return false;

    std::error_code ec;
    return std::filesystem::remove(path, ec);
}

}  // namespace thorstream
