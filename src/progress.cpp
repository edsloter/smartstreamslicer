/*
 * SmartStreamSlicer (sss) - smart streaming splitter/joiner
 * Copyright (C) 2026 Edward Sloter
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "progress.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sss {

namespace {

using Clock = std::chrono::steady_clock;

long long now_ns() { return Clock::now().time_since_epoch().count(); }

std::string human_size(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f TiB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f GiB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024));
    } else if (bytes >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

std::string human_bps(double bytes_per_sec) {
    char buf[64];
    if (bytes_per_sec >= 1024.0 * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f GiB/s", bytes_per_sec / (1024.0 * 1024 * 1024));
    } else if (bytes_per_sec >= 1024.0 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MiB/s", bytes_per_sec / (1024.0 * 1024));
    } else if (bytes_per_sec >= 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f KiB/s", bytes_per_sec / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f B/s", bytes_per_sec);
    }
    return buf;
}

std::string hms(long long seconds) {
    if (seconds < 0) seconds = 0;
    const long long h = seconds / 3600;
    const long long m = (seconds % 3600) / 60;
    const long long s = seconds % 60;
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    else
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
    return buf;
}

constexpr const char* kCyan = "\033[36m";   // cyan (percent, speed, preparing)
constexpr const char* kYellow = "\033[33m";  // yellow (ETA)
constexpr const char* kGreen = "\033[32m";   // green (bar fill)
constexpr const char* kReset = "\033[0m";

constexpr int kBarWidth = 24;
constexpr double kRedrawSec = 0.1;

std::string bar_line(int fill) {
    fill = std::clamp(fill, 0, kBarWidth);
    std::string b;
    b.reserve(static_cast<size_t>(kBarWidth) * 3);
    for (int i = 0; i < kBarWidth; ++i)
        b += (i < fill) ? "\xe2\x96\x88" : "\xe2\x96\x91";
    return b;
}

} // namespace

bool want_progress(FILE* out) {
    if (std::getenv("SSS_FORCE_PROGRESS")) return true;
    const int fd = fileno(out);
    return fd >= 0 && isatty(fd) != 0;
}

Progress::Progress(FILE* out, uint64_t total_bytes, const char* phase, bool enabled)
    : out_(out), total_(total_bytes), enabled_(enabled) {
    phase_.assign(phase ? phase : "");
    const long long t = now_ns();
    start_ns_ = t;
    last_ns_ = 0;  // never throttle away the opening phase frame
    if (enabled_) {
#ifdef _WIN32
        // UTF-8 bar glyphs (U+2588/U+2591) land in the console's OEM codepage
        // by default; ask the attached console to interpret UTF-8 instead.
        if (isatty(fileno(out_)) != 0) SetConsoleOutputCP(CP_UTF8);
#endif
        draw();
    }
}

Progress::~Progress() {
    if (enabled_ && !finished_) finish();
}

void Progress::add(uint64_t delta_bytes) {
    if (!enabled_) return;
    done_.fetch_add(delta_bytes, std::memory_order_relaxed);
    draw();
}

void Progress::poll() { draw(); }

void Progress::next_phase(uint64_t new_total, const char* phase) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> g(mu_);
    phase_.assign(phase ? phase : "");
    total_ = new_total;
    done_.store(0, std::memory_order_relaxed);
    last_done_ = 0;
    have_ema_ = false;
    render();
}

void Progress::finish() {
    if (!enabled_ || finished_) return;
    finished_ = true;
    draw();          // force the final frame
    std::fputc('\n', out_);
    std::fflush(out_);
}

void Progress::draw() {
    std::lock_guard<std::mutex> g(mu_);
    render();
}

void Progress::render() {
    const long long t = now_ns();
    if (!finished_ && (t - last_ns_) < static_cast<long long>(kRedrawSec * 1e9)) return;

    const uint64_t d = done_.load(std::memory_order_relaxed);
    const double dt = static_cast<double>(t - last_ns_) / 1e9;
    if (dt > 0.0) {
        const double inst = static_cast<double>(d - last_done_) / dt;
        ema_bps_ = have_ema_ ? 0.7 * ema_bps_ + 0.3 * inst : inst;
        have_ema_ = true;
    }
    last_ns_ = t;
    last_done_ = d;

    double frac = total_ > 0 ? static_cast<double>(d) / static_cast<double>(total_) : 1.0;
    frac = std::clamp(frac, 0.0, 1.0);
    if (total_ == 0 && !finished_) frac = 0.0;
    const int fill = static_cast<int>(frac * kBarWidth + 0.999999);
    const long long elapsed_s = (t - start_ns_) / 1000000000LL;

    char pct[16];
    std::snprintf(pct, sizeof(pct), "%5.1f%%", frac * 100.0);

    std::string line;
    line.reserve(160);
    line += "\r";
    line += kGreen;
    line += bar_line(fill);
    line += kReset;
    line += " ";
    line += kCyan;
    line += pct;
    line += kReset;

    const bool idle = d == 0;
    if (finished_ && frac >= 1.0) {
        line += " \033[32mdone\033[0m";
    } else if (idle) {
        line += " ";
        line += kCyan;
        line += phase_;
        line += "\xe2\x80\xa6";
        line += kReset;
    } else {
        line += " ";
        line += kCyan;
        line += phase_;
        line += kReset;
        line += " ";
        line += kCyan;
        line += human_bps(ema_bps_);
        line += kReset;

        if (frac < 1.0 && ema_bps_ > 0.0) {
            const long long eta_s =
                static_cast<long long>((static_cast<double>(total_ - d)) / ema_bps_);
            line += " ";
            line += kYellow;
            line += "ETA ";
            line += hms(eta_s);
            line += kReset;
        }
    }

    char suf[64];
    std::snprintf(suf, sizeof(suf), " (%s / %s, %s)",
                  human_size(d).c_str(), human_size(total_).c_str(), hms(elapsed_s).c_str());
    line += suf;

    // A `\r`-only redraw leaves the trailing fragments of any longer previous
    // frame on screen. Pad the new frame with spaces up to the previous frame's
    // emitted length (>= its cell width, so this always clears fully). Track the
    // emitted width, not the natural one, so a short final frame still clears a
    // longer predecessor.
    const size_t content_len = line.size() - 1;  // exclude the leading \r
    const size_t emit_len = last_bytes_ > content_len ? last_bytes_ : content_len;
    if (emit_len > content_len) {
        line.append(emit_len - content_len, ' ');
    }
    last_bytes_ = emit_len;

    std::fputs(line.c_str(), out_);
    std::fflush(out_);
}

} // namespace sss