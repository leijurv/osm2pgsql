#ifndef OSM2PGSQL_CODA_STORE_HPP
#define OSM2PGSQL_CODA_STORE_HPP

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
 * Storage of the CODA middle: an LMDB environment in a directory with one
 * database per block type (keyed by block number) and a meta database with
 * the format version, the tag dictionary and information about the data.
 */

#include "coda-format.hpp"

#include <lmdb.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <cstring>
#include <string>
#include <string_view>

namespace coda {

enum class db_t : unsigned
{
    ways,
    nodes, ///< loose nodes
    rels,
    n2w,
    n2r,
    w2r,
    r2r,
    meta
};

inline constexpr unsigned NUM_DBS = 8;

char const *db_name(db_t db) noexcept;

/// Shift from member id to block number of the parent index databases.
unsigned index_shift(db_t db) noexcept;

class store_t
{
public:
    enum class mode_t
    {
        create, ///< remove existing data, fast writes without sync
        update, ///< transactional updates with random access
        read    ///< read only, mostly sequential access
    };

    store_t(std::string dir, mode_t mode);
    ~store_t() noexcept;

    store_t(store_t const &) = delete;
    store_t &operator=(store_t const &) = delete;
    store_t(store_t &&) = delete;
    store_t &operator=(store_t &&) = delete;

    MDB_env *env() const noexcept { return m_env; }
    MDB_dbi dbi(db_t db) const noexcept
    {
        return m_dbi.at(static_cast<unsigned>(db));
    }

    std::string const &dir() const noexcept { return m_dir; }

    tag_dict_t const &dict() const noexcept { return *m_dict; }

    /// Flush everything to disk (needed after writes in create mode).
    void sync();

    /// Size of the database file and of its free pages in bytes.
    std::pair<std::uint64_t, std::uint64_t> file_and_free_size() const;

    /**
     * Incremented after every commit of a write transaction, so that
     * readers know when their caches are stale.
     */
    std::uint64_t generation() const noexcept { return m_generation; }
    void bump_generation() const noexcept { ++m_generation; }

private:
    std::string m_dir;
    MDB_env *m_env = nullptr;
    std::array<MDB_dbi, NUM_DBS> m_dbi{};
    std::unique_ptr<tag_dict_t> m_dict;
    mutable std::atomic<std::uint64_t> m_generation{0};
}; // class store_t

/// A transaction on a store (RAII: aborted unless committed).
class txn_t
{
public:
    txn_t(store_t const &store, bool read_only);
    ~txn_t() noexcept;

    txn_t(txn_t const &) = delete;
    txn_t &operator=(txn_t const &) = delete;
    txn_t(txn_t &&) = delete;
    txn_t &operator=(txn_t &&) = delete;

    void commit();

    /// Release the snapshot of a read transaction ...
    void reset() noexcept;

    /// ... and get a new, current one.
    void renew();

    std::optional<std::string_view> get(db_t db, id_type key) const;
    void put(db_t db, id_type key, std::string_view value, bool append = false);
    void del(db_t db, id_type key);

    std::optional<std::string_view> get_meta(std::string_view key) const;
    void put_meta(std::string_view key, std::string_view value);

    /// Call func(key, value) for all blocks of db in key order.
    template <typename FUNC>
    void for_each(db_t db, FUNC const &func) const
    {
        for_each_in(db, 0, ~static_cast<id_type>(0), func);
    }

    /// Call func(key, value) for all blocks with first <= key <= last.
    template <typename FUNC>
    void for_each_in(db_t db, id_type first, id_type last,
                     FUNC const &func) const
    {
        cursor_t const cursor{*this, db};
        MDB_val k{sizeof(first), &first};
        MDB_val v;
        int rc = mdb_cursor_get(cursor.get(), &k, &v, MDB_SET_RANGE);
        while (rc == MDB_SUCCESS) {
            id_type key = 0; // LMDB only aligns keys to 2 bytes
            std::memcpy(&key, k.mv_data, sizeof(key));
            if (key > last) {
                break;
            }
            func(key, std::string_view{static_cast<char const *>(v.mv_data),
                                       v.mv_size});
            rc = mdb_cursor_get(cursor.get(), &k, &v, MDB_NEXT);
        }
        if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
            throw_error(rc, "cursor");
        }
    }

    MDB_txn *get() const noexcept { return m_txn; }

    [[noreturn]] static void throw_error(int rc, char const *what);

private:
    class cursor_t
    {
    public:
        cursor_t(txn_t const &txn, db_t db);
        ~cursor_t() noexcept;
        cursor_t(cursor_t const &) = delete;
        cursor_t &operator=(cursor_t const &) = delete;
        cursor_t(cursor_t &&) = delete;
        cursor_t &operator=(cursor_t &&) = delete;
        MDB_cursor *get() const noexcept { return m_cursor; }

    private:
        MDB_cursor *m_cursor = nullptr;
    };

    store_t const *m_store;
    MDB_txn *m_txn = nullptr;
    bool m_read_only;
}; // class txn_t

} // namespace coda

#endif // OSM2PGSQL_CODA_STORE_HPP
