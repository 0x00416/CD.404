#include <windows.h>

#include <shlobj.h>
#include <winsqlite/winsqlite3.h>

#include <cd404/platform/windows/listenbrainz_queue.hpp>

#include <algorithm>
#include <system_error>

namespace cd404::platform::windows {
namespace {

class SqliteStatement final {
public:
    SqliteStatement(sqlite3* database, const char* sql) noexcept
    {
        if (database != nullptr) {
            static_cast<void>(sqlite3_prepare_v2(
                database,
                sql,
                -1,
                &statement_,
                nullptr));
        }
    }
    ~SqliteStatement()
    {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_{};
};

[[nodiscard]] bool bind_owner(
    sqlite3_stmt* statement,
    const std::string_view owner_key) noexcept
{
    return statement != nullptr &&
        sqlite3_bind_text(
            statement,
            1,
            owner_key.data(),
            static_cast<int>(owner_key.size()),
            SQLITE_TRANSIENT) == SQLITE_OK;
}

} // namespace

std::filesystem::path default_listenbrainz_queue_path()
{
    wchar_t* local_app_data{};
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData,
            KF_FLAG_CREATE,
            nullptr,
            &local_app_data)) ||
        local_app_data == nullptr) {
        return {};
    }
    const std::filesystem::path base(local_app_data);
    CoTaskMemFree(local_app_data);
    return base / L"CD.404" / L"listenbrainz.db";
}

struct ListenBrainzQueue::Implementation final {
    sqlite3* database{};
    int version{};

    ~Implementation()
    {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }

