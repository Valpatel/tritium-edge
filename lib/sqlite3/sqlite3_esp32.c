// SQLite3 amalgamation wrapper for ESP32.
// NDEBUG must be the VERY FIRST thing defined to disable assert()/TESTONLY().
#define NDEBUG 1

#include "sqlite3_config.h"

// ESP-IDF's assert.h evaluates the expression even with NDEBUG when
// CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE is set. SQLite's debug-only
// variables (declared inside assert/TESTONLY blocks) won't exist in
// release builds, causing undeclared identifier errors. Force a true
// no-op assert before including the amalgamation.
#include <assert.h>
#undef assert
#define assert(x) ((void)0)

// Also neutralize TESTONLY — sqlite3 uses it for debug-only statements.
// With NDEBUG it should already be empty, but be explicit.
#undef TESTONLY
#define TESTONLY(x)

// Suppress all warnings from the 250K-line amalgamation
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wint-conversion"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#pragma GCC diagnostic ignored "-Wpointer-sign"

#include "sqlite3.c"

#pragma GCC diagnostic pop
