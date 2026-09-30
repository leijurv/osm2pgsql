/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "coda-source.hpp"

#include "format.hpp"
#include "logging.hpp"

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace coda {

namespace {

#ifndef _WIN32
class file_source_t : public byte_source_t
{
public:
    explicit file_source_t(std::string const &filename)
    : m_fd(::open(filename.c_str(), O_RDONLY))
    {
        if (m_fd < 0) {
            throw std::system_error{errno, std::system_category(),
                                    "Opening '" + filename + "' failed"};
        }
        struct stat st{};
        if (::fstat(m_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(m_fd);
            throw std::runtime_error{"not a regular file"};
        }
        m_size = static_cast<std::uint64_t>(st.st_size);
    }

    ~file_source_t() noexcept override { ::close(m_fd); }

    file_source_t(file_source_t const &) = delete;
    file_source_t &operator=(file_source_t const &) = delete;
    file_source_t(file_source_t &&) = delete;
    file_source_t &operator=(file_source_t &&) = delete;

    std::uint64_t size() const noexcept override { return m_size; }

    std::string read(std::uint64_t offset, std::size_t length) const override
    {
        if (offset >= m_size) {
            return {};
        }
        length = static_cast<std::size_t>(
            std::min<std::uint64_t>(length, m_size - offset));
        std::string data(length, '\0');
        std::size_t done = 0;
        while (done < length) {
            auto const n = ::pread(m_fd, data.data() + done, length - done,
                                   static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                throw std::system_error{errno, std::system_category(),
                                        "Reading input file failed"};
            }
            done += static_cast<std::size_t>(n);
        }
        return data;
    }

    unsigned parallelism() const noexcept override { return 1; }

private:
    int m_fd;
    std::uint64_t m_size = 0;
};
#endif

#ifdef HAVE_CURL
constexpr unsigned HTTP_CONNECTIONS = 16;
constexpr int HTTP_TRIES = 6;

std::size_t append_data(char *ptr, std::size_t size, std::size_t nmemb,
                        void *userdata)
{
    static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

class http_source_t : public byte_source_t
{
public:
    explicit http_source_t(std::string url) : m_url(std::move(url))
    {
        static std::once_flag init;
        std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

        auto *curl = handle();
        curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        auto const rc = curl_easy_perform(curl);
        long status = 0; // NOLINT(google-runtime-int) curl API
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_off_t length = -1;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
        if (rc != CURLE_OK || status != 200 || length <= 0) {
            throw fmt_error("HEAD request for '{}' failed ({}, HTTP {}).",
                            m_url, curl_easy_strerror(rc), status);
        }
        m_size = static_cast<std::uint64_t>(length);
    }

    std::uint64_t size() const noexcept override { return m_size; }

    std::string read(std::uint64_t offset, std::size_t length) const override
    {
        if (offset >= m_size || length == 0) {
            return {};
        }
        length = static_cast<std::size_t>(
            std::min<std::uint64_t>(length, m_size - offset));
        auto const range = fmt::format("{}-{}", offset, offset + length - 1);
        for (int attempt = 1;; ++attempt) {
            std::string data;
            data.reserve(length);
            auto *curl = handle();
            curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
            curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_data);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
            auto const rc = curl_easy_perform(curl);
            long status = 0; // NOLINT(google-runtime-int) curl API
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_setopt(curl, CURLOPT_RANGE, nullptr);
            if (rc == CURLE_OK && status == 206 && data.size() == length) {
                return data;
            }
            if (attempt == HTTP_TRIES) {
                throw fmt_error("Reading bytes {} of '{}' failed ({}, HTTP "
                                "{}).",
                                range, m_url, curl_easy_strerror(rc), status);
            }
            log_debug("Retrying bytes {} of '{}' ({}, HTTP {}).", range, m_url,
                      curl_easy_strerror(rc), status);
            std::this_thread::sleep_for(
                std::chrono::seconds{1U << static_cast<unsigned>(attempt)});
        }
    }

    unsigned parallelism() const noexcept override { return HTTP_CONNECTIONS; }

private:
    /// One handle per thread, so connections are kept alive.
    static CURL *handle()
    {
        struct handle_t
        {
            CURL *curl = curl_easy_init();
            handle_t() = default;
            handle_t(handle_t const &) = delete;
            handle_t &operator=(handle_t const &) = delete;
            handle_t(handle_t &&) = delete;
            handle_t &operator=(handle_t &&) = delete;
            ~handle_t() noexcept { curl_easy_cleanup(curl); }
        };
        thread_local handle_t const h;
        curl_easy_setopt(h.curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h.curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(h.curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(h.curl, CURLOPT_USERAGENT, "osm2pgsql");
        return h.curl;
    }

    std::string m_url;
    std::uint64_t m_size = 0;
};
#endif

bool is_url(std::string const &name)
{
    return name.rfind("http://", 0) == 0 || name.rfind("https://", 0) == 0;
}

} // anonymous namespace

std::unique_ptr<byte_source_t> byte_source_t::open(std::string const &name)
{
    if (name.empty() || name == "-") {
        return nullptr;
    }
    if (is_url(name)) {
#ifdef HAVE_CURL
        return std::make_unique<http_source_t>(name);
#else
        return nullptr;
#endif
    }
#ifdef _WIN32
    return nullptr;
#else
    return std::make_unique<file_source_t>(name);
#endif
}

} // namespace coda
