/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "coda-store.hpp"

#include "format.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace coda {

static_assert(sizeof(id_type) == sizeof(std::size_t),
              "The CODA middle needs a 64 bit system.");

namespace {

constexpr std::size_t MAP_SIZE = 1ULL << 42U; // 4 TB of address space
constexpr unsigned MAX_READERS = 1024;

void check(int rc, char const *what)
{
    if (rc != MDB_SUCCESS) {
        txn_t::throw_error(rc, what);
    }
}

std::string const &meta_format_key()
{
    static std::string const key{"format_version"};
    return key;
}

/// LMDB takes keys and values as non-const pointers, but doesn't change them.
MDB_val to_val(std::string_view data) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return MDB_val{data.size(), const_cast<char *>(data.data())};
}

} // anonymous namespace

char const *db_name(db_t db) noexcept
{
    static std::array<char const *, NUM_DBS> const names = {
        "ways", "nodes", "rels", "n2w", "n2r", "w2r", "r2r", "meta"};
    return names.at(static_cast<unsigned>(db));
}

unsigned index_shift(db_t db) noexcept
{
    return db == db_t::n2w ? N2W_SHIFT : X2R_SHIFT;
}

store_t::store_t(std::string dir, mode_t mode) : m_dir(std::move(dir))
{
    std::filesystem::path const path{m_dir};
    if (mode == mode_t::create) {
        std::filesystem::create_directories(path);
        std::filesystem::remove(path / "data.mdb");
        std::filesystem::remove(path / "lock.mdb");
    } else if (!std::filesystem::exists(path / "data.mdb")) {
        throw fmt_error("No CODA middle found in directory '{}'.", m_dir);
    }

    check(mdb_env_create(&m_env), "env_create");
    try {
        check(mdb_env_set_mapsize(m_env, MAP_SIZE), "set_mapsize");
        check(mdb_env_set_maxdbs(m_env, NUM_DBS), "set_maxdbs");
        check(mdb_env_set_maxreaders(m_env, MAX_READERS), "set_maxreaders");

        // Transactions are not tied to threads, readers renew them.
        unsigned flags = MDB_NOTLS;
        if (mode == mode_t::create) {
            flags |= MDB_NOSYNC; // sync() at the end
        }
        if (mode == mode_t::update) {
            flags |= MDB_NORDAHEAD; // random access only
        }
        check(mdb_env_open(m_env, m_dir.c_str(), flags, 0644), "env_open");

        txn_t txn{*this, false};
        for (unsigned i = 0; i < NUM_DBS; ++i) {
            auto const db = static_cast<db_t>(i);
            unsigned const db_flags =
                MDB_CREATE | (db == db_t::meta ? 0U : MDB_INTEGERKEY);
            check(mdb_dbi_open(txn.get(), db_name(db), db_flags, &m_dbi.at(i)),
                  "dbi_open");
        }

        if (mode == mode_t::create) {
            txn.put_meta(meta_format_key(), std::to_string(FORMAT_VERSION));
            txn.put_meta("dict_entries", tag_dict_t::default_entries());
            txn.put_meta("dict_zstd", tag_dict_t::default_zstd_dict());
        } else {
            auto const version = txn.get_meta(meta_format_key());
            if (!version || *version != std::to_string(FORMAT_VERSION)) {
                throw fmt_error("CODA middle in '{}' has format version '{}', "
                                "this version of osm2pgsql needs '{}'.",
                                m_dir, version ? *version : "(none)",
                                FORMAT_VERSION);
            }
        }
        m_dict = std::make_unique<tag_dict_t>(
            txn.get_meta("dict_entries").value_or(""),
            txn.get_meta("dict_zstd").value_or(""));
        txn.commit();
    } catch (...) {
        mdb_env_close(m_env);
        throw;
    }
}

store_t::~store_t() noexcept { mdb_env_close(m_env); }

void store_t::sync() { check(mdb_env_sync(m_env, 1), "env_sync"); }

