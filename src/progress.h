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
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace sss {

// True when a smooth single-line progress bar makes sense: the destination is
// a live terminal, or SSS_FORCE_PROGRESS is set (any value). Callers gate this
// on g_verbose == 1 so -v 0 stays fully silent and -v 2 keeps per-file detail
// without a re-drawing frame.
bool want_progress(FILE* out);

// Render-on-stderr progress bar with % done, EMA-smoothed MiB/s, and ETA.
// The bar carries a phase label ("Defining Content", "Hashing", "Splitting",
// "Restoring", ...) so each pass identifies itself; while no bytes have
// arrived yet the label gets an ellipsis suffix, once bytes flow MiB/s and
// ETA join the % and label. Callers switch passes with next_phase(new_total,
// label), which resets the counters for the new pass. Safe to poke from
// multiple writer threads: add() accumulates atomically and only the thread
// that wins the redraw mutex samples/redraws (throttled to the display
// rate). Emits nothing at all when constructed disabled.
class Progress {
public:
    Progress(FILE* out, uint64_t total_bytes, const char* phase, bool enabled);
    ~Progress();

    void add(uint64_t delta_bytes);
    void poll();
    void next_phase(uint64_t new_total, const char* phase);
    void finish();

private:
    void draw();
    void render();

    FILE* out_ = nullptr;
    uint64_t total_ = 0;
    bool enabled_ = false;
    bool finished_ = false;
    std::string phase_;

    std::atomic<uint64_t> done_{0};
    std::mutex mu_;

    long long start_ns_ = 0;
    long long last_ns_ = 0;
    uint64_t last_done_ = 0;
    size_t last_bytes_ = 0;
    double ema_bps_ = 0.0;
    bool have_ema_ = false;
};

} // namespace sss