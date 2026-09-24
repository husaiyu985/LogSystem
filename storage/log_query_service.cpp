#include "storage/log_query_service.h"

#include "storage/file_service.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace cloud {

namespace {
std::string trim(std::string value) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    });
    return value;
}

bool contains_any(const std::string& value, const std::vector<std::string>& words) {
    return std::any_of(words.begin(), words.end(), [&](const std::string& word) {
        return value.find(word) != std::string::npos;
    });
}

std::string quoted_text(const std::string& message) {
    const std::vector<std::pair<std::string, std::string>> quotes = {
        {"\"", "\""}, {"'", "'"}, {"`", "`"}, {"“", "”"}, {"‘", "’"}
    };
    for (const auto& [opening, closing] : quotes) {
        const auto begin = message.find(opening);
        if (begin == std::string::npos) continue;
        const auto content_begin = begin + opening.size();
        const auto end = message.find(closing, content_begin);
        if (end != std::string::npos && end > content_begin)
            return trim(message.substr(content_begin, end - content_begin));
    }
    return {};
}

void remove_suffix(std::string& value, const std::string& suffix) {
    const auto position = value.find(suffix);
    if (position != std::string::npos) value.erase(position);
}

std::string clean_search_candidate(std::string candidate) {
    candidate = trim(std::move(candidate));
    const std::vector<std::string> prefixes = {
        "请问", "请帮我", "帮我", "请", "查找一下", "搜索一下", "查一下", "查下",
        "查找", "搜索", "一下"
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& prefix : prefixes) {
            if (candidate.rfind(prefix, 0) == 0) {
                candidate = trim(candidate.substr(prefix.size()));
                changed = true;
                break;
            }
        }
    }

    const std::vector<std::string> suffixes = {
        "的所有日志", "的日志有哪些", "的日志在哪", "的日志", "的所有文件",
        "的文件有哪些", "的文件在哪", "的文件", "出现在哪些日志", "出现在哪个日志",
        "出现在哪些文件", "出现在哪个文件", "出现在哪", "在哪些日志", "在哪个日志",
        "在哪些文件", "在哪个文件", "并列出", "并分析", "然后分析", "日志并",
        "文件并", "吗", "呢", "。", "！", "？", "?", "!"
    };
    for (const auto& suffix : suffixes) remove_suffix(candidate, suffix);
    candidate = trim(candidate);
    if (candidate == "指定字符串" || candidate == "字符串" || candidate == "关键字" ||
        candidate == "关键词")
        return {};
    return candidate;
}

std::string inferred_search_text(const std::string& message) {
    if (message.rfind("/search", 0) == 0) return clean_search_candidate(message.substr(7));
    if (message.rfind("/查找", 0) == 0) return clean_search_candidate(message.substr(7));
    if (const auto quoted = quoted_text(message); !quoted.empty()) return quoted;
    if (contains_any(message, {"指定字符串", "指定关键字", "指定关键词"})) return {};

    const std::vector<std::string> markers = {
        "包含", "带有", "含有", "匹配", "查找一下", "搜索一下", "查一下", "查下",
        "搜索", "查找"
    };
    for (const auto& marker : markers) {
        const auto position = message.find(marker);
        if (position == std::string::npos) continue;
        const auto candidate = clean_search_candidate(message.substr(position + marker.size()));
        if (!candidate.empty()) return candidate;
    }

    const std::vector<std::string> locations = {
        "出现在哪些日志", "出现在哪个日志", "出现在哪些文件", "出现在哪个文件",
        "在哪些日志", "在哪个日志", "在哪些文件", "在哪个文件"
    };
    for (const auto& location : locations) {
        const auto position = message.find(location);
        if (position == std::string::npos) continue;
        const auto candidate = clean_search_candidate(message.substr(0, position));
        if (!candidate.empty()) return candidate;
    }

    const bool global_scope = contains_any(message,
        {"日志系统", "所有日志", "全部日志", "哪些日志", "哪个日志",
         "所有文件", "全部文件", "哪些文件", "哪个文件"});
    if (global_scope) {
        const auto position = message.rfind("有");
        if (position != std::string::npos) {
            const auto candidate = clean_search_candidate(message.substr(position + std::string("有").size()));
            if (!candidate.empty()) return candidate;
        }
    }
    return {};
}

