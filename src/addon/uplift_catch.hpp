#pragma once

#include <atomic>

#include "nr/log.hpp"

// I1: no exception may ever escape into ReShade or the game. Every event handler and the overlay
// callback wraps its whole body in `try { ... }` immediately followed by this macro, which logs an
// ERR at most once per handler (the static is one instance per expansion site) and returns the
// event's safe default: nothing for a void handler, `false` for reshade_open_overlay. The `try` opens
// before any lock is taken, so unwinding already ran the lock's destructor; stay lock-free here.
#define UPLIFT_CATCH(handler_name, default_return)                                                          \
  catch (...) {                                                                                             \
    static std::atomic<bool> logged = false;                                                                \
    if (!logged.exchange(true)) {                                                                           \
      uplift::nr::Log(uplift::nr::LogLevel::ERR,                                                            \
                      handler_name " threw an exception; the event was ignored so the game keeps running"); \
    }                                                                                                       \
    return default_return;                                                                                  \
  }
