#include "logistics/central_server/database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace logistics::central_server {
namespace {

DatabaseStatus FromSqlite(int code, sqlite3* database, std::string_view context) {
    if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW) {
        return DatabaseStatus::Ok();
    }
    DatabaseStatusCode status = DatabaseStatusCode::kSqlError;
    const int primary = code & 0xff;
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
        status = DatabaseStatusCode::kBusy;
    } else if (primary == SQLITE_CONSTRAINT) {
        status = DatabaseStatusCode::kConstraint;
    } else if (primary == SQLITE_IOERR || primary == SQLITE_CANTOPEN || primary == SQLITE_READONLY) {
        status = DatabaseStatusCode::kIoError;
    }
    std::string message{ context };
    message += ": ";
    message += database == nullptr ? sqlite3_errstr(code) : sqlite3_errmsg(database);
    return { status, std::move(message) };
}

std::string Checksum(std::string_view input) {
    // FNV-1a is used only to detect an accidentally modified, already-applied migration.
    std::uint64_t value = 14695981039346656037ULL;
    for (const unsigned char byte : input) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::string ReadFile(const std::filesystem::path& path, DatabaseStatus& status) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        status = { DatabaseStatusCode::kMigrationError, "cannot read migration: " + path.string() };
        return {};
    }
    std::ostringstream content;
    content << input.rdbuf();
    status = DatabaseStatus::Ok();
    return content.str();
}

}  // namespace

Statement::Statement(sqlite3_stmt* statement) noexcept : statement_(statement) {}
Statement::~Statement() {
    sqlite3_finalize(statement_);
}

Statement::Statement(Statement&& other) noexcept : statement_(std::exchange(other.statement_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(statement_);
        statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
}

DatabaseStatus Statement::Bind(int index, std::string_view value) {
    return FromSqlite(
        sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT),
        sqlite3_db_handle(statement_), "bind text");
}

DatabaseStatus Statement::Bind(int index, std::int64_t value) {
    return FromSqlite(sqlite3_bind_int64(statement_, index, value), sqlite3_db_handle(statement_), "bind integer");
}

DatabaseStatus Statement::Bind(int index, int value) {
    return FromSqlite(sqlite3_bind_int(statement_, index, value), sqlite3_db_handle(statement_), "bind integer");
}

DatabaseStatus Statement::Bind(int index, double value) {
    return FromSqlite(sqlite3_bind_double(statement_, index, value), sqlite3_db_handle(statement_), "bind real");
}

DatabaseStatus Statement::BindNull(int index) {
    return FromSqlite(sqlite3_bind_null(statement_, index), sqlite3_db_handle(statement_), "bind null");
}

DatabaseStatus Statement::Step(bool& has_row) {
    const int code = sqlite3_step(statement_);
    has_row = code == SQLITE_ROW;
    return FromSqlite(code, sqlite3_db_handle(statement_), "execute statement");
}

DatabaseStatus Statement::Reset() {
    sqlite3_clear_bindings(statement_);
    return FromSqlite(sqlite3_reset(statement_), sqlite3_db_handle(statement_), "reset statement");
}

std::string Statement::ColumnText(int index) const {
    const auto* text = sqlite3_column_text(statement_, index);
    const int bytes = sqlite3_column_bytes(statement_, index);
    return text == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(text), bytes);
}

std::int64_t Statement::ColumnInt64(int index) const {
    return sqlite3_column_int64(statement_, index);
}
int Statement::ColumnInt(int index) const {
    return sqlite3_column_int(statement_, index);
}
double Statement::ColumnDouble(int index) const {
    return sqlite3_column_double(statement_, index);
}

