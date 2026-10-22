#include "sql.h"

#include "sqlite3.h"
#include "util.h"

Stmt::Stmt(sqlite3* db, const char* sql) : m_db(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &m_s, nullptr) != SQLITE_OK) {
        logLine(std::string("sql: ") + sqlite3_errmsg(db) + " in: " + sql);
        m_s = nullptr;
    }
}

Stmt::~Stmt() {
    if (m_s) sqlite3_finalize(m_s);
}

Stmt& Stmt::operator=(Stmt&& o) noexcept {
    if (this != &o) {
        if (m_s) sqlite3_finalize(m_s);
        m_s = o.m_s;
        m_db = o.m_db;
        o.m_s = nullptr;
    }
    return *this;
}

Stmt& Stmt::bind(int i, int64_t v) {
    if (m_s) sqlite3_bind_int64(m_s, i, v);
    return *this;
}
Stmt& Stmt::bind(int i, const std::string& v) {
    if (m_s) sqlite3_bind_text(m_s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
    return *this;
}
Stmt& Stmt::bind(int i, const std::vector<uint8_t>& v) {
    if (!m_s) return *this;
    if (v.empty()) sqlite3_bind_null(m_s, i);
    else sqlite3_bind_blob(m_s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
    return *this;
}
Stmt& Stmt::bindNull(int i) {
    if (m_s) sqlite3_bind_null(m_s, i);
    return *this;
}

bool Stmt::step() {
    if (!m_s) return false;
    int rc = sqlite3_step(m_s);
    if (rc == SQLITE_ROW) return true;
    if (rc != SQLITE_DONE) logLine(std::string("sql step: ") + sqlite3_errmsg(m_db));
    return false;
}

bool Stmt::run() {
    if (!m_s) return false;
    int rc;
    while ((rc = sqlite3_step(m_s)) == SQLITE_ROW) {}
    bool ok = rc == SQLITE_DONE;
    if (!ok) logLine(std::string("sql run: ") + sqlite3_errmsg(m_db));
    sqlite3_reset(m_s);
    sqlite3_clear_bindings(m_s);
    return ok;
}

void Stmt::reset() {
    if (!m_s) return;
    sqlite3_reset(m_s);
    sqlite3_clear_bindings(m_s);
}

int64_t Stmt::i64(int c) const { return m_s ? sqlite3_column_int64(m_s, c) : 0; }
std::string Stmt::str(int c) const {
    if (!m_s) return "";
    const unsigned char* t = sqlite3_column_text(m_s, c);
    return t ? std::string((const char*)t, sqlite3_column_bytes(m_s, c)) : "";
}
std::vector<uint8_t> Stmt::blob(int c) const {
    if (!m_s) return {};
    const uint8_t* p = (const uint8_t*)sqlite3_column_blob(m_s, c);
    int n = sqlite3_column_bytes(m_s, c);
    return p ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>();
}
bool Stmt::null(int c) const { return !m_s || sqlite3_column_type(m_s, c) == SQLITE_NULL; }

Sql::~Sql() { close(); }

bool Sql::open(const std::string& path) {
    close();
    if (sqlite3_open_v2(path.c_str(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        logLine("sql: can't open " + path + ": " + (m_db ? sqlite3_errmsg(m_db) : "?"));
        close();
        return false;
    }
    sqlite3_busy_timeout(m_db, 10000);
    return true;
}

void Sql::close() {
    if (m_db) sqlite3_close_v2(m_db);
    m_db = nullptr;
}

bool Sql::exec(const std::string& sql) {
    char* err = nullptr;
    if (!m_db || sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        logLine("sql: " + std::string(err ? err : "no database") + " in: " + sql.substr(0, 200));
        sqlite3_free(err);
        return false;
    }
    return true;
}

int64_t Sql::lastId() const { return m_db ? sqlite3_last_insert_rowid(m_db) : 0; }
int Sql::changes() const { return m_db ? sqlite3_changes(m_db) : 0; }
std::string Sql::error() const { return m_db ? sqlite3_errmsg(m_db) : "no database"; }
