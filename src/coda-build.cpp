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
 * Build a CODA middle in three phases:
 *
 *   ways       Input ways and relations: way blocks without locations go to
 *              temporary files, one per way bucket (WB_SHIFT); unique
 *              (node, way) pairs are scattered into temporary files, one per
 *              node bucket (NB_SHIFT). Relation blocks and the parent
 *              indexes of relation members are written to the middle.
 *   nodes      Input nodes, in id order: each node bucket in turn is loaded
 *              and sorted (the next one is loaded in the background) and
 *              merged with the node stream. This writes the node->way index
 *              and the loose nodes to the middle and appends the locations
 *              of all way nodes to temporary files, one per way bucket, in
 *              the order of (way bucket, node id).
 *   locations  For each way bucket (in parallel): attach the locations to
 *              the way blocks and write them to the middle.
 *
 * Temporary files are zstd compressed and deleted as soon as they are used.
 */

#include "coda-build.hpp"

#include "coda-source.hpp"
#include "coda-store.hpp"
#include "coda-temp.hpp"
#include "format.hpp"
#include "logging.hpp"
#include "util.hpp"

#include <osmium/handler.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/osm.hpp>
#include <osmium/visitor.hpp>

#include <protozero/pbf_message.hpp>
#include <protozero/pbf_reader.hpp>

#include <zlib.h>
#include <zstd.h>

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>

#include <csignal>
#endif

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <thread>

namespace coda {

namespace {

constexpr unsigned WB_SHIFT = 21; // way ids per temporary way bucket
constexpr unsigned NB_SHIFT = 26; // node ids per temporary node bucket
constexpr std::size_t COMMIT_EVERY = 10000; // puts per transaction

using clock_type = std::chrono::steady_clock;

std::chrono::microseconds elapsed(clock_type::time_point start)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        clock_type::now() - start);
}

/// Writes (sorted) blocks to the middle, committing every so often.
class block_sink_t
{
public:
    explicit block_sink_t(store_t const &store) : m_store(&store) { begin(); }

    void put(db_t db, id_type key, std::string const &value)
    {
        m_txn->put(db, key, value, true);
        if (++m_puts >= COMMIT_EVERY) {
            commit();
            begin();
        }
    }

    void commit()
    {
        m_txn->commit();
        m_txn.reset();
        m_puts = 0;
    }

    void begin() { m_txn = std::make_unique<txn_t>(*m_store, false); }

    txn_t &txn() noexcept { return *m_txn; }

private:
    store_t const *m_store;
    std::unique_ptr<txn_t> m_txn;
    std::size_t m_puts = 0;
};

void write_index(block_sink_t *sink, db_t db, pairs_t *pairs,
                 tag_dict_t const &dict)
{
    auto const shift = index_shift(db);
    std::sort(pairs->begin(), pairs->end());
    pairs->erase(std::unique(pairs->begin(), pairs->end()), pairs->end());
    std::size_t i = 0;
    pairs_t group;
    while (i < pairs->size()) {
        auto const block = (*pairs)[i].first >> shift;
        group.clear();
        while (i < pairs->size() && ((*pairs)[i].first >> shift) == block) {
            group.push_back((*pairs)[i++]);
        }
        sink->put(db, block, encode_pairs(group, block << shift, dict));
    }
    log_debug("CODA build: {} pairs in index {}.", pairs->size(), db_name(db));
    pairs_t{}.swap(*pairs);
}

id_type checked_id(osmium::OSMObject const &object, id_type *last)
{
    if (object.id() <= 0) {
        throw fmt_error("The CODA middle only supports positive ids ({} {}).",
                        osmium::item_type_to_name(object.type()), object.id());
    }
    auto const id = static_cast<id_type>(object.id());
    if (id <= *last) {
        throw fmt_error("Input for the CODA middle must be sorted and must not "
                        "contain duplicates ({} {} after {}).",
                        osmium::item_type_to_name(object.type()), id, *last);
    }
    *last = id;
    return id;
}

// ---------------------------------------------------------------- PBF sections

/**
 * Byte ranges of an OSM PBF file: the header blob and the node, way and
 * relation sections. Found by bisection over blobs (a few dozen small reads),
 * so each pass can read only the sections it needs.
 */
struct pbf_sections_t
{
    std::uint64_t size = 0;
    std::uint64_t header_end = 0;
    std::uint64_t ways_start = 0;
    std::uint64_t rels_start = 0;
};

class pbf_probe_t
{
public:
    explicit pbf_probe_t(byte_source_t const &source) : m_source(&source) {}

    std::uint64_t size() const noexcept { return m_source->size(); }

    std::string read(std::uint64_t offset, std::size_t length) const
    {
        return m_source->read(offset, length);
    }

    struct blob_header_t
    {
        bool osm_data = false;
        std::uint64_t header_size = 0; // including the 4 byte length
        std::uint64_t data_size = 0;
    };

    /// Parse a blob header at offset pos of data.
    static std::optional<blob_header_t> parse_header(std::string_view data,
                                                     std::size_t pos)
    {
        constexpr std::uint32_t MAX_HEADER = 64U * 1024U;
        constexpr std::uint64_t MAX_BLOB = 64ULL * 1024U * 1024U;
        if (pos + 4 > data.size()) {
            return std::nullopt;
        }
        auto const *p = reinterpret_cast<unsigned char const *>(data.data());
        std::uint32_t const len = (std::uint32_t{p[pos]} << 24U) |
                                  (std::uint32_t{p[pos + 1]} << 16U) |
                                  (std::uint32_t{p[pos + 2]} << 8U) |
                                  std::uint32_t{p[pos + 3]};
        if (len < 8 || len > MAX_HEADER || pos + 4 + len > data.size()) {
            return std::nullopt;
        }
        blob_header_t header;
        std::string_view type;
        try {
            protozero::pbf_reader reader{data.data() + pos + 4, len};
            while (reader.next()) {
                if (reader.tag() == 1 &&
                    reader.wire_type() ==
                        protozero::pbf_wire_type::length_delimited) {
                    auto const v = reader.get_view();
                    type = std::string_view{v.data(), v.size()};
                } else if (reader.tag() == 3 &&
                           reader.wire_type() ==
                               protozero::pbf_wire_type::varint) {
                    header.data_size = reader.get_uint64();
                } else {
                    reader.skip();
                }
            }
        } catch (protozero::exception const &) {
            return std::nullopt;
        }
        if ((type != "OSMData" && type != "OSMHeader") ||
            header.data_size == 0 || header.data_size > MAX_BLOB) {
            return std::nullopt;
        }
        header.osm_data = type == "OSMData";
        header.header_size = 4 + len;
        return header;
    }

    blob_header_t header_at(std::uint64_t offset) const
    {
        auto const h = parse_header(read(offset, 4 + (64UL * 1024UL)), 0);
        if (!h) {
            throw std::runtime_error{"no blob header"};
        }
        return *h;
    }

