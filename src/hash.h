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

struct XXH3_state_s;

namespace sss {

uint64_t xxh3_64(const void* data, size_t len);

class Xxh3Stream {
public:
    Xxh3Stream();
    ~Xxh3Stream();
    Xxh3Stream(const Xxh3Stream&) = delete;
    Xxh3Stream& operator=(const Xxh3Stream&) = delete;

    void update(const void* data, size_t len);
    uint64_t digest();

private:
    XXH3_state_s* state_ = nullptr;
};

} // namespace sss