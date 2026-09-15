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

#include <cstddef>
#include <cstdint>

// Forward declarations of the libhs (Vectorscan) opaque types. The real
// definitions only need to be visible to matcher.cpp, which is compiled when
// sss is built with SSS_HAVE_VECTORSCAN.
typedef struct hs_database hs_database_t;
typedef struct hs_scratch hs_scratch_t;

namespace sss {

// Thin wrapper around the Vectorscan engine used to drive one "smart" rule
// during content-boundary refinement: snap a candidate cut onto the nearest
// record/line delimiter (NUL, LF, CR) within a small look-around window.
class BoundaryMatcher {
public:
    BoundaryMatcher() = default;
    ~BoundaryMatcher();
    BoundaryMatcher(const BoundaryMatcher&) = delete;
    BoundaryMatcher& operator=(const BoundaryMatcher&) = delete;

    // Compiles the delimiter database; false (and no state) if unavailable.
    bool compile();
    bool ok() const { return db_ != nullptr && scratch_ != nullptr; }

    // Within buf[0..len), finds the delimiter nearest to `near`. Returns
    // true and sets `best` when at least one delimiter exists.
    bool nearest_delimiter(const uint8_t* buf, size_t len, size_t near, size_t& best) const;

private:
    hs_database_t* db_ = nullptr;
    hs_scratch_t* scratch_ = nullptr;
};

} // namespace sss