Database::~Database() {
    static_cast<void>(Close());
}
Database::Database(Database&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        static_cast<void>(Close());
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

DatabaseStatus Database::Open(const DatabaseConfig& config) {
    if (handle_ != nullptr || config.path.empty() || config.busy_timeout_ms < 0) {
        return { DatabaseStatusCode::kInvalidArgument, "invalid database configuration or database already open" };
    }
    std::error_code filesystem_error;
    if (!config.path.parent_path().empty()) {
        std::filesystem::create_directories(config.path.parent_path(), filesystem_error);
    }
    if (filesystem_error) {
        return { DatabaseStatusCode::kIoError, "cannot create database directory: " + filesystem_error.message() };
    }
    const int code = sqlite3_open_v2(config.path.string().c_str(), &handle_,
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (code != SQLITE_OK) {
        const auto failure = FromSqlite(code, handle_, "open database");
        sqlite3_close(handle_);
        handle_ = nullptr;
        return failure;
    }
    sqlite3_extended_result_codes(handle_, 1);
    sqlite3_busy_timeout(handle_, config.busy_timeout_ms);
    auto status = Execute("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    if (!status.ok()) {
        return status;
    }
    return DatabaseStatus::Ok();
}

DatabaseStatus Database::Execute(std::string_view sql) {
    char* error = nullptr;
    const int code = sqlite3_exec(handle_, std::string(sql).c_str(), nullptr, nullptr, &error);
    if (code == SQLITE_OK) {
        return DatabaseStatus::Ok();
    }
    std::string message = error == nullptr ? sqlite3_errmsg(handle_) : error;
    sqlite3_free(error);
    auto status = FromSqlite(code, handle_, "execute SQL");
    status.message += " (" + message + ")";
    return status;
}

DatabaseStatus Database::Prepare(std::string_view sql, Statement& output) {
    sqlite3_stmt* statement = nullptr;
    const int code = sqlite3_prepare_v2(handle_, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
    if (code != SQLITE_OK) {
        return FromSqlite(code, handle_, "prepare SQL");
    }
    output = Statement(statement);
    return DatabaseStatus::Ok();
}

DatabaseStatus Database::Begin() {
    return Execute("BEGIN IMMEDIATE");
}
DatabaseStatus Database::Commit() {
    return Execute("COMMIT");
}
DatabaseStatus Database::Rollback() {
    return Execute("ROLLBACK");
}

DatabaseStatus Database::IntegrityCheck() {
    Statement statement;
    auto status = Prepare("PRAGMA quick_check", statement);
    if (!status.ok()) {
        return status;
    }
    bool row = false;
    status = statement.Step(row);
    if (!status.ok() || !row || statement.ColumnText(0) != "ok") {
        return { DatabaseStatusCode::kSqlError, "SQLite quick_check failed" };
    }
    return DatabaseStatus::Ok();
}

DatabaseStatus Database::Checkpoint() {
    if (handle_ == nullptr) {
        return { DatabaseStatusCode::kInvalidArgument, "database is not open" };
    }
    return Execute("PRAGMA wal_checkpoint(TRUNCATE)");
}

DatabaseStatus Database::Close() {
    if (handle_ == nullptr) {
        return DatabaseStatus::Ok();
    }
    const int code = sqlite3_close(handle_);
    if (code != SQLITE_OK) {
        return FromSqlite(code, handle_, "close database");
    }
    handle_ = nullptr;
    return DatabaseStatus::Ok();
}

Transaction::Transaction(Database& database) : database_(database), status_(database.Begin()), active_(status_.ok()) {}
Transaction::~Transaction() {
    if (active_) {
        static_cast<void>(database_.Rollback());
    }
}

DatabaseStatus Transaction::Commit() {
    if (!active_) {
        return status_.ok() ? DatabaseStatus{ DatabaseStatusCode::kInvalidArgument, "transaction is not active" }
                            : status_;
    }
    status_ = database_.Commit();
    if (status_.ok()) {
        active_ = false;
    }
    return status_;
}

DatabaseStatus MigrationRunner::Apply(Database& database, const std::filesystem::path& migration_dir) {
    auto status = database.Execute(
        "CREATE TABLE IF NOT EXISTS schema_migrations("
        "version INTEGER PRIMARY KEY, name TEXT NOT NULL, checksum TEXT NOT NULL, applied_at_ms INTEGER NOT NULL)");
    if (!status.ok()) {
        return status;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(migration_dir, error)) {
        return { DatabaseStatusCode::kMigrationError, "migration directory not found: " + migration_dir.string() };
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(migration_dir, error)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sql") {
            files.push_back(entry.path());
        }
    }
    if (error) {
        return { DatabaseStatusCode::kMigrationError, "cannot enumerate migrations: " + error.message() };
    }
    std::sort(files.begin(), files.end());
    int expected_version = 1;
    for (const auto& path : files) {
        const std::string filename = path.filename().string();
        if (filename.size() < 4 || filename[3] != '_') {
            return { DatabaseStatusCode::kMigrationError, "invalid migration filename: " + filename };
        }
        int version = 0;
        try {
            version = std::stoi(filename.substr(0, 3));
        } catch (...) {
            return { DatabaseStatusCode::kMigrationError, "invalid migration version: " + filename };
        }
        if (version != expected_version++) {
            return { DatabaseStatusCode::kMigrationError, "migration sequence has a gap at: " + filename };
        }
        std::string sql = ReadFile(path, status);
        if (!status.ok()) {
            return status;
        }
        const std::string checksum = Checksum(sql);
        Statement lookup;
        status = database.Prepare("SELECT checksum FROM schema_migrations WHERE version=?", lookup);
        if (!status.ok() || !(status = lookup.Bind(1, version)).ok()) {
            return status;
        }
        bool row = false;
        status = lookup.Step(row);
        if (!status.ok()) {
            return status;
        }
        if (row) {
            if (lookup.ColumnText(0) != checksum) {
                return { DatabaseStatusCode::kMigrationError, "applied migration checksum differs: " + filename };
            }
            continue;
        }
        Transaction transaction(database);
        if (!transaction.status().ok()) {
            return transaction.status();
        }
        if (!(status = database.Execute(sql)).ok()) {
            return status;
        }
        Statement insert;
        status = database.Prepare(
            "INSERT INTO schema_migrations(version,name,checksum,applied_at_ms) "
            "VALUES(?,?,?,CAST((julianday('now')-2440587.5)*86400000 AS INTEGER))",
            insert);
        if (!status.ok() || !(status = insert.Bind(1, version)).ok() || !(status = insert.Bind(2, filename)).ok() ||
            !(status = insert.Bind(3, checksum)).ok()) {
            return status;
        }
        bool ignored = false;
        if (!(status = insert.Step(ignored)).ok() || !(status = transaction.Commit()).ok()) {
            return status;
        }
    }
    return DatabaseStatus::Ok();
}

}  // namespace logistics::central_server
