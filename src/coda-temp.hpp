#ifndef OSM2PGSQL_CODA_TEMP_HPP
#define OSM2PGSQL_CODA_TEMP_HPP

/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

/**
 * \file
 *
 * Temporary files of the CODA middle's sort-merge joins: append-only streams
 * of varints, compressed in zstd frames, one file per bucket.
 */

#include "coda-format.hpp"
#include "format.hpp"

#include <zstd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace coda {

inline std::string temp_path(std::string const &dir, char const *kind,
                             id_type bucket)
{
    return fmt::format("{}/{}-{:06}", dir, kind, bucket);
}

class file_t
{
public:
    file_t(std::string const &path, char const *mode)
    : m_file(std::fopen(path.c_str(), mode))
    {
    }

    ~file_t() noexcept
    {
        if (m_file) {
            std::fclose(m_file); // NOLINT(cert-err33-c)
        }
    }

    file_t(file_t const &) = delete;
    file_t &operator=(file_t const &) = delete;
    file_t(file_t &&) = delete;
    file_t &operator=(file_t &&) = delete;

    explicit operator bool() const noexcept { return m_file != nullptr; }

    void write(void const *data, std::size_t size)
    {
        if (std::fwrite(data, 1, size, m_file) != size) {
            throw std::system_error{errno, std::system_category(),
                                    "CODA build: writing temporary file"};
        }
    }

    bool read(void *data, std::size_t size)
    {
        return std::fread(data, 1, size, m_file) == size;
    }

private:
    std::FILE *m_file;
};

/// Append data to the file at path.
inline void append_file(std::string const &path, std::string_view data)
{
    file_t file{path, "ab"};
    if (!file) {
        throw fmt_error("CODA build: can not open '{}'.", path);
    }
    file.write(data.data(), data.size());
}

/// Compress data into one frame of a temporary stream (with its length).
inline std::string make_frame(std::string_view data)
{
    std::string frame(sizeof(std::uint32_t) + ZSTD_compressBound(data.size()),
                      '\0');
    auto const size =
        ZSTD_compress(frame.data() + sizeof(std::uint32_t),
                      frame.size() - sizeof(std::uint32_t), data.data(),
                      data.size(), 1);
    if (ZSTD_isError(size)) {
        throw std::runtime_error{"CODA build: zstd compression failed."};
    }
    auto const len = static_cast<std::uint32_t>(size);
    std::memcpy(frame.data(), &len, sizeof(len));
    frame.resize(sizeof(len) + size);
    return frame;
}

/// An append-only temporary stream of varints, written as zstd frames.
class frame_writer_t
{
public:
    frame_writer_t(std::string path, std::size_t limit)
    : m_path(std::move(path)), m_limit(limit)
    {
    }

    writer_t &writer() noexcept { return m_writer; }

    void maybe_flush()
    {
        if (m_writer.data().size() >= m_limit) {
            flush();
        }
    }

    void flush()
    {
        auto &data = m_writer.data();
        if (data.empty()) {
            return;
        }
        auto const frame = make_frame(data);
        append_file(m_path, frame);
        m_stored += frame.size();
        data.clear();
    }

    std::uint64_t stored() const noexcept { return m_stored; }

private:
    writer_t m_writer;
    std::string m_path;
    std::size_t m_limit;
    std::uint64_t m_stored = 0;
};

/**
 * Read and delete a temporary stream written with frame_writer_t, calling
 * fn with the contents of each frame.
 */
template <typename FUNC>
void for_each_frame(std::string const &path, FUNC const &fn)
{
    {
        file_t file{path, "rb"};
        if (!file) {
            return;
        }
        std::string frame;
        std::string data;
        std::uint32_t len = 0;
        while (file.read(&len, sizeof(len))) {
            frame.resize(len);
            if (!file.read(frame.data(), len)) {
                throw fmt_error("CODA build: short temporary file '{}'.", path);
            }
            auto const raw = ZSTD_getFrameContentSize(frame.data(), len);
            if (raw == ZSTD_CONTENTSIZE_ERROR ||
                raw == ZSTD_CONTENTSIZE_UNKNOWN) {
                throw fmt_error("CODA build: bad temporary file '{}'.", path);
            }
            data.resize(raw);
            auto const got =
                ZSTD_decompress(data.data(), raw, frame.data(), len);
            if (ZSTD_isError(got) || got != raw) {
                throw fmt_error("CODA build: bad temporary file '{}'.", path);
            }
            fn(std::string_view{data});
        }
    }
    std::filesystem::remove(path);
}

/// Read and delete a temporary stream written with frame_writer_t.
inline std::string take_frames(std::string const &path)
{
    std::string result;
    for_each_frame(path, [&](std::string_view data) { result.append(data); });
    return result;
}

} // namespace coda

#endif // OSM2PGSQL_CODA_TEMP_HPP
