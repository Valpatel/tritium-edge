// SQLite3 configuration for ESP32 — minimal footprint, no threading, no mmap
#pragma once

// Disable assertions and test-only code
#ifndef NDEBUG
#define NDEBUG 1
#endif

#define SQLITE_OS_OTHER         1
#define SQLITE_THREADSAFE       0
#define SQLITE_TEMP_STORE       3
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_OMIT_LOAD_EXTENSION
#define SQLITE_OMIT_AUTHORIZATION
#define SQLITE_OMIT_DEPRECATED
#define SQLITE_OMIT_PROGRESS_CALLBACK
#define SQLITE_OMIT_TRACE
#define SQLITE_OMIT_COMPLETE
#define SQLITE_OMIT_DECLTYPE
#define SQLITE_OMIT_DESERIALIZE
#define SQLITE_DQS              0
#define SQLITE_DEFAULT_WAL_SYNCHRONOUS 1
#define SQLITE_MAX_EXPR_DEPTH   0
#define SQLITE_OMIT_WAL         1
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT 65536
#define SQLITE_DEFAULT_PAGE_SIZE 1024
#define SQLITE_DEFAULT_CACHE_SIZE -32
#define SQLITE_UNTESTABLE        1
#define SQLITE_NO_SYNC           1
#define SQLITE_OMIT_COMPILEOPTION_DIAGS
// Note: SQLITE_OMIT_VIRTUALTABLE and SQLITE_OMIT_ALTERTABLE cannot be used
// because the amalgamation's parser has grammar rules baked in that reference
// these functions. The OMIT flags strip the function bodies but leave the
// parser call sites, causing linker errors. Leave them compiled in — they
// add ~20KB but are never called at runtime.
