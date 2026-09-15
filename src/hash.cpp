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
#include "hash.h"

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

namespace sss {

uint64_t xxh3_64(const void* data, size_t len) {
    return static_cast<uint64_t>(XXH3_64bits(data, len));
}

Xxh3Stream::Xxh3Stream() : state_(XXH3_createState()) {
    if (state_ != nullptr) {
        XXH3_64bits_reset(state_);
    }
}

Xxh3Stream::~Xxh3Stream() {
    XXH3_freeState(state_);
}

void Xxh3Stream::update(const void* data, size_t len) {
    XXH3_64bits_update(state_, data, len);
}

uint64_t Xxh3Stream::digest() {
    return static_cast<uint64_t>(XXH3_64bits_digest(state_));
}

} // namespace sss