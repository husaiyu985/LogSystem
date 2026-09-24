#include "assistant/ai_service.h"

#include "assistant/chat_completion_parser.h"
#include "logger/logger.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace cloud {

namespace {
std::string log_summary(std::string value, std::size_t maximum_size = 400) {
    std::replace_if(value.begin(), value.end(),
                    [](unsigned char c) { return c < 0x20; }, ' ');
    if (value.size() > maximum_size) {
        value.resize(maximum_size);
        value += "...";
    }
    return value;
}

#ifdef _WIN32
class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) noexcept : handle_(handle) {}
    ~WinHttpHandle() {
        if (handle_) WinHttpCloseHandle(handle_);
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

std::wstring to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

void log_winhttp_error(const char* operation, DWORD error_code) {
    LOG_ERROR(std::string("DeepSeek WinHTTP failure: operation=") + operation +
              " win32_error=" + std::to_string(error_code));
}
#endif
} // namespace

AiService::AiService(AppConfig config) : config_(std::move(config)) {}

std::string AiService::fallback_reply(const std::string& message) const {
    if (message.empty()) return "请输入问题，我可以协助你查询文件、解释系统状态或规划云存储功能。";
    if (message.find("文件") != std::string::npos || message.find("上传") != std::string::npos ||
        message.find("下载") != std::string::npos) {
        return "CloudLog 文件服务支持浅度和深度存储。可以通过 /api/upload 上传文件，通过 /api/files 查看元数据，通过 /api/download/{id} 下载文件。";
    }
    if (message.find("日志") != std::string::npos) {
        return "日志模块采用生产者-消费者模型：业务线程将消息放入队列，后台线程负责控制台、文件输出、滚动和重要日志备份。";
    }
    if (message.find("帮助") != std::string::npos || message.find("功能") != std::string::npos) {
        return "我可以帮助你了解文件上传下载、存储模式、日志配置、接口调用和项目结构。配置 AI_ENDPOINT 后还可以接入远程大模型。";
    }
    if (!config_.ai_endpoint.empty() && !config_.ai_api_key.empty())
        return "DeepSeek 请求未成功，请检查网络、API Key、账户额度及服务端日志。";
    return "尚未配置 DEEPSEEK_API_KEY，我先提供本地帮助：请尝试询问文件、日志、上传、下载或项目功能。";
}

std::string AiService::remote_reply(const std::vector<ChatMessage>& messages) const {
#ifdef _WIN32
    if (config_.ai_endpoint.empty() || config_.ai_api_key.empty()) return {};

    const std::wstring endpoint = to_wide(config_.ai_endpoint);
    if (endpoint.empty()) {
        LOG_ERROR("DeepSeek endpoint is not valid UTF-8");
        return {};
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(endpoint.c_str(), 0, 0, &parts)) {
        log_winhttp_error("WinHttpCrackUrl", GetLastError());
        return {};
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring request_path;
    if (parts.lpszUrlPath && parts.dwUrlPathLength > 0)
        request_path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (request_path.empty()) request_path = L"/";
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0)
        request_path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    WinHttpHandle session(WinHttpOpen(L"CloudLog-AI/1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        log_winhttp_error("WinHttpOpen", GetLastError());
        return {};
    }

    WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) {
        log_winhttp_error("WinHttpConnect", GetLastError());
        return {};
    }

    const DWORD request_flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"POST", request_path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, request_flags));
    if (!request) {
        log_winhttp_error("WinHttpOpenRequest", GetLastError());
        return {};
    }

    if (!WinHttpSetTimeouts(request.get(), 10000, 10000, 60000, 180000))
        LOG_WARN("DeepSeek WinHTTP warning: unable to set request timeouts");

    // WinHTTP advertises the supported encodings and transparently decompresses the response.
    DWORD decompression_flags = WINHTTP_DECOMPRESSION_FLAG_GZIP |
                                WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION,
                          &decompression_flags, sizeof(decompression_flags))) {
        LOG_WARN("DeepSeek WinHTTP warning: response decompression is unavailable; win32_error=" +
                 std::to_string(GetLastError()));
    }

    const std::string body = build_chat_completion_request(
        config_.ai_model, config_.ai_system_prompt, messages);
    if (body.size() > (std::numeric_limits<DWORD>::max)()) {
        LOG_ERROR("DeepSeek request body is too large for WinHTTP");
        return {};
    }

    const std::wstring headers = L"Content-Type: application/json\r\n"
                                 L"Accept: application/json\r\n"
                                 L"Authorization: Bearer " +
                                 to_wide(config_.ai_api_key) + L"\r\n";
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1L),
                            const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        log_winhttp_error("WinHttpSendRequest", GetLastError());
        return {};
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        log_winhttp_error("WinHttpReceiveResponse", GetLastError());
        return {};
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        log_winhttp_error("WinHttpQueryHeaders(status)", GetLastError());
    }

    constexpr std::size_t maximum_response_size = 8U * 1024U * 1024U;
    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            log_winhttp_error("WinHttpQueryDataAvailable", GetLastError());
            return {};
        }
        if (available == 0) break;
        if (available > maximum_response_size - response.size()) {
            LOG_ERROR("DeepSeek response exceeded the 8 MiB safety limit");
            return {};
        }

        std::string chunk(available, '\0');
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), available, &bytes_read)) {
            log_winhttp_error("WinHttpReadData", GetLastError());
            return {};
        }
        if (bytes_read == 0) break;
        response.append(chunk.data(), bytes_read);
    }

    LOG_INFO("DeepSeek HTTP response: status=" + std::to_string(status_code) +
             " bytes=" + std::to_string(response.size()));

    const auto parsed = parse_chat_completion_response(response);
    const bool http_success = status_code == 0 || (status_code >= 200 && status_code < 300);
    if (!http_success) {
        const std::string detail = parsed.error_message.empty()
            ? "remote endpoint returned a non-success status"
            : log_summary(parsed.error_message);
        LOG_ERROR("DeepSeek HTTP failure: status=" + std::to_string(status_code) +
                  " error_type=" + log_summary(parsed.error_type) +
                  " message=" + detail);
        return {};
    }

    if (!parsed.ok()) {
        LOG_WARN("DeepSeek JSON response is invalid: status=" + std::to_string(status_code) +
                 " error_type=" + log_summary(parsed.error_type) +
                 " message=" + log_summary(parsed.error_message));
        return {};
    }

    LOG_INFO("DeepSeek chat completion parsed successfully: finish_reason=" +
             log_summary(parsed.finish_reason));
    return parsed.content;
#else
    (void)messages;
#endif
    return {};
}

std::string AiService::reply(const std::string& message) const {
    return reply(std::vector<ChatMessage>{{"user", message}});
}

std::string AiService::reply(const std::vector<ChatMessage>& messages) const {
    const auto last_user = std::find_if(messages.rbegin(), messages.rend(),
        [](const ChatMessage& message) { return message.role == "user"; });
    const std::string fallback_message = last_user == messages.rend()
        ? std::string{} : last_user->content;
    try {
        if (const auto remote = remote_reply(messages); !remote.empty()) return remote;
    } catch (const std::exception& error) {
        LOG_ERROR("AI remote request failed: " + log_summary(error.what()));
    } catch (...) {
        LOG_ERROR("AI remote request failed with an unknown exception");
    }
    return fallback_reply(fallback_message);
}

} // namespace cloud
