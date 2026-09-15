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
#include <string>
#include <string_view>

namespace sss {

enum class Rc {
    Ok = 0,
    CliError = 1,
    IoError = 2,
    FormatError = 3,
    DataError = 4,
    NotImplemented = 5,
};

extern int g_verbose;

void logv(int level, const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

int set_binary_io();

bool parse_size(std::string_view text, uint64_t& out);
bool safe_write(FILE* f, const void* data, size_t len);
bool safe_read(FILE* f, void* data, size_t len);
bool file_seek(FILE* f, int64_t offset, int origin);

} // namespace sss