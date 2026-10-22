// A thin layer over SQLite: open, exec, prepared statements with bind/read.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

class Stmt {
public:
    Stmt() = default;
    Stmt(sqlite3* db, const char* sql);
    ~Stmt();
    Stmt(Stmt&& o) noexcept : m_s(o.m_s), m_db(o.m_db) { o.m_s = nullptr; }
    Stmt& operator=(Stmt&& o) noexcept;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    bool ok() const { return m_s != nullptr; }

    // 1-based, like SQLite
    Stmt& bind(int i, int64_t v);
    Stmt& bind(int i, int v) { return bind(i, (int64_t)v); }
    Stmt& bind(int i, const std::string& v);
    Stmt& bind(int i, const char* v) { return bind(i, std::string(v)); }
    Stmt& bind(int i, const std::vector<uint8_t>& v);  // a blob (NULL when empty)
    Stmt& bindNull(int i);
    bool step();  // true while there's a row
    bool run();   // steps to the end; false on an error
    void reset();

    // 0-based columns
    int64_t i64(int c) const;
    int i32(int c) const { return (int)i64(c); }
    std::string str(int c) const;
    std::vector<uint8_t> blob(int c) const;
    bool null(int c) const;

private:
    sqlite3_stmt* m_s = nullptr;
    sqlite3* m_db = nullptr;
};

class Sql {
public:
    ~Sql();
    bool open(const std::string& path);  // UTF-8
    void close();
    bool exec(const std::string& sql);   // false (and logged) on error
    Stmt prepare(const char* sql) { return Stmt(m_db, sql); }
    int64_t lastId() const;
    int changes() const;
    std::string error() const;
    sqlite3* raw() { return m_db; }

private:
    sqlite3* m_db = nullptr;
};

// BEGIN ... COMMIT (ROLLBACK when not committed)
class Tx {
public:
    explicit Tx(Sql& db) : m_db(db) { m_db.exec("BEGIN"); }
    ~Tx() { if (!m_done) m_db.exec("ROLLBACK"); }
    bool commit() { m_done = true; return m_db.exec("COMMIT"); }

private:
    Sql& m_db;
    bool m_done = false;
};
