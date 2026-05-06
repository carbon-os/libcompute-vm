#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>

namespace logger {

namespace detail {
    // C++17 inline variables ensure only one instance exists across all translation units.
    inline std::mutex  g_mtx;
    inline std::string g_pending;

    // Write [data, data+n) to fd, translating bare '\n' → "\r\n".
    inline void raw_emit(int fd, const char* data, std::size_t n) noexcept {
        const char* p   = data;
        const char* end = data + n;
        while (p < end) {
            const char* nl = static_cast<const char*>(
                std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
            if (!nl) {
                ::write(fd, p, static_cast<std::size_t>(end - p));
                return;
            }
            ::write(fd, p, static_cast<std::size_t>(nl - p));
            ::write(fd, "\r\n", 2);
            p = nl + 1;
        }
    }

    // Flush any buffered partial guest line to stdout.
    // Caller must hold g_mtx.
    inline void flush_pending() noexcept {
        if (g_pending.empty()) return;
        ::write(STDOUT_FILENO, g_pending.data(), g_pending.size());
        ::write(STDOUT_FILENO, "\r\n", 2);
        g_pending.clear();
    }

    inline void vlog(const char* prefix, const char* fmt, va_list ap) noexcept {
        va_list ap_copy;
        va_copy(ap_copy, ap);
        const int need = std::vsnprintf(nullptr, 0, fmt, ap);
        if (need <= 0) { va_end(ap_copy); return; }

        std::string buf(static_cast<std::size_t>(need), '\0');
        std::vsnprintf(buf.data(), static_cast<std::size_t>(need) + 1, fmt, ap_copy);
        va_end(ap_copy);

        std::lock_guard<std::mutex> lk(g_mtx);
        // Flush any partial guest line first so stderr never splits a guest line.
        flush_pending();
        
        if (prefix) {
            raw_emit(STDERR_FILENO, prefix, std::strlen(prefix));
        }
        raw_emit(STDERR_FILENO, buf.data(), buf.size());
    }
} // namespace detail

inline void init() noexcept {}

inline void shutdown() noexcept {
    std::lock_guard<std::mutex> lk(detail::g_mtx);
    detail::flush_pending();
    ::fsync(STDOUT_FILENO);
    ::fsync(STDERR_FILENO);
}

// ── Severity Logging APIs ─────────────────────────────────────────────────────

inline void Info(const char* fmt, ...) noexcept __attribute__((format(printf, 1, 2)));
inline void Info(const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    detail::vlog("[info] ", fmt, ap);
    va_end(ap);
}

inline void Warn(const char* fmt, ...) noexcept __attribute__((format(printf, 1, 2)));
inline void Warn(const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    detail::vlog("[warn] ", fmt, ap);
    va_end(ap);
}

inline void Error(const char* fmt, ...) noexcept __attribute__((format(printf, 1, 2)));
inline void Error(const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    detail::vlog("[error] ", fmt, ap);
    va_end(ap);
}

// ── Guest Console Output ──────────────────────────────────────────────────────

/// Guest console output → stdout (line-buffered, \n flushes atomically)
inline void write(const char* data, std::size_t len) noexcept {
    if (!data || !len) return;
    std::lock_guard<std::mutex> lk(detail::g_mtx);
    for (std::size_t i = 0; i < len; ++i) {
        const char c = data[i];
        if (c == '\r') continue;   // strip bare CR, we add our own on '\n'
        if (c == '\n') {
            // Emit the complete line atomically to stdout.
            ::write(STDOUT_FILENO, detail::g_pending.data(), detail::g_pending.size());
            ::write(STDOUT_FILENO, "\r\n", 2);
            detail::g_pending.clear();
        } else {
            detail::g_pending += c;
        }
    }
}

} // namespace logger