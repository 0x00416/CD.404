#include <cd404/ui/playback_presenter.hpp>

#include <cd404/platform/windows/device_lifecycle.hpp>

#include <cstdint>
#include <format>

namespace cd404::ui {

std::wstring playback_error_message(
    const platform::windows::CddaPlaybackResult& result)
{
    using platform::windows::CddaPlaybackError;

    switch (result.error) {
    case CddaPlaybackError::no_ready_audio_cd:
        return L"当前光盘已不可用";
    case CddaPlaybackError::source_open_failed:
    case CddaPlaybackError::read_failed:
        return L"读取光盘失败：" +
            platform::windows::format_system_error(result.system_error);
    case CddaPlaybackError::endpoint_underrun:
        return L"光驱供给不足，播放已停止";
    case CddaPlaybackError::output_open_failed:
    case CddaPlaybackError::output_failed:
        if (platform::windows::is_recoverable_default_endpoint_failure(result)) {
            return std::format(
                L"默认音频设备恢复失败：0x{:08X}，请检查输出设备",
                static_cast<std::uint32_t>(result.audio_status));
        }
        return L"音频设备错误：" +
            platform::windows::describe_wasapi_status(result.audio_status);
    default:
        return L"播放未能完成";
    }
}

} // namespace cd404::ui
