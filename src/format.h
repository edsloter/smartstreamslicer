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

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "version.h"

namespace sss {

inline constexpr std::array<uint8_t, 4> kMagic = {0x43, 0x53, 0x54, 0x52};

struct StreamHeader {
    std::array<uint8_t, 4> magic = kMagic;
    uint32_t format_version = ver::current_packed();
    uint32_t total_chunks = 0;
    uint64_t total_stream_size = 0;
    uint64_t chunk_target = 0;
    uint32_t num_chunks_requested = 0;
    uint64_t file_count = 0;
};

struct ChunkHeader {
    std::array<uint8_t, 4> magic = kMagic;
    uint32_t format_version = ver::current_packed();
    uint32_t chunk_index = 0;
    uint64_t chunk_size = 0;
    uint32_t entry_count = 0;
};

struct ChunkFooter {
    std::array<uint8_t, 4> magic = kMagic;
    uint64_t stream_xxh3 = 0;
    uint64_t chunk_size = 0;
};

struct Segment {
    uint32_t chunk_index = 0;
    uint64_t offset_in_chunk = 0;
    uint64_t length = 0;
};

struct FileEntry {
    std::string name;
    uint64_t size = 0;
    uint64_t payload_xxh3 = 0;
    std::vector<Segment> segments;
};

// Fixed byte sizes of the on-disk structures (no VLA/varints).
inline constexpr uint32_t kStreamHeaderLen = 4 + 4 + 4 + 8 + 8 + 4 + 8;
inline constexpr uint32_t kChunkHeaderLen = 4 + 4 + 4 + 8 + 4;
inline constexpr uint32_t kChunkFooterLen = 4 + 8 + 8;

inline uint64_t file_entry_len(const FileEntry& e) {
    return 2 + e.name.size() + 8 + 8 + 4 + e.segments.size() * 20;
}

bool encode_stream_header(const StreamHeader& h, std::vector<uint8_t>& out);
bool encode_chunk_header(const ChunkHeader& h, std::vector<uint8_t>& out);
bool encode_chunk_footer(const ChunkFooter& f, std::vector<uint8_t>& out);
bool encode_file_entry(const FileEntry& e, std::vector<uint8_t>& out);

bool decode_stream_header(std::string_view data, StreamHeader& out);
bool decode_chunk_header(std::string_view data, ChunkHeader& out);
bool decode_chunk_footer(std::string_view data, ChunkFooter& out);
bool decode_file_entry(std::string_view data, FileEntry& out);

} // namespace sss