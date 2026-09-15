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
#include <filesystem>
#include <string>
#include <vector>

namespace sss {

struct ScanOptions {
    bool recursive = true;
    bool ignore_dot = false;
    std::vector<std::string> ignore_ext;
};

struct PathEntry {
    std::filesystem::path abs_path;
    std::string rel_name;
    uint64_t size = 0;
};

struct ScanResult {
    std::vector<PathEntry> entries;
    uint64_t total_size = 0;
    std::string error;
    bool ok = true;
};

ScanResult scan_inputs(const std::vector<std::string>& roots, const ScanOptions& opts);
std::vector<std::string> read_file_list(const std::string& path);

uint32_t part_width(uint64_t total_parts);
std::string make_part_name(const std::string& base, uint64_t index, uint64_t total_parts);
bool looks_like_part_name(const std::string& name, std::string& base_out);

} // namespace sss