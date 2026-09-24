#include "storage/file_service.h"

#include "common/durable_file.h"
#include "logger/logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <streambuf>

#ifdef _WIN32
#include <windows.h>
#include <compressapi.h>
#endif

namespace cloud {
namespace {

constexpr std::uint64_t maximum_object_size = 128ULL * 1024ULL * 1024ULL;
constexpr std::size_t compression_chunk_size = 1024ULL * 1024ULL;

class TemporaryFile {
public:
    explicit TemporaryFile(const char* purpose)
        : path_(std::filesystem::temp_directory_path() /
                (std::string("cloudlog-") + purpose + "-" + unique_token() + ".tmp")) {}
    ~TemporaryFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

void write_u32(std::ostream& output, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool read_u32(std::istream& input, std::uint32_t& value) {
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) return false;
    value = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return true;
}

bool copy_limited(std::istream& input, std::ostream& output, std::uint64_t maximum_bytes,
                  std::uint64_t& written) {
    std::array<char, 64 * 1024> buffer{};
    written = 0;
    while (input && written < maximum_bytes) {
        const auto wanted = static_cast<std::streamsize>(std::min<std::uint64_t>(
            buffer.size(), maximum_bytes - written));
        input.read(buffer.data(), wanted);
        const auto count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
            if (!output) return false;
            written += static_cast<std::uint64_t>(count);
        }
    }
    return !input.bad();
}

class ComparingBuffer final : public std::streambuf {
public:
    explicit ComparingBuffer(std::istream& expected) : expected_(expected) {}
    bool complete() {
        if (!equal_) return false;
        char extra = 0;
        expected_.read(&extra, 1);
        return expected_.gcount() == 0 && !expected_.bad();
    }
protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        std::array<char, 64 * 1024> expected{};
        std::streamsize offset = 0;
        while (offset < count) {
            const auto wanted = std::min<std::streamsize>(
                static_cast<std::streamsize>(expected.size()), count - offset);
            expected_.read(expected.data(), wanted);
            const auto actual = expected_.gcount();
            if (actual != wanted || std::memcmp(expected.data(), data + offset,
                                                 static_cast<std::size_t>(wanted)) != 0) {
                equal_ = false;
                return count;
            }
            offset += wanted;
        }
        return count;
    }
    int overflow(int character) override {
        if (character == traits_type::eof()) return traits_type::not_eof(character);
        const char value = static_cast<char>(character);
        return xsputn(&value, 1) == 1 ? character : traits_type::eof();
    }
private:
    std::istream& expected_;
    bool equal_ = true;
};

class TailBuffer final : public std::streambuf {
public:
    explicit TailBuffer(std::size_t capacity) : data_(capacity, '\0') {}
    std::string value() const {
        if (data_.empty() || total_ == 0) return {};
        const auto used = std::min(total_, data_.size());
        if (total_ <= data_.size()) return std::string(data_.data(), used);
        const auto begin = total_ % data_.size();
        return std::string(data_.data() + begin, data_.size() - begin) +
               std::string(data_.data(), begin);
    }
protected:
    std::streamsize xsputn(const char* source, std::streamsize count) override {
        if (data_.empty()) { total_ += static_cast<std::size_t>(count); return count; }
        for (std::streamsize i = 0; i < count; ++i) {
            data_[total_ % data_.size()] = source[i];
            ++total_;
        }
        return count;
    }
    int overflow(int character) override {
        if (character == traits_type::eof()) return traits_type::not_eof(character);
        const char value = static_cast<char>(character);
        return xsputn(&value, 1) == 1 ? character : traits_type::eof();
    }
private:
    std::vector<char> data_;
    std::size_t total_ = 0;
};

bool decode_legacy(std::istream& input, std::ostream& output, std::uint64_t expected_size,
                   std::uint64_t maximum_bytes) {
    unsigned char compressed = 0;
    input.read(reinterpret_cast<char*>(&compressed), 1);
    std::uint64_t original_size = 0;
    input.read(reinterpret_cast<char*>(&original_size), sizeof(original_size));
    if (!input || original_size > maximum_object_size || original_size != expected_size)
        return false;

    std::ostringstream payload;
    payload << input.rdbuf();
    if (input.bad()) return false;
    const auto encoded = payload.str();
    if (!compressed) {
        if (encoded.size() != original_size) return false;
        output.write(encoded.data(), static_cast<std::streamsize>(
            std::min<std::uint64_t>(encoded.size(), maximum_bytes)));
        return output.good();
    }
#ifdef _WIN32
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor)) return false;
    std::string decoded(static_cast<std::size_t>(original_size), '\0');
    SIZE_T written = 0;
    const bool ok = Decompress(decompressor, encoded.data(), encoded.size(), decoded.data(),
                               decoded.size(), &written) && written == original_size;
    CloseDecompressor(decompressor);
    if (!ok) return false;
    output.write(decoded.data(), static_cast<std::streamsize>(
        std::min<std::uint64_t>(decoded.size(), maximum_bytes)));
    return output.good();
