#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cd404/platform/windows/wasapi_output.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kSkipped = 2;
constexpr std::uint32_t kMarkerFrames = 44'100;

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::vector<std::int16_t> make_marker()
{
    std::vector<std::int16_t> samples(
        static_cast<std::size_t>(kMarkerFrames) * 2);
    for (std::uint32_t frame = 0; frame < kMarkerFrames; ++frame) {
        const double phase = 2.0 * std::numbers::pi * 997.0 *
            static_cast<double>(frame) / 44'100.0;
        const auto envelope = frame < 2'205 || frame >= 41'895 ? 0.0 : 1.0;
        const auto sample = static_cast<std::int16_t>(
            std::sin(phase) * 8'192.0 * envelope);
        samples[static_cast<std::size_t>(frame) * 2] = sample;
        samples[static_cast<std::size_t>(frame) * 2 + 1] =
            static_cast<std::int16_t>(-sample);
    }
    return samples;
}

void print_usage()
{
    std::wcout
        << L"Usage: cd404_wasapi_probe [--list] [--render-shared|--render-exclusive|"
           L"--loopback-shared] "
           L"[--endpoint <endpoint-id>]\n"
        << L"Default behavior only enumerates active render endpoints. Rendering "
           L"emits a one-second 997 Hz antiphase marker after 50 ms silence.\n";
}

[[nodiscard]] int render_marker(
    const cd404::platform::windows::WasapiOpenOptions& options)
{
    cd404::platform::windows::WasapiOutput output;
    const auto opened = output.open(options);
    if (!opened.succeeded()) {
        std::wcerr << L"ERROR: requested "
                   << cd404::platform::windows::to_string(options.mode)
                   << L" session failed: "
                   << cd404::platform::windows::describe_wasapi_status(
                          opened.status)
                   << L'\n';
        return 1;
    }

    const auto marker = make_marker();
    std::uint32_t submitted{};
    bool started{};
    while (submitted < kMarkerFrames) {
        const auto remaining = kMarkerFrames - submitted;
        const auto offset = static_cast<std::size_t>(submitted) * 2;
        const auto written = output.write_interleaved(
            std::span<const std::int16_t>(marker).subspan(offset),
            remaining);
        if (written.status < 0 || written.frames_written == 0) {
            std::wcerr << L"ERROR: marker write failed: "
                       << cd404::platform::windows::describe_wasapi_status(
                              written.status)
                       << L'\n';
            return 1;
        }
        submitted += written.frames_written;
        if (!started) {
            const auto start_status = output.start();
            if (start_status < 0) {
                std::wcerr << L"ERROR: marker start failed: "
                           << cd404::platform::windows::describe_wasapi_status(
                                  start_status)
                           << L'\n';
                return 1;
            }
            started = true;
        }
    }

    const auto drain_status = output.drain();
    if (drain_status < 0) {
        std::wcerr << L"ERROR: marker drain failed: "
                   << cd404::platform::windows::describe_wasapi_status(
                          drain_status)
                   << L'\n';
        return 1;
    }
    return 0;
}

struct LoopbackSession final {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* format{};

    ~LoopbackSession()
    {
        if (client) {
            static_cast<void>(client->Stop());
        }
        CoTaskMemFree(format);
    }

    [[nodiscard]] HRESULT open(const std::wstring& endpoint_id)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT status = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(status)) {
            return status;
        }

        ComPtr<IMMDevice> endpoint;
        status = endpoint_id.empty()
            ? enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint)
            : enumerator->GetDevice(endpoint_id.c_str(), &endpoint);
        if (FAILED(status)) {
            return status;
        }
        status = endpoint->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            &client);
        if (FAILED(status)) {
            return status;
        }
        status = client->GetMixFormat(&format);
        if (FAILED(status)) {
            return status;
        }
        status = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            0,
            0,
            format,
            nullptr);
        if (FAILED(status)) {
            return status;
        }
        status = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(status)) {
            return status;
        }
        return client->Start();
    }
};

