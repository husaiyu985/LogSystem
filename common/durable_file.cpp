#include "common/durable_file.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif

namespace cloud {
std::string unique_token() {
    static std::atomic<std::uint64_t> counter{0};
    thread_local std::mt19937_64 random{std::random_device{}()};
    std::ostringstream out;
    out << std::hex << std::chrono::steady_clock::now().time_since_epoch().count()
        << '-' << random() << '-' << counter.fetch_add(1);
    return out.str();
}

bool sync_file(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    // Same-directory temporary file; never delete the previous snapshot first.
    return MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) return false;
    const auto dir = to.parent_path().empty() ? std::filesystem::path(".") : to.parent_path();
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
    // Rename is the commit point; never report a rollback after publication.
    return true;
#endif
}

bool atomic_write(const std::filesystem::path& path, std::istream& input) {
    const auto temp = std::filesystem::path(path.string() + ".cloudlog-tmp-" + unique_token());
    std::error_code ec;
    try {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        char buffer[64 * 1024];
        while (input) {
            input.read(buffer, sizeof(buffer));
            const auto n = input.gcount();
            if (n > 0) out.write(buffer, n);
        }
        if (input.bad() || (input.fail() && !input.eof())) {
            out.close(); std::filesystem::remove(temp, ec); return false;
        }
        out.flush();
        const bool good = out.good();
        out.close();
        if (!good || out.fail() || !sync_file(temp) || !replace_file(temp, path)) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    } catch (...) {
        std::filesystem::remove(temp, ec);
        return false;
    }
}

ProcessFileLock::ProcessFileLock(const std::filesystem::path& path) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
#ifdef _WIN32
    const auto h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) handle_ = reinterpret_cast<std::intptr_t>(h);
#else
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0) handle_ = fd;
    else if (fd >= 0) ::close(fd);
#endif
    if (handle_ == -1) throw std::runtime_error("Store is locked or not writable: " + path.string());
}
ProcessFileLock::~ProcessFileLock() {
    if (handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    ::flock(static_cast<int>(handle_), LOCK_UN);
    ::close(static_cast<int>(handle_));
#endif
}
}

