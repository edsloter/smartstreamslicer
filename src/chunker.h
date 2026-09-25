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

#include <cstdint>
#include <cstdio>
#include <vector>

namespace sss {

struct Options;

struct SplitEnv {
    uint64_t target = 0;
    uint64_t min_sz = 0;
    uint64_t max_sz = 0;
    uint64_t max_mem = 0;  // --max-mem RAM budget (0 = unset)
    uint64_t feed_sz = 0;  // selected scan feed block (0 = default)
};

SplitEnv make_split_env(const Options& o);

// Default read/feed block for the content-detection scan. Larger blocks
// amortize the FastCDC chunker's internal buffering (see chunker.cpp); the
// --max-mem flag enlarges the block inside a RAM budget without ever changing
// chunk sizing, so output stays byte-identical with or without it.
inline constexpr uint64_t kChunkFeedDefault = 64ULL << 20;

// Picks the scan feed block for the given environment. Honors env.max_mem as
// a working-set budget of roughly (env.max_sz + 2 * feed). Only ever enlarges
// the feed relative to kChunkFeedDefault; when the budget cannot accommodate
// even the default block, `warning` is set (the block is kept at the default
// because a smaller one only slows the scan without shrinking the chunker's
// own ~max_sz buffer).
uint64_t pick_feed_size(const SplitEnv& env, bool& warning);

// Streams `size` bytes from `f` (rewound to offset 0) and computes content-
// defined sub-chunk boundaries with the FastCDC chunker, refined by two
// deterministic rules backed by boundary analysis:
//   - a cut never splits a run of identical bytes (it is pushed past the run)
//   - a cut snaps onto the nearest record/line delimiter (NUL/LF/CR) within
//     a small look-around window when both neighbours stay non-trivial
// On success `out` receives the sub-chunk lengths (always summing to `size`)
// and true is returned. When `size` <= env.max_sz the file is not split, `out`
// is left empty and true is returned (caller keeps a single segment). If the
// input cannot be read, false is returned. If content chunking is unusable for
// the given environment, `out` is left empty and true is returned so the
// caller falls back to fixed-size slicing (identical output shape guarantees).
// When `prog` is non-null, bytes read by the main scanning pass are reported
// to it so the bar tracks the content-detection work.
bool content_boundaries(FILE* f, uint64_t size, const SplitEnv& env,
                        const class BoundaryMatcher& matcher, std::vector<uint64_t>& out,
                        class Progress* prog = nullptr);

} // namespace sss