    /// 0 nodes, 1 ways, 2 relations, 3 other (from the first group).
    int blob_kind(std::uint64_t offset, blob_header_t const &header) const
    {
        auto const blob = read(offset + header.header_size,
                               static_cast<std::size_t>(header.data_size));
        std::string raw;
        std::uint64_t raw_size = 0;
        protozero::pbf_reader reader{blob};
        while (reader.next()) {
            switch (reader.tag()) {
            case 1: {
                auto const v = reader.get_view();
                raw.assign(v.data(), v.size());
                break;
            }
            case 2:
                raw_size = reader.get_uint64();
                break;
            case 3: {
                auto const v = reader.get_view();
                raw.resize(raw_size);
                auto dest_len = static_cast<uLongf>(raw_size);
                if (::uncompress(reinterpret_cast<Bytef *>(raw.data()),
                                 &dest_len,
                                 reinterpret_cast<Bytef const *>(v.data()),
                                 static_cast<uLong>(v.size())) != Z_OK) {
                    throw std::runtime_error{"zlib"};
                }
                break;
            }
            case 7: {
                auto const v = reader.get_view();
                raw.resize(raw_size);
                auto const got =
                    ZSTD_decompress(raw.data(), raw.size(), v.data(), v.size());
                if (ZSTD_isError(got)) {
                    throw std::runtime_error{"zstd"};
                }
                break;
            }
            default:
                if (reader.tag() >= 4 && reader.tag() <= 6) {
                    throw std::runtime_error{"unsupported compression"};
                }
                reader.skip();
            }
        }
        protozero::pbf_reader block{raw};
        while (block.next(2)) { // primitivegroup
            protozero::pbf_reader group = block.get_message();
            if (group.next()) {
                switch (group.tag()) {
                case 1:
                case 2:
                    return 0;
                case 3:
                    return 1;
                case 4:
                    return 2;
                default:
                    return 3;
                }
            }
        }
        return 3;
    }

    /// Offset of the first blob at or after offset (limit if none).
    std::uint64_t first_blob_from(std::uint64_t offset,
                                  std::uint64_t limit) const
    {
        constexpr std::size_t WINDOW = 1U << 20U;
        while (offset < limit) {
            auto const data = read(offset, WINDOW + (64UL * 1024UL) + 4);
            std::size_t q = 0;
            while ((q = data.find("OSMData", q)) != std::string::npos &&
                   q < WINDOW + 6) {
                auto const p = q - 6; // 4 bytes length, 2 bytes field tag
                ++q;
                if (q < 7) {
                    continue;
                }
                auto const h = parse_header(data, p);
                if (!h) {
                    continue;
                }
                auto const next = offset + p + h->header_size + h->data_size;
                if (next >= limit ||
                    parse_header(read(next, 4 + 64 * 1024), 0)) {
                    return offset + p;
                }
            }
            offset += WINDOW;
        }
        return limit;
    }

    /// First blob start in [lo, hi) of kind >= kind (lo is a blob start).
    std::uint64_t section_start(int kind, std::uint64_t lo,
                                std::uint64_t hi) const
    {
        constexpr std::uint64_t WALK = 2ULL << 20U; // then blob by blob
        while (hi - lo > WALK) {
            auto const mid = lo + (hi - lo) / 2;
            auto const b = first_blob_from(mid, hi);
            if (b >= hi) {
                hi = mid;
                continue;
            }
            if (blob_kind(b, header_at(b)) >= kind) {
                hi = b;
            } else {
                lo = b;
            }
        }
        for (auto offset = lo; offset < hi;) {
            auto const h = header_at(offset);
            if (h.osm_data && blob_kind(offset, h) >= kind) {
                return offset;
            }
            offset += h.header_size + h.data_size;
        }
        return hi;
    }

private:
    byte_source_t const *m_source;
};

std::optional<pbf_sections_t> find_pbf_sections(byte_source_t const &source)
{
    try {
        pbf_probe_t const probe{source};
        pbf_sections_t sections;
        sections.size = probe.size();
        auto const header = probe.header_at(0);
        if (header.osm_data) {
            return std::nullopt;
        }
        sections.header_end = header.header_size + header.data_size;
        sections.ways_start =
            probe.section_start(1, sections.header_end, sections.size);
        sections.rels_start =
            probe.section_start(2, sections.ways_start, sections.size);
        return sections;
    } catch (std::exception const &e) {
        log_debug("CODA build: can not find PBF sections ({}), reading the "
                  "whole file in each pass.",
                  e.what());
        return std::nullopt;
    }
}

using ranges_t = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

#ifndef _WIN32

/**
 * Feeds byte ranges of the input into a pipe, from threads of their own, so
 * an osmium reader can read just these ranges. Chunks are read in parallel
 * if the source is remote and written to the pipe in order.
 */
class range_pipe_t
{
public:
    range_pipe_t(byte_source_t const &source, ranges_t const &ranges)
    : m_source(&source)
    {
        constexpr std::uint64_t CHUNK = 8U << 20U;
        for (auto const &[start, end] : ranges) {
            for (auto pos = start; pos < end; pos += CHUNK) {
                m_chunks.emplace_back(pos, std::min(end, pos + CHUNK));
            }
        }
        if (::pipe(m_fds.data()) != 0) {
            throw std::system_error{errno, std::system_category(), "pipe"};
        }
        m_thread = std::thread{[this]() {
            // Get EPIPE instead of a signal when the reader stops early.
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &set, nullptr);
            try {
                feed();
            } catch (std::exception const &e) {
                // The reader sees the truncated input and reports an error.
                log_error("{}", e.what());
            }
            ::close(m_fds[1]);
        }};
    }

    ~range_pipe_t() noexcept
    {
        ::close(m_fds[0]); // makes the feeder stop with EPIPE
        m_thread.join();
    }

    range_pipe_t(range_pipe_t const &) = delete;
    range_pipe_t &operator=(range_pipe_t const &) = delete;
    range_pipe_t(range_pipe_t &&) = delete;
    range_pipe_t &operator=(range_pipe_t &&) = delete;

    osmium::io::File file() const
    {
        return osmium::io::File{fmt::format("/dev/fd/{}", m_fds[0]), "pbf"};
    }

