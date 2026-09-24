#include "common/config.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace cloud {

namespace {
std::string read_all(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

std::string string_value(const std::string& json, const std::string& key,
                         const std::string& fallback) {
    const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch match;
    return std::regex_search(json, match, pattern) ? match[1].str() : fallback;
}

std::size_t size_value(const std::string& json, const std::string& key, std::size_t fallback) {
    const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) return fallback;
    try { return static_cast<std::size_t>(std::stoull(match[1].str())); }
    catch (...) { return fallback; }
}
}

AppConfig ConfigManager::load(const std::string& path) {
    AppConfig config;
    const auto json = read_all(path);
    if (json.empty()) return config;

    config.data_root = string_value(json, "data_root", config.data_root);
    config.metadata_file = string_value(json, "metadata_file", config.metadata_file);
    config.log_file = string_value(json, "log_file", config.log_file);
    config.critical_log_file = string_value(json, "critical_log_file", config.critical_log_file);
    config.server_host = string_value(json, "server_host", config.server_host);
    config.server_port = static_cast<unsigned short>(size_value(json, "server_port", config.server_port));
    config.web_root = string_value(json, "web_root", config.web_root);
    config.ai_endpoint = string_value(json, "ai_endpoint", config.ai_endpoint);
    config.ai_api_key = string_value(json, "ai_api_key", config.ai_api_key);
    config.ai_model = string_value(json, "ai_model", config.ai_model);
    config.ai_system_prompt = string_value(json, "ai_system_prompt", config.ai_system_prompt);
    config.log_queue_capacity = size_value(json, "log_queue_capacity", config.log_queue_capacity);
    config.log_max_file_size = size_value(json, "log_max_file_size", config.log_max_file_size);
    config.log_max_backups = size_value(json, "log_max_backups", config.log_max_backups);
    config.log_queue_bytes = size_value(json, "log_queue_bytes", config.log_queue_bytes);
    config.log_flush_interval_ms = size_value(json, "log_flush_interval_ms", config.log_flush_interval_ms);
    config.log_console_enabled = size_value(json, "log_console_enabled", config.log_console_enabled);
    config.log_critical_durable = size_value(json, "log_critical_durable", config.log_critical_durable);
    config.log_critical_max_file_size = size_value(json, "log_critical_max_file_size", config.log_critical_max_file_size);
    config.log_critical_max_backups = size_value(json, "log_critical_max_backups", config.log_critical_max_backups);
    config.archive_batch_size = size_value(json, "archive_batch_size", config.archive_batch_size);
    config.archive_retry_ms = size_value(json, "archive_retry_ms", config.archive_retry_ms);
    config.http_worker_count = size_value(json, "http_worker_count", config.http_worker_count);
    config.http_queue_capacity = size_value(json, "http_queue_capacity", config.http_queue_capacity);
    config.http_io_timeout_ms = size_value(json, "http_io_timeout_ms", config.http_io_timeout_ms);
    config.http_request_timeout_ms = size_value(json, "http_request_timeout_ms", config.http_request_timeout_ms);
    config.http_max_body_bytes = size_value(json, "http_max_body_bytes", config.http_max_body_bytes);
    config.http_ai_max_concurrent = size_value(json, "http_ai_max_concurrent", config.http_ai_max_concurrent);

    // Prefer an environment variable so the DeepSeek key is not committed to source control.
    if (config.ai_api_key.empty()) {
        if (const char* key = std::getenv("DEEPSEEK_API_KEY")) config.ai_api_key = key;
    }

    const std::filesystem::path config_dir = std::filesystem::path(path).parent_path();
    auto resolve = [&](std::string& value) {
        if (!value.empty() && std::filesystem::path(value).is_relative() && !config_dir.empty())
            value = (config_dir / value).lexically_normal().string();
    };
    resolve(config.data_root);
    resolve(config.metadata_file);
    resolve(config.log_file);
    resolve(config.critical_log_file);
    resolve(config.web_root);
    return config;
}

} // namespace cloud
