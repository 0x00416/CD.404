#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cd404::platform::windows {

struct ListenBrainzPendingListen final {
    std::int64_t id{};
    std::string payload;
    unsigned int attempts{};
    std::int64_t next_attempt{};
};

[[nodiscard]] std::filesystem::path default_listenbrainz_queue_path();

class ListenBrainzQueue final {
public:
    explicit ListenBrainzQueue(
        std::filesystem::path path = default_listenbrainz_queue_path());
    ~ListenBrainzQueue();

    ListenBrainzQueue(const ListenBrainzQueue&) = delete;
    ListenBrainzQueue& operator=(const ListenBrainzQueue&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] int schema_version() const noexcept;
    [[nodiscard]] bool enqueue(
        std::string_view owner_key,
        std::string_view session_id,
        std::string_view payload) noexcept;
    [[nodiscard]] std::optional<ListenBrainzPendingListen> next(
        std::string_view owner_key) const noexcept;
    [[nodiscard]] bool complete(std::int64_t id) noexcept;
    [[nodiscard]] bool schedule_retry(
        std::int64_t id,
        unsigned int attempts,
        std::int64_t next_attempt,
        unsigned long status,
        unsigned long system_error) noexcept;
    [[nodiscard]] bool mark_failed(
        std::int64_t id,
        unsigned long status,
        unsigned long system_error) noexcept;
    [[nodiscard]] bool retry_all(std::string_view owner_key) noexcept;
    [[nodiscard]] bool clear_owner(std::string_view owner_key) noexcept;
    [[nodiscard]] std::size_t pending_count(
        std::string_view owner_key) const noexcept;
    [[nodiscard]] std::size_t failed_count(
        std::string_view owner_key) const noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace cd404::platform::windows
