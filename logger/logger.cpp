#include "logger/logger.h"
#include "common/durable_file.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace cloud {
const char* to_string(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "UNKNOWN";
}
Logger::~Logger() { stop(); }
Logger& Logger::instance() { static Logger logger; return logger; }

void Logger::start(const AppConfig& config) {
    std::lock_guard lifecycle(lifecycle_mutex_);
    std::lock_guard lock(mutex_);
    if (running_) return;
    if (!config.log_queue_capacity || !config.log_queue_bytes || !config.log_flush_interval_ms ||
        (config.log_max_file_size && !config.log_max_backups) ||
        (config.log_critical_max_file_size && !config.log_critical_max_backups) ||
        std::filesystem::absolute(config.log_file).lexically_normal() ==
        std::filesystem::absolute(config.critical_log_file).lexically_normal())
        throw std::invalid_argument("Invalid logger limits or identical log paths");
    config_ = config;
    const std::filesystem::path path(config.log_file), critical(config.critical_log_file);
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    if (!critical.parent_path().empty()) std::filesystem::create_directories(critical.parent_path());
    file_.clear(); critical_file_.clear();
    file_.open(path, std::ios::app | std::ios::binary);
    critical_file_.open(critical, std::ios::app | std::ios::binary);
    if (!file_ || !critical_file_) {
        file_.close(); critical_file_.close();
        throw std::runtime_error("Cannot open normal/critical log file");
    }
    current_size_ = std::filesystem::file_size(path);
    critical_size_ = std::filesystem::file_size(critical);
    queue_.clear(); queued_bytes_ = peak_queued_ = fallback_inflight_ = 0;
    accepted_ = completed_ = flush_target_ = 0;
    written_ = dropped_ = rejected_ = fallback_ = critical_written_ = write_errors_ = rotation_errors_ = 0;
    flush_requested_ = false; flush_result_ = true;
    running_ = true; exited_ = false;
    try { worker_ = std::thread(&Logger::worker_loop, this); }
    catch (...) { running_ = false; exited_ = true; file_.close(); critical_file_.close(); throw; }
}
void Logger::stop() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    {
        std::lock_guard lock(mutex_);
        if (!running_ && !worker_.joinable()) return;
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    {
        std::unique_lock lock(mutex_);
        completed_cv_.wait(lock, [&] { return fallback_inflight_ == 0; });
    }
    std::lock_guard sink(sink_mutex_);
    flush_sinks();
    file_.close(); critical_file_.close();
}
bool Logger::running() const { std::lock_guard lock(mutex_); return running_; }
LoggerStats Logger::stats() const {
    std::lock_guard lock(mutex_);
    return {accepted_, written_.load(), dropped_.load(), rejected_.load(), fallback_.load(),
            critical_written_.load(), write_errors_.load(), rotation_errors_.load(),
            queue_.size(), queued_bytes_, peak_queued_};
}
void Logger::set_rotation_callback(RotationCallback callback) {
    std::lock_guard lock(callback_mutex_); rotation_callback_ = std::move(callback);
}
std::string Logger::format_line(LogLevel level, const std::string& message) {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream out;
    out << '[' << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] [" << to_string(level)
        << "] [tid=" << std::this_thread::get_id() << "] ";
    // Bound a single record even when a caller passes a huge message.
    constexpr std::size_t max_message = 64 * 1024;
    out.write(message.data(), static_cast<std::streamsize>(std::min(message.size(), max_message)));
    if (message.size() > max_message) out << " [message truncated]";
    return out.str();
}
void Logger::log(LogLevel level, std::string message) {
    const auto line = format_line(level, message);
    std::unique_lock lock(mutex_);
    if (!running_) { ++rejected_; return; }
    if (queue_.size() >= config_.log_queue_capacity ||
        line.size() > config_.log_queue_bytes - queued_bytes_) {
        if (level != LogLevel::Error && level != LogLevel::Fatal) { ++dropped_; return; }
        ++fallback_; ++fallback_inflight_;
        lock.unlock();
        {
            std::lock_guard sink(sink_mutex_);
            try { write_critical(line); } catch (...) { ++write_errors_; }
        }
        lock.lock();
        --fallback_inflight_;
        lock.unlock();
        completed_cv_.notify_all();
        return;
    }
    const auto next = accepted_ + 1;
    queue_.push_back({level, line, next});
    accepted_ = next; queued_bytes_ += line.size();
    peak_queued_ = std::max(peak_queued_, queue_.size());
    lock.unlock(); cv_.notify_one();
}
bool Logger::flush() {
    // Serializes callers; a later flush cannot move an earlier caller's barrier.
    std::lock_guard serial(flush_mutex_);
    std::unique_lock lock(mutex_);
    if (exited_) return write_errors_.load() == 0;
    flush_target_ = accepted_;
    flush_requested_ = true;
    cv_.notify_one();
    completed_cv_.wait(lock, [&] { return !flush_requested_ || exited_; });
    return flush_result_ && write_errors_.load() == 0;
}
bool Logger::write_critical(const std::string& line) {
    if (!critical_file_.is_open()) { ++write_errors_; return false; }
    if (config_.log_critical_max_file_size && critical_size_ > 0 &&
        critical_size_ + line.size() + 1 > config_.log_critical_max_file_size) {
        critical_file_.close();
        try {
            const auto path = std::filesystem::path(config_.critical_log_file);
            for (std::size_t i = config_.log_critical_max_backups; i > 1; --i) {
                const auto from = std::filesystem::path(path.string() + "." + std::to_string(i-1));
                const auto to = std::filesystem::path(path.string() + "." + std::to_string(i));
                if (std::filesystem::exists(from) && !replace_file(from,to))
                    throw std::runtime_error("Critical log rotation failed");
            }
            if (!replace_file(path, path.string() + ".1"))
                throw std::runtime_error("Critical log rotation failed");
        } catch (...) { ++rotation_errors_; }
        critical_file_.clear();
        critical_file_.open(config_.critical_log_file, std::ios::app | std::ios::binary);
        std::error_code ec;
        critical_size_ = std::filesystem::file_size(config_.critical_log_file, ec);
        if (ec) critical_size_ = 0;
    }
    critical_file_.clear();
    critical_file_ << line << '\n';
    if (critical_file_) critical_size_ += line.size() + 1;
    critical_file_.flush();
    const bool ok = critical_file_.good() &&
        (!config_.log_critical_durable || sync_file(config_.critical_log_file));
    if (ok) ++critical_written_; else ++write_errors_;
    return ok;
}
bool Logger::flush_sinks() {
    bool ok = true;
    if (file_.is_open()) { file_.flush(); ok = file_.good(); }
    if (critical_file_.is_open()) { critical_file_.flush(); ok = critical_file_.good() && ok; }
    if (!ok) ++write_errors_;
    return ok;
}
std::filesystem::path Logger::rotate_if_needed(std::size_t size) {
    if (!config_.log_max_file_size || current_size_ == 0 ||
        current_size_ + size <= config_.log_max_file_size) return {};
    const std::filesystem::path path(config_.log_file);
    file_.close();
    std::filesystem::path rotated;
    try {
        for (std::size_t i = config_.log_max_backups; i > 1; --i) {
            const auto from = std::filesystem::path(path.string() + "." + std::to_string(i - 1));
            const auto to = std::filesystem::path(path.string() + "." + std::to_string(i));
            if (std::filesystem::exists(from) && !replace_file(from, to))
                throw std::runtime_error("Log rotation rename failed");
        }
        rotated = path.string() + ".1";
        if (!replace_file(path, rotated)) throw std::runtime_error("Active log rename failed");
    } catch (...) { ++rotation_errors_; rotated.clear(); }
    file_.clear();
    file_.open(path, std::ios::app | std::ios::binary);
    if (!file_) ++write_errors_;
    std::error_code ec;
    current_size_ = std::filesystem::file_size(path, ec);
    if (ec) current_size_ = 0;
    return rotated;
}
void Logger::worker_loop() {
    using Clock = std::chrono::steady_clock;
    auto last_flush = Clock::now();
    const auto interval = std::chrono::milliseconds(config_.log_flush_interval_ms);
    for (;;) {
        std::vector<Record> batch;
        bool ending = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_until(lock, last_flush + interval,
                [&] { return !queue_.empty() || !running_ || flush_requested_; });
            ending = !running_ && queue_.empty();
            while (!queue_.empty() && batch.size() < 128) {
                queued_bytes_ -= queue_.front().line.size();
                batch.push_back(std::move(queue_.front())); queue_.pop_front();
            }
        }
        for (const auto& record : batch) {
            std::filesystem::path rotated;
            try {
                std::lock_guard sink(sink_mutex_);
                rotated = rotate_if_needed(record.line.size() + 1);
                if (config_.log_console_enabled) std::cout << record.line << '\n';
                if (file_.is_open()) {
                    file_.clear(); file_ << record.line << '\n';
                    if (file_) { current_size_ += record.line.size() + 1; ++written_; }
                    else ++write_errors_;
                } else ++write_errors_;
                if (record.level == LogLevel::Error || record.level == LogLevel::Fatal)
                    write_critical(record.line);
            } catch (...) { ++write_errors_; }
            if (!rotated.empty()) {
                RotationCallback callback;
                { std::lock_guard lock(callback_mutex_); callback = rotation_callback_; }
                if (callback) {
                    try { callback(rotated); } catch (...) { ++rotation_errors_; }
                }
            }
            { std::lock_guard lock(mutex_); completed_ = record.sequence; }
        }
        bool barrier;
        {
            std::lock_guard lock(mutex_);
            barrier = flush_requested_ && completed_ >= flush_target_;
        }
        if (ending || barrier || Clock::now() - last_flush >= interval) {
            bool ok;
            { std::lock_guard sink(sink_mutex_); ok = flush_sinks(); }
            last_flush = Clock::now();
            {
                std::lock_guard lock(mutex_);
                if (barrier || ending) { flush_result_ = ok; flush_requested_ = false; }
                if (ending) exited_ = true;
            }
            completed_cv_.notify_all();
        }
        if (ending) return;
    }
}
} // namespace cloud