std::pair<std::uint64_t, std::uint64_t> store_t::file_and_free_size() const
{
    MDB_envinfo info;
    check(mdb_env_info(m_env, &info), "env_info");
    MDB_stat stat;
    check(mdb_env_stat(m_env, &stat), "env_stat");

    // The freelist is database 0, values are page lists starting with
    // their length.
    std::uint64_t free_pages = 0;
    txn_t const txn{*this, true};
    MDB_cursor *cursor = nullptr;
    check(mdb_cursor_open(txn.get(), 0, &cursor), "cursor_open");
    MDB_val k;
    MDB_val v;
    for (int rc = mdb_cursor_get(cursor, &k, &v, MDB_FIRST); rc == MDB_SUCCESS;
         rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT)) {
        std::size_t n = 0; // first element of the page list
        std::memcpy(&n, v.mv_data, sizeof(n));
        free_pages += n;
    }
    mdb_cursor_close(cursor);

    return {(info.me_last_pgno + 1) * stat.ms_psize,
            free_pages * stat.ms_psize};
}

// ---------------------------------------------------------------- txn_t

void txn_t::throw_error(int rc, char const *what)
{
    throw fmt_error("CODA middle: {} failed: {}", what, mdb_strerror(rc));
}

txn_t::txn_t(store_t const &store, bool read_only)
: m_store(&store), m_read_only(read_only)
{
    check(mdb_txn_begin(store.env(), nullptr, read_only ? MDB_RDONLY : 0U,
                        &m_txn),
          "txn_begin");
}

txn_t::~txn_t() noexcept
{
    if (m_txn) {
        mdb_txn_abort(m_txn);
    }
}

void txn_t::commit()
{
    auto *txn = m_txn;
    m_txn = nullptr;
    check(mdb_txn_commit(txn), "txn_commit");
    if (!m_read_only) {
        m_store->bump_generation();
    }
}

void txn_t::reset() noexcept { mdb_txn_reset(m_txn); }

void txn_t::renew() { check(mdb_txn_renew(m_txn), "txn_renew"); }

std::optional<std::string_view> txn_t::get(db_t db, id_type key) const
{
    MDB_val k{sizeof(key), &key};
    MDB_val v;
    int const rc = mdb_get(m_txn, m_store->dbi(db), &k, &v);
    if (rc == MDB_NOTFOUND) {
        return std::nullopt;
    }
    check(rc, "get");
    return std::string_view{static_cast<char const *>(v.mv_data), v.mv_size};
}

void txn_t::put(db_t db, id_type key, std::string_view value, bool append)
{
    MDB_val k{sizeof(key), &key};
    MDB_val v = to_val(value);
    check(mdb_put(m_txn, m_store->dbi(db), &k, &v, append ? MDB_APPEND : 0U),
          "put");
}

void txn_t::del(db_t db, id_type key)
{
    MDB_val k{sizeof(key), &key};
    int const rc = mdb_del(m_txn, m_store->dbi(db), &k, nullptr);
    if (rc != MDB_NOTFOUND) {
        check(rc, "del");
    }
}

std::optional<std::string_view> txn_t::get_meta(std::string_view key) const
{
    MDB_val k = to_val(key);
    MDB_val v;
    int const rc = mdb_get(m_txn, m_store->dbi(db_t::meta), &k, &v);
    if (rc == MDB_NOTFOUND) {
        return std::nullopt;
    }
    check(rc, "get");
    return std::string_view{static_cast<char const *>(v.mv_data), v.mv_size};
}

void txn_t::put_meta(std::string_view key, std::string_view value)
{
    MDB_val k = to_val(key);
    MDB_val v = to_val(value);
    check(mdb_put(m_txn, m_store->dbi(db_t::meta), &k, &v, 0), "put");
}

txn_t::cursor_t::cursor_t(txn_t const &txn, db_t db)
{
    check(mdb_cursor_open(txn.get(), txn.m_store->dbi(db), &m_cursor),
          "cursor_open");
}

txn_t::cursor_t::~cursor_t() noexcept { mdb_cursor_close(m_cursor); }

} // namespace coda
