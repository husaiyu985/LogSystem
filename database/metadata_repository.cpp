#include "database/metadata_repository.h"

#include <sqlite3.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace cloud {
namespace {

using Records = std::unordered_map<FileId, FileMetadata>;

class Statement {
public:
    Statement(sqlite3* database, const char* sql) {
        if (sqlite3_prepare_v2(database, sql, -1, &value_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("SQLite prepare failed: ") + sqlite3_errmsg(database));
    }
    ~Statement() { sqlite3_finalize(value_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const noexcept { return value_; }
private:
    sqlite3_stmt* value_ = nullptr;
};

void execute(sqlite3* database, const char* sql) {
    char* message = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) return;
    const std::string detail = message ? message : sqlite3_errmsg(database);
    sqlite3_free(message);
    throw std::runtime_error("SQLite operation failed: " + detail);
}

void bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
    if (sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("SQLite text binding failed");
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    return value && bytes > 0
        ? std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(bytes))
        : std::string{};
}

FileMetadata read_metadata(sqlite3_stmt* statement) {
    FileMetadata item;
    item.id = column_text(statement, 0);
    item.owner_id = column_text(statement, 1);
    item.name = column_text(statement, 2);
    item.parent_id = column_text(statement, 3);
    item.object_key = column_text(statement, 4);
    item.size = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 5));
    item.content_hash = column_text(statement, 6);
    item.is_folder = sqlite3_column_int(statement, 7) != 0;
    item.storage_mode = sqlite3_column_int(statement, 8) == 1
        ? StorageMode::Deep : StorageMode::Shallow;
    item.created_at = sqlite3_column_int64(statement, 9);
    return item;
}

void create_schema(sqlite3* database) {
    execute(database,
        "CREATE TABLE IF NOT EXISTS files("
        "id TEXT PRIMARY KEY NOT NULL,"
        "owner_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "parent_id TEXT NOT NULL DEFAULT '',"
        "object_key TEXT NOT NULL UNIQUE,"
        "size INTEGER NOT NULL CHECK(size >= 0),"
        "content_hash TEXT NOT NULL,"
        "is_folder INTEGER NOT NULL DEFAULT 0 CHECK(is_folder IN (0,1)),"
        "storage_mode INTEGER NOT NULL CHECK(storage_mode IN (0,1)),"
        "created_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS idx_files_owner_parent_created "
        "ON files(owner_id,parent_id,created_at DESC);"
        "CREATE INDEX IF NOT EXISTS idx_files_duplicate "
        "ON files(owner_id,parent_id,content_hash,size);"
        "PRAGMA user_version=1;");
}

void bind_metadata(sqlite3_stmt* statement, const FileMetadata& item) {
    bind_text(statement, 1, item.id);
    bind_text(statement, 2, item.owner_id);
    bind_text(statement, 3, item.name);
    bind_text(statement, 4, item.parent_id);
    bind_text(statement, 5, item.object_key);
    sqlite3_bind_int64(statement, 6, static_cast<sqlite3_int64>(item.size));
    bind_text(statement, 7, item.content_hash);
    sqlite3_bind_int(statement, 8, item.is_folder ? 1 : 0);
    sqlite3_bind_int(statement, 9, item.storage_mode == StorageMode::Deep ? 1 : 0);
    sqlite3_bind_int64(statement, 10, item.created_at);
}

std::uint64_t snapshot_checksum(const std::string& text) {
    std::uint64_t value = 14695981039346656037ULL;
    for (unsigned char c : text) { value ^= c; value *= 1099511628211ULL; }
    return value;
}

Records parse_legacy_snapshot(const std::filesystem::path& file) {
    std::ifstream input_file(file, std::ios::binary);
    if (!input_file) throw std::runtime_error("Cannot read legacy metadata snapshot");
    std::ostringstream raw;
    raw << input_file.rdbuf();
    if (input_file.bad()) throw std::runtime_error("Legacy metadata read error");

    std::string payload = raw.str();
    std::size_t expected_count = 0;
    const bool versioned = payload.rfind("CLOUDLOG_META_V2 ", 0) == 0;
    if (versioned) {
        const auto newline = payload.find('\n');
        if (newline == std::string::npos) throw std::runtime_error("Incomplete metadata header");
        std::istringstream header(payload.substr(0, newline));
        std::string tag, extra;
        std::uint64_t checksum = 0;
        if (!(header >> tag >> expected_count >> checksum) || (header >> extra))
            throw std::runtime_error("Invalid metadata header");
        payload.erase(0, newline + 1);
        if (snapshot_checksum(payload) != checksum)
            throw std::runtime_error("Metadata checksum mismatch; restore snapshot before starting");
    }

    std::istringstream input(payload);
    Records records;
    for (;;) {
        input >> std::ws;
        if (input.eof()) break;
        FileMetadata item;
        int mode = 0;
        if (!(input >> std::quoted(item.id) >> std::quoted(item.owner_id) >> std::quoted(item.name)
                    >> std::quoted(item.parent_id) >> std::quoted(item.object_key) >> item.size
                    >> std::quoted(item.content_hash) >> item.is_folder >> mode >> item.created_at) ||
            item.id.empty() || item.object_key.empty() || (mode != 0 && mode != 1) ||
            records.count(item.id))
            throw std::runtime_error("Corrupt metadata: refusing migration");
        item.storage_mode = mode == 1 ? StorageMode::Deep : StorageMode::Shallow;
        records.emplace(item.id, std::move(item));
    }
    if (input.bad()) throw std::runtime_error("Legacy metadata read error");
    if (versioned && records.size() != expected_count)
        throw std::runtime_error("Metadata record count mismatch");
    return records;
}

