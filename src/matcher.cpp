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
#include "matcher.h"

#if defined(SSS_HAVE_VECTORSCAN)
#include <hs.h>
#endif

#include <cstdlib>

namespace sss {

#if defined(SSS_HAVE_VECTORSCAN)

namespace {

constexpr unsigned int kMode = HS_MODE_BLOCK;
constexpr unsigned int kFlags = HS_FLAG_SOM_LEFTMOST;

struct DelimCtx {
    size_t near_ = 0;
    size_t best_ = 0;
    bool any_ = false;
};

int on_delim(unsigned int, unsigned long long from, unsigned long long, unsigned int, void* ctxt) {
    DelimCtx* d = static_cast<DelimCtx*>(ctxt);
    const size_t f = static_cast<size_t>(from);
    auto dist = [](size_t a, size_t b) -> size_t { return a > b ? a - b : b - a; };
    if (!d->any_ || dist(f, d->near_) < dist(d->best_, d->near_)) {
        d->best_ = f;
        d->any_ = true;
    }
    return 0;
}

} // namespace

BoundaryMatcher::~BoundaryMatcher() {
    if (scratch_ != nullptr) hs_free_scratch(scratch_);
    if (db_ != nullptr) hs_free_database(db_);
}

bool BoundaryMatcher::compile() {
    hs_compile_error_t* err = nullptr;
    // Record/line delimiters: NUL (record separator), LF, CR.
    const char* expr = "[\\x00\\x0A\\x0D]";
    if (hs_compile(expr, kFlags, kMode, nullptr, &db_, &err) != HS_SUCCESS) {
        if (err != nullptr) hs_free_compile_error(err);
        db_ = nullptr;
        return false;
    }
    if (db_ == nullptr || hs_alloc_scratch(db_, &scratch_) != HS_SUCCESS) return false;
    return true;
}

bool BoundaryMatcher::nearest_delimiter(const uint8_t* buf, size_t len, size_t near, size_t& best) const {
    if (!ok() || buf == nullptr || len == 0) return false;
    DelimCtx ctx;
    ctx.near_ = near;
    if (hs_scan(db_, reinterpret_cast<const char*>(buf), len, 0, scratch_, on_delim, &ctx) !=
        HS_SUCCESS) {
        return false;
    }
    if (ctx.any_) {
        best = ctx.best_;
        return true;
    }
    return false;
}

#else // !SSS_HAVE_VECTORSCAN

BoundaryMatcher::~BoundaryMatcher() = default;

bool BoundaryMatcher::compile() { return false; }

bool BoundaryMatcher::nearest_delimiter(const uint8_t*, size_t, size_t, size_t&) const {
    return false;
}

#endif // SSS_HAVE_VECTORSCAN

} // namespace sss