[[nodiscard]] int capture_shared_loopback(
    const cd404::platform::windows::WasapiOpenOptions& options)
{
    LoopbackSession loopback;
    const HRESULT open_status = loopback.open(options.endpoint_id);
    if (FAILED(open_status)) {
        std::wcerr << L"ERROR: shared loopback initialization failed: "
                   << cd404::platform::windows::describe_wasapi_status(
                          open_status)
                   << L'\n';
        return 1;
    }

    std::atomic<bool> render_finished{};
    std::atomic<int> render_status{1};
    std::thread renderer([&] {
        const HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(status)) {
            render_status = render_marker(options);
            CoUninitialize();
        }
        render_finished = true;
    });

    std::uint64_t captured_frames{};
    std::uint64_t non_silent_bytes{};
    std::uint64_t packet_count{};
    bool discontinuity{};
    const auto hard_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto finish_deadline = hard_deadline;
    while (std::chrono::steady_clock::now() < hard_deadline &&
           (!render_finished || std::chrono::steady_clock::now() < finish_deadline)) {
        if (render_finished && finish_deadline == hard_deadline) {
            finish_deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        }
        UINT32 next_frames{};
        HRESULT status = loopback.capture->GetNextPacketSize(&next_frames);
        while (SUCCEEDED(status) && next_frames != 0) {
            BYTE* bytes{};
            UINT32 frames{};
            DWORD flags{};
            status = loopback.capture->GetBuffer(
                &bytes, &frames, &flags, nullptr, nullptr);
            if (FAILED(status)) {
                break;
            }
            if (packet_count != 0 &&
                (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                discontinuity = true;
            }
            captured_frames += frames;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && bytes != nullptr) {
                const auto byte_count = static_cast<std::size_t>(frames) *
                    loopback.format->nBlockAlign;
                for (std::size_t index = 0; index < byte_count; ++index) {
                    non_silent_bytes += bytes[index] != 0 ? 1U : 0U;
                }
            }
            ++packet_count;
            status = loopback.capture->ReleaseBuffer(frames);
            if (SUCCEEDED(status)) {
                status = loopback.capture->GetNextPacketSize(&next_frames);
            }
        }
        if (FAILED(status)) {
            std::wcerr << L"ERROR: loopback capture failed: "
                       << cd404::platform::windows::describe_wasapi_status(status)
                       << L'\n';
            renderer.join();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    renderer.join();

    const auto minimum_frames =
        static_cast<std::uint64_t>(loopback.format->nSamplesPerSec) / 2;
    if (render_status != 0 || captured_frames < minimum_frames ||
        non_silent_bytes == 0 || discontinuity) {
        std::wcerr << L"FAIL: loopback frames=" << captured_frames
                   << L", non-zero-bytes=" << non_silent_bytes
                   << L", discontinuity=" << (discontinuity ? L"yes" : L"no")
                   << L", render-status=" << render_status.load() << L'\n';
        return 1;
    }
    std::wcout << L"PASS: shared loopback captured " << captured_frames
               << L" frames in " << loopback.format->nSamplesPerSec
               << L" Hz mix format without a discontinuity flag; non-zero bytes="
               << non_silent_bytes << L".\n";
    return 0;
}

} // namespace

int wmain(const int argument_count, wchar_t** arguments)
{
    bool render{};
    bool loopback{};
    auto mode = cd404::platform::windows::WasapiShareMode::shared;
    std::wstring endpoint_id;
    for (int index = 1; index < argument_count; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--list") {
            continue;
        }
        if (argument == L"--render-shared") {
            render = true;
            mode = cd404::platform::windows::WasapiShareMode::shared;
            continue;
        }
        if (argument == L"--render-exclusive") {
            render = true;
            mode = cd404::platform::windows::WasapiShareMode::exclusive;
            continue;
        }
        if (argument == L"--loopback-shared") {
            render = true;
            loopback = true;
            mode = cd404::platform::windows::WasapiShareMode::shared;
            continue;
        }
        if (argument == L"--endpoint" && index + 1 < argument_count) {
            endpoint_id = arguments[++index];
            continue;
        }
        print_usage();
        return 64;
    }

    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_status)) {
        std::wcerr << L"ERROR: COM initialization failed: 0x" << std::hex
                   << static_cast<std::uint32_t>(com_status) << L'\n';
        return 1;
    }

    std::int32_t enumeration_status{};
    const auto endpoints =
        cd404::platform::windows::enumerate_wasapi_render_endpoints(
            &enumeration_status);
    if (enumeration_status < 0) {
        std::wcerr << L"ERROR: endpoint enumeration failed: "
                   << cd404::platform::windows::describe_wasapi_status(
                          enumeration_status)
                   << L'\n';
        CoUninitialize();
        return 1;
    }
    if (endpoints.empty()) {
        std::wcout << L"SKIP: no active WASAPI render endpoint is available.\n";
        CoUninitialize();
        return kSkipped;
    }

    for (const auto& endpoint : endpoints) {
        std::wcout << (endpoint.is_default ? L"* " : L"  ") << endpoint.name
                   << L"\n    " << endpoint.id << L'\n';
    }
    if (!render) {
        CoUninitialize();
        return 0;
    }

    cd404::platform::windows::WasapiOpenOptions options;
    options.endpoint_id = endpoint_id;
    options.mode = mode;
    options.allow_shared_fallback = false;
    const int result = loopback ? capture_shared_loopback(options)
                                : render_marker(options);
    CoUninitialize();
    if (result != 0) {
        return result;
    }

    if (!loopback) {
        std::wcout << L"PASS: rendered 44100 exact PCM frames as a 997 Hz stereo "
                      L"antiphase marker in "
                   << cd404::platform::windows::to_string(mode) << L" mode.\n";
    }
    return 0;
}
