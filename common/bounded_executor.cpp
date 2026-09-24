#include "common/bounded_executor.h"
#include <algorithm>
#include <stdexcept>
namespace cloud {
BoundedExecutor::BoundedExecutor(std::size_t workers, std::size_t capacity) : capacity_(capacity) {
    if (!workers || !capacity) throw std::invalid_argument("Executor limits must be positive");
    stats_.workers = workers;
    try { for (std::size_t i = 0; i < workers; ++i) threads_.emplace_back(&BoundedExecutor::run, this); }
    catch (...) { shutdown(); throw; }
}
BoundedExecutor::~BoundedExecutor() { shutdown(); }
bool BoundedExecutor::submit(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || tasks_.size() >= capacity_) { ++stats_.rejected; return false; }
        tasks_.push_back(std::move(task));
        ++stats_.accepted;
        stats_.peak_queued = std::max(stats_.peak_queued, tasks_.size());
    }
    cv_.notify_one();
    return true;
}
void BoundedExecutor::shutdown() {
    std::lock_guard lifecycle(shutdown_mutex_);
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cv_.notify_all();
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
}
ExecutorStats BoundedExecutor::stats() const {
    std::lock_guard lock(mutex_);
    auto result = stats_; result.queued = tasks_.size(); return result;
}
void BoundedExecutor::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty() && stopping_) return;
            task = std::move(tasks_.front()); tasks_.pop_front();
            ++stats_.active;
            stats_.peak_active = std::max(stats_.peak_active, stats_.active);
        }
        bool failed = false;
        try { task(); } catch (...) { failed = true; }
        {
            std::lock_guard lock(mutex_);
            --stats_.active; ++stats_.completed;
            if (failed) ++stats_.failed;
        }
    }
}
} // namespace cloud
