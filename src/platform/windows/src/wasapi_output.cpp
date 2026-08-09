#include <windows.h>

#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <cd404/platform/windows/wasapi_output.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <new>
#include <utility>

namespace cd404::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kEventWaitMilliseconds = 2'000;

[[nodiscard]] constexpr HRESULT invalid_state() noexcept
{
    return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
}

[[nodiscard]] constexpr HRESULT operation_cancelled() noexcept
{
    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
}

[[nodiscard]] HRESULT wait_for_audio_event(
    const HANDLE audio_event,
    const HANDLE cancel_event) noexcept
{
    if (audio_event == nullptr) {
        const DWORD wait_result = WaitForSingleObject(cancel_event, 2);
        if (wait_result == WAIT_OBJECT_0) {
            return operation_cancelled();
        }
        if (wait_result == WAIT_TIMEOUT) {
            return S_OK;
        }
        return HRESULT_FROM_WIN32(GetLastError());
    }
    // Cancellation is first so it wins when both handles are already signaled.
    const HANDLE handles[]{cancel_event, audio_event};
    const DWORD wait_result = WaitForMultipleObjects(
        static_cast<DWORD>(std::size(handles)),
        handles,
        FALSE,
        kEventWaitMilliseconds);
    if (wait_result == WAIT_OBJECT_0) {
        return operation_cancelled();
    }
    if (wait_result == WAIT_OBJECT_0 + 1) {
        return S_OK;
    }
    if (wait_result == WAIT_FAILED) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

} // namespace

WasapiOpenResult open_wasapi_session(
    IWasapiSessionBackend& backend,
    const WasapiOpenOptions& options) noexcept
{
    WasapiOpenResult result;
    result.requested_mode = options.mode;
    result.actual_mode = options.mode;
    const WasapiPcmFormat format;

    if (options.mode == WasapiShareMode::shared) {
        result.status = backend.initialize(
            options.endpoint_id,
            WasapiShareMode::shared,
            format);
        return result;
    }

    const std::int32_t support = backend.query_format_support(
        options.endpoint_id,
        WasapiShareMode::exclusive,
        format);
    if (support >= 0) {
        result.status = backend.initialize(
            options.endpoint_id,
            WasapiShareMode::exclusive,
            format);
    } else {
        result.status = support;
    }
    if (result.status >= 0 || !options.allow_shared_fallback) {
        return result;
    }

    result.fallback_attempted = true;
    result.fallback_reason = result.status;
    result.actual_mode = WasapiShareMode::shared;
    result.status = backend.initialize(
        options.endpoint_id,
        WasapiShareMode::shared,
        format);
    return result;
}

const wchar_t* to_string(const WasapiShareMode mode) noexcept
{
    return mode == WasapiShareMode::exclusive ? L"独占" : L"共享";
}

bool uses_event_driven_wasapi_buffering(const WasapiShareMode mode) noexcept
{
    return mode == WasapiShareMode::shared;
}

std::wstring describe_wasapi_status(const std::int32_t status)
{
    switch (status) {
    case S_OK:
        return L"成功";
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return L"设备不支持 44.1 kHz / 16 位 / 双声道 PCM";
    case AUDCLNT_E_DEVICE_IN_USE:
        return L"输出设备正被其他应用独占";
    case AUDCLNT_E_DEVICE_INVALIDATED:
        return L"所选输出设备已移除或失效";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return L"Windows 音频服务未运行";
    case E_ACCESSDENIED:
        return L"没有访问输出设备的权限";
    default:
        return std::format(L"WASAPI 错误 0x{:08X}", static_cast<unsigned int>(status));
    }
}

