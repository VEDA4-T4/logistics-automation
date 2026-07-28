#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#ifndef LOGISTICS_DEFAULT_MIGRATION_DIR
#define LOGISTICS_DEFAULT_MIGRATION_DIR "/usr/share/logistics/migrations"
#endif

struct sqlite3;
struct sqlite3_stmt;

namespace logistics::central_server {

enum class DatabaseStatusCode : std::uint8_t {
    kOk,
    kNotFound,
    kBusy,
    kConstraint,
    kIoError,
    kInvalidArgument,
    kMigrationError,
    kSqlError,
};

struct DatabaseStatus {
    DatabaseStatusCode code{ DatabaseStatusCode::kOk };
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return code == DatabaseStatusCode::kOk;
    }
    [[nodiscard]] bool retryable() const noexcept {
        return code == DatabaseStatusCode::kBusy;
    }
    [[nodiscard]] static DatabaseStatus Ok() {
        return {};
    }
};

struct DatabaseConfig {
    std::filesystem::path path{ "/var/lib/logistics/logistics.db" };
    std::filesystem::path migration_dir{ LOGISTICS_DEFAULT_MIGRATION_DIR };
    int busy_timeout_ms{ 5000 };
};

class Statement final {
public:
    Statement() = default;
    explicit Statement(sqlite3_stmt* statement) noexcept;
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] DatabaseStatus Bind(int index, std::string_view value);
    [[nodiscard]] DatabaseStatus Bind(int index, std::int64_t value);
    [[nodiscard]] DatabaseStatus Bind(int index, int value);
    [[nodiscard]] DatabaseStatus BindNull(int index);
    [[nodiscard]] DatabaseStatus Step(bool& has_row);
    [[nodiscard]] DatabaseStatus Reset();
    [[nodiscard]] std::string ColumnText(int index) const;
    [[nodiscard]] std::int64_t ColumnInt64(int index) const;
    [[nodiscard]] int ColumnInt(int index) const;

private:
    sqlite3_stmt* statement_{ nullptr };
};

class Database final {
public:
    Database() = default;
    ~Database();
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] DatabaseStatus Open(const DatabaseConfig& config);
    [[nodiscard]] DatabaseStatus Execute(std::string_view sql);
    [[nodiscard]] DatabaseStatus Prepare(std::string_view sql, Statement& output);
    [[nodiscard]] DatabaseStatus Begin();
    [[nodiscard]] DatabaseStatus Commit();
    [[nodiscard]] DatabaseStatus Rollback();
    [[nodiscard]] DatabaseStatus IntegrityCheck();
    [[nodiscard]] DatabaseStatus Checkpoint();
    [[nodiscard]] DatabaseStatus Close();
    [[nodiscard]] bool IsOpen() const noexcept {
        return handle_ != nullptr;
    }

private:
    sqlite3* handle_{ nullptr };
};

class Transaction final {
public:
    explicit Transaction(Database& database);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    [[nodiscard]] const DatabaseStatus& status() const noexcept {
        return status_;
    }
    [[nodiscard]] DatabaseStatus Commit();

private:
    Database& database_;
    DatabaseStatus status_;
    bool active_{ false };
};

class MigrationRunner final {
public:
    [[nodiscard]] static DatabaseStatus Apply(Database& database, const std::filesystem::path& migration_dir);
};

}  // namespace logistics::central_server