bool is_sqlite_database(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    char header[16]{};
    input.read(header, sizeof(header));
    return input.gcount() == sizeof(header) &&
           std::memcmp(header, "SQLite format 3\0", sizeof(header)) == 0;
}

void insert_records(sqlite3* database, const Records& records) {
    execute(database, "BEGIN IMMEDIATE;");
    try {
        Statement insert(database,
            "INSERT INTO files(id,owner_id,name,parent_id,object_key,size,content_hash,"
            "is_folder,storage_mode,created_at) VALUES(?,?,?,?,?,?,?,?,?,?);");
        for (const auto& [id, item] : records) {
            (void)id;
            bind_metadata(insert.get(), item);
            if (sqlite3_step(insert.get()) != SQLITE_DONE)
                throw std::runtime_error(std::string("SQLite migration insert failed: ") +
                                         sqlite3_errmsg(database));
            sqlite3_reset(insert.get());
            sqlite3_clear_bindings(insert.get());
        }
        execute(database, "COMMIT;");
    } catch (...) {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

std::vector<FileMetadata> collect_rows(sqlite3* database, sqlite3_stmt* statement) {
    std::vector<FileMetadata> result;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
        result.push_back(read_metadata(statement));
    if (step != SQLITE_DONE)
        throw std::runtime_error(std::string("SQLite query failed: ") + sqlite3_errmsg(database));
    return result;
}

} // namespace

FileId MetadataRepository::new_id() { return unique_token(); }

MetadataRepository::MetadataRepository(std::filesystem::path file)
    : file_(std::filesystem::absolute(std::move(file)).lexically_normal()),
      process_lock_(file_.string() + ".lock") {
    open_or_migrate();
}

MetadataRepository::~MetadataRepository() {
    std::lock_guard lock(mutex_);
    if (database_) sqlite3_close(database_);
}

void MetadataRepository::open_or_migrate() {
    if (!file_.parent_path().empty()) std::filesystem::create_directories(file_.parent_path());
    if (!std::filesystem::exists(file_) || is_sqlite_database(file_)) {
        open_database();
        initialize_schema();
        return;
    }

    const auto records = parse_legacy_snapshot(file_);
    const auto temporary = std::filesystem::path(file_.string() + ".sqlite-migration-" + unique_token());
    auto backup = std::filesystem::path(file_.string() + ".legacy-snapshot.bak");
    if (std::filesystem::exists(backup))
        backup = std::filesystem::path(backup.string() + "." + unique_token());

    sqlite3* migration = nullptr;
    try {
        if (sqlite3_open_v2(temporary.string().c_str(), &migration,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot create SQLite migration database");
        execute(migration, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
        create_schema(migration);
        insert_records(migration, records);
        if (sqlite3_close(migration) != SQLITE_OK)
            throw std::runtime_error("Cannot close SQLite migration database");
        migration = nullptr;
        if (!sync_file(temporary)) throw std::runtime_error("Cannot flush SQLite migration database");
        if (!replace_file(file_, backup)) throw std::runtime_error("Cannot preserve legacy metadata snapshot");
        if (!replace_file(temporary, file_)) {
            replace_file(backup, file_);
            throw std::runtime_error("Cannot publish SQLite metadata database");
        }
    } catch (...) {
        if (migration) sqlite3_close(migration);
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        throw;
    }

    open_database();
    initialize_schema();
}

void MetadataRepository::open_database() {
    if (sqlite3_open_v2(file_.string().c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string detail = database_ ? sqlite3_errmsg(database_) : "unknown SQLite error";
        if (database_) sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error("Cannot open metadata database: " + detail);
    }
    sqlite3_busy_timeout(database_, 5000);
}

void MetadataRepository::initialize_schema() {
    execute(database_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
    create_schema(database_);
}

FileId MetadataRepository::create(FileMetadata metadata) {
    if (metadata.id.empty()) metadata.id = new_id();
    if (metadata.object_key.empty()) return {};
    if (!metadata.created_at)
        metadata.created_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::lock_guard lock(mutex_);
    try {
        Statement statement(database_,
            "INSERT INTO files(id,owner_id,name,parent_id,object_key,size,content_hash,"
            "is_folder,storage_mode,created_at) VALUES(?,?,?,?,?,?,?,?,?,?);");
        bind_metadata(statement.get(), metadata);
        return sqlite3_step(statement.get()) == SQLITE_DONE ? metadata.id : FileId{};
    } catch (...) {
        return {};
    }
}

std::optional<FileMetadata> MetadataRepository::find(const FileId& id) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_,
        "SELECT id,owner_id,name,parent_id,object_key,size,content_hash,is_folder,"
        "storage_mode,created_at FROM files WHERE id=?;");
    bind_text(statement.get(), 1, id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) return read_metadata(statement.get());
    if (result != SQLITE_DONE)
        throw std::runtime_error(std::string("SQLite find failed: ") + sqlite3_errmsg(database_));
    return std::nullopt;
}

std::vector<FileMetadata> MetadataRepository::list(const UserId& owner,
                                                   const std::string& parent) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_,
        "SELECT id,owner_id,name,parent_id,object_key,size,content_hash,is_folder,"
        "storage_mode,created_at FROM files WHERE owner_id=? AND parent_id=? "
        "ORDER BY created_at DESC,id DESC;");
    bind_text(statement.get(), 1, owner);
    bind_text(statement.get(), 2, parent);
    return collect_rows(database_, statement.get());
}

std::vector<FileMetadata> MetadataRepository::list_all() const {
    std::lock_guard lock(mutex_);
    Statement statement(database_,
        "SELECT id,owner_id,name,parent_id,object_key,size,content_hash,is_folder,"
        "storage_mode,created_at FROM files ORDER BY created_at DESC,id DESC;");
    return collect_rows(database_, statement.get());
}

std::vector<FileMetadata> MetadataRepository::find_duplicates(
    const UserId& owner, const std::string& parent, const std::string& content_hash,
    std::uint64_t size) const {
    std::lock_guard lock(mutex_);
    Statement statement(database_,
        "SELECT id,owner_id,name,parent_id,object_key,size,content_hash,is_folder,"
        "storage_mode,created_at FROM files WHERE owner_id=? AND parent_id=? "
        "AND content_hash=? AND size=? AND is_folder=0;");
    bind_text(statement.get(), 1, owner);
    bind_text(statement.get(), 2, parent);
    bind_text(statement.get(), 3, content_hash);
    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(size));
    return collect_rows(database_, statement.get());
}

bool MetadataRepository::remove(const FileId& id) {
    std::lock_guard lock(mutex_);
    try {
        Statement statement(database_, "DELETE FROM files WHERE id=?;");
        bind_text(statement.get(), 1, id);
        return sqlite3_step(statement.get()) == SQLITE_DONE && sqlite3_changes(database_) == 1;
    } catch (...) {
        return false;
    }
}

bool MetadataRepository::update(const FileMetadata& metadata) {
    std::lock_guard lock(mutex_);
    try {
        Statement statement(database_,
            "UPDATE files SET owner_id=?,name=?,parent_id=?,object_key=?,size=?,content_hash=?,"
            "is_folder=?,storage_mode=?,created_at=? WHERE id=?;");
        bind_text(statement.get(), 1, metadata.owner_id);
        bind_text(statement.get(), 2, metadata.name);
        bind_text(statement.get(), 3, metadata.parent_id);
        bind_text(statement.get(), 4, metadata.object_key);
        sqlite3_bind_int64(statement.get(), 5, static_cast<sqlite3_int64>(metadata.size));
        bind_text(statement.get(), 6, metadata.content_hash);
        sqlite3_bind_int(statement.get(), 7, metadata.is_folder ? 1 : 0);
        sqlite3_bind_int(statement.get(), 8,
                         metadata.storage_mode == StorageMode::Deep ? 1 : 0);
        sqlite3_bind_int64(statement.get(), 9, metadata.created_at);
        bind_text(statement.get(), 10, metadata.id);
        return sqlite3_step(statement.get()) == SQLITE_DONE && sqlite3_changes(database_) == 1;
    } catch (...) {
        return false;
    }
}

} // namespace cloud
