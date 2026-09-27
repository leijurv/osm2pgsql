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
 * Bulk import without random access to node locations or member ways.
 *
 * Four phases in three sequential passes over the input file:
 *
 *   scatter    PBF ways -> (node_low, way_id) pairs into bucket files keyed
 *              by node_id >> NODE_SHIFT. PBF relations -> (member_low, rel_id)
 *              pairs for their node members (keyed like the ways' node refs)
 *              and for their way members (keyed by way_id >> WAY_SHIFT).
 *   nodes      PBF nodes, in id order: every node goes to the middle and,
 *              whole buffers at a time, to the worker outputs. Meanwhile the
 *              node refs are resolved: each node bucket in turn is radix
 *              sorted by node id and merged with the node stream, emitting
 *              (node_id, x, y, way_low) into bucket files keyed by
 *              way_id >> WAY_SHIFT, and the locations of relation node members
 *              into member files keyed by rel_id >> REL_SHIFT.
 *   ways       PBF ways, in id order: each way bucket in turn is radix sorted
 *              by way id and the way locations are set in place from it; ways
 *              go to the middle and, whole buffers at a time, to the worker
 *              outputs. Every way that is a relation member is also written,
 *              with all its node locations, to the member files.
 *   relations  The same reader goes on into the PBF relations: each member
 *              bucket in turn is sorted by relation id, the members of every
 *              relation are assembled from it, and relations go to the middle
 *              and, together with their members, to a new set of worker
 *              outputs.
 *
 * Middle and Lua see the same order as in a normal import: all nodes, then
 * all ways, then all relations. Every bucket is sorted in RAM right before it
 * is consumed and deleted right after. Worker outputs each have their own Lua
 * state and database connections (output_t::clone_worker()), so Lua, geometry
 * building and COPY run in parallel.
 */

#include "bulk-import.hpp"

#include "db-copy.hpp"
#include "format.hpp"
#include "logging.hpp"
#include "middle.hpp"
#include "options.hpp"
#include "osmdata.hpp"
#include "output.hpp"
#include "util.hpp"

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/osm/node.hpp>
#include <osmium/osm/relation.hpp>
#include <osmium/osm/way.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr unsigned NODE_SHIFT = 28; // 268M node ids per bucket
constexpr unsigned WAY_SHIFT = 24;  // 16.7M way ids per bucket
constexpr unsigned REL_SHIFT = 16;  // 65k relation ids per bucket
constexpr uint64_t NODE_MASK = (1ULL << NODE_SHIFT) - 1;
constexpr uint64_t WAY_MASK = (1ULL << WAY_SHIFT) - 1;
constexpr uint64_t REL_MASK = (1ULL << REL_SHIFT) - 1;
constexpr unsigned MAX_BUCKETS = 4096;
constexpr std::size_t WRITE_BUFFER_SIZE = 4UL << 20U;
constexpr std::size_t MEMBER_WRITE_BUFFER_SIZE = 1UL << 20U;

/// Relations with more members are ignored (as in osmdata_t::relation()).
constexpr std::size_t MAX_MEMBERS = 32767;

#pragma pack(push, 1)
/// Scatter output; the bucket index holds the high node id bits.
struct node_way_t
{
    uint32_t node_low;
    uint32_t way_id;
};

/// Nodes phase output; the bucket index holds the high way id bits.
struct way_loc_t
{
    uint64_t node_id;
    int32_t x;
    int32_t y;
    uint32_t way_low;
};

/// Scatter output for relation members; the bucket index holds the high bits
/// of the member id.
struct member_rel_t
{
    uint32_t member_low;
    uint32_t rel_id;
};

/**
 * A resolved relation member, as written by the nodes phase (node members)
 * and the ways phase (way members). A node member is followed by its x and
 * y as two int32, a way member by num_nodes member_node_t. The bucket index
 * holds the high relation id bits.
 */
struct member_t
{
    uint32_t rel_low;
    uint32_t num_nodes; ///< NODE_MEMBER for a node member
    int64_t id;
};

struct member_node_t
{
    int64_t id;
    int32_t x;
    int32_t y;
};

/// Index entry into a loaded member bucket.
struct member_entry_t
{
    uint64_t offset;
    uint32_t rel_low;
};
#pragma pack(pop)

constexpr uint32_t NODE_MEMBER = UINT32_MAX;

static_assert(sizeof(node_way_t) == 8);
static_assert(sizeof(way_loc_t) == 20);
static_assert(sizeof(member_rel_t) == 8);
static_assert(sizeof(member_t) == 16);
static_assert(sizeof(member_node_t) == 16);

std::string bucket_path(std::string const &dir, char const *kind, unsigned b)
{
    return fmt::format("{}/osm2pgsql-bulk-{}-{:04}.bin", dir, kind, b);
}

void write_all(int fd, char const *data, std::size_t size)
{
    while (size > 0) {
        auto const written = ::write(fd, data, size);
        if (written < 0) {
            throw std::system_error{errno, std::system_category(),
                                    "Writing bucket file failed"};
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

/// One append-only file per bucket, each with its own write buffer.
class bucket_writer_t
{
public:
    bucket_writer_t(std::string dir, char const *kind,
                    std::size_t buffer_size = WRITE_BUFFER_SIZE)
    : m_dir(std::move(dir)), m_kind(kind), m_buffer_size(buffer_size)
    {}

    bucket_writer_t(bucket_writer_t const &) = delete;
    bucket_writer_t &operator=(bucket_writer_t const &) = delete;
    bucket_writer_t(bucket_writer_t &&) = delete;
    bucket_writer_t &operator=(bucket_writer_t &&) = delete;

    ~bucket_writer_t() noexcept
    {
        for (auto &b : m_buckets) {
            if (b.fd >= 0) {
                ::close(b.fd);
            }
        }
    }

    void add(unsigned bucket, void const *record, std::size_t size)
    {
        if (bucket >= MAX_BUCKETS) {
            throw std::runtime_error{"Object id too large for bulk import."};
        }
        if (bucket >= m_buckets.size()) {
            m_buckets.resize(bucket + 1);
        }
        auto &b = m_buckets[bucket];
        if (b.fd < 0) {
            auto const path = bucket_path(m_dir, m_kind, bucket);
            b.fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (b.fd < 0) {
                throw std::system_error{
                    errno, std::system_category(),
                    fmt::format("Creating bucket file '{}' failed", path)};
            }
            b.buffer.resize(m_buffer_size);
        }
        if (b.used + size > m_buffer_size) {
            write_all(b.fd, b.buffer.data(), b.used);
            b.used = 0;
            if (size > m_buffer_size) {
                write_all(b.fd, static_cast<char const *>(record), size);
                m_bytes += size;
                return;
            }
        }
        std::memcpy(b.buffer.data() + b.used, record, size);
        b.used += size;
        m_bytes += size;
    }

    /// Flush and close all files, return the number of buckets.
    unsigned close()
    {
        for (auto &b : m_buckets) {
            if (b.fd >= 0) {
                write_all(b.fd, b.buffer.data(), b.used);
                ::close(b.fd);
                b.fd = -1;
                b.buffer = {};
            }
        }
        return static_cast<unsigned>(m_buckets.size());
    }

    uint64_t bytes() const noexcept { return m_bytes; }

private:
    struct bucket_t
    {
        int fd = -1;
        std::vector<char> buffer;
        std::size_t used = 0;
    };

    std::string m_dir;
    char const *m_kind;
    std::size_t m_buffer_size;
    std::vector<bucket_t> m_buckets;
    uint64_t m_bytes = 0;
};

/// Read a whole bucket file (a missing file is an empty bucket), delete it.
template <typename T>
std::vector<T> take_bucket(std::string const &path)
{
    std::vector<T> records;
    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return records;
    }
    struct stat st
    {};
    ::fstat(fd, &st);
    records.resize(static_cast<std::size_t>(st.st_size) / sizeof(T));
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    auto *data = reinterpret_cast<char *>(records.data());
    std::size_t left = records.size() * sizeof(T);
    while (left > 0) {
        auto const got = ::read(fd, data, left);
        if (got <= 0) {
            ::close(fd);
            throw fmt_error("Reading bucket file '{}' failed.", path);
        }
        data += got;
        left -= static_cast<std::size_t>(got);
    }
    ::close(fd);
    ::unlink(path.c_str());
    return records;
}

/// Stable LSD radix sort on the low key_bits of key(record).
template <typename T, typename KEY>
void radix_sort(std::vector<T> *records, unsigned key_bits, KEY key)
{
    constexpr unsigned DIGIT = 11;
    constexpr std::size_t RADIX = 1U << DIGIT;
    std::size_t const n = records->size();
    std::vector<T> tmp(n);
    T *src = records->data();
    T *dst = tmp.data();
    std::vector<std::size_t> count(RADIX);
    for (unsigned shift = 0; shift < key_bits; shift += DIGIT) {
        std::fill(count.begin(), count.end(), 0);
        for (std::size_t i = 0; i < n; ++i) {
            ++count[(key(src[i]) >> shift) & (RADIX - 1)];
        }
        std::size_t sum = 0;
        for (auto &c : count) {
            auto const t = c;
            c = sum;
            sum += t;
        }
        for (std::size_t i = 0; i < n; ++i) {
            dst[count[(key(src[i]) >> shift) & (RADIX - 1)]++] = src[i];
        }
        std::swap(src, dst);
    }
    if (src != records->data()) {
        records->swap(tmp);
    }
}

/// Sort member_rel_t records by member id, in the scatter output order for
/// equal member ids.
void sort_member_rels(std::vector<member_rel_t> *records, unsigned key_bits)
{
    radix_sort(records, key_bits,
               [](member_rel_t const &r) { return r.member_low; });
}

/// Advance *pos to the group of records with key(record) == value and
/// return its end. Records must be sorted by key, value must not be smaller
/// than the key at *pos.
template <typename T, typename KEY>
std::size_t find_group(std::vector<T> const &records, std::size_t *pos,
                       uint32_t value, KEY key)
{
    while (*pos < records.size() && key(records[*pos]) < value) {
        ++*pos;
    }
    auto end = *pos;
    while (end < records.size() && key(records[end]) == value) {
        ++end;
    }
    return end;
}

/**
 * Forward-only cursor over the nodes of a PBF file. Reading stops at the
 * first way, so the rest of the file is never decompressed. Every buffer is
 * handed to on_done once the cursor has moved past it.
 */
template <typename ON_DONE>
class node_stream_t
{
public:
    node_stream_t(osmium::io::File const &file, ON_DONE on_done)
    : m_reader(file,
               osmium::osm_entity_bits::node | osmium::osm_entity_bits::way),
      m_on_done(std::move(on_done))
    {
        refill();
    }

    osmium::io::Header header() { return m_reader.header(); }

    /// First node with id >= target, or nullptr at end of the nodes.
    osmium::Node const *seek(osmium::object_id_type target)
    {
        while (!m_eof) {
            for (; m_it != m_end; ++m_it) {
                if (m_it->id() >= target) {
                    return &*m_it;
                }
            }
            refill();
        }
        return nullptr;
    }

    /// Consume all remaining nodes.
    void drain()
    {
        while (!m_eof) {
            refill();
        }
        m_reader.close();
    }

private:
    void refill()
    {
        if (m_buffer) {
            m_on_done(std::move(m_buffer));
            m_buffer = osmium::memory::Buffer{};
        }
        while (!m_last) {
            m_buffer = m_reader.read();
            if (!m_buffer) {
                break;
            }
            auto const ways = m_buffer.select<osmium::Way>();
            m_last = ways.begin() != ways.end();
            m_it = m_buffer.cbegin<osmium::Node>();
            m_end = m_buffer.cend<osmium::Node>();
            if (m_it != m_end) {
                return;
            }
            m_buffer = osmium::memory::Buffer{};
        }
        m_eof = true;
    }

    osmium::io::Reader m_reader;
    ON_DONE m_on_done;
    osmium::memory::Buffer m_buffer;
    osmium::memory::Buffer::t_const_iterator<osmium::Node> m_it;
    osmium::memory::Buffer::t_const_iterator<osmium::Node> m_end;
    bool m_last = false; ///< the current buffer contains the first way
    bool m_eof = false;
};

/**
 * Middle query used by the worker outputs: way node locations are already
 * set, and relation members are handed over with each relation (see
 * set_members()), so there is nothing to look up. Every node location that
 * exists in the input has been resolved, so an unresolved one is invalid.
 * Everything else is passed on to a real middle query.
 */
class resolved_middle_query_t : public middle_query_t
{
public:
    explicit resolved_middle_query_t(std::shared_ptr<middle_query_t> real)
    : m_real(std::move(real))
    {}

    /// Members for the next relation(s): the items in [begin, end) of buffer.
    void set_members(osmium::memory::Buffer const *buffer, std::size_t begin,
                     std::size_t end) noexcept
    {
        m_members = buffer;
        m_members_begin = begin;
        m_members_end = end;
    }

    osmium::Location get_node_location(osmid_t /*id*/) const override
    {
        return osmium::Location{};
    }

    std::size_t nodes_get_list(osmium::WayNodeList *nodes) const override
    {
        return static_cast<std::size_t>(
            std::count_if(nodes->begin(), nodes->end(), [](auto const &nr) {
                return nr.location().valid();
            }));
    }

    bool node_get(osmid_t id, osmium::memory::Buffer *buffer) const override
    {
        return m_real->node_get(id, buffer);
    }

    bool way_get(osmid_t id, osmium::memory::Buffer *buffer) const override
    {
        return m_real->way_get(id, buffer);
    }

    std::size_t
    rel_members_get(osmium::Relation const & /*rel*/,
                    osmium::memory::Buffer *buffer,
                    osmium::osm_entity_bits::type types) const override
    {
        if (!m_members) {
            throw std::runtime_error{
                "Relation members requested outside of the relation pass."};
        }
        std::size_t found = 0;
        auto const *p = m_members->data() + m_members_begin;
        auto const *const end = m_members->data() + m_members_end;
        while (p < end) {
            auto const &item =
                *reinterpret_cast<osmium::memory::Item const *>(p);
            if (types & osmium::osm_entity_bits::from_item_type(item.type())) {
                buffer->add_item(item);
                buffer->commit();
                ++found;
            }
            p += item.padded_size();
        }
        return found;
    }

    bool relation_get(osmid_t id, osmium::memory::Buffer *buffer) const override
    {
        return m_real->relation_get(id, buffer);
    }

private:
    std::shared_ptr<middle_query_t> m_real;
    osmium::memory::Buffer const *m_members = nullptr;
    std::size_t m_members_begin = 0;
    std::size_t m_members_end = 0;
};

/// A unit of work for the worker outputs.
struct job_t
{
    std::shared_ptr<osmium::memory::Buffer> buffer;

    /// Relation jobs only: the relations in buffer to process and, for each,
    /// the byte range of its members in the members buffer.
    struct relation_t
    {
        osmium::Relation const *relation;
        std::size_t members_begin;
        std::size_t members_end;
    };
    std::vector<relation_t> relations;
    osmium::memory::Buffer members;
};

/**
 * A pool of worker outputs, each in its own thread, taking jobs from a
 * bounded queue.
 */
class worker_pool_t
{
public:
    struct worker_t
    {
        std::shared_ptr<output_t> output;
        std::shared_ptr<db_copy_thread_t> copy_thread;
        std::shared_ptr<resolved_middle_query_t> midq;
        std::unique_ptr<middle_way_writer_t> way_writer;
    };

    worker_pool_t(std::shared_ptr<middle_t> const &mid,
                  std::shared_ptr<output_t> const &output,
                  options_t const &options)
    {
        for (unsigned i = 0; i < options.bulk_threads; ++i) {
            auto copy_thread =
                std::make_shared<db_copy_thread_t>(options.connection_params);
            auto midq = std::make_shared<resolved_middle_query_t>(
                mid->get_query_instance());
            m_workers.push_back({output->clone_worker(midq, copy_thread),
                                 copy_thread, midq, nullptr});
        }
    }

    /**
     * Give every worker its own middle way writer. Returns false (and gives
     * none) if the middle can't write ways in parallel.
     */
    bool add_way_writers(middle_t *mid)
    {
        for (auto &worker : m_workers) {
            worker.way_writer = mid->make_way_writer();
            if (!worker.way_writer) {
                for (auto &w : m_workers) {
                    w.way_writer.reset();
                }
                return false;
            }
        }
        return true;
    }

    /// Run fn(worker, job) for every job pushed until finish().
    template <typename FN>
    void start(FN fn)
    {
        m_done = false;
        for (auto &worker : m_workers) {
            m_threads.emplace_back([this, &worker, fn]() {
                try {
                    while (auto job = pop()) {
                        fn(&worker, job.get());
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> const guard{m_mutex};
                    if (!m_error) {
                        m_error = std::current_exception();
                    }
                    m_queue.clear();
                    m_done = true;
                    m_not_full.notify_all();
                    m_not_empty.notify_all();
                }
            });
        }
    }

    void push(std::shared_ptr<job_t> job)
    {
        std::unique_lock<std::mutex> lock{m_mutex};
        m_not_full.wait(lock, [this] {
            return m_queue.size() < 2 * m_workers.size() || m_error;
        });
        if (m_error) {
            std::rethrow_exception(m_error);
        }
        m_queue.push_back(std::move(job));
        m_not_empty.notify_one();
    }

    void push(osmium::memory::Buffer &&buffer)
    {
        auto job = std::make_shared<job_t>();
        job->buffer =
            std::make_shared<osmium::memory::Buffer>(std::move(buffer));
        push(std::move(job));
    }

    /// Wait until all pushed jobs are processed.
    void finish()
    {
        {
            std::lock_guard<std::mutex> const guard{m_mutex};
            m_done = true;
        }
        m_not_empty.notify_all();
        for (auto &thread : m_threads) {
            thread.join();
        }
        m_threads.clear();
        if (m_error) {
            std::rethrow_exception(m_error);
        }
    }

    /// Flush everything the workers wrote to the database.
    void sync()
    {
        for (auto &worker : m_workers) {
            worker.output->sync();
        }
        for (auto &worker : m_workers) {
            worker.copy_thread->finish();
            if (worker.way_writer) {
                worker.way_writer->finish();
            }
        }
    }

private:
    std::shared_ptr<job_t> pop()
    {
        std::unique_lock<std::mutex> lock{m_mutex};
        m_not_empty.wait(lock, [this] { return !m_queue.empty() || m_done; });
        if (m_queue.empty()) {
            return {};
        }
        auto job = std::move(m_queue.front());
        m_queue.pop_front();
        m_not_full.notify_one();
        return job;
    }

    std::vector<worker_t> m_workers;
    std::vector<std::thread> m_threads;
    std::deque<std::shared_ptr<job_t>> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_not_empty;
    std::condition_variable m_not_full;
    std::exception_ptr m_error;
    bool m_done = false;
};

void check_order(osmium::OSMObject const &object, osmium::object_id_type *last)
{
    if (object.id() <= *last) {
        throw fmt_error("Input for bulk import must be sorted and must not "
                        "contain duplicates (seen {} {} after {}).",
                        osmium::item_type_to_name(object.type()), object.id(),
                        *last);
    }
    *last = object.id();
}

void update_timestamp(osmium::OSMObject const &object, file_info *finfo)
{
    if (object.timestamp() > finfo->last_timestamp) {
        finfo->last_timestamp = object.timestamp();
    }
}

// Scatter --------------------------------------------------------------------

struct scatter_result_t
{
    unsigned node_buckets = 0;
    unsigned way_buckets = 0;
};

scatter_result_t scatter_refs(osmium::io::File const &file,
                              std::string const &dir)
{
    util::timer_t timer;
    bucket_writer_t out{dir, "nw"};
    bucket_writer_t node_members{dir, "rn", MEMBER_WRITE_BUFFER_SIZE};
    bucket_writer_t way_members{dir, "rw", MEMBER_WRITE_BUFFER_SIZE};
    osmium::io::Reader reader{
        file, osmium::osm_entity_bits::way | osmium::osm_entity_bits::relation,
        osmium::io::read_meta::no};

    osmium::object_id_type last_way = 0;
    osmium::object_id_type last_rel = 0;
    uint64_t ways = 0;
    uint64_t refs = 0;
    uint64_t rels = 0;
    uint64_t num_node_members = 0;
    uint64_t num_way_members = 0;
    std::vector<uint64_t> ids;
    while (auto buffer = reader.read()) {
        for (auto const &object : buffer.select<osmium::OSMObject>()) {
            if (object.type() == osmium::item_type::way) {
                auto const &way = static_cast<osmium::Way const &>(object);
                check_order(way, &last_way);
                if (static_cast<uint64_t>(way.id()) > UINT32_MAX) {
                    throw std::runtime_error{
                        "Way id too large for bulk import."};
                }
                for (auto const &nr : way.nodes()) {
                    if (nr.ref() <= 0) {
                        continue; // can never be resolved
                    }
                    auto const id = static_cast<uint64_t>(nr.ref());
                    node_way_t const rec{static_cast<uint32_t>(id & NODE_MASK),
                                         static_cast<uint32_t>(way.id())};
                    out.add(static_cast<unsigned>(id >> NODE_SHIFT), &rec,
                            sizeof(rec));
                    ++refs;
                }
                ++ways;
                continue;
            }

            if (object.type() != osmium::item_type::relation) {
                continue;
            }
            auto const &rel = static_cast<osmium::Relation const &>(object);
            check_order(rel, &last_rel);
            if (static_cast<uint64_t>(rel.id()) > UINT32_MAX) {
                throw std::runtime_error{
                    "Relation id too large for bulk import."};
            }
            ++rels;
            if (rel.members().size() > MAX_MEMBERS) {
                continue;
            }
            auto const rel_id = static_cast<uint32_t>(rel.id());

            // Each member is resolved once per relation, even if listed
            // several times.
            auto const add_members =
                [&](osmium::item_type type, bucket_writer_t *writer,
                    unsigned shift, uint64_t mask, uint64_t max_id) {
                    ids.clear();
                    for (auto const &member : rel.members()) {
                        if (member.type() == type && member.ref() > 0 &&
                            static_cast<uint64_t>(member.ref()) <= max_id) {
                            ids.push_back(static_cast<uint64_t>(member.ref()));
                        }
                    }
                    std::sort(ids.begin(), ids.end());
                    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                    for (auto const id : ids) {
                        member_rel_t const rec{static_cast<uint32_t>(id & mask),
                                               rel_id};
                        writer->add(static_cast<unsigned>(id >> shift), &rec,
                                    sizeof(rec));
                    }
                    return ids.size();
                };
            // Ids beyond these can't be in the input: the scatter would have
            // stopped at such a way, the nodes phase at such a node.
            num_node_members += add_members(
                osmium::item_type::node, &node_members, NODE_SHIFT, NODE_MASK,
                (static_cast<uint64_t>(MAX_BUCKETS) << NODE_SHIFT) - 1);
            num_way_members += add_members(osmium::item_type::way, &way_members,
                                           WAY_SHIFT, WAY_MASK, UINT32_MAX);
        }
    }
    reader.close();

    scatter_result_t result;
    result.node_buckets = std::max(out.close(), node_members.close());
    result.way_buckets = way_members.close();

    log_info("Bulk scatter: {} ways with {} node refs, {} relations with {} "
             "node and {} way members, in {}.",
             ways, refs, rels, num_node_members, num_way_members,
             util::human_readable_duration(timer.stop()));
    return result;
}

// Nodes (resolving the node refs) ---------------------------------------------

struct node_bucket_t
{
    std::vector<node_way_t> way_refs;
    std::vector<member_rel_t> rel_members;
};

node_bucket_t load_node_bucket(std::string const &dir, unsigned b)
{
    node_bucket_t bucket;
    bucket.way_refs = take_bucket<node_way_t>(bucket_path(dir, "nw", b));
    radix_sort(&bucket.way_refs, NODE_SHIFT,
               [](node_way_t const &r) { return r.node_low; });
    bucket.rel_members = take_bucket<member_rel_t>(bucket_path(dir, "rn", b));
    sort_member_rels(&bucket.rel_members, NODE_SHIFT);
    return bucket;
}

unsigned resolve_node_refs(osmium::io::File const &file, std::string const &dir,
                           unsigned buckets, middle_t *mid,
                           worker_pool_t *workers, bucket_writer_t *members,
                           file_info *finfo)
{
    util::timer_t timer;
    bucket_writer_t out{dir, "wl"};

    workers->start([](worker_pool_t::worker_t *worker, job_t *job) {
        for (auto const &node : job->buffer->select<osmium::Node>()) {
            if (node.location().valid()) {
                worker->output->node_add(node);
            }
        }
    });

    osmium::object_id_type last = 0;
    uint64_t nodes_count = 0;
    auto on_done = [&](osmium::memory::Buffer &&buffer) {
        for (auto const &node : buffer.select<osmium::Node>()) {
            check_order(node, &last);
            if (!node.location().valid()) {
                log_warn("Ignored node {} (version {}) with invalid location.",
                         node.id(), node.version());
                continue;
            }
            mid->node(node);
            update_timestamp(node, finfo);
            ++nodes_count;
        }
        workers->push(std::move(buffer));
    };
    node_stream_t<decltype(on_done)> nodes{file, on_done};
    finfo->header = nodes.header();

    uint64_t resolved = 0;
    uint64_t missing = 0;
    uint64_t members_resolved = 0;
    std::future<node_bucket_t> next;
    if (buckets > 0) {
        next = std::async(std::launch::async, load_node_bucket, dir, 0U);
    }
    for (unsigned b = 0; b < buckets; ++b) {
        auto const bucket = next.get();
        if (b + 1 < buckets) {
            next = std::async(std::launch::async, load_node_bucket, dir, b + 1);
        }

        // Merge the way node refs and the relation node members, both
        // sorted by node id, against the node stream.
        auto const &refs = bucket.way_refs;
        auto const &rms = bucket.rel_members;
        uint64_t const base = static_cast<uint64_t>(b) << NODE_SHIFT;
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < refs.size() || j < rms.size()) {
            bool const way_ref =
                j == rms.size() ||
                (i < refs.size() && refs[i].node_low <= rms[j].member_low);
            auto const low = way_ref ? refs[i].node_low : rms[j].member_low;
            auto const id = static_cast<osmium::object_id_type>(base | low);
            auto const *node = nodes.seek(id);
            bool const found = node && node->id() == id;

            if (way_ref) {
                auto const &p = refs[i++];
                if (found) {
                    auto const loc = node->location();
                    way_loc_t const rec{
                        static_cast<uint64_t>(id), loc.x(), loc.y(),
                        static_cast<uint32_t>(p.way_id & WAY_MASK)};
                    out.add(p.way_id >> WAY_SHIFT, &rec, sizeof(rec));
                    ++resolved;
                } else {
                    ++missing;
                }
                continue;
            }

            // Unresolved node members are left out, the relation pass
            // gives them an invalid location.
            auto const &m = rms[j++];
            if (found) {
                auto const loc = node->location();
                struct
                {
                    member_t header;
                    int32_t x;
                    int32_t y;
                } __attribute__((packed))
                const rec{{static_cast<uint32_t>(m.rel_id & REL_MASK),
                           NODE_MEMBER, id},
                          loc.x(),
                          loc.y()};
                members->add(m.rel_id >> REL_SHIFT, &rec, sizeof(rec));
                ++members_resolved;
            }
        }
    }
    nodes.drain(); // nodes after the last referenced one still need processing
    auto const way_buckets = out.close();
    workers->finish();

    log_info("Bulk nodes: {} nodes processed, {} node refs resolved, {} "
             "missing, {} relation node members resolved, in {}.",
             nodes_count, resolved, missing, members_resolved,
             util::human_readable_duration(timer.stop()));
    return way_buckets;
}

// Ways (assembling the way locations) -----------------------------------------

struct way_bucket_t
{
    std::vector<way_loc_t> locs;
    std::vector<member_rel_t> rel_members;
};

way_bucket_t load_way_bucket(std::string const &dir, unsigned b)
{
    way_bucket_t bucket;
    bucket.locs = take_bucket<way_loc_t>(bucket_path(dir, "wl", b));
    radix_sort(&bucket.locs, WAY_SHIFT,
               [](way_loc_t const &r) { return r.way_low; });
    bucket.rel_members = take_bucket<member_rel_t>(bucket_path(dir, "rw", b));
    sort_member_rels(&bucket.rel_members, WAY_SHIFT);
    return bucket;
}

/**
 * Process all ways from the reader. Returns the first buffer that contains
 * relations (it may also contain ways, which are processed), or nullptr if
 * there are none.
 */
std::shared_ptr<osmium::memory::Buffer>
assemble_ways(osmium::io::Reader *reader, std::string const &dir,
              unsigned buckets, middle_t *mid, worker_pool_t *workers,
              bucket_writer_t *members, file_info *finfo)
{
    util::timer_t timer;

    bool const parallel_middle = workers->add_way_writers(mid);
    if (!parallel_middle) {
        log_info("Bulk ways: middle can not write ways in parallel.");
    }
    workers->start([](worker_pool_t::worker_t *worker, job_t *job) {
        for (auto &way : job->buffer->select<osmium::Way>()) {
            if (worker->way_writer) {
                worker->way_writer->way(way);
            }
            worker->output->way_add(&way);
        }
    });

    way_bucket_t current_bucket;
    std::size_t pos = 0;
    std::size_t member_pos = 0;
    unsigned current = 0; // index of the loaded bucket, plus one
    std::future<way_bucket_t> next;
    if (buckets > 0) {
        next = std::async(std::launch::async, load_way_bucket, dir, 0U);
    }

    osmium::object_id_type last = 0;
    uint64_t ways = 0;
    uint64_t member_ways = 0;
    std::vector<member_node_t> member_nodes;
    std::shared_ptr<osmium::memory::Buffer> first_relations;
    while (!first_relations) {
        auto buffer = std::make_shared<osmium::memory::Buffer>(reader->read());
        if (!*buffer) {
            break;
        }
        for (auto &object : buffer->select<osmium::OSMObject>()) {
            if (object.type() == osmium::item_type::relation) {
                first_relations = buffer;
                break;
            }
            if (object.type() != osmium::item_type::way) {
                continue;
            }
            auto &way = static_cast<osmium::Way &>(object);
            check_order(way, &last);
            if (!parallel_middle) {
                mid->way(way);
            }
            update_timestamp(way, finfo);
            ++ways;

            auto const bucket = static_cast<unsigned>(
                static_cast<uint64_t>(way.id()) >> WAY_SHIFT);
            while (current <= bucket && current < buckets) {
                current_bucket = next.get();
                pos = 0;
                member_pos = 0;
                ++current;
                if (current < buckets) {
                    next = std::async(std::launch::async, load_way_bucket, dir,
                                      current);
                }
            }
            if (bucket + 1 != current) {
                continue; // no locations at all for ways in this range
            }

            // Within a group the records are in ascending node id order:
            // the nodes phase emits in that order and the radix sort is stable.
            auto const low = static_cast<uint32_t>(
                static_cast<uint64_t>(way.id()) & WAY_MASK);
            auto const &locs = current_bucket.locs;
            auto const group_end = find_group(
                locs, &pos, low, [](way_loc_t const &r) { return r.way_low; });
            auto const *const first = locs.data() + pos;
            auto const *const end = locs.data() + group_end;
            pos = group_end;

            for (auto &nr : way.nodes()) {
                auto const id = static_cast<uint64_t>(nr.ref());
                auto const *it = std::lower_bound(
                    first, end, id, [](way_loc_t const &r, uint64_t v) {
                        return r.node_id < v;
                    });
                if (it != end && it->node_id == id) {
                    nr.set_location(osmium::Location{it->x, it->y});
                }
            }

            // Relations this way is a member of get the way with its
            // locations.
            auto const &rms = current_bucket.rel_members;
            auto const members_end =
                find_group(rms, &member_pos, low,
                           [](member_rel_t const &r) { return r.member_low; });
            if (member_pos == members_end) {
                continue;
            }
            member_nodes.clear();
            member_nodes.push_back({}); // space for the member_t header
            for (auto const &nr : way.nodes()) {
                member_nodes.push_back(
                    {nr.ref(), nr.location().x(), nr.location().y()});
            }
            for (; member_pos < members_end; ++member_pos) {
                auto const rel_id = rms[member_pos].rel_id;
                member_t const header{static_cast<uint32_t>(rel_id & REL_MASK),
                                      static_cast<uint32_t>(way.nodes().size()),
                                      way.id()};
                std::memcpy(member_nodes.data(), &header, sizeof(header));
                members->add(rel_id >> REL_SHIFT, member_nodes.data(),
                             member_nodes.size() * sizeof(member_node_t));
                ++member_ways;
            }
        }
        workers->push([&] {
            auto job = std::make_shared<job_t>();
            job->buffer = buffer;
            return job;
        }());
    }
    workers->finish();

    log_info("Bulk ways: {} ways processed, {} written as relation members, "
             "in {}.",
             ways, member_ways, util::human_readable_duration(timer.stop()));
    return first_relations;
}

// Relations ------------------------------------------------------------------

struct member_bucket_t
{
    std::vector<char> data;
    std::vector<member_entry_t> entries; ///< sorted by relation id
};

member_bucket_t load_member_bucket(std::string const &dir, unsigned b)
{
    member_bucket_t bucket;
    bucket.data = take_bucket<char>(bucket_path(dir, "rm", b));
    std::size_t offset = 0;
    while (offset < bucket.data.size()) {
        member_t header{};
        std::memcpy(&header, bucket.data.data() + offset, sizeof(header));
        bucket.entries.push_back({offset, header.rel_low});
        offset +=
            sizeof(member_t) + (header.num_nodes == NODE_MEMBER
                                    ? 2 * sizeof(int32_t)
                                    : header.num_nodes * sizeof(member_node_t));
    }
    if (offset != bucket.data.size()) {
        throw fmt_error("Bulk member bucket {} is corrupt.", b);
    }
    radix_sort(&bucket.entries, REL_SHIFT,
               [](member_entry_t const &e) { return e.rel_low; });
    return bucket;
}

/**
 * Process all relations: the ones in first (if any) and all remaining
 * buffers from the reader.
 */
void assemble_relations(osmium::io::Reader *reader,
                        std::shared_ptr<osmium::memory::Buffer> first,
                        std::string const &dir, unsigned buckets, middle_t *mid,
                        worker_pool_t *workers, file_info *finfo)
{
    util::timer_t timer;

    workers->start([](worker_pool_t::worker_t *worker, job_t *job) {
        for (auto const &r : job->relations) {
            worker->midq->set_members(&job->members, r.members_begin,
                                      r.members_end);
            worker->output->relation_add(*r.relation);
        }
        worker->midq->set_members(nullptr, 0, 0);
    });

    member_bucket_t current_bucket;
    std::size_t pos = 0;
    unsigned current = 0; // index of the loaded bucket, plus one
    std::future<member_bucket_t> next;
    if (buckets > 0) {
        next = std::async(std::launch::async, load_member_bucket, dir, 0U);
    }

    osmium::object_id_type last = 0;
    uint64_t count = 0;
    uint64_t members_found = 0;
    auto buffer = std::move(first);
    while (buffer && *buffer) {
        auto job = std::make_shared<job_t>();
        job->buffer = buffer;
        job->members = osmium::memory::Buffer{
            1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes};

        for (auto const &rel : buffer->select<osmium::Relation>()) {
            check_order(rel, &last);
            update_timestamp(rel, finfo);
            ++count;
            if (rel.members().size() > MAX_MEMBERS) {
                log_warn("Relation id {} ignored, because it has more than "
                         "32767 members",
                         rel.id());
                continue;
            }
            mid->relation(rel);

            // The member group of this relation: first the node members in
            // node id order (written by the nodes phase), then the way
            // members in way id order (written by the ways phase).
            auto const bucket = static_cast<unsigned>(
                static_cast<uint64_t>(rel.id()) >> REL_SHIFT);
            while (current <= bucket && current < buckets) {
                current_bucket = next.get();
                pos = 0;
                ++current;
                if (current < buckets) {
                    next = std::async(std::launch::async, load_member_bucket,
                                      dir, current);
                }
            }
            member_entry_t const *group_first = nullptr;
            member_entry_t const *group_end = nullptr;
            member_entry_t const *ways_first = nullptr;
            if (bucket + 1 == current) {
                auto const low = static_cast<uint32_t>(
                    static_cast<uint64_t>(rel.id()) & REL_MASK);
                auto const &entries = current_bucket.entries;
                auto const end_pos =
                    find_group(entries, &pos, low, [](member_entry_t const &e) {
                        return e.rel_low;
                    });
                group_first = entries.data() + pos;
                group_end = entries.data() + end_pos;
                pos = end_pos;
            }
            auto const header = [&](member_entry_t const &e) {
                member_t h{};
                std::memcpy(&h, current_bucket.data.data() + e.offset,
                            sizeof(h));
                return h;
            };
            ways_first = std::partition_point(
                group_first, group_end, [&](member_entry_t const &e) {
                    return header(e).num_nodes == NODE_MEMBER;
                });
            auto const find =
                [&](member_entry_t const *first, member_entry_t const *end,
                    osmium::object_id_type id) -> member_entry_t const * {
                auto const *it = std::lower_bound(
                    first, end, id,
                    [&](member_entry_t const &e, osmium::object_id_type v) {
                        return header(e).id < v;
                    });
                return (it != end && header(*it).id == id) ? it : nullptr;
            };

            // Build the members in member order, like the middle would.
            auto const begin = job->members.committed();
            for (auto const &member : rel.members()) {
                if (member.type() == osmium::item_type::node) {
                    osmium::Location loc{};
                    if (auto const *e =
                            find(group_first, ways_first, member.ref())) {
                        std::array<int32_t, 2> xy{};
                        std::memcpy(xy.data(),
                                    current_bucket.data.data() + e->offset +
                                        sizeof(member_t),
                                    sizeof(xy));
                        loc = osmium::Location{xy[0], xy[1]};
                    }
                    {
                        osmium::builder::NodeBuilder builder{job->members};
                        builder.set_id(member.ref());
                        builder.set_location(loc);
                    }
                    job->members.commit();
                    ++members_found;
                } else if (member.type() == osmium::item_type::way) {
                    auto const *e = find(ways_first, group_end, member.ref());
                    if (!e) {
                        continue;
                    }
                    auto const h = header(*e);
                    auto const *nodes = current_bucket.data.data() + e->offset +
                                        sizeof(member_t);
                    {
                        osmium::builder::WayBuilder builder{job->members};
                        builder.set_id(member.ref());
                        osmium::builder::WayNodeListBuilder wnl{job->members,
                                                                &builder};
                        for (uint32_t k = 0; k < h.num_nodes; ++k) {
                            member_node_t n{};
                            std::memcpy(&n, nodes + k * sizeof(member_node_t),
                                        sizeof(n));
                            wnl.add_node_ref(n.id, osmium::Location{n.x, n.y});
                        }
                    }
                    job->members.commit();
                    ++members_found;
                }
            }
            job->relations.push_back({&rel, begin, job->members.committed()});
        }
        workers->push(std::move(job));

        buffer = std::make_shared<osmium::memory::Buffer>(reader->read());
    }
    workers->finish();

    log_info("Bulk relations: {} relations with {} node and way members "
             "processed in {}.",
             count, members_found, util::human_readable_duration(timer.stop()));
}

} // anonymous namespace

file_info bulk_import(osmium::io::File const &file,
                      std::shared_ptr<middle_t> const &mid,
                      std::shared_ptr<output_t> const &output,
                      options_t const &options, osmdata_t *osmdata)
{
    util::timer_t timer;
    file_info finfo;
    auto const &dir = options.bulk_tmpdir;

    log_info("Bulk import with {} worker threads, bucket files in '{}'.",
             options.bulk_threads, dir);

    // Reads ways in the ways phase and goes on into the relations.
    std::unique_ptr<osmium::io::Reader> reader;
    std::shared_ptr<osmium::memory::Buffer> first_relations;
    unsigned member_buckets = 0;
    {
        worker_pool_t workers{mid, output, options};
        bucket_writer_t members{dir, "rm", MEMBER_WRITE_BUFFER_SIZE};

        auto const scattered = scatter_refs(file, dir);
        auto const way_buckets =
            std::max(resolve_node_refs(file, dir, scattered.node_buckets,
                                       mid.get(), &workers, &members, &finfo),
                     scattered.way_buckets);
        osmdata->after_nodes();
        reader = std::make_unique<osmium::io::Reader>(
            file,
            osmium::osm_entity_bits::way | osmium::osm_entity_bits::relation);
        first_relations = assemble_ways(reader.get(), dir, way_buckets,
                                        mid.get(), &workers, &members, &finfo);
        workers.sync();
        member_buckets = members.close();
        log_info("Bulk: {:.1f} GB of relation members in {} buckets.",
                 static_cast<double>(members.bytes()) / 1e9, member_buckets);
    }
    worker_pool_t workers{mid, output, options};
    osmdata->after_ways();

    {
        auto nodes_ways_timer = timer;
        log_info("Bulk: nodes and ways done in {}.",
                 util::human_readable_duration(nodes_ways_timer.stop()));
    }

    assemble_relations(reader.get(), std::move(first_relations), dir,
                       member_buckets, mid.get(), &workers, &finfo);
    workers.sync();
    reader->close();
    osmdata->after_relations();

    log_info("Bulk: nodes, ways and relations done in {}.",
             util::human_readable_duration(timer.stop()));

    return finfo;
}
