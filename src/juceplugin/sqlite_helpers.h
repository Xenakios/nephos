#pragma once
#include <stdexcept>
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include "sqlite3.h"
#include "containers/choc_Value.h"

struct SqliteError : std::runtime_error
{
    explicit SqliteError(const std::string &msg) : std::runtime_error(msg) {}
};

class SqliteDb
{
  public:
    explicit SqliteDb(const std::string &path)
    {
        sqlite3 *raw = nullptr;
        if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK)
        {
            std::string err = sqlite3_errmsg(raw);
            sqlite3_close(raw);
            throw SqliteError("Failed to open db: " + err);
        }
        db_.reset(raw);
        sqlite3_exec(db_.get(), "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    }

    sqlite3 *get() const { return db_.get(); }

  private:
    struct Deleter
    {
        void operator()(sqlite3 *p) const { sqlite3_close(p); }
    };
    std::unique_ptr<sqlite3, Deleter> db_;
};

class SqliteStmt
{
  public:
    SqliteStmt() = default;
    SqliteStmt(sqlite3 *db, const std::string &sql)
    {
        sqlite3_stmt *raw = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        {
            throw SqliteError("Failed to prepare: " + std::string(sqlite3_errmsg(db)));
        }
        stmt_.reset(raw);
    }

    sqlite3_stmt *get() const { return stmt_.get(); }

  private:
    struct Deleter
    {
        void operator()(sqlite3_stmt *p) const { sqlite3_finalize(p); }
    };
    std::unique_ptr<sqlite3_stmt, Deleter> stmt_;
};

inline void presetsInitSchema(SqliteDb &db)
{
    const char *sql = R"(
        CREATE TABLE IF NOT EXISTS presets (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            name        TEXT NOT NULL,
            category    TEXT,
            author      TEXT,
            tags        TEXT,
            is_factory  INTEGER DEFAULT 0,
            created_at  INTEGER NOT NULL,
            modified_at INTEGER NOT NULL,
            data_format INTEGER DEFAULT 1,
            data        BLOB NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_presets_category ON presets(category);
        CREATE INDEX IF NOT EXISTS idx_presets_name ON presets(name);
    )";
    char *errMsg = nullptr;
    if (sqlite3_exec(db.get(), sql, nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        std::string msg = errMsg ? errMsg : "unknown error";
        sqlite3_free(errMsg);
        throw SqliteError("Schema init failed: " + msg);
    }
}

inline bool presetExists(SqliteDb &db, int64_t ID)
{
    SqliteStmt stmt(db.get(), "SELECT id FROM presets WHERE id = ?");
    sqlite3_bind_int64(stmt.get(), 1, ID);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

inline int64_t findPresetWithNameCategory(SqliteDb &db, std::string name, std::string category)
{
    SqliteStmt stmt(db.get(), "SELECT id FROM presets WHERE name = ? AND category = ?");
    sqlite3_bind_text(stmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, category.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW)
    {
        return sqlite3_column_int64(stmt.get(), 0);
    }
    return -1;
}

inline int64_t insertPreset(SqliteDb &db, std::string name, std::string category,
                            choc::value::ValueView statedata)
{
    auto sdata = statedata.serialise();
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    SqliteStmt outerstmt = SqliteStmt(db.get(), R"(
        INSERT INTO presets (name, category, created_at, modified_at, data)
        VALUES (?, ?, ?, ?, ?)
    )");
    sqlite3_bind_text(outerstmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outerstmt.get(), 2, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(outerstmt.get(), 3, now);
    sqlite3_bind_int64(outerstmt.get(), 4, now);
    // SQLITE_TRANSIENT tells sqlite to copy the bytes now, since `data`
    // may go out of scope before the statement executes
    sqlite3_bind_blob(outerstmt.get(), 5, sdata.data.data(), static_cast<int>(sdata.data.size()),
                      SQLITE_TRANSIENT);
    if (sqlite3_step(outerstmt.get()) != SQLITE_DONE)
    {
        throw SqliteError("Insert failed: " + std::string(sqlite3_errmsg(db.get())));
    }
    return sqlite3_last_insert_rowid(db.get());
}

inline void updatePreset(SqliteDb &db, int64_t presetID, std::string name, std::string category,
                         choc::value::ValueView statedata)
{
    auto sdata = statedata.serialise();
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    SqliteStmt outerstmt = SqliteStmt(db.get(), R"(
        UPDATE presets
        SET name = ?, category = ?, data = ?, modified_at = ?
        WHERE id = ?
    )");
    sqlite3_bind_text(outerstmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outerstmt.get(), 2, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(outerstmt.get(), 3, sdata.data.data(), static_cast<int>(sdata.data.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(outerstmt.get(), 4, now);
    sqlite3_bind_int64(outerstmt.get(), 5, presetID);
    if (sqlite3_step(outerstmt.get()) != SQLITE_DONE)
    {
        throw SqliteError("Update failed: " + std::string(sqlite3_errmsg(db.get())));
    }
}

struct PresetRecord
{
    int64_t id;
    std::string name;
    std::string category;
    std::vector<uint8_t> data;
};

inline std::optional<PresetRecord> presetsLoadPreset(SqliteDb &db, int64_t presetID)
{
    SqliteStmt stmt(db.get(), "SELECT id, name, category, data FROM presets WHERE id = ?");
    sqlite3_bind_int64(stmt.get(), 1, presetID);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW)
    {
        return std::nullopt; // not found
    }

    PresetRecord rec;
    rec.id = sqlite3_column_int64(stmt.get(), 0);
    rec.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 1));

    if (const unsigned char *catText = sqlite3_column_text(stmt.get(), 2))
        rec.category = reinterpret_cast<const char *>(catText);

    const void *blobPtr = sqlite3_column_blob(stmt.get(), 3);
    int blobSize = sqlite3_column_bytes(stmt.get(), 3);
    rec.data.assign(static_cast<const uint8_t *>(blobPtr),
                    static_cast<const uint8_t *>(blobPtr) + blobSize);

    return rec;
}

struct PresetSummary
{
    int64_t id;
    std::string name;
    std::string category;
};

inline std::vector<PresetSummary> listPresets(SqliteDb &db, const std::string &categoryFilter = "")
{
    std::vector<PresetSummary> results;
    // we expect the query will produce results
    results.reserve(64);
    std::string sql = "SELECT id, name, category FROM presets";
    if (!categoryFilter.empty())
        sql += " WHERE category = ?";
    sql += " ORDER BY name COLLATE NOCASE";

    SqliteStmt stmt(db.get(), sql);
    if (!categoryFilter.empty())
        sqlite3_bind_text(stmt.get(), 1, categoryFilter.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW)
    {
        PresetSummary s;
        s.id = sqlite3_column_int64(stmt.get(), 0);
        s.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 1));
        if (const unsigned char *cat = sqlite3_column_text(stmt.get(), 2))
            s.category = reinterpret_cast<const char *>(cat);
        results.push_back(std::move(s));
    }
    return results;
}