std::vector<WasapiEndpoint> enumerate_wasapi_render_endpoints(
    std::int32_t* status) noexcept
{
    if (status != nullptr) {
        *status = S_OK;
    }
    std::vector<WasapiEndpoint> endpoints;
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = com_status == S_OK || com_status == S_FALSE;
    if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
        if (status != nullptr) {
            *status = com_status;
        }
        return endpoints;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&enumerator));
    ComPtr<IMMDeviceCollection> collection;
    if (SUCCEEDED(result)) {
        result = enumerator->EnumAudioEndpoints(
            eRender,
            DEVICE_STATE_ACTIVE,
            &collection);
    }

    std::wstring default_id;
    if (SUCCEEDED(result)) {
        ComPtr<IMMDevice> default_device;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
                eRender,
                eMultimedia,
                &default_device))) {
            LPWSTR id{};
            if (SUCCEEDED(default_device->GetId(&id)) && id != nullptr) {
                default_id = id;
                CoTaskMemFree(id);
            }
        }
    }

    UINT count{};
    if (SUCCEEDED(result)) {
        result = collection->GetCount(&count);
    }
    for (UINT index = 0; SUCCEEDED(result) && index < count; ++index) {
        ComPtr<IMMDevice> device;
        result = collection->Item(index, &device);
        if (FAILED(result)) {
            break;
        }
        LPWSTR raw_id{};
        result = device->GetId(&raw_id);
        if (FAILED(result) || raw_id == nullptr) {
            break;
        }
        WasapiEndpoint endpoint;
        endpoint.id = raw_id;
        CoTaskMemFree(raw_id);
        endpoint.is_default = endpoint.id == default_id;

        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
                endpoint.name = value.pwszVal;
            }
            PropVariantClear(&value);
        }
        if (endpoint.name.empty()) {
            endpoint.name = endpoint.id;
        }
        endpoints.push_back(std::move(endpoint));
    }

    if (status != nullptr) {
        *status = result;
    }
    if (uninitialize) {
        CoUninitialize();
    }
    if (FAILED(result)) {
        endpoints.clear();
    }
    return endpoints;
}

struct WasapiOutput::Implementation final {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> audio_client;
    ComPtr<IAudioRenderClient> render_client;
    HANDLE buffer_event{};
    HANDLE cancel_event{};
    DWORD owner_thread{};
    UINT32 buffer_frames{};
    bool com_owned{};
    bool started{};
    bool open{};
    WasapiOpenResult last_open_result{};

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

    void release_session_resources() noexcept
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
        if (buffer_event != nullptr) {
            CloseHandle(buffer_event);
            buffer_event = nullptr;
        }
        if (cancel_event != nullptr) {
            CloseHandle(cancel_event);
            cancel_event = nullptr;
        }
    }

    void release_resources() noexcept
    {
        release_session_resources();
        enumerator.Reset();
    }
};

WasapiOutput::WasapiOutput() noexcept = default;

WasapiOutput::~WasapiOutput()
{
    delete implementation_;
}

std::int32_t WasapiOutput::open_default_shared() noexcept
{
    return open(WasapiOpenOptions{}).status;
}

