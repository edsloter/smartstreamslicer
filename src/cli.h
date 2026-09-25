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
#include <set>
#include <string>

namespace sss {

enum class Mode { Help, LongHelp, Version, Encode, Decode, Error };

struct Options {
    bool force = false;
    bool keep = false;
    bool no_recursion = false;
    bool ignore_dot = false;
    bool dry_run = false;
    bool verify = false;

    int jobs = 0;
    int verbose = 1;

    uint64_t target = 0;
    uint64_t min_sz = 0;
    uint64_t max_sz = 0;
    uint64_t max_mem = 0;
    uint64_t num_chunks = 0;
    bool num_chunks_set = false;

    std::string input;
    std::string output;
    std::string file_list;
    std::set<std::string> ignore_ext;
};

struct ParseResult {
    Mode mode = Mode::Error;
    Options opts;
    std::string error;
};

ParseResult parse_args(int argc, char** argv);
void print_banner(FILE* f);
void print_short_help(FILE* f);
void print_long_help(FILE* f);

} // namespace sss