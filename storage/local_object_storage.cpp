#include "storage/local_object_storage.h"
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace cloud {
namespace {
bool link_like(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return std::filesystem::is_symlink(path);
#endif
}
}
LocalObjectStorage::LocalObjectStorage(std::filesystem::path root)
    : root_(std::filesystem::absolute(std::move(root)).lexically_normal()),
      process_lock_(root_ / ".cloudlog.lock") {}
std::filesystem::path LocalObjectStorage::resolve(const std::string& key) const {
    const std::filesystem::path relative(key);
    if (relative.empty() || relative.has_root_path()) return {};
    auto result = root_;
    for (const auto& part : relative) {
        if (part == ".." || part == "." || part.empty()) return {};
        result /= part;
        if (link_like(result)) return {};
    }
    return result.lexically_normal();
}
bool LocalObjectStorage::put(const std::string& key, std::istream& input) {
    const auto path = resolve(key);
    if (path.empty()) return false;
    return atomic_write(path, input);
}
bool LocalObjectStorage::get(const std::string& key, std::ostream& output) {
    auto input = open_read(key);
    if (!input) return false;
    char buffer[64 * 1024];
    while (*input) {
        input->read(buffer, sizeof(buffer));
        if (input->gcount() > 0) output.write(buffer, input->gcount());
    }
    return !input->bad() && output.good();
}
std::unique_ptr<std::istream> LocalObjectStorage::open_read(const std::string& key) {
    const auto path = resolve(key);
    if (path.empty()) return {};
    auto input = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*input) return {};
    return input;
}
bool LocalObjectStorage::remove(const std::string& key) {
    const auto path = resolve(key);
    if (path.empty()) return false;
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    return !ec && (removed || !std::filesystem::exists(path));
}
bool LocalObjectStorage::exists(const std::string& key) const {
    const auto path = resolve(key);
    return !path.empty() && std::filesystem::is_regular_file(path);
}
std::uint64_t LocalObjectStorage::size(const std::string& key) const {
    const auto path = resolve(key);
    return !path.empty() && std::filesystem::is_regular_file(path) ? std::filesystem::file_size(path) : 0;
}
StorageRecoveryReport LocalObjectStorage::recover(const std::unordered_set<std::string>& live_keys) {
    StorageRecoveryReport report;
    for (const auto& key : live_keys) if (!exists(key)) report.missing_keys.push_back(key);
    // A broken authoritative snapshot must not trigger cleanup.
    if (!report.missing_keys.empty()) return report;
    for (const auto* mode : {"shallow", "deep"}) {
        const auto directory = root_ / mode;
        if (!std::filesystem::exists(directory)) continue;
        if (link_like(directory)) throw std::runtime_error("Object directory must not be a link");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (link_like(entry.path())) throw std::runtime_error("Object store contains a link");
            if (!entry.is_regular_file()) continue;
            const auto key = std::filesystem::relative(entry.path(), root_).generic_string();
            if (live_keys.count(key)) continue;
            // Retain orphaned and partial objects, rather than deleting user data.
            const auto target = root_ / ".recovery" / unique_token() / entry.path().filename();
            std::filesystem::create_directories(target.parent_path());
            if (!replace_file(entry.path(), target))
                throw std::runtime_error("Cannot quarantine uncommitted object");
            ++report.quarantined;
        }
    }
    return report;
}
}