std::size_t count_occurrences(const std::string& line, const std::string& needle,
                              bool case_sensitive) {
    const std::string haystack = case_sensitive ? line : ascii_lower(line);
    const std::string pattern = case_sensitive ? needle : ascii_lower(needle);
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = haystack.find(pattern, position)) != std::string::npos) {
        ++count;
        position += std::max<std::size_t>(1, pattern.size());
    }
    return count;
}

std::string excerpt_for(const std::string& line, const std::string& needle,
                        bool case_sensitive) {
    constexpr std::size_t radius = 120;
    const std::string haystack = case_sensitive ? line : ascii_lower(line);
    const std::string pattern = case_sensitive ? needle : ascii_lower(needle);
    const auto match = haystack.find(pattern);
    if (match == std::string::npos) return {};
    const auto start = match > radius ? match - radius : 0;
    const auto end = std::min(line.size(), match + needle.size() + radius);
    std::string excerpt = line.substr(start, end - start);
    std::replace(excerpt.begin(), excerpt.end(), '\t', ' ');
    if (start > 0) excerpt.insert(0, "...");
    if (end < line.size()) excerpt += "...";
    return excerpt;
}

std::string markdown_text(std::string value) {
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

bool is_probably_text(const std::string& value) {
    if (value.empty()) return true;
    std::size_t controls = 0;
    for (const unsigned char c : value) {
        if (c == 0) return false;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') ++controls;
    }
    return controls * 100 <= value.size();
}

std::size_t utf8_safe_prefix(const std::string& value, std::size_t offset,
                             std::size_t maximum) {
    std::size_t length = std::min(maximum, value.size() - offset);
    if (offset + length == value.size()) return length;
    while (length > 0) {
        const auto byte = static_cast<unsigned char>(value[offset + length]);
        if ((byte & 0xC0U) != 0x80U) break;
        --length;
    }
    return length == 0 ? std::min(maximum, value.size() - offset) : length;
}

std::string human_bytes(std::uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' ' << units[unit];
    return out.str();
}

class LineSearchBuffer final : public std::streambuf {
public:
    LineSearchBuffer(const std::string& query, const LogSearchOptions& options,
                     LogFileMatch& file, LogSearchResult& result)
        : query_(query), options_(options), file_(file), result_(result) {}
    void finish() { if (!line_.empty()) process_line(); }
protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        for (std::streamsize index = 0; index < count; ++index) {
            if (data[index] == '\n') {
                process_line();
            } else if (line_.size() < maximum_line_bytes) {
                line_.push_back(data[index]);
            } else {
                line_truncated_ = true;
            }
        }
        return count;
    }
    int overflow(int character) override {
        if (character == traits_type::eof()) return traits_type::not_eof(character);
        const char value = static_cast<char>(character);
        return xsputn(&value, 1) == 1 ? character : traits_type::eof();
    }
private:
    void process_line() {
        ++line_number_;
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        if (result_.total_occurrences < options_.maximum_matches) {
            const auto occurrences = count_occurrences(line_, query_, options_.case_sensitive);
            if (occurrences > 0) {
                file_.occurrences += occurrences;
                result_.total_occurrences += occurrences;
                file_.lines.push_back({line_number_, occurrences,
                    excerpt_for(line_, query_, options_.case_sensitive)});
                if (result_.total_occurrences >= options_.maximum_matches)
                    result_.truncated = true;
            }
        }
        if (line_truncated_) result_.truncated = true;
        line_.clear();
        line_truncated_ = false;
    }
    static constexpr std::size_t maximum_line_bytes = 1024 * 1024;
    const std::string& query_;
    const LogSearchOptions& options_;
    LogFileMatch& file_;
    LogSearchResult& result_;
    std::string line_;
    std::size_t line_number_ = 0;
    bool line_truncated_ = false;
};
} // namespace

LogQueryService::LogQueryService(FileService& files) : files_(files) {}

