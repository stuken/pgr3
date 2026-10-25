// pgr3 - ReXGlue Recompiled Project
//
// PGR3_* logging macros bound to the pgr3 log category. Ported from reblue's
// core/logging.h (Copyright (c) 2026 Tom Clay, BSD 3-Clause).

#pragma once

#include <atomic>

#include <rex/logging.h>

#include "core/settings.h"

REXLOG_DEFINE_CATEGORY(pgr3)

#define PGR3_TRACE(...) REXLOG_CAT_TRACE(::rex::log::pgr3(), __VA_ARGS__)
#define PGR3_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::pgr3(), __VA_ARGS__)
#define PGR3_INFO(...) REXLOG_CAT_INFO(::rex::log::pgr3(), __VA_ARGS__)
#define PGR3_WARN(...) REXLOG_CAT_WARN(::rex::log::pgr3(), __VA_ARGS__)
#define PGR3_ERROR(...) REXLOG_CAT_ERROR(::rex::log::pgr3(), __VA_ARGS__)
#define PGR3_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::pgr3(), __VA_ARGS__)

#define PGR3_DEV_WARN(...)                                                      \
  do {                                                                         \
    if (::pgr3::Settings::Get().Devmode())                                      \
      PGR3_WARN(__VA_ARGS__);                                                   \
  } while (0)

#define PGR3_DEV_INFO(...)                                                      \
  do {                                                                         \
    if (::pgr3::Settings::Get().Devmode())                                      \
      PGR3_INFO(__VA_ARGS__);                                                   \
  } while (0)

// Warns on the first `limit` hits of one call site. Marks paths the renderer
// doesn't model yet, so a run shows which ones the game reaches.
#define PGR3_WARN_FIRST(limit, ...)                                             \
  do {                                                                         \
    static std::atomic<unsigned> s_pgr3_warned{0};                              \
    if (s_pgr3_warned.fetch_add(1, std::memory_order_relaxed) < (limit))        \
      PGR3_WARN(__VA_ARGS__);                                                   \
  } while (0)

// The same at the debug level, for paths that are modelled but worth seeing
// while debugging.
#define PGR3_DEBUG_FIRST(limit, ...)                                            \
  do {                                                                         \
    static std::atomic<unsigned> s_pgr3_noted{0};                               \
    if (s_pgr3_noted.fetch_add(1, std::memory_order_relaxed) < (limit))         \
      PGR3_DEBUG(__VA_ARGS__);                                                  \
  } while (0)
