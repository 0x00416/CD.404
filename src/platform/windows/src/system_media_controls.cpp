#include <windows.h>

#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>

#include <cd404/platform/windows/system_media_controls.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace cd404::platform::windows {
namespace {

using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Media::MediaPlaybackStatus;
using winrt::Windows::Media::MediaPlaybackType;
using winrt::Windows::Media::SystemMediaTransportControls;
using winrt::Windows::Media::SystemMediaTransportControlsButton;
using winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties;

[[nodiscard]] TimeSpan milliseconds_to_timespan(
    const std::uint64_t milliseconds) noexcept
{
    return std::chrono::duration_cast<TimeSpan>(
        std::chrono::milliseconds(milliseconds));
}

} // namespace

struct SystemMediaControls::Implementation final {
    ~Implementation()
    {
        if (controls) {
            try {
                controls.ButtonPressed(button_token);
                controls.PlaybackPositionChangeRequested(position_token);
                controls.IsEnabled(false);
            } catch (const winrt::hresult_error&) {
            }
        }
    }

    SystemMediaTransportControls controls{nullptr};
    RequestCallback callback;
    winrt::event_token button_token{};
    winrt::event_token position_token{};
};

SystemMediaControls::SystemMediaControls()
    : implementation_(std::make_unique<Implementation>())
{
}

SystemMediaControls::~SystemMediaControls() = default;

bool SystemMediaControls::initialize(
    void* const window_handle,
    RequestCallback callback)
{
    if (window_handle == nullptr || !callback || implementation_ == nullptr) {
        return false;
    }

    try {
        auto interop = winrt::get_activation_factory<
            SystemMediaTransportControls,
            ISystemMediaTransportControlsInterop>();
        SystemMediaTransportControls controls{nullptr};
        winrt::check_hresult(interop->GetForWindow(
            static_cast<HWND>(window_handle),
            winrt::guid_of<SystemMediaTransportControls>(),
            winrt::put_abi(controls)));

        implementation_->controls = controls;
        implementation_->callback = std::move(callback);
        controls.IsEnabled(true);
        controls.IsPlayEnabled(true);
        controls.IsPauseEnabled(true);
        controls.IsStopEnabled(true);
        controls.IsPreviousEnabled(true);
        controls.IsNextEnabled(true);
        implementation_->button_token = controls.ButtonPressed(
            [this](const auto&, const auto& arguments) {
                if (implementation_ == nullptr || !implementation_->callback) {
                    return;
                }
                SystemMediaRequest request;
                switch (arguments.Button()) {
                case SystemMediaTransportControlsButton::Play:
                    request.command = SystemMediaCommand::play;
                    break;
                case SystemMediaTransportControlsButton::Pause:
                    request.command = SystemMediaCommand::pause;
                    break;
                case SystemMediaTransportControlsButton::Stop:
                    request.command = SystemMediaCommand::stop;
                    break;
                case SystemMediaTransportControlsButton::Previous:
                    request.command = SystemMediaCommand::previous;
                    break;
                case SystemMediaTransportControlsButton::Next:
                    request.command = SystemMediaCommand::next;
                    break;
                default:
                    return;
                }
                implementation_->callback(request);
            });
        implementation_->position_token = controls.PlaybackPositionChangeRequested(
            [this](const auto&, const auto& arguments) {
                if (implementation_ == nullptr || !implementation_->callback) {
                    return;
                }
                const auto requested = std::chrono::duration_cast<std::chrono::milliseconds>(
                    arguments.RequestedPlaybackPosition());
                implementation_->callback(SystemMediaRequest{
                    SystemMediaCommand::seek,
                    requested.count() < 0
                        ? 0U
                        : static_cast<std::uint64_t>(requested.count()),
                });
            });
        set_playback_state(SystemMediaPlaybackState::stopped);
        return true;
    } catch (const winrt::hresult_error&) {
        implementation_->controls = nullptr;
        implementation_->callback = {};
        return false;
    }
}

bool SystemMediaControls::available() const noexcept
{
    return implementation_ != nullptr && implementation_->controls != nullptr;
}

void SystemMediaControls::update_metadata(
    const std::wstring_view title,
    const std::wstring_view artist,
    const std::wstring_view album)
{
    if (!available()) {
        return;
    }
    try {
        const auto updater = implementation_->controls.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);
        const auto properties = updater.MusicProperties();
        properties.Title(title);
        properties.Artist(artist);
        properties.AlbumTitle(album);
        updater.Update();
    } catch (const winrt::hresult_error&) {
    }
}

void SystemMediaControls::update_timeline(
    const std::uint64_t position_milliseconds,
    const std::uint64_t duration_milliseconds)
{
    if (!available()) {
        return;
    }
    try {
        SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(milliseconds_to_timespan(0));
        timeline.MinSeekTime(milliseconds_to_timespan(0));
        timeline.Position(milliseconds_to_timespan(
            std::min(position_milliseconds, duration_milliseconds)));
        timeline.MaxSeekTime(milliseconds_to_timespan(duration_milliseconds));
        timeline.EndTime(milliseconds_to_timespan(duration_milliseconds));
        implementation_->controls.UpdateTimelineProperties(timeline);
    } catch (const winrt::hresult_error&) {
    }
}

void SystemMediaControls::set_playback_state(
    const SystemMediaPlaybackState state)
{
    if (!available()) {
        return;
    }
    try {
        MediaPlaybackStatus status = MediaPlaybackStatus::Closed;
        switch (state) {
        case SystemMediaPlaybackState::closed:
            status = MediaPlaybackStatus::Closed;
            break;
        case SystemMediaPlaybackState::stopped:
            status = MediaPlaybackStatus::Stopped;
            break;
        case SystemMediaPlaybackState::playing:
            status = MediaPlaybackStatus::Playing;
            break;
        case SystemMediaPlaybackState::paused:
            status = MediaPlaybackStatus::Paused;
            break;
        }
        implementation_->controls.PlaybackStatus(status);
    } catch (const winrt::hresult_error&) {
    }
}

void SystemMediaControls::clear()
{
    if (!available()) {
        return;
    }
    try {
        const auto updater = implementation_->controls.DisplayUpdater();
        updater.ClearAll();
        updater.Update();
        set_playback_state(SystemMediaPlaybackState::closed);
    } catch (const winrt::hresult_error&) {
    }
}

} // namespace cd404::platform::windows