std::vector<FileMetadata> LogQueryService::searchable_files(const UserId& external_owner) const {
    auto external = files_.list(external_owner.empty() ? "anonymous" : external_owner);
    auto archives = files_.list("system", "system-logs");
    external.erase(std::remove_if(external.begin(), external.end(),
                                  [](const FileMetadata& item) { return item.is_folder; }),
                   external.end());
    archives.erase(std::remove_if(archives.begin(), archives.end(),
                                  [](const FileMetadata& item) { return item.is_folder; }),
                   archives.end());
    external.insert(external.end(), archives.begin(), archives.end());
    std::sort(external.begin(), external.end(), [](const FileMetadata& left,
                                                   const FileMetadata& right) {
        return left.created_at > right.created_at;
    });
    return external;
}

LogInventory LogQueryService::inventory(const UserId& external_owner) const {
    LogInventory result;
    for (const auto& item : files_.list(external_owner.empty() ? "anonymous" : external_owner)) {
        if (item.is_folder) continue;
        ++result.external_count;
        result.external_bytes += item.size;
    }
    for (const auto& item : files_.list("system", "system-logs")) {
        if (item.is_folder) continue;
        ++result.archive_count;
        result.archive_bytes += item.size;
    }
    return result;
}

LogSearchResult LogQueryService::search(const UserId& external_owner, const std::string& query,
                                        const LogSearchOptions& options) const {
    LogSearchResult result;
    result.query = query;
    if (query.empty()) return result;

    const auto candidates = searchable_files(external_owner);
    result.candidate_files = candidates.size();
    for (const auto& metadata : candidates) {
        if (metadata.size > options.maximum_file_bytes ||
            metadata.size > options.maximum_total_bytes - result.scanned_bytes) {
            ++result.skipped_files;
            result.truncated = true;
            continue;
        }

        LogFileMatch file_match;
        file_match.metadata = metadata;
        LineSearchBuffer search_buffer(query, options, file_match, result);
        std::ostream search_stream(&search_buffer);
        if (!files_.download(metadata.owner_id, metadata.id, search_stream)) {
            ++result.skipped_files;
            continue;
        }
        search_buffer.finish();
        ++result.scanned_files;
        result.scanned_bytes += metadata.size;
        if (!file_match.lines.empty()) result.files.push_back(std::move(file_match));
        if (result.total_occurrences >= options.maximum_matches) break;
    }
    return result;
}

LogLibraryPayload LogQueryService::build_ai_payload(
    const UserId& external_owner, std::size_t maximum_batch_bytes,
    std::uint64_t maximum_total_bytes) const {
    LogLibraryPayload result;
    maximum_batch_bytes = std::max<std::size_t>(64 * 1024, maximum_batch_bytes);
    const auto candidates = searchable_files(external_owner);
    result.candidate_files = candidates.size();

    LogLibraryBatch current;
    auto flush = [&]() {
        if (current.content.empty()) return;
        result.batches.push_back(std::move(current));
        current = {};
    };

    for (const auto& metadata : candidates) {
        if (metadata.size > maximum_total_bytes - result.included_bytes) {
            ++result.skipped_files;
            result.truncated = true;
            result.skipped_names.push_back(metadata.name + "（超过总容量限制）");
            continue;
        }

        std::ostringstream stream(std::ios::binary);
        if (!files_.download(metadata.owner_id, metadata.id, stream)) {
            ++result.skipped_files;
            result.skipped_names.push_back(metadata.name + "（读取失败）");
            continue;
        }
        const std::string content = stream.str();
        if (!is_probably_text(content)) {
            ++result.skipped_files;
            result.skipped_names.push_back(metadata.name + "（不是文本日志）");
            continue;
        }

        ++result.included_files;
        result.included_bytes += content.size();
        std::size_t offset = 0;
        do {
            std::ostringstream header;
            header << "\n<LOG_FILE name=\"" << metadata.name << "\" id=\"" << metadata.id
                   << "\" source=\""
                   << (metadata.owner_id == "system" ? "system-archive" : "external-upload")
                   << "\" storage=\"" << to_string(metadata.storage_mode)
                   << "\" byte_offset=\"" << offset << "\">\n";
            const std::string prefix = header.str();
            const std::string suffix = "\n</LOG_FILE>\n";

            if (!current.content.empty() &&
                current.content.size() + prefix.size() + suffix.size() + 1 >= maximum_batch_bytes)
                flush();

            const auto overhead = current.content.size() + prefix.size() + suffix.size();
            const auto capacity = overhead < maximum_batch_bytes
                ? maximum_batch_bytes - overhead : std::size_t{0};
            if (capacity == 0) {
                flush();
                continue;
            }
            const auto take = utf8_safe_prefix(content, offset, capacity);
            current.content += prefix;
            current.content.append(content, offset, take);
            current.content += suffix;
            ++current.file_fragments;
            current.raw_bytes += take;
            offset += take;
            if (offset < content.size()) flush();
        } while (offset < content.size());
    }
    flush();
    return result;
}

