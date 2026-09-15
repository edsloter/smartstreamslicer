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
#include "format.h"

namespace sss {

namespace {

void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }
}

void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }
}

void put_magic(std::vector<uint8_t>& out, const std::array<uint8_t, 4>& m) {
    out.insert(out.end(), m.begin(), m.end());
}

bool get_u16(std::string_view d, size_t& p, uint16_t& out) {
    if (p + 2 > d.size()) return false;
    out = static_cast<uint16_t>(static_cast<uint8_t>(d[p])) |
          static_cast<uint16_t>(static_cast<uint8_t>(d[p + 1]) << 8);
    p += 2;
    return true;
}

bool get_u32(std::string_view d, size_t& p, uint32_t& out) {
    if (p + 4 > d.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
        out |= static_cast<uint32_t>(static_cast<uint8_t>(d[p + i])) << (8 * i);
    }
    p += 4;
    return true;
}

bool get_u64(std::string_view d, size_t& p, uint64_t& out) {
    if (p + 8 > d.size()) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out |= static_cast<uint64_t>(static_cast<uint8_t>(d[p + i])) << (8 * i);
    }
    p += 8;
    return true;
}

bool get_magic(std::string_view d, size_t& p, std::array<uint8_t, 4>& out) {
    if (p + 4 > d.size()) return false;
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(d[p + i]);
    }
    p += 4;
    return true;
}

} // namespace

bool encode_stream_header(const StreamHeader& h, std::vector<uint8_t>& out) {
    out.clear();
    put_magic(out, h.magic);
    put_u32(out, h.format_version);
    put_u32(out, h.total_chunks);
    put_u64(out, h.total_stream_size);
    put_u64(out, h.chunk_target);
    put_u32(out, h.num_chunks_requested);
    put_u64(out, h.file_count);
    return true;
}

bool encode_chunk_header(const ChunkHeader& h, std::vector<uint8_t>& out) {
    out.clear();
    put_magic(out, h.magic);
    put_u32(out, h.format_version);
    put_u32(out, h.chunk_index);
    put_u64(out, h.chunk_size);
    put_u32(out, h.entry_count);
    return true;
}

bool encode_chunk_footer(const ChunkFooter& f, std::vector<uint8_t>& out) {
    out.clear();
    put_magic(out, f.magic);
    put_u64(out, f.stream_xxh3);
    put_u64(out, f.chunk_size);
    return true;
}

bool encode_file_entry(const FileEntry& e, std::vector<uint8_t>& out) {
    if (e.name.size() > 0xFFFF) return false;
    out.clear();
    put_u16(out, static_cast<uint16_t>(e.name.size()));
    out.insert(out.end(), e.name.begin(), e.name.end());
    put_u64(out, e.size);
    put_u64(out, e.payload_xxh3);
    put_u32(out, static_cast<uint32_t>(e.segments.size()));
    for (const auto& s : e.segments) {
        put_u32(out, s.chunk_index);
        put_u64(out, s.offset_in_chunk);
        put_u64(out, s.length);
    }
    return true;
}

bool decode_stream_header(std::string_view data, StreamHeader& out) {
    size_t p = 0;
    if (!get_magic(data, p, out.magic)) return false;
    if (!get_u32(data, p, out.format_version)) return false;
    if (!get_u32(data, p, out.total_chunks)) return false;
    if (!get_u64(data, p, out.total_stream_size)) return false;
    if (!get_u64(data, p, out.chunk_target)) return false;
    if (!get_u32(data, p, out.num_chunks_requested)) return false;
    if (!get_u64(data, p, out.file_count)) return false;
    return true;
}

bool decode_chunk_header(std::string_view data, ChunkHeader& out) {
    size_t p = 0;
    if (!get_magic(data, p, out.magic)) return false;
    if (!get_u32(data, p, out.format_version)) return false;
    if (!get_u32(data, p, out.chunk_index)) return false;
    if (!get_u64(data, p, out.chunk_size)) return false;
    if (!get_u32(data, p, out.entry_count)) return false;
    return true;
}

bool decode_chunk_footer(std::string_view data, ChunkFooter& out) {
    size_t p = 0;
    if (!get_magic(data, p, out.magic)) return false;
    if (!get_u64(data, p, out.stream_xxh3)) return false;
    if (!get_u64(data, p, out.chunk_size)) return false;
    return true;
}

bool decode_file_entry(std::string_view data, FileEntry& out) {
    size_t p = 0;
    uint16_t name_len = 0;
    if (!get_u16(data, p, name_len)) return false;
    if (p + name_len > data.size()) return false;
    out.name.assign(data.substr(p, name_len));
    p += name_len;
    if (!get_u64(data, p, out.size)) return false;
    if (!get_u64(data, p, out.payload_xxh3)) return false;
    uint32_t seg_count = 0;
    if (!get_u32(data, p, seg_count)) return false;
    out.segments.clear();
    out.segments.reserve(seg_count);
    for (uint32_t i = 0; i < seg_count; ++i) {
        Segment s;
        if (!get_u32(data, p, s.chunk_index)) return false;
        if (!get_u64(data, p, s.offset_in_chunk)) return false;
        if (!get_u64(data, p, s.length)) return false;
        out.segments.push_back(s);
    }
    return true;
}

} // namespace sss