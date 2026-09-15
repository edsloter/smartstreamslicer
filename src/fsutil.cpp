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
#include "fsutil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace sss {
namespace fs = std::filesystem;

std::vector<std::string> read_file_list(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        out.push_back(line);
    }
    return out;
}

namespace {

std::string lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool is_dot_path(const fs::path& p) {
    for (const auto& comp : p) {
        const std::string s = comp.string();
        if (!s.empty() && s.front() == '.') return true;
    }
    return false;
}

bool is_ignored(const fs::path& p, const ScanOptions& opts) {
    if (opts.ignore_dot && is_dot_path(p)) return true;
    if (!opts.ignore_ext.empty()) {
        std::string ext = lower(p.extension().string());
        if (!ext.empty() && ext.front() == '.') {
            ext.erase(0, 1);
            if (std::find(opts.ignore_ext.begin(), opts.ignore_ext.end(), ext) !=
                opts.ignore_ext.end()) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

ScanResult scan_inputs(const std::vector<std::string>& roots, const ScanOptions& opts) {
    ScanResult r;

    for (const auto& root_str : roots) {
        if (root_str == "-") {
            r.error = "stdin can only be used as -i in streaming mode, not as a scanned root";
            r.ok = false;
            return r;
        }

        fs::path root(root_str);
        std::error_code ec;
        if (!fs::exists(root, ec) || ec) {
            r.error = "input does not exist: " + root_str;
            r.ok = false;
            return r;
        }
        const bool is_dir = fs::is_directory(root, ec);
        if (ec) {
            r.error = "cannot stat input: " + root_str;
            r.ok = false;
            return r;
        }

        if (!is_dir) {
            if (is_ignored(root, opts)) continue;
            PathEntry e;
            e.abs_path = fs::absolute(root);
            e.rel_name = root.filename().string();
            e.size = static_cast<uint64_t>(fs::file_size(root, ec));
            if (ec) {
                r.error = "cannot stat file: " + root_str;
                r.ok = false;
                return r;
            }
            r.entries.push_back(std::move(e));
            continue;
        }

        const fs::path base = fs::absolute(root);

        auto walk = [&](const fs::path& p) {
            if (is_ignored(p, opts)) return;
            std::error_code ec2;
            if (!fs::is_regular_file(p, ec2) || ec2) return;
            PathEntry e;
            e.abs_path = p;
            fs::path rel = fs::relative(p, base, ec2);
            if (ec2) return;
            e.rel_name = rel.generic_string();
            e.size = static_cast<uint64_t>(fs::file_size(p, ec2));
            if (ec2) return;
            r.entries.push_back(std::move(e));
        };

        if (opts.recursive) {
            fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            for (; it != end; it.increment(ec)) {
                if (ec) break;
                walk(it->path());
            }
        } else {
            fs::directory_iterator it(base, ec);
            const fs::directory_iterator end;
            for (; it != end; it.increment(ec)) {
                if (ec) break;
                walk(it->path());
            }
        }
    }

    std::sort(r.entries.begin(), r.entries.end(),
              [](const PathEntry& a, const PathEntry& b) { return a.rel_name < b.rel_name; });

    for (const auto& e : r.entries) r.total_size += e.size;
    return r;
}

uint32_t part_width(uint64_t total_parts) {
    uint32_t digits = 3;
    uint64_t n = 1000;
    while (total_parts >= n) {
        digits++;
        if (n > UINT64_MAX / 10) break;
        n *= 10;
    }
    return digits;
}

std::string make_part_name(const std::string& base, uint64_t index, uint64_t total_parts) {
    const uint32_t w = part_width(total_parts);
    std::ostringstream os;
    os << base << ".sss" << std::setw(static_cast<int>(w)) << std::setfill('0') << (index + 1);
    return os.str();
}

bool looks_like_part_name(const std::string& name, std::string& base_out) {
    const std::string suffix = ".sss";
    const size_t pos = name.rfind(suffix);
    if (pos == std::string::npos) return false;
    const std::string digits = name.substr(pos + suffix.size());
    if (digits.empty() || digits.size() < 2) return false;
    if (digits.find_first_not_of("0123456789") != std::string::npos) return false;
    base_out = name.substr(0, pos);
    return true;
}

} // namespace sss