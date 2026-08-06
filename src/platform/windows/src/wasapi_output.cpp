#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cd404/platform/windows/wasapi_output.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>

namespace cd404::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kEventWaitMilliseconds = 2'000;

[[nodiscard]] constexpr HRESULT invalid_state() noexcept
{
    return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
}

[[nodiscard]] HRESULT wait_for_audio_event(const HANDLE event_handle) noexcept
{
    const DWORD wait_result = WaitForSingleObject(event_handle, kEventWaitMilliseconds);
    if (wait_result == WAIT_OBJECT_0) {
        return S_OK;
    }
    if (wait_result == WAIT_FAILED) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

} // namespace

struct WasapiOutput::Implementation final {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> audio_client;
    ComPtr<IAudioRenderClient> render_client;
    HANDLE buffer_event{};
    DWORD owner_thread{};
    UINT32 buffer_frames{};
    bool com_owned{};
    bool started{};
    bool open{};

    ~Implementation()
    {
        release_resources();
        if (com_owned && owner_thread == GetCurrentThreadId()) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT check_thread() const noexcept
    {
        return owner_thread == 0 || owner_thread == GetCurrentThreadId()
            ? S_OK
            : RPC_E_WRONG_THREAD;
    }

    void release_resources() noexcept
    {
        if (audio_client && started) {
            static_cast<void>(audio_client->Stop());
        }
        started = false;
        open = false;
        buffer_frames = 0;
        render_client.Reset();
        audio_client.Reset();
        device.Reset();
        enumerator.Reset();
        if (buffer_event != nullptr) {
            CloseHandle(buffer_event);
            buffer_event = nullptr;
        }
    }
};

WasapiOutput::WasapiOutput() noexcept = default;

WasapiOutput::~WasapiOutput()
{
    delete implementation_;
}

std::int32_t WasapiOutput::open_default_shared() noexcept
{
    if (implementation_ == nullptr) {
        implementation_ = new (std::nothrow) Implementation;
        if (implementation_ == nullptr) {
            return E_OUTOFMEMORY;
        }
    }

    Implementation& state = *implementation_;
    HRESULT result = state.check_thread();
    if (FAILED(result)) {
        return result;
    }
    if (state.open) {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    }

    if (state.owner_thread == 0) {
        state.owner_thread = GetCurrentThreadId();
        result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == S_OK || result == S_FALSE) {
            state.com_owned = true;
        } else if (result != RPC_E_CHANGED_MODE) {
            state.owner_thread = 0;
            return result;
        }
    }

    result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&state.enumerator));
    if (FAILED(result)) {
        state.release_resources();
        return result;
    }

    result = state.enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &state.device);
    if (FAILED(result)) {
        state.release_resources();
        return result;
    }

    result = state.device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_INPROC_SERVER,
        nullptr,
        reinterpret_cast<void**>(state.audio_client.GetAddressOf()));
    if (FAILED(result)) {
        state.release_resources();
        return result;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = channel_count;
    format.nSamplesPerSec = sample_rate;
    format.wBitsPerSample = bits_per_sample;
    format.nBlockAlign = static_cast<WORD>(channel_count * (bits_per_sample / 8));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    constexpr DWORD stream_flags =
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_NOPERSIST |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    result = state.audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        0,
        0,
        &format,
        nullptr);
    if (FAILED(result)) {
        state.release_resources();
        return result;
    }

    result = state.audio_client->GetBufferSize(&state.buffer_frames);
    if (FAILED(result)) {
        state.release_resources();
        return result;
    }

    state.buffer_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (state.buffer_event == nullptr) {
        result = HRESULT_FROM_WIN32(GetLastError());
        state.release_resources();
        return result;
    }

    result = state.audio_client->SetEventHandle(state.buffer_event);
    if (FAILED(result)) {
        state.release_resources();
        return result;
    }

    result = state.audio_client->GetService(IID_PPV_ARGS(&state.render_client));
    if (FAILED(result)) {
        state.release_resources();
        return result;
    }

    state.open = true;
    return S_OK;
}

