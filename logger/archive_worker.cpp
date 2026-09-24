#include "logger/archive_worker.h"
#include "common/durable_file.h"
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace cloud {
ArchiveWorker::ArchiveWorker(const AppConfig& config, FileService& files)
    : config_(config), files_(files),
      spool_(std::filesystem::path(config.log_file).parent_path() / ".archive-spool") {
    if (!config.archive_batch_size || config.archive_batch_size > 256 || !config.archive_retry_ms)
        throw std::invalid_argument("Invalid archive worker limits");
    std::filesystem::create_directories(spool_);
    // Recover logs rotated before a crash happened between rename and enqueue.
    for (std::size_t i = 1; i <= config.log_max_backups; ++i) {
        const auto rotated = std::filesystem::path(config.log_file + "." + std::to_string(i));
        if (std::filesystem::is_regular_file(rotated)) enqueue(rotated);
    }
    thread_ = std::thread(&ArchiveWorker::run, this);
}
ArchiveWorker::~ArchiveWorker() { stop(); }
void ArchiveWorker::enqueue(const std::filesystem::path& rotated) {
    const auto name = "cloud_" + unique_token() + ".log.pending";
    const auto pending = spool_ / name;
    const auto temp = spool_ / (name + ".tmp");
    std::error_code ec;
    std::filesystem::create_hard_link(rotated, temp, ec);
    bool ok = false;
    if (!ec) {
        ok = sync_file(temp) && replace_file(temp, pending);
        if (!ok) std::filesystem::remove(temp, ec);
    } else {
        std::ifstream input(rotated, std::ios::binary);
        ok = input && atomic_write(pending, input);
    }
    if (!ok) { ++failures_; throw std::runtime_error("Cannot persist archive job"); }
    { std::lock_guard lock(mutex_); notified_ = true; }
    cv_.notify_one();
}
void ArchiveWorker::stop() {
    std::lock_guard serial(stop_mutex_);
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}
ArchiveStats ArchiveWorker::stats() const {
    return {archived_.load(), duplicates_.load(), failures_.load(), pending_.load()};
}
void ArchiveWorker::run() {
    try {
    // Recover a completed hard-link snapshot whose publication was interrupted.
    for (const auto& entry : std::filesystem::directory_iterator(spool_)) {
        auto name = entry.path().filename().string();
        if (entry.is_regular_file() && name.size() >= 12 &&
            name.compare(name.size() - 12, 12, ".pending.tmp") == 0) {
            const auto target = spool_ / name.substr(0, name.size() - 4);
            if (!sync_file(entry.path()) || !replace_file(entry.path(), target)) ++failures_;
        }
    }
    } catch (...) { ++failures_; }
    for (;;) {
        bool immediate = false;
        try {
            std::vector<std::filesystem::path> batch;
            std::uint64_t count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(spool_)) {
                if (entry.is_regular_file() && entry.path().extension() == ".pending") {
                    ++count;
                    if (batch.size() < config_.archive_batch_size) batch.push_back(entry.path());
                }
            }
            pending_ = count;
            bool failed = false;
            for (const auto& path : batch) {
                try {
                    std::ifstream input(path, std::ios::binary);
                    if (!input) throw std::runtime_error("Archive snapshot missing");
                    auto name = path.filename().string();
                    name.resize(name.size() - std::string(".pending").size());
                    const auto result = files_.upload_checked("system", name, input, "system-logs",
                                                              StorageMode::Deep, false);
                    input.close();
                    if (result.status == UploadStatus::Failed) throw std::runtime_error("Archive upload failed");
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                    if (ec) throw std::runtime_error("Archive cleanup failed");
                    if (result.status == UploadStatus::Duplicate) ++duplicates_; else ++archived_;
                } catch (...) { ++failures_; failed = true; }
            }
            immediate = !failed && count > batch.size();
        } catch (...) { ++failures_; }
        std::unique_lock lock(mutex_);
        if (stopping_) return;
        if (immediate) continue;
        cv_.wait_for(lock, std::chrono::milliseconds(config_.archive_retry_ms),
                     [&] { return stopping_ || notified_; });
        notified_ = false;
        if (stopping_) return;
    }
}
}
