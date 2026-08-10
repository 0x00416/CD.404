#pragma once

#include <cd404/disc/gnudb.hpp>

#include <optional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows {

struct GnudbMetadata final {
    std::wstring album_title;
    std::wstring album_artist;
    std::wstring category;
    std::wstring year;
    std::vector<std::wstring> track_titles;
    std::vector<std::wstring> track_artists;
    std::vector<std::wstring> cover_urls;
    std::vector<std::wstring> cover_art_ids;
    unsigned int revision{};
};

struct CddbServerEndpoint final {
    std::wstring host;
    std::wstring query_path;
    std::wstring submit_path;
    unsigned short port{};
    bool secure{};
};

struct CddbClientConfiguration final {
    std::wstring server{L"gnudb.gnudb.org"};
    std::wstring email;
};

struct GnudbLookupResult final {
    std::optional<GnudbMetadata> metadata;
    unsigned long system_error{};
    unsigned long http_status{};
    unsigned int protocol_status{};
    std::wstring message;
};

struct GnudbQueryMatch final {
    std::string category;
    std::string gnucdid;
};

struct GnudbQueryResponse final {
    std::optional<GnudbQueryMatch> match;
    unsigned int protocol_status{};
    std::string message;
};

enum class CddbSubmissionMode {
    test,
    submit,
};

struct CddbSubmissionResult final {
    bool accepted{};
    unsigned long system_error{};
    unsigned long http_status{};
    std::wstring message;
};

struct CddbSubmissionRequest final {
    CddbServerEndpoint endpoint;
    std::wstring headers;
    std::vector<std::uint8_t> body;
};

[[nodiscard]] std::optional<CddbServerEndpoint> parse_cddb_server(
    std::wstring_view server);
[[nodiscard]] bool is_valid_cddb_email(std::wstring_view email) noexcept;
[[nodiscard]] GnudbQueryResponse parse_gnudb_query_response(
    std::string_view response);

[[nodiscard]] std::optional<CddbSubmissionRequest> build_cddb_submission_request(
    const CddbClientConfiguration& configuration,
    const disc::GnudbSubmissionMetadataUtf8& metadata,
    const disc::Toc& toc,
    CddbSubmissionMode mode);

[[nodiscard]] GnudbLookupResult lookup_gnudb(
    const disc::Toc& toc,
    const CddbClientConfiguration& configuration = {});

[[nodiscard]] CddbSubmissionResult submit_gnudb(
    const CddbClientConfiguration& configuration,
    const disc::GnudbSubmissionMetadataUtf8& metadata,
    const disc::Toc& toc,
    CddbSubmissionMode mode);

} // namespace cd404::platform::windows