WasapiWriteResult WasapiOutput::write_interleaved(
    const std::span<const std::int16_t> samples,
    const std::uint32_t frame_count) noexcept
{
    if (implementation_ == nullptr || !implementation_->open) {
        return WasapiWriteResult{invalid_state(), 0};
    }

    Implementation& state = *implementation_;
    HRESULT result = state.check_thread();
    if (FAILED(result)) {
        return WasapiWriteResult{result, 0};
    }
    if (frame_count > std::numeric_limits<std::size_t>::max() / channel_count ||
        samples.size() < static_cast<std::size_t>(frame_count) * channel_count) {
        return WasapiWriteResult{E_INVALIDARG, 0};
    }
    if (frame_count == 0) {
        return WasapiWriteResult{S_OK, 0};
    }

    std::uint32_t written{};
    while (written < frame_count) {
        UINT32 padding{};
        result = state.audio_client->GetCurrentPadding(&padding);
        if (FAILED(result)) {
            return WasapiWriteResult{result, written};
        }
        if (padding > state.buffer_frames) {
            return WasapiWriteResult{E_UNEXPECTED, written};
        }

        const UINT32 available = state.buffer_frames - padding;
        if (available == 0) {
            result = wait_for_audio_event(state.buffer_event);
            if (FAILED(result)) {
                return WasapiWriteResult{result, written};
            }
            continue;
        }

        const UINT32 frames_to_write = std::min(available, frame_count - written);
        BYTE* destination{};
        result = state.render_client->GetBuffer(frames_to_write, &destination);
        if (FAILED(result)) {
            return WasapiWriteResult{result, written};
        }

        const auto source_offset = static_cast<std::size_t>(written) * channel_count;
        const auto byte_count =
            static_cast<std::size_t>(frames_to_write) * channel_count * sizeof(std::int16_t);
        std::memcpy(destination, samples.data() + source_offset, byte_count);
        result = state.render_client->ReleaseBuffer(frames_to_write, 0);
        if (FAILED(result)) {
            return WasapiWriteResult{result, written};
        }
        written += frames_to_write;

        if (!state.started) {
            result = state.audio_client->Start();
            if (FAILED(result)) {
                return WasapiWriteResult{result, written};
            }
            state.started = true;
        }
    }

    return WasapiWriteResult{S_OK, written};
}

std::int32_t WasapiOutput::drain() noexcept
{
    if (implementation_ == nullptr || !implementation_->open) {
        return invalid_state();
    }

    Implementation& state = *implementation_;
    HRESULT result = state.check_thread();
    if (FAILED(result)) {
        return result;
    }
    if (!state.started) {
        return S_OK;
    }

    for (;;) {
        UINT32 padding{};
        result = state.audio_client->GetCurrentPadding(&padding);
        if (FAILED(result) || padding == 0) {
            return result;
        }
        result = wait_for_audio_event(state.buffer_event);
        if (FAILED(result)) {
            return result;
        }
    }
}

std::int32_t WasapiOutput::stop() noexcept
{
    if (implementation_ == nullptr) {
        return S_OK;
    }
    Implementation& state = *implementation_;
    HRESULT result = state.check_thread();
    if (FAILED(result)) {
        return result;
    }
    if (!state.open || !state.started) {
        return S_OK;
    }

    result = state.audio_client->Stop();
    if (FAILED(result)) {
        return result;
    }
    state.started = false;
    return state.audio_client->Reset();
}

void WasapiOutput::close() noexcept
{
    if (implementation_ == nullptr || FAILED(implementation_->check_thread())) {
        return;
    }
    implementation_->release_resources();
}

bool WasapiOutput::is_open() const noexcept
{
    return implementation_ != nullptr && implementation_->open;
}

std::uint32_t WasapiOutput::buffer_frame_count() const noexcept
{
    return implementation_ == nullptr ? 0 : implementation_->buffer_frames;
}

} // namespace cd404::platform::windows