#else
    (void)output; (void)maximum_bytes;
    return false;
#endif
}

} // namespace

FileService::FileService(std::shared_ptr<IObjectStorage> storage,
                         std::shared_ptr<MetadataRepository> repository,
                         std::function<void(const char*)> checkpoint)
    : storage_(std::move(storage)), repository_(std::move(repository)),
      checkpoint_(std::move(checkpoint)) {}

StorageRecoveryReport FileService::recover() {
    std::lock_guard lock(recovery_mutex_);
    std::unordered_set<std::string> live;
    for (const auto& item : repository_->list_all())
        if (!item.is_folder) live.insert(item.object_key);
    return storage_->recover(live);
}

std::string FileService::sanitize_name(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_' || c >= 128)
            result.push_back(static_cast<char>(c));
        else
            result.push_back('_');
    }
    if (result.empty() || result == "." || result == "..") result = "file.bin";
    return result;
}

bool FileService::stage_input(std::istream& input, const std::filesystem::path& path,
                              std::uint64_t& size, std::string& content_hash) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    std::uint64_t hash = 1469598103934665603ULL;
    size = 0;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) continue;
        if (size + static_cast<std::uint64_t>(count) > maximum_object_size) return false;
        output.write(buffer.data(), count);
        if (!output) return false;
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
        size += static_cast<std::uint64_t>(count);
    }
    if (input.bad() || (input.fail() && !input.eof())) return false;
    output.flush();
    if (!output.good()) return false;
    std::ostringstream formatted;
    formatted << std::hex << hash;
    content_hash = formatted.str();
    return true;
}

bool FileService::encode_deep_stream(std::istream& input, std::ostream& output) {
    output.write("CLDG2", 5);
    std::vector<char> original(compression_chunk_size);
#ifdef _WIN32
    COMPRESSOR_HANDLE compressor = nullptr;
    CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor);
#endif
    bool success = true;
    while (input) {
        input.read(original.data(), static_cast<std::streamsize>(original.size()));
        const auto count = input.gcount();
        if (count <= 0) continue;
        const auto original_size = static_cast<std::uint32_t>(count);
        bool compressed = false;
        std::vector<char> stored;
#ifdef _WIN32
        if (compressor) {
            stored.resize(static_cast<std::size_t>(count) + 64 * 1024);
            SIZE_T written = 0;
            if (Compress(compressor, original.data(), static_cast<SIZE_T>(count), stored.data(),
                         stored.size(), &written) && written < static_cast<SIZE_T>(count)) {
                stored.resize(written);
                compressed = true;
            } else {
                stored.assign(original.begin(), original.begin() + count);
            }
        } else
#endif
        {
            stored.assign(original.begin(), original.begin() + count);
        }
        write_u32(output, original_size);
        write_u32(output, static_cast<std::uint32_t>(stored.size()));
        output.put(compressed ? 1 : 0);
        output.write(stored.data(), static_cast<std::streamsize>(stored.size()));
        if (!output) { success = false; break; }
    }
#ifdef _WIN32
    if (compressor) CloseCompressor(compressor);
#endif
    return success && !input.bad() && output.good();
}

