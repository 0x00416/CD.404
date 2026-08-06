#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <wincodec.h>
#include <winhttp.h>

int wmain()
{
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HINTERNET session = WinHttpOpen(
        L"CD.404 dependency probe",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (session != nullptr) {
        WinHttpCloseHandle(session);
    }

    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }

    return session == nullptr ? 1 : 0;
}
