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
#include <string>

#define SSS_VERSION_MAJOR 0
#define SSS_VERSION_MINOR 0
#define SSS_VERSION_PATCH 1
#define SSS_VERSION_STRING "0.0.1"

namespace sss {

inline constexpr const char* kDownloadUrl =
    "https://github.com/edsloter/smartstreamslicer";

namespace ver {

// Pack MAJOR.MINOR.PATCH into a uint32 so the stream header can carry the
// encoding app's SemVer directly (each component up to 999).
constexpr uint32_t kMaxPacked = 999999999u;

inline constexpr uint32_t pack(uint32_t major, uint32_t minor, uint32_t patch) {
    return major * 1000000u + minor * 1000u + patch;
}

inline constexpr uint32_t current_packed() {
    return pack(SSS_VERSION_MAJOR, SSS_VERSION_MINOR, SSS_VERSION_PATCH);
}

inline bool valid(uint32_t p) { return p >= 1u && p <= kMaxPacked; }

inline std::string to_string(uint32_t p) {
    return "v" + std::to_string(p / 1000000u) + "." +
           std::to_string((p / 1000u) % 1000u) + "." + std::to_string(p % 1000u);
}

} // namespace ver

} // namespace sss