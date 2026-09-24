#include "assistant/chat_completion_parser.h"

#include <nlohmann/json.hpp>

namespace cloud {

namespace {
std::string optional_string(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

ChatCompletionParseResult invalid_response(std::string message) {
    ChatCompletionParseResult result;
    result.error_type = "invalid_response";
    result.error_message = std::move(message);
    return result;
}
} // namespace

ChatCompletionParseResult parse_chat_completion_response(const std::string& body) {
    if (body.empty()) return invalid_response("response body is empty");

    try {
        const auto root = nlohmann::json::parse(body);
        if (!root.is_object()) return invalid_response("response root is not a JSON object");

        if (const auto error_it = root.find("error"); error_it != root.end()) {
            ChatCompletionParseResult result;
            result.error_type = "api_error";
            if (error_it->is_object()) {
                result.error_type = optional_string(*error_it, "type");
                if (result.error_type.empty()) result.error_type = "api_error";
                result.error_message = optional_string(*error_it, "message");
                if (result.error_message.empty()) result.error_message = error_it->dump();
            } else {
                result.error_message = error_it->dump();
            }
            return result;
        }

        const auto choices_it = root.find("choices");
        if (choices_it == root.end() || !choices_it->is_array() || choices_it->empty())
            return invalid_response("response does not contain a non-empty choices array");

        const auto& choice = choices_it->front();
        if (!choice.is_object()) return invalid_response("choices[0] is not a JSON object");

        ChatCompletionParseResult result;
        result.finish_reason = optional_string(choice, "finish_reason");

        const auto message_it = choice.find("message");
        if (message_it == choice.end() || !message_it->is_object())
            return invalid_response("choices[0].message is missing or invalid");

        const auto content_it = message_it->find("content");
        if (content_it == message_it->end() || !content_it->is_string())
            return invalid_response("choices[0].message.content is missing or is not a string");

        result.content = content_it->get<std::string>();
        if (result.content.empty())
            return invalid_response("choices[0].message.content is empty");
        return result;
    } catch (const nlohmann::json::exception& error) {
        ChatCompletionParseResult result;
        result.error_type = "invalid_json";
        result.error_message = error.what();
        return result;
    }
}

std::string build_chat_completion_request(const std::string& model,
                                          const std::string& system_prompt,
                                          const std::string& user_message) {
    return build_chat_completion_request(model, system_prompt,
                                         std::vector<ChatMessage>{{"user", user_message}});
}

std::string build_chat_completion_request(const std::string& model,
                                           const std::string& system_prompt,
                                           const std::vector<ChatMessage>& messages) {
    nlohmann::json request_messages = nlohmann::json::array();
    request_messages.push_back({{"role", "system"}, {"content", system_prompt}});
    for (const auto& message : messages)
        request_messages.push_back({{"role", message.role}, {"content", message.content}});
    const nlohmann::json request = {
        {"model", model},
        {"messages", std::move(request_messages)},
        {"stream", false}
    };
    return request.dump();
}

AssistantHttpRequest parse_assistant_http_request(const std::string& body) {
    AssistantHttpRequest result;
    if (body.empty()) {
        result.error_message = "request body is empty";
        return result;
    }

    try {
        const auto root = nlohmann::json::parse(body);
        if (!root.is_object()) {
            result.error_message = "request root is not a JSON object";
            return result;
        }

        const auto read_string = [&](const char* key, std::string& target) -> bool {
            const auto it = root.find(key);
            if (it == root.end() || it->is_null()) return true;
            if (!it->is_string()) {
                result.error_message = std::string(key) + " must be a string";
                return false;
            }
            target = it->get<std::string>();
            return true;
        };

        if (!read_string("message", result.message) ||
            !read_string("file_id", result.file_id) ||
            !read_string("request_id", result.request_id) ||
            !read_string("conversation_id", result.conversation_id)) {
            return result;
        }
        const auto messages_it = root.find("messages");
        if (messages_it != root.end() && !messages_it->is_null()) {
            if (!messages_it->is_array() || messages_it->empty() || messages_it->size() > 20) {
                result.error_message = "messages must contain between 1 and 20 entries";
                return result;
            }
            std::size_t total_bytes = 0;
            for (const auto& entry : *messages_it) {
                if (!entry.is_object()) {
                    result.error_message = "each message must be an object";
                    return result;
                }
                const auto role_it = entry.find("role");
                const auto content_it = entry.find("content");
                if (role_it == entry.end() || content_it == entry.end() ||
                    !role_it->is_string() || !content_it->is_string()) {
                    result.error_message = "message role/content must be strings";
                    return result;
                }
                ChatMessage message{role_it->get<std::string>(), content_it->get<std::string>()};
                if ((message.role != "user" && message.role != "assistant") ||
                    message.content.empty() || message.content.size() > 64 * 1024) {
                    result.error_message = "message role/content is invalid";
                    return result;
                }
                total_bytes += message.content.size();
                if (total_bytes > 512 * 1024) {
                    result.error_message = "conversation context is too large";
                    return result;
                }
                if (message.role == "user") result.message = message.content;
                result.messages.push_back(std::move(message));
            }
            if (result.messages.back().role != "user") {
                result.error_message = "the last conversation message must be from the user";
                return result;
            }
        } else if (!result.message.empty()) {
            result.messages.push_back({"user", result.message});
        }
        return result;
    } catch (const nlohmann::json::exception& error) {
        result.error_message = error.what();
        return result;
    }
}

std::string build_assistant_http_response(const std::string& request_id,
                                          const std::string& reply) {
    return nlohmann::json{{"request_id", request_id}, {"reply", reply}}.dump();
}

} // namespace cloud
