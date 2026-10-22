// Host-test stand-in for libarchive: "archives" are directories.
#pragma once
#include <cstddef>
typedef long la_ssize_t;
struct archive;
struct archive_entry;
#define ARCHIVE_OK 0
#define ARCHIVE_EOF 1
#define AE_IFREG 0100000
archive* archive_read_new();
int archive_read_support_format_zip(archive*);
int archive_read_support_format_all(archive*);
int archive_read_support_filter_all(archive*);
int archive_read_open_filename(archive*, const char* path, size_t);
int archive_read_next_header(archive*, archive_entry**);
la_ssize_t archive_read_data(archive*, void* buf, size_t n);
int archive_read_data_skip(archive*);
int archive_read_free(archive*);
