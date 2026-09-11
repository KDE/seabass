// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/work_counters.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace seabass::infrastructure::onelibrary
{

namespace
{

using OpenV2Fn = int (*)(const char *, sqlite3 **, int, const char *);
using CloseFn = int (*)(sqlite3 *);
using ExecFn = int (*)(sqlite3 *, const char *, void *, void *, char **);
using PrepareV2Fn = int (*)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
using StepFn = int (*)(sqlite3_stmt *);
using FinalizeFn = int (*)(sqlite3_stmt *);
using BindInt64Fn = int (*)(sqlite3_stmt *, int, int64_t);
using BindTextFn = int (*)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
using BindNullFn = int (*)(sqlite3_stmt *, int);
using ColumnInt64Fn = int64_t (*)(sqlite3_stmt *, int);
using ColumnTextFn = const unsigned char *(*)(sqlite3_stmt *, int);
using ColumnTypeFn = int (*)(sqlite3_stmt *, int);
using ErrmsgFn = const char *(*)(sqlite3 *);

// SQLite's own convention for "the string is only valid for the duration
// of this call -- make your own copy" (as opposed to SQLITE_STATIC).
void *const SqliteTransient = reinterpret_cast<void *>(-1);

#ifdef _WIN32
void *loadLibrary()
{
    return LoadLibraryA("libsqlcipher-0.dll");
}
void *resolveSymbol(void *mod, const char *name)
{
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(mod), name));
}
void unloadLibrary(void *mod)
{
    FreeLibrary(static_cast<HMODULE>(mod));
}
const char *libraryNotFoundHint()
{
    return "could not load libsqlcipher-0.dll -- is the mingw-w64-ucrt-x86_64-sqlcipher package's "
           "DLL on PATH or next to the executable?";
}
#else
// Distros disagree on the installed soname (Debian/Ubuntu's libsqlcipher1
// package ships "libsqlcipher.so.1"; other packagings use ".so.0"; the
// unversioned "libsqlcipher.so" symlink only exists if the *-dev package
// is installed) -- try each in turn rather than hard-coding one.
void *loadLibrary()
{
    for (const char *name : {"libsqlcipher.so.0", "libsqlcipher.so.1", "libsqlcipher.so"}) {
        if (void *mod = dlopen(name, RTLD_NOW)) {
            return mod;
        }
    }
    return nullptr;
}
void *resolveSymbol(void *mod, const char *name)
{
    return dlsym(mod, name);
}
void unloadLibrary(void *mod)
{
    dlclose(mod);
}
const char *libraryNotFoundHint()
{
    return "could not load libsqlcipher (tried libsqlcipher.so.0/.so.1/.so) -- is the libsqlcipher1 "
           "(or equivalent) package installed?";
}
#endif

template<typename Fn>
Fn resolve(void *mod, const char *name)
{
    auto fn = reinterpret_cast<Fn>(resolveSymbol(mod, name));
    if (!fn) {
        throw std::runtime_error(std::string("libsqlcipher is missing expected symbol: ") + name);
    }
    return fn;
}

}  // namespace

struct SqlCipherLibrary::Fns
{
    OpenV2Fn open;
    CloseFn close;
    ExecFn exec;
    PrepareV2Fn prepare;
    StepFn step;
    FinalizeFn finalize;
    BindInt64Fn bindInt64;
    BindTextFn bindText;
    BindNullFn bindNull;
    ColumnInt64Fn columnInt64;
    ColumnTextFn columnText;
    ColumnTypeFn columnType;
    ErrmsgFn errmsg;
};

SqlCipherLibrary::SqlCipherLibrary()
{
    void *mod = loadLibrary();
    if (!mod) {
        throw std::runtime_error(libraryNotFoundHint());
    }
    m_module = mod;
    m_fns = new Fns{
        resolve<OpenV2Fn>(mod, "sqlite3_open_v2"),
        resolve<CloseFn>(mod, "sqlite3_close"),
        resolve<ExecFn>(mod, "sqlite3_exec"),
        resolve<PrepareV2Fn>(mod, "sqlite3_prepare_v2"),
        resolve<StepFn>(mod, "sqlite3_step"),
        resolve<FinalizeFn>(mod, "sqlite3_finalize"),
        resolve<BindInt64Fn>(mod, "sqlite3_bind_int64"),
        resolve<BindTextFn>(mod, "sqlite3_bind_text"),
        resolve<BindNullFn>(mod, "sqlite3_bind_null"),
        resolve<ColumnInt64Fn>(mod, "sqlite3_column_int64"),
        resolve<ColumnTextFn>(mod, "sqlite3_column_text"),
        resolve<ColumnTypeFn>(mod, "sqlite3_column_type"),
        resolve<ErrmsgFn>(mod, "sqlite3_errmsg"),
    };
}

SqlCipherLibrary::~SqlCipherLibrary()
{
    delete m_fns;
    if (m_module) {
        unloadLibrary(m_module);
    }
}