bool FileService::decode_deep_stream(std::istream& input, std::ostream& output,
                                     std::uint64_t expected_size,
                                     std::uint64_t maximum_bytes) {
    char signature[5]{};
    input.read(signature, sizeof(signature));
    if (input.gcount() != sizeof(signature)) return false;
    if (std::memcmp(signature, "CLDG1", 5) == 0)
        return decode_legacy(input, output, expected_size, maximum_bytes);
    if (std::memcmp(signature, "CLDG2", 5) != 0) return false;

    std::uint64_t total = 0;
#ifdef _WIN32
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor);
#endif
    bool success = true;
    while (total < expected_size && total < maximum_bytes) {
        std::uint32_t original_size = 0, stored_size = 0;
        if (!read_u32(input, original_size) || !read_u32(input, stored_size)) {
            success = false;
            break;
        }
        const int flag = input.get();
        if (flag == std::char_traits<char>::eof() || original_size == 0 ||
            original_size > compression_chunk_size || stored_size == 0 ||
            stored_size > compression_chunk_size + 64 * 1024 ||
            total + original_size > expected_size) {
            success = false;
            break;
        }
        std::vector<char> stored(stored_size);
        input.read(stored.data(), static_cast<std::streamsize>(stored.size()));
        if (input.gcount() != static_cast<std::streamsize>(stored.size())) {
            success = false;
            break;
        }
        std::vector<char> decoded;
        const char* data = stored.data();
        std::size_t data_size = stored.size();
        if (flag == 1) {
#ifdef _WIN32
            if (!decompressor) { success = false; break; }
            decoded.resize(original_size);
            SIZE_T written = 0;
            if (!Decompress(decompressor, stored.data(), stored.size(), decoded.data(),
                            decoded.size(), &written) || written != original_size) {
                success = false;
                break;
            }
            data = decoded.data();
            data_size = decoded.size();
#else
            success = false;
            break;
#endif
        } else if (stored_size != original_size) {
            success = false;
            break;
        }
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(
            data_size, maximum_bytes - total));
        output.write(data, static_cast<std::streamsize>(wanted));
        if (!output) { success = false; break; }
        total += original_size;
    }
#ifdef _WIN32
    if (decompressor) CloseDecompressor(decompressor);
#endif
    if (!success) return false;
    if (maximum_bytes < expected_size && total >= maximum_bytes) return true;
    return total == expected_size;
}

bool FileService::stream_object(const FileMetadata& metadata, std::ostream& output,
                                std::uint64_t maximum_bytes) const {
    auto input = storage_->open_read(metadata.object_key);
    if (!input) {
        TemporaryFile staged("read");
        std::ofstream temporary(staged.path(), std::ios::binary | std::ios::trunc);
        if (!temporary || !storage_->get(metadata.object_key, temporary)) return false;
        temporary.close();
        auto fallback = std::make_unique<std::ifstream>(staged.path(), std::ios::binary);
        if (!*fallback) return false;
        if (metadata.storage_mode == StorageMode::Deep)
            return decode_deep_stream(*fallback, output, metadata.size, maximum_bytes);
        std::uint64_t written = 0;
        const auto expected = std::min(metadata.size, maximum_bytes);
        return copy_limited(*fallback, output, expected, written) && written == expected;
    }
    if (metadata.storage_mode == StorageMode::Deep)
        return decode_deep_stream(*input, output, metadata.size, maximum_bytes);
    std::uint64_t written = 0;
    const auto expected = std::min(metadata.size, maximum_bytes);
    return copy_limited(*input, output, expected, written) && written == expected;
}

bool FileService::content_equals(const FileMetadata& metadata,
                                 const std::filesystem::path& staged_path) const {
    std::ifstream expected(staged_path, std::ios::binary);
    if (!expected) return false;
    ComparingBuffer buffer(expected);
    std::ostream comparison(&buffer);
    return stream_object(metadata, comparison) && comparison.good() && buffer.complete();
}

std::mutex& FileService::content_mutex(const UserId& owner, const std::string& parent,
                                       const std::string& hash, std::uint64_t size) {
    const auto key = owner + '\n' + parent + '\n' + hash + '\n' + std::to_string(size);
    return content_mutexes_[std::hash<std::string>{}(key) % content_mutexes_.size()];
}

FileId FileService::upload(const UserId& owner, const std::string& name, std::istream& input,
                           const std::string& parent, StorageMode mode) {
    const auto result = upload_checked(owner, name, input, parent, mode);
    return result.status == UploadStatus::Failed ? FileId{} : result.id;
}

