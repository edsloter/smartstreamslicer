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
#include "sss_io.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace sss {

int g_verbose = 1;

void logv(int level, const std::string& msg) {
    if (g_verbose >= level) {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

void log_warn(const std::string& msg) {
    std::fprintf(stderr, "warning: %s\n", msg.c_str());
}

void log_error(const std::string& msg) {
    std::fprintf(stderr, "error: %s\n", msg.c_str());
}

int set_binary_io() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return -1;
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return -1;
#endif
    return 0;
}

bool parse_size(std::string_view text, uint64_t& out) {
    if (text.empty()) return false;

    uint64_t num = 0;
    size_t i = 0;
    bool any_digit = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') break;
        any_digit = true;
        if (num > (UINT64_MAX - 9) / 10) return false;
        num = num * 10 + static_cast<uint64_t>(c - '0');
    }
    if (!any_digit) return false;

    const std::string suffix = std::string(text.substr(i));
    uint64_t mult = 1;
    if (suffix.empty()) {
        mult = 1;
    } else if (suffix == "k" || suffix == "K") {
        mult = 1024ULL;
    } else if (suffix == "m" || suffix == "M") {
        mult = 1024ULL * 1024;
    } else if (suffix == "g" || suffix == "G") {
        mult = 1024ULL * 1024 * 1024;
    } else if (suffix == "t" || suffix == "T") {
        mult = 1024ULL * 1024 * 1024 * 1024;
    } else {
        return false;
    }

    if (num > UINT64_MAX / mult) return false;
    out = num * mult;
    return true;
}

bool safe_write(FILE* f, const void* data, size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    while (len > 0) {
        const size_t n = std::fwrite(p, 1, len, f);
        if (n == 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool safe_read(FILE* f, void* data, size_t len) {
    auto* p = static_cast<unsigned char*>(data);
    while (len > 0) {
        const size_t n = std::fread(p, 1, len, f);
        if (n == 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool file_seek(FILE* f, int64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(offset), origin) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), origin) == 0;
#endif
}

} // namespace sss