int SqlCipherLibrary::open(const char *filename, sqlite3 **db, int flags) const
{
    return m_fns->open(filename, db, flags, nullptr);
}
int SqlCipherLibrary::close(sqlite3 *db) const
{
    return m_fns->close(db);
}
int SqlCipherLibrary::exec(sqlite3 *db, const char *sql) const
{
    return m_fns->exec(db, sql, nullptr, nullptr, nullptr);
}
int SqlCipherLibrary::prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) const
{
    return m_fns->prepare(db, sql, -1, stmt, nullptr);
}
int SqlCipherLibrary::step(sqlite3_stmt *stmt) const
{
    return m_fns->step(stmt);
}
int SqlCipherLibrary::finalize(sqlite3_stmt *stmt) const
{
    return m_fns->finalize(stmt);
}
int SqlCipherLibrary::bindInt64(sqlite3_stmt *stmt, int index, int64_t value) const
{
    return m_fns->bindInt64(stmt, index, value);
}
int SqlCipherLibrary::bindText(sqlite3_stmt *stmt, int index, const std::string &value) const
{
    return m_fns->bindText(stmt, index, value.c_str(), static_cast<int>(value.size()),
                            reinterpret_cast<void (*)(void *)>(SqliteTransient));
}
int SqlCipherLibrary::bindNull(sqlite3_stmt *stmt, int index) const
{
    return m_fns->bindNull(stmt, index);
}
int64_t SqlCipherLibrary::columnInt64(sqlite3_stmt *stmt, int index) const
{
    return m_fns->columnInt64(stmt, index);
}
std::string SqlCipherLibrary::columnText(sqlite3_stmt *stmt, int index) const
{
    const unsigned char *text = m_fns->columnText(stmt, index);
    return text ? reinterpret_cast<const char *>(text) : "";
}
int SqlCipherLibrary::columnType(sqlite3_stmt *stmt, int index) const
{
    return m_fns->columnType(stmt, index);
}
std::string SqlCipherLibrary::errmsg(sqlite3 *db) const
{
    const char *msg = m_fns->errmsg(db);
    return msg ? msg : "unknown error";
}

SqlCipherDb::SqlCipherDb(const SqlCipherLibrary &lib, const std::string &path, bool readOnly) : m_lib(lib)
{
    // Counted here rather than at each call site: the expensive part is
    // the key derivation every open pays, and this is the one place every
    // open goes through.
    WorkCounters::instance().noteEncryptedDatabaseOpen();
    int flags = readOnly ? 0x00000001 /* SQLITE_OPEN_READONLY */ : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (lib.open(path.c_str(), &m_db, flags) != SQLITE_OK) {
        std::string message = m_db ? lib.errmsg(m_db) : "failed to open database";
        if (m_db) {
            lib.close(m_db);
        }
        throw std::runtime_error("onelibrary: failed to open " + path + ": " + message);
    }
}

SqlCipherDb::~SqlCipherDb()
{
    if (m_db) {
        m_lib.close(m_db);
    }
}

void SqlCipherDb::exec(const std::string &sql) const
{
    if (m_lib.exec(m_db, sql.c_str()) != SQLITE_OK) {
        throw std::runtime_error("onelibrary: statement failed: " + m_lib.errmsg(m_db));
    }
}

SqlCipherStatement::SqlCipherStatement(const SqlCipherDb &db, const std::string &sql) : m_db(db)
{
    if (db.lib().prepare(db.handle(), sql.c_str(), &m_stmt) != SQLITE_OK) {
        throw std::runtime_error("onelibrary: failed to prepare statement: " + db.lib().errmsg(db.handle()));
    }
}

SqlCipherStatement::~SqlCipherStatement()
{
    m_db.lib().finalize(m_stmt);
}

void SqlCipherStatement::bindInt64(int index, int64_t value)
{
    m_db.lib().bindInt64(m_stmt, index, value);
}
void SqlCipherStatement::bindText(int index, const std::string &value)
{
    m_db.lib().bindText(m_stmt, index, value);
}
void SqlCipherStatement::bindNull(int index)
{
    m_db.lib().bindNull(m_stmt, index);
}

void SqlCipherStatement::run()
{
    int rc = m_db.lib().step(m_stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error("onelibrary: statement failed: " + m_db.lib().errmsg(m_db.handle()));
    }
}

bool SqlCipherStatement::step()
{
    int rc = m_db.lib().step(m_stmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw std::runtime_error("onelibrary: statement failed: " + m_db.lib().errmsg(m_db.handle()));
}

int64_t SqlCipherStatement::columnInt64(int index)
{
    return m_db.lib().columnInt64(m_stmt, index);
}
std::string SqlCipherStatement::columnText(int index)
{
    return m_db.lib().columnText(m_stmt, index);
}
bool SqlCipherStatement::columnIsNull(int index)
{
    return m_db.lib().columnType(m_stmt, index) == SQLITE_NULL;
}

}  // namespace seabass::infrastructure::onelibrary
