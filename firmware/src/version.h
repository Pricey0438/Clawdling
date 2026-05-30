#pragma once

// All three macros are injected at build time by firmware/scripts/fw_version.py.
// The #ifndef fallbacks make stray IDE / lint use of this header safe even
// when the script hasn't run.

#ifndef FW_VERSION_STR
#define FW_VERSION_STR "unknown"
#endif

#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif

#ifndef FW_BOARD_ENV
#define FW_BOARD_ENV "unknown"
#endif