UploadResult FileService::upload_checked(const UserId& owner, const std::string& name,
                                         std::istream& input, const std::string& parent,
                                         StorageMode mode, bool emit_log) {
    TemporaryFile staged("upload");
    std::uint64_t size = 0;
    std::string content_hash;
    if (!stage_input(input, staged.path(), size, content_hash)) return {};

    const UserId effective_owner = owner.empty() ? "anonymous" : owner;
    std::lock_guard content_lock(content_mutex(effective_owner, parent, content_hash, size));
    for (const auto& existing :
         repository_->find_duplicates(effective_owner, parent, content_hash, size)) {
        if (!content_equals(existing, staged.path())) continue;
        if (emit_log) LOG_WARN("duplicate upload rejected, existing id=" + existing.id);
        return {UploadStatus::Duplicate, existing.id};
    }

    FileMetadata metadata;
    metadata.id = MetadataRepository::new_id();
    metadata.owner_id = effective_owner;
    metadata.name = sanitize_name(name);
    metadata.parent_id = parent;
    metadata.storage_mode = mode;
    metadata.size = size;
    metadata.content_hash = content_hash;
    metadata.created_at = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    metadata.object_key = std::string(to_string(mode)) + "/" + metadata.id + "_" + metadata.name;

    std::ifstream raw(staged.path(), std::ios::binary);
    if (!raw) return {};
    bool stored = false;
    if (mode == StorageMode::Shallow) {
        stored = storage_->put(metadata.object_key, raw);
    } else {
        TemporaryFile encoded("compressed");
        std::ofstream encoded_output(encoded.path(), std::ios::binary | std::ios::trunc);
        if (!encoded_output || !encode_deep_stream(raw, encoded_output)) return {};
        encoded_output.flush();
        if (!encoded_output.good()) return {};
        encoded_output.close();
        std::ifstream encoded_input(encoded.path(), std::ios::binary);
        stored = encoded_input && storage_->put(metadata.object_key, encoded_input);
    }
    if (!stored) {
        if (emit_log) LOG_ERROR("object storage put failed for id=" + metadata.id);
        return {};
    }
    if (checkpoint_) checkpoint_("upload_object_published");
    if (repository_->create(metadata).empty()) {
        storage_->remove(metadata.object_key);
        return {};
    }
    if (checkpoint_) checkpoint_("upload_committed");
    if (emit_log) LOG_INFO("uploaded file id=" + metadata.id + ", size=" +
                           std::to_string(metadata.size));
    return {UploadStatus::Created, metadata.id};
}

bool FileService::download(const UserId& owner, const FileId& id, std::ostream& output) {
    return download_limited(owner, id, output,
                            (std::numeric_limits<std::uint64_t>::max)());
}

bool FileService::download_limited(const UserId& owner, const FileId& id, std::ostream& output,
                                   std::uint64_t maximum_bytes) {
    const auto item = repository_->find(id);
    return item && item->owner_id == owner && stream_object(*item, output, maximum_bytes);
}

std::string FileService::tail(const UserId& owner, const FileId& id, std::size_t maximum_bytes) {
    const auto item = repository_->find(id);
    if (!item || item->owner_id != owner) return {};
    TailBuffer buffer(maximum_bytes);
    std::ostream output(&buffer);
    return stream_object(*item, output) && output.good() ? buffer.value() : std::string{};
}

bool FileService::delete_file(const UserId& owner, const FileId& id) {
    const auto item = repository_->find(id);
    if (!item || item->owner_id != owner) return false;
    if (!repository_->remove(id)) return false;
    if (checkpoint_) checkpoint_("delete_committed");
    if (!storage_->remove(item->object_key))
        LOG_WARN("delete committed; object cleanup deferred, id=" + id);
    LOG_INFO("deleted file id=" + id);
    return true;
}

std::vector<FileMetadata> FileService::list(const UserId& owner,
                                            const std::string& parent) const {
    return repository_->list(owner, parent);
}

std::optional<FileMetadata> FileService::metadata(const FileId& id) const {
    return repository_->find(id);
}

} // namespace cloud