private:
    bool write_all(std::string const &data)
    {
        for (std::size_t done = 0; done < data.size();) {
            auto const w =
                ::write(m_fds[1], data.data() + done, data.size() - done);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false; // reader is gone (EPIPE)
            }
            done += static_cast<std::size_t>(w);
        }
        return true;
    }

    void feed()
    {
        auto const workers = m_source->parallelism();
        if (workers <= 1) {
            for (auto const &[start, end] : m_chunks) {
                if (!write_all(m_source->read(
                        start, static_cast<std::size_t>(end - start)))) {
                    return;
                }
            }
            return;
        }

        // Parallel reads of chunks at most `ahead` chunks in front of the
        // one written next.
        std::size_t const ahead = 2UL * workers;
        std::mutex mutex;
        std::condition_variable cv;
        std::map<std::size_t, std::string> ready;
        std::size_t next_read = 0;
        std::size_t next_write = 0;
        bool stop = false;
        std::exception_ptr error;

        std::vector<std::thread> threads;
        for (unsigned i = 0; i < workers; ++i) {
            threads.emplace_back([&]() {
                while (true) {
                    std::size_t chunk = 0;
                    {
                        std::unique_lock<std::mutex> lock{mutex};
                        cv.wait(lock, [&] {
                            return stop || next_read >= m_chunks.size() ||
                                   next_read < next_write + ahead;
                        });
                        if (stop || next_read >= m_chunks.size()) {
                            return;
                        }
                        chunk = next_read++;
                    }
                    try {
                        auto const [start, end] = m_chunks[chunk];
                        auto data = m_source->read(
                            start, static_cast<std::size_t>(end - start));
                        std::lock_guard<std::mutex> const guard{mutex};
                        ready.emplace(chunk, std::move(data));
                    } catch (...) {
                        std::lock_guard<std::mutex> const guard{mutex};
                        error = std::current_exception();
                        stop = true;
                    }
                    cv.notify_all();
                }
            });
        }

        while (next_write < m_chunks.size()) {
            std::string data;
            {
                std::unique_lock<std::mutex> lock{mutex};
                cv.wait(lock, [&] { return stop || ready.count(next_write); });
                if (stop) {
                    break;
                }
                auto const it = ready.find(next_write);
                data = std::move(it->second);
                ready.erase(it);
            }
            bool const ok = write_all(data);
            {
                std::lock_guard<std::mutex> const guard{mutex};
                ++next_write;
                if (!ok) {
                    stop = true;
                }
            }
            cv.notify_all();
            if (!ok) {
                break;
            }
        }
        {
            std::lock_guard<std::mutex> const guard{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto &thread : threads) {
            thread.join();
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    byte_source_t const *m_source;
    ranges_t m_chunks;
    std::array<int, 2> m_fds{-1, -1};
    std::thread m_thread;
};
#endif

/// The input file and, for PBF files, its sections.
struct input_t
{
    explicit input_t(osmium::io::File f) : file(std::move(f))
    {
        if (file.format() != osmium::io::file_format::pbf) {
            return;
        }
        try {
            source = byte_source_t::open(file.filename());
        } catch (std::exception const &e) {
            log_debug("CODA build: no random access to input ({}).", e.what());
        }
        if (source) {
            sections = find_pbf_sections(*source);
        }
    }

    osmium::io::File file;
    std::unique_ptr<byte_source_t> source;
    std::optional<pbf_sections_t> sections;
};

// ---------------------------------------------------------------- pipeline

using blocks_t = std::vector<std::pair<id_type, std::string>>;

/**
 * The results of tasks, in the order they were started. The tasks are
 * started by one thread (the one reading the input) and their results are
 * consumed by another one (the one writing to the middle). At most threads
 * tasks run at the same time, in the order they were started, but more
 * results can wait to be written, so that no thread has to wait for the
 * slowest task.
 */
template <typename RESULT>
class ordered_queue_t
{
public:
    explicit ordered_queue_t(unsigned threads)
    : m_threads(std::max(threads, 1U)), m_capacity(3UL * m_threads)
    {
    }

    /// Run task in a thread of its own; blocks while the queue is full.
    template <typename TASK>
    void start(TASK task)
    {
        std::unique_lock<std::mutex> lock{m_mutex};
        m_cond.wait(lock, [&] {
            return m_aborted || m_queue.size() < m_capacity;
        });
        if (m_aborted) {
            throw std::runtime_error{"CODA build: aborted."};
        }
        m_queue.push_back(std::async(
            std::launch::async,
            [this, ticket = m_tickets++,
             task = std::move(task)]() mutable {
                slot_t const slot{this, ticket};
                return task();
            }));
        m_cond.notify_all();
    }

    /// Returns false when the queue is closed and empty.
    bool pop(std::future<RESULT> *result)
    {
        std::unique_lock<std::mutex> lock{m_mutex};
        m_cond.wait(lock, [&] { return m_closed || !m_queue.empty(); });
        if (m_queue.empty()) {
            return false;
        }
        *result = std::move(m_queue.front());
        m_queue.pop_front();
        m_cond.notify_all();
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> const lock{m_mutex};
        m_closed = true;
        m_cond.notify_all();
    }

    void abort()
    {
        std::lock_guard<std::mutex> const lock{m_mutex};
        m_aborted = true;
        m_cond.notify_all();
    }

private:
    /// While a task runs (tasks get it in the order of their tickets).
    class slot_t
    {
    public:
        slot_t(ordered_queue_t *queue, std::uint64_t ticket) : m_queue(queue)
        {
            std::unique_lock<std::mutex> lock{m_queue->m_slot_mutex};
            m_queue->m_slot_cond.wait(lock, [&] {
                return m_queue->m_running < m_queue->m_threads &&
                       m_queue->m_next_ticket == ticket;
            });
            ++m_queue->m_next_ticket;
            ++m_queue->m_running;
            m_queue->m_slot_cond.notify_all();
        }

        ~slot_t() noexcept
        {
            std::lock_guard<std::mutex> const lock{m_queue->m_slot_mutex};
            --m_queue->m_running;
            m_queue->m_slot_cond.notify_all();
        }

        slot_t(slot_t const &) = delete;
        slot_t &operator=(slot_t const &) = delete;
        slot_t(slot_t &&) = delete;
        slot_t &operator=(slot_t &&) = delete;

    private:
        ordered_queue_t *m_queue;
    };

    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::deque<std::future<RESULT>> m_queue;
    unsigned m_threads;
    std::size_t m_capacity;
    std::uint64_t m_tickets = 0;
    bool m_closed = false;
    bool m_aborted = false;

    std::mutex m_slot_mutex;
    std::condition_variable m_slot_cond;
    unsigned m_running = 0;
    std::uint64_t m_next_ticket = 0;
};

/**
 * Call read(queue) in a thread of its own, which starts tasks, and
 * write(result) for the result of each task in order in this thread (the
 * only one that writes to the middle).
 */
template <typename RESULT, typename READ, typename WRITE>
void run_ordered(unsigned threads, READ const &read, WRITE const &write)
{
    ordered_queue_t<RESULT> queue{threads};
    std::exception_ptr read_error;
    std::thread reader{[&]() {
        try {
            read(&queue);
        } catch (...) {
            read_error = std::current_exception();
        }
        queue.close();
    }};

    std::exception_ptr write_error;
    std::future<RESULT> result;
    while (queue.pop(&result)) {
        try {
            write(result.get());
        } catch (...) {
            write_error = std::current_exception();
            queue.abort();
            break;
        }
    }
    reader.join();
    if (write_error) {
        std::rethrow_exception(write_error);
    }
    if (read_error) {
        std::rethrow_exception(read_error);
    }
}

/// A temporary stream of whole frames, written about 1 MB at a time.
class frame_file_t
{
public:
    explicit frame_file_t(std::string path) : m_path(std::move(path)) {}

    void add(std::string const &frame)
    {
        m_pending.append(frame);
        m_stored += frame.size();
        if (m_pending.size() >= (1U << 20U)) {
            flush();
        }
    }

    void flush()
    {
        if (!m_pending.empty()) {
            append_file(m_path, m_pending);
            m_pending.clear();
        }
    }

    std::uint64_t stored() const noexcept { return m_stored; }

private:
    std::string m_path;
    std::string m_pending;
    std::uint64_t m_stored = 0;
};

/// One frame_file_t per bucket.
class frame_files_t
{
public:
    frame_files_t(std::string tmpdir, char const *kind)
    : m_tmpdir(std::move(tmpdir)), m_kind(kind)
    {
    }

    void add(blocks_t const &frames)
    {
        for (auto const &[bucket, frame] : frames) {
            if (m_files.size() <= bucket) {
                m_files.resize(bucket + 1);
            }
            auto &file = m_files[bucket];
            if (!file) {
                file = std::make_unique<frame_file_t>(
                    temp_path(m_tmpdir, m_kind, bucket));
            }
            file->add(frame);
        }
    }

    /// Returns the number of bytes written.
    std::uint64_t flush()
    {
        std::uint64_t bytes = 0;
        for (auto &file : m_files) {
            if (file) {
                file->flush();
                bytes += file->stored();
            }
        }
        return bytes;
    }

    std::size_t size() const noexcept { return m_files.size(); }

private:
    std::string m_tmpdir;
    char const *m_kind;
    std::vector<std::unique_ptr<frame_file_t>> m_files;
};

/// Per-bucket delta coded streams of one task, each becomes one frame.
template <typename STATE>
class bucket_streams_t
{
public:
    struct stream_t
    {
        writer_t out;
        STATE state;
    };

    stream_t &operator[](id_type bucket)
    {
        if (m_streams.size() <= bucket) {
            m_streams.resize(bucket + 1);
        }
        return m_streams[bucket];
    }

    blocks_t frames()
    {
        blocks_t result;
        for (std::size_t b = 0; b < m_streams.size(); ++b) {
            auto const &data = m_streams[b].out.data();
            if (!data.empty()) {
                result.emplace_back(b, make_frame(data));
            }
        }
        return result;
    }

private:
    std::vector<stream_t> m_streams;
};

// ---------------------------------------------------------------- phase 1

/**
 * The ways and relations pass: the reading thread collects the objects of
 * each range of ids into a buffer, and a task for each range encodes them.
 */
constexpr unsigned WAY_TASK_SHIFT = 18;
constexpr unsigned REL_TASK_SHIFT = 14;
static_assert(WAY_TASK_SHIFT >= WAY_SHIFT && WAY_TASK_SHIFT <= WB_SHIFT);
static_assert(REL_TASK_SHIFT >= REL_SHIFT);

struct ways_result_t
{
    id_type way_bucket = 0;
    /// Records for the temporary way bucket file.
    std::string way_blocks;
    /// (node bucket, frame) with (node, way) pairs.
    blocks_t node_refs;
    blocks_t relations;
    pairs_t node_rels;
    pairs_t way_rels;
    pairs_t rel_rels;
    std::uint64_t num_ways = 0;
    std::uint64_t num_rels = 0;
    std::uint64_t refs = 0;
    std::uint64_t pairs = 0;
};

struct node_refs_state_t
{
    id_type last_way = 0;
    std::int64_t last_node = 0;
};

ways_result_t process_ways(osmium::memory::Buffer const &buffer,
                           tag_dict_t const &dict)
{
    ways_result_t result;
    bucket_streams_t<node_refs_state_t> node_buckets;
    std::vector<way_t> ways;
    id_type block = ~static_cast<id_type>(0);
    auto const flush = [&]() {
        if (ways.empty()) {
            return;
        }
        auto const data = encode_ways(ways, false, dict);
        auto const len = static_cast<std::uint32_t>(data.size());
        result.way_blocks.append(reinterpret_cast<char const *>(&block),
                                 sizeof(block));
        result.way_blocks.append(reinterpret_cast<char const *>(&len),
                                 sizeof(len));
        result.way_blocks.append(data);
        result.num_ways += ways.size();
        ways.clear();
    };

    std::vector<id_type> unique;
    for (auto const &way : buffer.select<osmium::Way>()) {
        way_t w;
        w.id = static_cast<id_type>(way.id());
        w.nodes.reserve(way.nodes().size());
        for (auto const &nr : way.nodes()) {
            if (nr.ref() <= 0) {
                throw fmt_error("The CODA middle only supports positive ids "
                                "(way {} has node {}).",
                                w.id, nr.ref());
            }
            w.nodes.push_back(static_cast<id_type>(nr.ref()));
        }
        w.tags = get_tags(way);
        result.refs += w.nodes.size();

        // scatter unique (node, way) pairs by node bucket
        unique = w.nodes;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        for (std::size_t i = 0; i < unique.size();) {
            auto const b = unique[i] >> NB_SHIFT;
            auto j = i;
            while (j < unique.size() && (unique[j] >> NB_SHIFT) == b) {
                ++j;
            }
            auto &[out, state] = node_buckets[b];
            out.varint(w.id - state.last_way);
            state.last_way = w.id;
            out.varint(j - i);
            for (auto k = i; k < j; ++k) {
                out.svarint(static_cast<std::int64_t>(unique[k]) -
                            state.last_node);
                state.last_node = static_cast<std::int64_t>(unique[k]);
            }
            result.pairs += j - i;
            i = j;
        }

        if ((w.id >> WAY_SHIFT) != block) {
            flush();
            block = w.id >> WAY_SHIFT;
        }
        ways.push_back(std::move(w));
    }
    flush();
    result.way_bucket = (block << WAY_SHIFT) >> WB_SHIFT;
    result.node_refs = node_buckets.frames();
    return result;
}

ways_result_t process_relations(osmium::memory::Buffer const &buffer,
                                tag_dict_t const &dict)
{
    ways_result_t result;
    std::vector<relation_t> rels;
    id_type block = ~static_cast<id_type>(0);
    auto const flush = [&]() {
        if (!rels.empty()) {
            result.relations.emplace_back(block,
                                          encode_relations(rels, dict));
            result.num_rels += rels.size();
            rels.clear();
        }
    };

    for (auto const &relation : buffer.select<osmium::Relation>()) {
        relation_t r;
        r.id = static_cast<id_type>(relation.id());
        for (auto const &m : relation.members()) {
            if (m.ref() <= 0) {
                throw fmt_error("The CODA middle only supports positive ids "
                                "(relation {} has member {}).",
                                r.id, m.ref());
            }
            auto const type = member_type(m.type());
            auto const ref = static_cast<id_type>(m.ref());
            r.members.push_back({type, ref, m.role()});
            (type == 0   ? result.node_rels
             : type == 1 ? result.way_rels
                         : result.rel_rels)
                .emplace_back(ref, r.id);
        }
        r.tags = get_tags(relation);
        if ((r.id >> REL_SHIFT) != block) {
            flush();
            block = r.id >> REL_SHIFT;
        }
        rels.push_back(std::move(r));
    }
    flush();
    return result;
}

/// Handler for the ways pass: starts a task for each range of ids.
class ways_handler_t : public osmium::handler::Handler
{
public:
    ways_handler_t(tag_dict_t const &dict,
                   ordered_queue_t<ways_result_t> *queue)
    : m_dict(&dict), m_queue(queue)
    {
    }

    void way(osmium::Way const &way)
    {
        auto const id = checked_id(way, &m_last_way);
        add(id >> WAY_TASK_SHIFT, false, way);
    }

    void relation(osmium::Relation const &relation)
    {
        auto const id = checked_id(relation, &m_last_rel);
        add(id >> REL_TASK_SHIFT, true, relation);
    }

    void finish() { send(); }

private:
    using buffer_ptr = std::unique_ptr<osmium::memory::Buffer>;

    void add(id_type range, bool relations, osmium::OSMObject const &object)
    {
        if (!m_buffer || range != m_range || relations != m_relations) {
            send();
            m_buffer = std::make_unique<osmium::memory::Buffer>(
                1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes);
            m_range = range;
            m_relations = relations;
        }
        m_buffer->add_item(object);
        m_buffer->commit();
    }

    void send()
    {
        if (!m_buffer) {
            return;
        }
        m_queue->start([dict = m_dict, relations = m_relations,
                        buffer = std::shared_ptr<osmium::memory::Buffer>{
                            std::move(m_buffer)}]() {
            return relations ? process_relations(*buffer, *dict)
                             : process_ways(*buffer, *dict);
        });
    }

    tag_dict_t const *m_dict;
    ordered_queue_t<ways_result_t> *m_queue;
    buffer_ptr m_buffer;
    id_type m_range = 0;
    bool m_relations = false;
    id_type m_last_way = 0;
    id_type m_last_rel = 0;
};

/// Writes the results of the ways pass in order.
class ways_writer_t
{
public:
    ways_writer_t(std::string tmpdir, block_sink_t *sink,
                  tag_dict_t const &dict)
    : m_tmpdir(std::move(tmpdir)), m_sink(sink), m_dict(&dict),
      m_node_buckets(m_tmpdir, "nb")
    {
    }

    void add(ways_result_t const &result)
    {
        if (!result.way_blocks.empty()) {
            if (!m_way_file || result.way_bucket != m_way_bucket) {
                m_way_file = std::make_unique<file_t>(
                    temp_path(m_tmpdir, "wb", result.way_bucket), "wb");
                if (!*m_way_file) {
                    throw std::runtime_error{
                        "CODA build: can not create temporary file."};
                }
                m_way_bucket = result.way_bucket;
            }
            m_way_file->write(result.way_blocks.data(),
                              result.way_blocks.size());
            m_way_bytes += result.way_blocks.size();
        }
        m_node_buckets.add(result.node_refs);
        for (auto const &[key, data] : result.relations) {
            m_sink->put(db_t::rels, key, data);
        }
        m_node_rels.insert(m_node_rels.end(), result.node_rels.begin(),
                           result.node_rels.end());
        m_way_rels.insert(m_way_rels.end(), result.way_rels.begin(),
                          result.way_rels.end());
        m_rel_rels.insert(m_rel_rels.end(), result.rel_rels.begin(),
                          result.rel_rels.end());
        m_num_ways += result.num_ways;
        m_num_rels += result.num_rels;
        m_refs += result.refs;
        m_pairs += result.pairs;
    }

    /// Returns the sorted, unique node ids that are relation members.
    std::vector<id_type> finish()
    {
        m_way_file.reset();
        auto const temp_bytes = m_node_buckets.flush();
        log_info("CODA build: {} ways with {} node refs, {} relations; {:.1f} "
                 "GB temporary way blocks, {:.1f} GB temporary node refs.",
                 m_num_ways, m_refs, m_num_rels,
                 static_cast<double>(m_way_bytes) / 1e9,
                 static_cast<double>(temp_bytes) / 1e9);

        std::vector<id_type> members;
        members.reserve(m_node_rels.size());
        for (auto const &p : m_node_rels) {
            members.push_back(p.first);
        }
        std::sort(members.begin(), members.end());
        members.erase(std::unique(members.begin(), members.end()),
                      members.end());

        write_index(m_sink, db_t::n2r, &m_node_rels, *m_dict);
        write_index(m_sink, db_t::w2r, &m_way_rels, *m_dict);
        write_index(m_sink, db_t::r2r, &m_rel_rels, *m_dict);
        return members;
    }

    std::size_t num_node_buckets() const noexcept
    {
        return m_node_buckets.size();
    }

private:
    std::string m_tmpdir;
    block_sink_t *m_sink;
    tag_dict_t const *m_dict;
    std::unique_ptr<file_t> m_way_file;
    id_type m_way_bucket = 0;
    frame_files_t m_node_buckets;
    pairs_t m_node_rels;
    pairs_t m_way_rels;
    pairs_t m_rel_rels;
    std::uint64_t m_num_ways = 0;
    std::uint64_t m_num_rels = 0;
    std::uint64_t m_refs = 0;
    std::uint64_t m_pairs = 0;
    std::uint64_t m_way_bytes = 0;
};

// ---------------------------------------------------------------- phase 2

/**
 * The nodes pass is split into units of node ids that are processed in
 * parallel: the main thread only collects the nodes of a unit, a worker
 * merges them with the (node, way) pairs of the unit, and the results are
 * written in order.
 */
constexpr unsigned UNIT_SHIFT = 20;
constexpr id_type UNITS_PER_BUCKET = id_type{1} << (NB_SHIFT - UNIT_SHIFT);
static_assert(UNIT_SHIFT >= N2W_SHIFT && UNIT_SHIFT >= NODE_SHIFT &&
              UNIT_SHIFT <= NB_SHIFT);

/// (node, way) pairs of one node bucket, grouped (but not sorted) by unit.
struct bucket_pairs_t
{
    pairs_t pairs;
    std::vector<std::size_t> starts; ///< UNITS_PER_BUCKET + 1 offsets
};

using bucket_future_t = std::shared_future<std::shared_ptr<bucket_pairs_t>>;

std::shared_ptr<bucket_pairs_t> load_bucket_pairs(std::string const &path)
{
    auto result = std::make_shared<bucket_pairs_t>();
    auto &starts = result->starts;
    starts.assign(UNITS_PER_BUCKET + 1, 0);
    std::vector<std::string> frames;
    for_each_frame(path, [&](std::string_view data) { frames.emplace_back(data); });
    auto const decode = [&frames](auto const &fn) {
        for (auto const &data : frames) {
            // each frame starts from 0
            reader_t reader{data};
            id_type way = 0;
            std::int64_t node = 0;
            while (!reader.done()) {
                way += reader.varint();
                auto const n = reader.varint();
                for (std::uint64_t i = 0; i < n; ++i) {
                    node += reader.svarint();
                    auto const id = static_cast<id_type>(node);
                    fn(id, (id >> UNIT_SHIFT) & (UNITS_PER_BUCKET - 1), way);
                }
            }
        }
    };
    decode([&](id_type, id_type unit, id_type) { ++starts[unit + 1]; });
    std::partial_sum(starts.begin(), starts.end(), starts.begin());
    result->pairs.resize(starts.back());
    auto pos = starts;
    decode([&](id_type node, id_type unit, id_type way) {
        result->pairs[pos[unit]++] = {node, way};
    });
    return result;
}

/// The nodes of a range of units in one node bucket.
struct nodes_task_t
{
    id_type first_unit = 0;
    id_type last_unit = 0; ///< exclusive
    bucket_future_t pairs;
    std::vector<std::pair<id_type, osmium::Location>> nodes;
    osmium::memory::Buffer tagged{64UL * 1024UL,
                                  osmium::memory::Buffer::auto_grow::yes};
};

struct nodes_result_t
{
    blocks_t n2w;
    blocks_t loose;
    /// (way bucket, frame) with the locations for that way bucket.
    blocks_t locations;
    std::uint64_t num_loose = 0;
    std::uint64_t missing = 0;
};

/**
 * Merge the nodes of a task with their (node, way) pairs: writes the
 * node->way index, the loose nodes and, for each way bucket, the locations
 * of its nodes in node order (one frame per way bucket, so that the frames
 * of all tasks can simply be concatenated).
 */
class nodes_worker_t
{
public:
    nodes_worker_t(tag_dict_t const &dict,
                   std::vector<id_type> const &relation_members)
    : m_dict(&dict), m_relation_members(&relation_members)
    {
    }

    nodes_result_t run(nodes_task_t const &task)
    {
        auto &bucket = *task.pairs.get();
        auto const first = task.first_unit & (UNITS_PER_BUCKET - 1);
        auto const last = first + (task.last_unit - task.first_unit);
        auto const begin = bucket.pairs.begin();
        for (auto u = first; u < last; ++u) {
            // units are disjoint, so tasks can sort their parts in parallel
            std::sort(begin + static_cast<std::ptrdiff_t>(bucket.starts[u]),
                      begin +
                          static_cast<std::ptrdiff_t>(bucket.starts[u + 1]));
        }
        m_pos = bucket.pairs.data() + bucket.starts[first];
        m_end = bucket.pairs.data() + bucket.starts[last];

        auto rel = std::lower_bound(m_relation_members->begin(),
                                    m_relation_members->end(),
                                    task.first_unit << UNIT_SHIFT);
        auto tagged = task.tagged.select<osmium::Node>();
        auto tit = tagged.begin();
        for (auto const &[id, location] : task.nodes) {
            drain_missing(id);
            auto const *const before = m_pos;
            emit(id, location);
            bool const in_way = m_pos != before;
            bool const tags = tit != tagged.end() &&
                              static_cast<id_type>(tit->id()) == id;
            while (rel != m_relation_members->end() && *rel < id) {
                ++rel;
            }
            bool const member = rel != m_relation_members->end() && *rel == id;
            if (tags || !in_way || member) {
                if ((id >> NODE_SHIFT) != m_loose_block) {
                    flush_loose();
                    m_loose_block = id >> NODE_SHIFT;
                }
                m_loose.push_back(
                    {id, location, tags ? get_tags(*tit) : tags_t{}});
            }
            if (tags) {
                ++tit;
            }
        }
        drain_missing(~static_cast<id_type>(0));
        flush_n2w();
        flush_loose();
        m_result.locations = m_locations.frames();
        return std::move(m_result);
    }

private:
    struct location_state_t
    {
        std::int64_t x = 0;
        std::int64_t y = 0;
    };

    /// All pairs with node < upto refer to nodes not in the input.
    void drain_missing(id_type upto)
    {
        while (m_pos != m_end && m_pos->first < upto) {
            ++m_result.missing;
            emit(m_pos->first, osmium::Location{});
        }
    }

    /// Emit all pairs of node (starting at m_pos) with this location.
    void emit(id_type node, osmium::Location location)
    {
        auto const block = node >> N2W_SHIFT;
        if (block != m_n2w_block) {
            flush_n2w();
            m_n2w_block = block;
        }
        id_type last_wb = ~static_cast<id_type>(0);
        for (; m_pos != m_end && m_pos->first == node; ++m_pos) {
            auto const way = m_pos->second;
            m_n2w.emplace_back(node, way);
            auto const wb = way >> WB_SHIFT;
            if (wb != last_wb) {
                auto &[out, state] = m_locations[wb];
                out.svarint(location.x() - state.x);
                out.svarint(location.y() - state.y);
                state.x = location.x();
                state.y = location.y();
                last_wb = wb;
            }
        }
    }

    void flush_n2w()
    {
        if (m_n2w.empty()) {
            return;
        }
        m_result.n2w.emplace_back(
            m_n2w_block,
            encode_pairs(m_n2w, m_n2w_block << N2W_SHIFT, *m_dict));
        m_n2w.clear();
    }

    void flush_loose()
    {
        if (m_loose.empty()) {
            return;
        }
        m_result.loose.emplace_back(m_loose_block,
                                    encode_nodes(m_loose, *m_dict));
        m_result.num_loose += m_loose.size();
        m_loose.clear();
    }

    tag_dict_t const *m_dict;
    std::vector<id_type> const *m_relation_members;
    std::pair<id_type, id_type> const *m_pos = nullptr;
    std::pair<id_type, id_type> const *m_end = nullptr;
    id_type m_n2w_block = ~static_cast<id_type>(0);
    pairs_t m_n2w;
    id_type m_loose_block = ~static_cast<id_type>(0);
    std::vector<node_t> m_loose;
    bucket_streams_t<location_state_t> m_locations;
    nodes_result_t m_result;
};

/**
 * Handler for the nodes pass: collects the nodes of each unit and starts a
 * worker for it. Every unit up to the last node bucket gets a task (ranges
 * without nodes are combined), because they can have refs to missing nodes.
 */
class nodes_handler_t : public osmium::handler::Handler
{
public:
    nodes_handler_t(std::string tmpdir, tag_dict_t const &dict,
                    std::vector<id_type> const &relation_members,
                    std::size_t num_buckets,
                    ordered_queue_t<nodes_result_t> *queue)
    : m_tmpdir(std::move(tmpdir)), m_dict(&dict),
      m_relation_members(&relation_members), m_num_buckets(num_buckets),
      m_queue(queue)
    {
    }

    void node(osmium::Node const &node)
    {
        auto const id = checked_id(node, &m_last_node);
        auto const unit = id >> UNIT_SHIFT;
        if (!m_task || unit != m_task->first_unit) {
            send_task();
            send_empty(unit);
            m_task = std::make_unique<nodes_task_t>();
            m_task->first_unit = unit;
            m_task->last_unit = unit + 1;
        }
        m_task->nodes.emplace_back(id, node.location());
        if (!node.tags().empty()) {
            m_task->tagged.add_item(node);
            m_task->tagged.commit();
        }
        ++m_num_nodes;
    }

    /// Returns the number of nodes.
    std::uint64_t finish()
    {
        send_task();
        send_empty(m_num_buckets * UNITS_PER_BUCKET);
        return m_num_nodes;
    }

private:
    void send_task()
    {
        if (m_task) {
            m_next_unit = m_task->last_unit;
            send(std::move(m_task));
        }
    }

    /// Tasks without nodes for all units before unit.
    void send_empty(id_type unit)
    {
        unit = std::min(unit, m_num_buckets * UNITS_PER_BUCKET);
        while (m_next_unit < unit) {
            auto task = std::make_unique<nodes_task_t>();
            task->first_unit = m_next_unit;
            task->last_unit = std::min(
                unit, (m_next_unit / UNITS_PER_BUCKET + 1) * UNITS_PER_BUCKET);
            m_next_unit = task->last_unit;
            send(std::move(task));
        }
        m_next_unit = std::max(m_next_unit, unit);
    }

    void send(std::unique_ptr<nodes_task_t> task)
    {
        task->pairs = bucket(task->first_unit / UNITS_PER_BUCKET);
        m_queue->start([dict = m_dict, members = m_relation_members,
                        t = std::shared_ptr<nodes_task_t>{std::move(task)}]() {
            return nodes_worker_t{*dict, *members}.run(*t);
        });
    }

    /// The pairs of bucket b, and start loading the next ones.
    bucket_future_t bucket(id_type b)
    {
        while (!m_buckets.empty() && m_buckets.begin()->first < b) {
            m_buckets.erase(m_buckets.begin());
        }
        for (auto i = b; i < b + PREFETCH_BUCKETS; ++i) {
            if (m_buckets.count(i) == 0) {
                if (i < m_num_buckets) {
                    m_buckets[i] = std::async(std::launch::async,
                                              load_bucket_pairs,
                                              temp_path(m_tmpdir, "nb", i))
                                       .share();
                } else {
                    std::promise<std::shared_ptr<bucket_pairs_t>> empty;
                    auto none = std::make_shared<bucket_pairs_t>();
                    none->starts.assign(UNITS_PER_BUCKET + 1, 0);
                    empty.set_value(std::move(none));
                    m_buckets[i] = empty.get_future().share();
                }
            }
        }
        return m_buckets[b];
    }

    static constexpr id_type PREFETCH_BUCKETS = 3;

    std::string m_tmpdir;
    tag_dict_t const *m_dict;
    std::vector<id_type> const *m_relation_members;
    id_type m_num_buckets;
    ordered_queue_t<nodes_result_t> *m_queue;
    std::map<id_type, bucket_future_t> m_buckets;
    std::unique_ptr<nodes_task_t> m_task;
    id_type m_next_unit = 0;
    id_type m_last_node = 0;
    std::uint64_t m_num_nodes = 0;
};

/// Writes the results of the nodes pass in order.
class nodes_writer_t
{
public:
    nodes_writer_t(std::string const &tmpdir, block_sink_t *sink)
    : m_sink(sink), m_locations(tmpdir, "lb")
    {
    }

    void add(nodes_result_t const &result)
    {
        for (auto const &[key, data] : result.n2w) {
            m_sink->put(db_t::n2w, key, data);
        }
        for (auto const &[key, data] : result.loose) {
            m_sink->put(db_t::nodes, key, data);
        }
        m_locations.add(result.locations);
        m_num_loose += result.num_loose;
        m_missing += result.missing;
    }

    void finish(std::uint64_t num_nodes)
    {
        auto const temp_bytes = m_locations.flush();
        log_info("CODA build: {} nodes, {} loose nodes, {} node refs to "
                 "missing nodes; {:.1f} GB temporary locations.",
                 num_nodes, m_num_loose, m_missing,
                 static_cast<double>(temp_bytes) / 1e9);
    }

private:
    block_sink_t *m_sink;
    frame_files_t m_locations;
    std::uint64_t m_num_loose = 0;
    std::uint64_t m_missing = 0;
};

// ---------------------------------------------------------------- phase 3

/**
 * Attach the locations to the way blocks of one way bucket. Two passes over
 * the compressed blocks, so only one block is decoded at a time: memory use
 * is about 16 bytes per unique node of the bucket.
 */
blocks_t assemble_way_bucket(std::string const &tmpdir, id_type bucket,
                             tag_dict_t const &dict)
{
    auto const way_path = temp_path(tmpdir, "wb", bucket);
    blocks_t blocks;
    {
        file_t file{way_path, "rb"};
        if (!file) {
            return blocks;
        }
        id_type key = 0;
        std::uint32_t len = 0;
        while (file.read(&key, sizeof(key))) {
            if (!file.read(&len, sizeof(len))) {
                throw std::runtime_error{"CODA build: short temporary file."};
            }
            std::string data(len, '\0');
            if (!file.read(data.data(), len)) {
                throw std::runtime_error{"CODA build: short temporary file."};
            }
            blocks.emplace_back(key, std::move(data));
        }
    }

    std::vector<id_type> nodes;
    for (auto const &[key, data] : blocks) {
        for (auto const &way : decode_ways(data, key, dict, false)) {
            nodes.insert(nodes.end(), way.nodes.begin(), way.nodes.end());
        }
    }
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    nodes.shrink_to_fit();

    // each frame of locations starts from (0, 0)
    std::vector<osmium::Location> locations;
    locations.reserve(nodes.size());
    for_each_frame(temp_path(tmpdir, "lb", bucket), [&](std::string_view data) {
        reader_t reader{data};
        std::int64_t x = 0;
        std::int64_t y = 0;
        while (!reader.done()) {
            x += reader.svarint();
            y += reader.svarint();
            locations.emplace_back(static_cast<int32_t>(x),
                                   static_cast<int32_t>(y));
        }
    });
    if (locations.size() != nodes.size()) {
        throw fmt_error("CODA build: {} locations for {} nodes in way bucket "
                        "{}.",
                        locations.size(), nodes.size(), bucket);
    }

    for (auto &[key, data] : blocks) {
        auto ways = decode_ways(data, key, dict);
        std::string{}.swap(data);
        for (auto &way : ways) {
            way.locations.resize(way.nodes.size());
            for (std::size_t i = 0; i < way.nodes.size(); ++i) {
                auto const it =
                    std::lower_bound(nodes.begin(), nodes.end(), way.nodes[i]);
                way.locations[i] =
                    locations[static_cast<std::size_t>(it - nodes.begin())];
            }
        }
        data = encode_ways(ways, true, dict);
    }
    std::filesystem::remove(way_path);
    return blocks;
}

void assemble_ways(std::string const &tmpdir, block_sink_t *sink,
                   tag_dict_t const &dict, unsigned threads)
{
    id_type num_buckets = 0;
    for (auto const &entry : std::filesystem::directory_iterator{tmpdir}) {
        auto const name = entry.path().filename().string();
        if (name.rfind("wb-", 0) == 0) {
            num_buckets =
                std::max<id_type>(num_buckets, std::stoull(name.substr(3)) + 1);
        }
    }

    std::uint64_t num_blocks = 0;
    std::uint64_t bytes = 0;
    run_ordered<blocks_t>(
        threads,
        [&](ordered_queue_t<blocks_t> *queue) {
            for (id_type b = 0; b < num_buckets; ++b) {
                queue->start([&tmpdir, &dict, b]() {
                    return assemble_way_bucket(tmpdir, b, dict);
                });
            }
        },
        [&](blocks_t const &blocks) {
            for (auto const &[key, data] : blocks) {
                sink->put(db_t::ways, key, data);
                ++num_blocks;
                bytes += data.size();
            }
        });
    log_info("CODA build: {} way blocks, {:.1f} GB.", num_blocks,
             static_cast<double>(bytes) / 1e9);
}

// ---------------------------------------------------------------- reading

/**
 * Read the input with an osmium reader: its header only, or the nodes, or
 * the ways and relations (only these sections of a PBF file if possible).
 */
template <typename HANDLER>
osmium::io::Header read_input(input_t const &input,
                              osmium::osm_entity_bits::type what,
                              osmium::io::read_meta read_meta, HANDLER *handler)
{
    bool const nodes = what == osmium::osm_entity_bits::node;
    // Without sections the nodes pass also asks for ways, so that it can
    // stop at the first one.
    auto const types =
        (nodes && !input.sections)
            ? (osmium::osm_entity_bits::node | osmium::osm_entity_bits::way)
            : what;

    auto run = [&](osmium::io::File const &file) {
        osmium::io::Reader reader{file, types, read_meta};
        auto header = reader.header();
        while (auto buffer = reader.read()) {
            bool done = false;
            for (auto const &object : buffer.select<osmium::OSMObject>()) {
                if (nodes && object.type() != osmium::item_type::node) {
                    done = true;
                    break;
                }
                osmium::apply_item(object, *handler);
            }
            if (done) {
                break;
            }
        }
        reader.close();
        return header;
    };

#ifndef _WIN32
    if (input.sections) {
        auto const &sec = *input.sections;
        ranges_t ranges{{0, sec.header_end}};
        if (nodes) {
            ranges.emplace_back(sec.header_end, sec.ways_start);
        } else if (what != osmium::osm_entity_bits::nothing) {
            ranges.emplace_back(sec.ways_start, sec.size);
        }
        range_pipe_t const pipe{*input.source, ranges};
        return run(pipe.file());
    }
#endif
    return run(input.file);
}

/// Handler for the "ways" pass that also finds the newest timestamp.
template <typename HANDLER>
class timestamp_handler_t : public osmium::handler::Handler
{
public:
    explicit timestamp_handler_t(HANDLER *handler) : m_handler(handler) {}

    void node(osmium::Node const &node)
    {
        update(node);
        m_handler->node(node);
    }

    void way(osmium::Way const &way)
    {
        update(way);
        m_handler->way(way);
    }

    void relation(osmium::Relation const &relation)
    {
        update(relation);
        m_handler->relation(relation);
    }

    osmium::Timestamp timestamp() const noexcept { return m_timestamp; }

private:
    void update(osmium::OSMObject const &object) noexcept
    {
        if (object.timestamp() > m_timestamp) {
            m_timestamp = object.timestamp();
        }
    }

    HANDLER *m_handler;
    osmium::Timestamp m_timestamp;
};

} // anonymous namespace

file_info build(osmium::io::File const &file, std::string const &dir,
                unsigned threads)
{
    util::timer_t timer;
    auto const start = clock_type::now();
    store_t store{dir, store_t::mode_t::create};
    auto const &dict = store.dict();
    auto const tmpdir = (std::filesystem::path{dir} / "build-tmp").string();
    std::filesystem::remove_all(tmpdir);
    std::filesystem::create_directories(tmpdir);

    input_t const input{file};
    auto const &sections = input.sections;
    if (sections) {
        log_debug("CODA build: PBF sections: header 0-{}, nodes -{}, ways -{}, "
                  "relations -{}.",
                  sections->header_end, sections->ways_start,
                  sections->rels_start, sections->size);
    }

    file_info finfo;
    block_sink_t sink{store};

    log_info("CODA build: reading ways and relations ({} threads)...",
             threads);
    std::vector<id_type> relation_members;
    std::size_t num_node_buckets = 0;
    {
        // Try the header first: if it has a replication timestamp, there is
        // no need to read the object metadata.
        osmium::handler::Handler none;
        finfo.header = read_input(input, osmium::osm_entity_bits::nothing,
                                  osmium::io::read_meta::no, &none);
        auto const ts = finfo.header.get("osmosis_replication_timestamp");
        if (!ts.empty()) {
            finfo.last_timestamp = osmium::Timestamp{ts};
        }
        auto const what =
            osmium::osm_entity_bits::way | osmium::osm_entity_bits::relation;
        ways_writer_t writer{tmpdir, &sink, dict};
        run_ordered<ways_result_t>(
            threads,
            [&](ordered_queue_t<ways_result_t> *queue) {
                ways_handler_t handler{dict, queue};
                if (!ts.empty()) {
                    read_input(input, what, osmium::io::read_meta::no,
                               &handler);
                } else {
                    timestamp_handler_t<ways_handler_t> th{&handler};
                    read_input(input, what, osmium::io::read_meta::yes, &th);
                    finfo.last_timestamp = th.timestamp();
                }
                handler.finish();
            },
            [&](ways_result_t const &result) { writer.add(result); });
        relation_members = writer.finish();
        num_node_buckets = writer.num_node_buckets();
    }
    log_info("CODA build: ways and relations done after {}.",
             util::human_readable_duration(elapsed(start)));

    log_info("CODA build: reading nodes ({} threads)...", threads);
    {
        nodes_writer_t writer{tmpdir, &sink};
        std::uint64_t num_nodes = 0;
        run_ordered<nodes_result_t>(
            threads,
            [&](ordered_queue_t<nodes_result_t> *queue) {
                nodes_handler_t handler{tmpdir, dict, relation_members,
                                        num_node_buckets, queue};
                read_input(input, osmium::osm_entity_bits::node,
                           osmium::io::read_meta::no, &handler);
                num_nodes = handler.finish();
            },
            [&](nodes_result_t const &result) { writer.add(result); });
        writer.finish(num_nodes);
    }
    std::vector<id_type>{}.swap(relation_members);
    log_info("CODA build: nodes done after {}.",
             util::human_readable_duration(elapsed(start)));

    log_info("CODA build: attaching locations to ways ({} threads)...",
             threads);
    assemble_ways(tmpdir, &sink, dict, std::max(threads, 1U));

    auto &txn = sink.txn();
    for (std::string const s : {"base_url", "sequence_number", "timestamp"}) {
        auto const value = finfo.header.get("osmosis_replication_" + s);
        txn.put_meta("replication_" + s, value);
    }
    if (finfo.last_timestamp.valid()) {
        txn.put_meta("import_timestamp", finfo.last_timestamp.to_iso());
        txn.put_meta("current_timestamp", finfo.last_timestamp.to_iso());
    }
    sink.commit();
    store.sync();
    std::filesystem::remove_all(tmpdir);

    auto const [file_size, free_size] = store.file_and_free_size();
    log_info("CODA build: done after {}, middle is {:.2f} GB.",
             util::human_readable_duration(timer.stop()),
             static_cast<double>(file_size - free_size) / 1e9);
    return finfo;
}

} // namespace coda