WasapiOpenResult WasapiOutput::open(const WasapiOpenOptions& options) noexcept
{
    if (implementation_ == nullptr) {
        implementation_ = new (std::nothrow) Implementation;
        if (implementation_ == nullptr) {
            return WasapiOpenResult{E_OUTOFMEMORY, options.mode, options.mode};
        }
    }

    Implementation& state = *implementation_;
    HRESULT result = state.check_thread();
    if (FAILED(result)) {
        return WasapiOpenResult{result, options.mode, options.mode};
    }
    if (state.open) {
        return WasapiOpenResult{
            HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED),
            options.mode,
            options.mode};
    }

    if (state.owner_thread == 0) {
        state.owner_thread = GetCurrentThreadId();
        result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == S_OK || result == S_FALSE) {
            state.com_owned = true;
        } else if (result != RPC_E_CHANGED_MODE) {
            state.owner_thread = 0;
            return WasapiOpenResult{result, options.mode, options.mode};
        }
    }

    result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&state.enumerator));
    if (FAILED(result)) {
        state.release_resources();
        return WasapiOpenResult{result, options.mode, options.mode};
    }

    const auto make_format = [](const WasapiPcmFormat& input) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = input.channel_count;
        format.nSamplesPerSec = input.sample_rate;
        format.wBitsPerSample = input.bits_per_sample;
        format.nBlockAlign = static_cast<WORD>(
            input.channel_count * (input.bits_per_sample / 8));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        return format;
    };
    const auto get_device = [&](
                                const std::wstring_view endpoint_id,
                                ComPtr<IMMDevice>& device) noexcept -> HRESULT {
        if (endpoint_id.empty()) {
            return state.enumerator->GetDefaultAudioEndpoint(
                eRender,
                eMultimedia,
                &device);
        }
        const std::wstring stable_id(endpoint_id);
        return state.enumerator->GetDevice(stable_id.c_str(), &device);
    };
    const auto activate = [&](
                              const std::wstring_view endpoint_id,
                              ComPtr<IMMDevice>& device,
                              ComPtr<IAudioClient>& client) noexcept -> HRESULT {
        HRESULT status = get_device(endpoint_id, device);
        if (FAILED(status)) {
            return status;
        }
        return device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_INPROC_SERVER,
            nullptr,
            reinterpret_cast<void**>(client.GetAddressOf()));
    };

    class ProductionBackend final : public IWasapiSessionBackend {
    public:
        using Callback = std::function<std::int32_t(
            std::wstring_view,
            WasapiShareMode,
            const WasapiPcmFormat&)>;

        ProductionBackend(Callback support, Callback initialize)
            : support_(std::move(support)), initialize_(std::move(initialize))
        {
        }

        std::int32_t query_format_support(
            std::wstring_view endpoint_id,
            WasapiShareMode mode,
            const WasapiPcmFormat& format) noexcept override
        {
            return support_(endpoint_id, mode, format);
        }

        std::int32_t initialize(
            std::wstring_view endpoint_id,
            WasapiShareMode mode,
            const WasapiPcmFormat& format) noexcept override
        {
            return initialize_(endpoint_id, mode, format);
        }

    private:
        Callback support_;
        Callback initialize_;
    };

    ProductionBackend backend(
        [&](const std::wstring_view endpoint_id,
            const WasapiShareMode mode,
            const WasapiPcmFormat& input) noexcept -> std::int32_t {
            ComPtr<IMMDevice> device;
            ComPtr<IAudioClient> client;
            HRESULT status = activate(endpoint_id, device, client);
            if (FAILED(status)) {
                return status;
            }
            WAVEFORMATEX format = make_format(input);
            WAVEFORMATEX* closest{};
            status = client->IsFormatSupported(
                mode == WasapiShareMode::exclusive
                    ? AUDCLNT_SHAREMODE_EXCLUSIVE
                    : AUDCLNT_SHAREMODE_SHARED,
                &format,
                mode == WasapiShareMode::shared ? &closest : nullptr);
            if (closest != nullptr) {
                CoTaskMemFree(closest);
            }
            return status;
        },
        [&](const std::wstring_view endpoint_id,
            const WasapiShareMode mode,
            const WasapiPcmFormat& input) noexcept -> std::int32_t {
            state.release_session_resources();
            HRESULT status = activate(
                endpoint_id,
                state.device,
                state.audio_client);
            if (FAILED(status)) {
                return status;
            }
            WAVEFORMATEX format = make_format(input);
            const bool exclusive = mode == WasapiShareMode::exclusive;
            DWORD stream_flags = AUDCLNT_STREAMFLAGS_NOPERSIST;
            if (uses_event_driven_wasapi_buffering(mode)) {
                stream_flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            }
            REFERENCE_TIME period{};
            if (exclusive) {
                REFERENCE_TIME default_period{};
                status = state.audio_client->GetDevicePeriod(
                    &default_period,
                    &period);
                if (FAILED(status)) {
                    state.release_session_resources();
                    return status;
                }
            } else {
                stream_flags |=
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            }
            status = state.audio_client->Initialize(
                exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE
                          : AUDCLNT_SHAREMODE_SHARED,
                stream_flags,
                exclusive ? period : 0,
                exclusive ? period : 0,
                &format,
                nullptr);
            if (status == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED && exclusive) {
                UINT32 aligned_frames{};
                if (SUCCEEDED(state.audio_client->GetBufferSize(&aligned_frames))) {
                    const REFERENCE_TIME aligned_period = static_cast<REFERENCE_TIME>(
                        (10'000'000ULL * aligned_frames + input.sample_rate - 1) /
                        input.sample_rate);
                    state.audio_client.Reset();
                    state.device.Reset();
                    status = activate(
                        endpoint_id,
                        state.device,
                        state.audio_client);
                    if (SUCCEEDED(status)) {
                        status = state.audio_client->Initialize(
                            AUDCLNT_SHAREMODE_EXCLUSIVE,
                            stream_flags,
                            aligned_period,
                            aligned_period,
                            &format,
                            nullptr);
                    }
                }
            }
            if (FAILED(status)) {
                state.release_session_resources();
                return status;
            }

            status = state.audio_client->GetBufferSize(&state.buffer_frames);
            if (FAILED(status)) {
                state.release_session_resources();
                return status;
            }

            state.cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (state.cancel_event == nullptr) {
                status = HRESULT_FROM_WIN32(GetLastError());
                state.release_session_resources();
                return status;
            }
            if (uses_event_driven_wasapi_buffering(mode)) {
                state.buffer_event =
                    CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (state.buffer_event == nullptr) {
                    status = HRESULT_FROM_WIN32(GetLastError());
                    state.release_session_resources();
                    return status;
                }
                status = state.audio_client->SetEventHandle(state.buffer_event);
                if (FAILED(status)) {
                    state.release_session_resources();
                    return status;
                }
            }
            status = state.audio_client->GetService(
                IID_PPV_ARGS(&state.render_client));
            if (FAILED(status)) {
                state.release_session_resources();
                return status;
            }
            state.open = true;
            return S_OK;
        });

    state.last_open_result = open_wasapi_session(backend, options);
    if (!state.last_open_result.succeeded()) {
        state.release_session_resources();
    }
    return state.last_open_result;
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
        if (WaitForSingleObject(state.cancel_event, 0) == WAIT_OBJECT_0) {
            return WasapiWriteResult{operation_cancelled(), written};
        }

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
            if (!state.started) {
                return WasapiWriteResult{S_FALSE, written};
            }
            result = wait_for_audio_event(state.buffer_event, state.cancel_event);
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
    }

    return WasapiWriteResult{S_OK, written};
}

