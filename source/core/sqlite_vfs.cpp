// SQLite's file layer for the Switch (the library is built with
// SQLITE_OS_OTHER, so the app supplies one). SQLite's own Unix layer wants file
// locks and mmap, which the Switch's SD card driver doesn't have. This one uses
// plain file calls and no locking: the app has one connection, to library.db,
// which it only reads.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

#include "app.h"
#include "sqlite3.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef __SWITCH__
#include <windows.h>
#define fsync _commit
static void randomGet(void* buf, size_t n) { for (size_t i = 0; i < n; i++) ((unsigned char*)buf)[i] = (unsigned char)rand(); }
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

// failures go to the log with the file and the reason, so they can be traced
#define VFS_FAIL(code, what, path) (appLog("sqlite: %s failed on %s: %s", what, path, strerror(errno)), (code))

namespace {

struct SwFile {
    sqlite3_file base;
    int fd;
    const char* name;  // SQLite keeps it valid until xClose
    char* deleteOnClose;
};

int swClose(sqlite3_file* f) {
    SwFile* p = (SwFile*)f;
    close(p->fd);
    if (p->deleteOnClose) {
        unlink(p->deleteOnClose);
        sqlite3_free(p->deleteOnClose);
    }
    return SQLITE_OK;
}

static int64_t sizeOf(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 ? (int64_t)st.st_size : -1;
}

// The Switch's file system may refuse a read that starts past the end of a
// file, or a write that leaves a gap after it (both fine on POSIX), so reads
// are clamped to the file and gaps are filled before writing.
int swRead(sqlite3_file* f, void* buf, int amt, sqlite3_int64 off) {
    SwFile* p = (SwFile*)f;
    int64_t size = sizeOf(p->fd);
    if (size < 0) return VFS_FAIL(SQLITE_IOERR_READ, "fstat", p->name);
    int want = off >= size ? 0 : (int)std::min<int64_t>(amt, size - off);
    if (want > 0 && lseek(p->fd, (off_t)off, SEEK_SET) != (off_t)off) return VFS_FAIL(SQLITE_IOERR_READ, "seek", p->name);
    int got = 0;
    while (got < want) {
        ssize_t n = read(p->fd, (char*)buf + got, want - got);
        if (n < 0) return VFS_FAIL(SQLITE_IOERR_READ, "read", p->name);
        if (n == 0) break;
        got += (int)n;
    }
    if (got < amt) {
        memset((char*)buf + got, 0, amt - got);  // SQLite requires the rest zeroed
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}

int swWrite(sqlite3_file* f, const void* buf, int amt, sqlite3_int64 off) {
    SwFile* p = (SwFile*)f;
    int64_t size = sizeOf(p->fd);
    if (size < 0) return VFS_FAIL(SQLITE_IOERR_WRITE, "fstat", p->name);
    if (off > size && ftruncate(p->fd, (off_t)off) != 0) return VFS_FAIL(SQLITE_IOERR_WRITE, "extend", p->name);
    if (lseek(p->fd, (off_t)off, SEEK_SET) != (off_t)off) return VFS_FAIL(SQLITE_IOERR_WRITE, "seek", p->name);
    int done = 0;
    while (done < amt) {
        ssize_t n = write(p->fd, (const char*)buf + done, amt - done);
        if (n <= 0) return VFS_FAIL(SQLITE_IOERR_WRITE, "write", p->name);
        done += (int)n;
    }
    return SQLITE_OK;
}

int swTruncate(sqlite3_file* f, sqlite3_int64 size) {
    SwFile* p = (SwFile*)f;
    return ftruncate(p->fd, (off_t)size) == 0 ? SQLITE_OK : VFS_FAIL(SQLITE_IOERR_TRUNCATE, "truncate", p->name);
}
int swSync(sqlite3_file* f, int) {
    SwFile* p = (SwFile*)f;
    return fsync(p->fd) == 0 ? SQLITE_OK : VFS_FAIL(SQLITE_IOERR_FSYNC, "flush", p->name);
}
int swFileSize(sqlite3_file* f, sqlite3_int64* size) {
    SwFile* p = (SwFile*)f;
    struct stat st;
    if (fstat(p->fd, &st) != 0) return VFS_FAIL(SQLITE_IOERR_FSTAT, "fstat", p->name);
    *size = st.st_size;
    return SQLITE_OK;
}
int swLock(sqlite3_file*, int) { return SQLITE_OK; }
int swUnlock(sqlite3_file*, int) { return SQLITE_OK; }
int swCheckReservedLock(sqlite3_file*, int* out) { *out = 0; return SQLITE_OK; }
int swFileControl(sqlite3_file*, int, void*) { return SQLITE_NOTFOUND; }
int swSectorSize(sqlite3_file*) { return 512; }
int swDeviceCharacteristics(sqlite3_file*) { return 0; }

const sqlite3_io_methods kIo = {
    1, swClose, swRead, swWrite, swTruncate, swSync, swFileSize, swLock, swUnlock, swCheckReservedLock,
    swFileControl, swSectorSize, swDeviceCharacteristics,
};

int swOpen(sqlite3_vfs*, const char* name, sqlite3_file* f, int flags, int* outFlags) {
    SwFile* p = (SwFile*)f;
    p->base.pMethods = nullptr;
    if (!name) return VFS_FAIL(SQLITE_IOERR, "open", "a temporary file");  // they should live in memory (SQLITE_TEMP_STORE=3)
    int mode = ((flags & SQLITE_OPEN_READWRITE) ? O_RDWR : O_RDONLY) | O_BINARY;
    if (flags & SQLITE_OPEN_CREATE) mode |= O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) mode |= O_EXCL;
    p->fd = open(name, mode, 0644);
    if (p->fd < 0) return VFS_FAIL(SQLITE_CANTOPEN, "open", name);
    p->name = name;
    p->deleteOnClose = (flags & SQLITE_OPEN_DELETEONCLOSE) ? sqlite3_mprintf("%s", name) : nullptr;
    if (outFlags) *outFlags = flags;
    p->base.pMethods = &kIo;
    return SQLITE_OK;
}

int swDelete(sqlite3_vfs*, const char* name, int) {
    if (unlink(name) == 0) return SQLITE_OK;
    struct stat st;
    return stat(name, &st) != 0 ? SQLITE_IOERR_DELETE_NOENT : VFS_FAIL(SQLITE_IOERR_DELETE, "delete", name);
}

int swAccess(sqlite3_vfs*, const char* name, int, int* out) {
    struct stat st;
    *out = stat(name, &st) == 0 && (st.st_size > 0 || S_ISDIR(st.st_mode));
    return SQLITE_OK;
}

int swFullPathname(sqlite3_vfs*, const char* name, int n, char* out) {
    snprintf(out, n, "%s", name);
    return SQLITE_OK;
}

int swRandomness(sqlite3_vfs*, int n, char* out) {
    randomGet(out, (size_t)n);
    return n;
}

int swSleep(sqlite3_vfs*, int us) {
    svcSleepThread((unsigned long long)us * 1000ULL);
    return us;
}

int swCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64* t) {
    *t = (sqlite3_int64)time(nullptr) * 1000 + 210866760000000LL;  // Julian day in ms
    return SQLITE_OK;
}

int swCurrentTime(sqlite3_vfs* v, double* t) {
    sqlite3_int64 ms;
    swCurrentTimeInt64(v, &ms);
    *t = ms / 86400000.0;
    return SQLITE_OK;
}

int swGetLastError(sqlite3_vfs*, int, char*) { return 0; }

sqlite3_vfs kVfs = {
    2, sizeof(SwFile), 768, nullptr, "switch", nullptr,
    swOpen, swDelete, swAccess, swFullPathname,
    nullptr, nullptr, nullptr, nullptr,  // no extensions
    swRandomness, swSleep, swCurrentTime, swGetLastError, swCurrentTimeInt64,
};

}  // namespace

// With SQLITE_OS_OTHER=1 the app supplies these.
extern "C" int sqlite3_os_init(void) { return sqlite3_vfs_register(&kVfs, 1); }
extern "C" int sqlite3_os_end(void) { return SQLITE_OK; }