    [[nodiscard]] bool execute(const char* sql) const noexcept
    {
        return database != nullptr &&
            sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    [[nodiscard]] bool has_column(const std::string_view column) const noexcept
    {
        SqliteStatement statement(database, "PRAGMA table_info(listen_queue);");
        while (statement.get() != nullptr &&
               sqlite3_step(statement.get()) == SQLITE_ROW) {
            const auto* name = sqlite3_column_text(statement.get(), 1);
            if (name != nullptr &&
                column == reinterpret_cast<const char*>(name)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t count(
        const std::string_view owner,
        const bool failed) const noexcept
    {
        SqliteStatement statement(database, failed
            ? "SELECT COUNT(*) FROM listen_queue WHERE owner_key=?1 AND failed<>0;"
            : "SELECT COUNT(*) FROM listen_queue WHERE owner_key=?1 AND failed=0;");
        if (!bind_owner(statement.get(), owner) ||
            sqlite3_step(statement.get()) != SQLITE_ROW) {
            return 0;
        }
        return static_cast<std::size_t>(std::max<std::int64_t>(
            0,
            sqlite3_column_int64(statement.get(), 0)));
    }
};

ListenBrainzQueue::ListenBrainzQueue(std::filesystem::path path)
    : implementation_(std::make_unique<Implementation>())
{
    if (path.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error || sqlite3_open16(path.c_str(), &implementation_->database) != SQLITE_OK) {
        implementation_.reset();
        return;
    }
    sqlite3_busy_timeout(implementation_->database, 2'000);
    if (!implementation_->execute("PRAGMA journal_mode=WAL;") ||
        !implementation_->execute("PRAGMA synchronous=FULL;") ||
        !implementation_->execute(
            "CREATE TABLE IF NOT EXISTS listen_queue ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "session_id TEXT NOT NULL UNIQUE,"
            "owner_key TEXT NOT NULL DEFAULT '',"
            "payload TEXT NOT NULL,"
            "attempts INTEGER NOT NULL DEFAULT 0,"
            "next_attempt INTEGER NOT NULL DEFAULT 0,"
            "failed INTEGER NOT NULL DEFAULT 0,"
            "last_status INTEGER NOT NULL DEFAULT 0,"
            "last_error INTEGER NOT NULL DEFAULT 0"
            ");") ||
        (!implementation_->has_column("owner_key") &&
         !implementation_->execute(
             "ALTER TABLE listen_queue ADD COLUMN owner_key TEXT NOT NULL DEFAULT '';")) ||
        !implementation_->execute(
            "CREATE INDEX IF NOT EXISTS idx_listen_queue_owner_next "
            "ON listen_queue(owner_key, failed, next_attempt, id);") ||
        !implementation_->execute("PRAGMA user_version=3;")) {
        implementation_.reset();
        return;
    }
    implementation_->version = 3;
}

ListenBrainzQueue::~ListenBrainzQueue() = default;

bool ListenBrainzQueue::available() const noexcept
{
    return implementation_ != nullptr && implementation_->database != nullptr;
}

int ListenBrainzQueue::schema_version() const noexcept
{
    return implementation_ == nullptr ? 0 : implementation_->version;
}

bool ListenBrainzQueue::enqueue(
    const std::string_view owner,
    const std::string_view session,
    const std::string_view payload) noexcept
{
    if (!available()) {
        return false;
    }
    SqliteStatement statement(implementation_->database,
        "INSERT OR IGNORE INTO listen_queue(owner_key, session_id, payload) "
        "VALUES(?1, ?2, ?3);");
    return bind_owner(statement.get(), owner) &&
        sqlite3_bind_text(statement.get(), 2, session.data(),
            static_cast<int>(session.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement.get(), 3, payload.data(),
            static_cast<int>(payload.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(statement.get()) == SQLITE_DONE;
}

std::optional<ListenBrainzPendingListen> ListenBrainzQueue::next(
    const std::string_view owner) const noexcept
{
    if (!available()) {
        return std::nullopt;
    }
    SqliteStatement statement(implementation_->database,
        "SELECT id, payload, attempts, next_attempt FROM listen_queue "
        "WHERE failed=0 AND owner_key=?1 ORDER BY next_attempt, id LIMIT 1;");
    if (!bind_owner(statement.get(), owner) ||
        sqlite3_step(statement.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    const auto* payload = sqlite3_column_text(statement.get(), 1);
    if (payload == nullptr) {
        return std::nullopt;
    }
    return ListenBrainzPendingListen{
        sqlite3_column_int64(statement.get(), 0),
        reinterpret_cast<const char*>(payload),
        static_cast<unsigned int>(std::max(0, sqlite3_column_int(statement.get(), 2))),
        sqlite3_column_int64(statement.get(), 3),
    };
}

bool ListenBrainzQueue::complete(const std::int64_t id) noexcept
{
    if (!available()) {
        return false;
    }
    SqliteStatement statement(
        implementation_->database,
        "DELETE FROM listen_queue WHERE id=?1;");
    return statement.get() != nullptr &&
        sqlite3_bind_int64(statement.get(), 1, id) == SQLITE_OK &&
        sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool ListenBrainzQueue::schedule_retry(
    const std::int64_t id,
    const unsigned int attempts,
    const std::int64_t next_attempt,
    const unsigned long status,
    const unsigned long system_error) noexcept
{
    if (!available()) {
        return false;
    }
    SqliteStatement statement(implementation_->database,
        "UPDATE listen_queue SET attempts=?2, next_attempt=?3, "
        "last_status=?4, last_error=?5 WHERE id=?1;");
    return statement.get() != nullptr &&
        sqlite3_bind_int64(statement.get(), 1, id) == SQLITE_OK &&
        sqlite3_bind_int(statement.get(), 2, static_cast<int>(attempts)) == SQLITE_OK &&
        sqlite3_bind_int64(statement.get(), 3, next_attempt) == SQLITE_OK &&
        sqlite3_bind_int64(statement.get(), 4, status) == SQLITE_OK &&
        sqlite3_bind_int64(statement.get(), 5, system_error) == SQLITE_OK &&
        sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool ListenBrainzQueue::mark_failed(
    const std::int64_t id,
    const unsigned long status,
    const unsigned long system_error) noexcept
{
    if (!available()) {
        return false;
    }
    SqliteStatement statement(implementation_->database,
        "UPDATE listen_queue SET failed=1, last_status=?2, last_error=?3 WHERE id=?1;");
    return statement.get() != nullptr &&
        sqlite3_bind_int64(statement.get(), 1, id) == SQLITE_OK &&
        sqlite3_bind_int64(statement.get(), 2, status) == SQLITE_OK &&
        sqlite3_bind_int64(statement.get(), 3, system_error) == SQLITE_OK &&
        sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool ListenBrainzQueue::retry_all(const std::string_view owner) noexcept
{
    if (!available()) {
        return false;
    }
    SqliteStatement statement(implementation_->database,
        "UPDATE listen_queue SET failed=0, attempts=0, next_attempt=0, "
        "last_status=0, last_error=0 WHERE owner_key=?1;");
    return bind_owner(statement.get(), owner) &&
        sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool ListenBrainzQueue::clear_owner(const std::string_view owner) noexcept
{
    if (!available()) {
        return false;
    }
    SqliteStatement statement(
        implementation_->database,
        "DELETE FROM listen_queue WHERE owner_key=?1;");
    return bind_owner(statement.get(), owner) &&
        sqlite3_step(statement.get()) == SQLITE_DONE;
}

std::size_t ListenBrainzQueue::pending_count(
    const std::string_view owner) const noexcept
{
    return available() ? implementation_->count(owner, false) : 0U;
}

std::size_t ListenBrainzQueue::failed_count(
    const std::string_view owner) const noexcept
{
    return available() ? implementation_->count(owner, true) : 0U;
}

} // namespace cd404::platform::windows
