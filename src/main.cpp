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
#include "cli.h"
#include "decoder.h"
#include "encoder.h"
#include "sss_io.h"
#include "version.h"

#include <cstdio>

namespace sss {

int run(int argc, char** argv) {
    if (set_binary_io() != 0) {
        log_error("failed to configure binary stdio");
        return static_cast<int>(Rc::IoError);
    }

    const ParseResult pr = parse_args(argc, argv);

    if (pr.mode != Mode::Version) {
        const int vb = (pr.mode == Mode::Encode || pr.mode == Mode::Decode)
                           ? ((pr.opts.input == "-" || pr.opts.output == "-") ? 0 : pr.opts.verbose)
                           : pr.opts.verbose;
        if (vb > 0) print_banner(stdout);
    }

    switch (pr.mode) {
        case Mode::Help:
            print_short_help(stdout);
            return static_cast<int>(Rc::Ok);
        case Mode::LongHelp:
            print_long_help(stdout);
            return static_cast<int>(Rc::Ok);
        case Mode::Version:
            std::printf("SmartStreamSlicer (sss) v%s\n", SSS_VERSION_STRING);
            return static_cast<int>(Rc::Ok);
        case Mode::Error:
            log_error(pr.error);
            std::fputs("Run 'sss -H' for full usage.\n", stderr);
            return static_cast<int>(Rc::CliError);
        case Mode::Encode:
        case Mode::Decode:
            break;
    }

    const Options& o = pr.opts;
    g_verbose = (o.input == "-" || o.output == "-") ? 0 : o.verbose;

    const Rc rc = pr.mode == Mode::Encode ? encode(o) : decode(o);
    if (rc != Rc::Ok) {
        log_error("operation failed");
        return static_cast<int>(rc);
    }
    return static_cast<int>(Rc::Ok);
}

} // namespace sss

int main(int argc, char** argv) {
    return sss::run(argc, argv);
}