std::int32_t WasapiOutput::start() noexcept
{
    if (implementation_ == nullptr || !implementation_->open) {
        return invalid_state();
    }

    Implementation& state = *implementation_;
    HRESULT result = state.check_thread();
    if (FAILED(result)) {
        return result;
    }
    if (WaitForSingleObject(state.cancel_event, 0) == WAIT_OBJECT_0) {
        return operation_cancelled();
    }
    if (state.started) {
        return S_OK;
    }

    result = state.audio_client->Start();
    if (SUCCEEDED(result)) {
        state.started = true;
    }
    return result;
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
        if (WaitForSingleObject(state.cancel_event, 0) == WAIT_OBJECT_0) {
            return operation_cancelled();
        }
        UINT32 padding{};
        result = state.audio_client->GetCurrentPadding(&padding);
        if (FAILED(result) || padding == 0) {
            return result;
        }
        result = wait_for_audio_event(state.buffer_event, state.cancel_event);
        if (FAILED(result)) {
            return result;
        }
    }
}

std::int32_t WasapiOutput::pause() noexcept
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

    result = state.audio_client->Stop();
    if (SUCCEEDED(result)) {
        state.started = false;
    }
    return result;
}

std::int32_t WasapiOutput::get_current_padding(
    std::uint32_t& frame_count) noexcept
{
    frame_count = 0;
    if (implementation_ == nullptr || !implementation_->open) {
        return invalid_state();
    }

    Implementation& state = *implementation_;
    const HRESULT result = state.check_thread();
    if (FAILED(result)) {
        return result;
    }
    return state.audio_client->GetCurrentPadding(&frame_count);
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
    if (!state.open) {
        return S_OK;
    }

    if (state.started) {
        result = state.audio_client->Stop();
        if (FAILED(result)) {
            return result;
        }
        state.started = false;
    }
    return state.audio_client->Reset();
}

void WasapiOutput::request_cancel() noexcept
{
    // request_cancel is intentionally the sole method without COM-thread
    // affinity. Lifetime synchronization remains the caller's responsibility.
    if (implementation_ != nullptr && implementation_->cancel_event != nullptr) {
        static_cast<void>(SetEvent(implementation_->cancel_event));
    }
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

WasapiOpenResult WasapiOutput::open_result() const noexcept
{
    return implementation_ == nullptr
        ? WasapiOpenResult{}
        : implementation_->last_open_result;
}

} // namespace cd404::platform::windows