LogToolRequest LogQueryService::detect_tool_request(const std::string& message) {
    LogToolRequest result;
    const std::string lowered = ascii_lower(message);
    const bool mentions_log = contains_any(lowered, {"日志", "文件", "log", "logs"});
    const bool inventory_words = contains_any(lowered,
        {"多少个日志", "多少个文件", "多少日志", "多少文件", "日志有多少", "文件有多少",
         "日志数量", "文件数量", "日志总数", "文件总数", "几个日志", "几个文件",
         "/count", "/stats"}) ||
        (mentions_log && contains_any(lowered, {"统计一下", "帮我统计", "统计当前"}));
    const bool search_words = contains_any(lowered,
        {"查找", "搜索", "找出", "包含", "带有", "含有", "匹配", "查一下", "查下",
         "出现在哪", "哪些日志", "哪个日志", "哪些文件", "哪个文件", "/search", "/查找"});

    if (inventory_words || lowered.rfind("/count", 0) == 0 ||
        lowered.rfind("/stats", 0) == 0) {
        result.kind = LogToolKind::Inventory;
    } else if ((mentions_log && search_words) || lowered.rfind("/search", 0) == 0 ||
               lowered.rfind("/查找", 0) == 0) {
        result.kind = LogToolKind::Search;
        result.search_text = inferred_search_text(message);
        result.needs_search_text = result.search_text.empty();
    }
    result.wants_ai_summary = contains_any(lowered, {"分析", "总结", "原因", "归纳"});
    return result;
}

std::string LogQueryService::inventory_markdown(const LogInventory& inventory) {
    std::ostringstream out;
    out << "## 日志库存统计\n\n"
        << "- 外部程序手动上传：**" << inventory.external_count << "** 个（"
        << human_bytes(inventory.external_bytes) << "）\n"
        << "- 系统历史日志自动归档：**" << inventory.archive_count << "** 个（"
        << human_bytes(inventory.archive_bytes) << "）\n"
        << "- 已存储日志合计：**" << inventory.total_count() << "** 个（"
        << human_bytes(inventory.total_bytes()) << "）\n\n"
        << "> 当前正在写入的 `cloud.log` 不属于已上传/已归档文件，因此未计入以上数量。";
    return out.str();
}

std::string LogQueryService::search_markdown(const LogSearchResult& result) {
    std::ostringstream out;
    out << "## 跨日志搜索结果\n\n"
        << "搜索字符串：`" << markdown_text(result.query) << "`\n\n"
        << "扫描 **" << result.scanned_files << " / " << result.candidate_files
        << "** 个文件，共找到 **" << result.total_occurrences << "** 处匹配，涉及 **"
        << result.files.size() << "** 个日志文件。\n";

    if (result.files.empty()) out << "\n没有找到包含该字符串的已上传日志。\n";
    for (const auto& file : result.files) {
        out << "\n### " << markdown_text(file.metadata.name) << "\n"
            << "- 文件 ID：`" << file.metadata.id << "`\n"
            << "- 来源：" << (file.metadata.owner_id == "system" ? "系统自动归档" : "外部上传")
            << "；存储方式：" << to_string(file.metadata.storage_mode)
            << "；匹配：" << file.occurrences << " 处\n";
        for (const auto& line : file.lines) {
            out << "- 第 " << line.line_number << " 行（" << line.occurrences << " 处）：\n"
                << "\n        " << markdown_text(line.excerpt) << "\n";
        }
    }
    if (result.skipped_files > 0) {
        out << "\n> 有 " << result.skipped_files
            << " 个文件因读取失败或超过安全扫描上限而跳过。";
    }
    if (result.truncated) {
        out << "\n> 搜索结果已达到安全上限，只展示部分匹配；可使用更具体的字符串缩小范围。";
    }
    return out.str();
}

} // namespace cloud
