#include "assistant/ai_service.h"
#include "common/config.h"
#include "database/metadata_repository.h"
#include "http/http_server.h"
#include "logger/logger.h"
#include "logger/archive_worker.h"
#include "storage/file_service.h"
#include "storage/local_object_storage.h"
#include <atomic>
#include <csignal>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> stop_requested{false};
void on_signal(int) { stop_requested.store(true); }
struct LoggerShutdown {
    ~LoggerShutdown() {
        cloud::Logger::instance().stop();
        cloud::Logger::instance().set_rotation_callback({});
    }
};
}
int main(int argc, char** argv) {
    try {
        const auto config = cloud::ConfigManager::load(argc > 1 ? argv[1] : "config/app.json");
        auto storage = std::make_shared<cloud::LocalObjectStorage>(
            std::filesystem::path(config.data_root) / "objects");
        auto repository = std::make_shared<cloud::MetadataRepository>(config.metadata_file);
        cloud::FileService files(storage, repository);
        const auto recovery = files.recover();
        if (!recovery.missing_keys.empty()) {
            std::cerr << "Recovery stopped: " << recovery.missing_keys.size()
                      << " committed objects are missing; restore data before starting.\n";
            return 2;
        }
        cloud::Logger::instance().start(config);
        // Guard startup too, before ArchiveWorker has been constructed.
        try {
            cloud::ArchiveWorker archive(config, files);
            LoggerShutdown shutdown;
            cloud::Logger::instance().set_rotation_callback(
                [&archive](const std::filesystem::path& path) { archive.enqueue(path); });
            LOG_INFO("CloudLog starting; quarantined uncommitted objects=" +
                     std::to_string(recovery.quarantined));
            cloud::AiService ai(config);
            cloud::HttpServer server(config, files, ai);
            std::signal(SIGINT, on_signal);
            std::signal(SIGTERM, on_signal);
            std::jthread monitor([&](std::stop_token token) {
                while (!token.stop_requested()) {
                    if (stop_requested.load()) { server.stop(); return; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
            const bool ok = server.run();
            monitor.request_stop();
            LOG_INFO("CloudLog stopping; draining accepted log records");
            return ok ? 0 : 1;
        } catch (...) {
            cloud::Logger::instance().stop();
            cloud::Logger::instance().set_rotation_callback({});
            throw;
        }
    } catch (const std::exception& e) {
        std::cerr << "CloudLog startup failure: " << e.what() << '\n';
        return 1;
    }
}

