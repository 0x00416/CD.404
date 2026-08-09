#include <windows.h>

#include <wincred.h>

#include <cd404/platform/windows/listenbrainz_reporter.hpp>

#include "listenbrainz_credentials.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>

namespace cd404::platform::windows {
namespace {

constexpr wchar_t kCredentialTarget[] = L"CD.404/ListenBrainz";
constexpr wchar_t kTokenEnvironment[] = L"CD404_LISTENBRAINZ_TOKEN";

[[nodiscard]] std::wstring token_from_environment()
{
    const DWORD required = GetEnvironmentVariableW(kTokenEnvironment, nullptr, 0);
    if (required <= 1U) {
        return {};
    }
    std::wstring token(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        kTokenEnvironment,
        token.data(),
        required);
    if (written == 0U || written >= required) {
        return {};
    }
    token.resize(written);
    return token;
}

} // namespace

bool is_listenbrainz_token_format_valid(const std::wstring_view token) noexcept
{
    return !token.empty() && token.size() <= 512U &&
        std::ranges::all_of(token, [](const wchar_t character) {
            return character >= L'!' && character <= L'~';
        });
}

bool save_listenbrainz_token(const std::wstring_view token) noexcept
{
    if (token.empty()) {
        if (CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0) != FALSE) {
            return true;
        }
        return GetLastError() == ERROR_NOT_FOUND;
    }
    if (!is_listenbrainz_token_format_valid(token) ||
        token.size() > static_cast<std::size_t>(
            std::numeric_limits<DWORD>::max()) / sizeof(wchar_t)) {
        return false;
    }

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(kCredentialTarget);
    credential.CredentialBlobSize = static_cast<DWORD>(
        token.size() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<wchar_t*>(token.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"ListenBrainz");
    return CredWriteW(&credential, 0) != FALSE;
}

namespace detail {

std::wstring load_listenbrainz_token()
{
    PCREDENTIALW credential{};
    if (CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential) != FALSE) {
        std::wstring token;
        if (credential->CredentialBlob != nullptr &&
            credential->CredentialBlobSize >= sizeof(wchar_t) &&
            credential->CredentialBlobSize % sizeof(wchar_t) == 0) {
            const auto* characters = reinterpret_cast<const wchar_t*>(
                credential->CredentialBlob);
            token.assign(characters,
                characters + credential->CredentialBlobSize / sizeof(wchar_t));
            while (!token.empty() && token.back() == L'\0') {
                token.pop_back();
            }
        }
        CredFree(credential);
        if (is_listenbrainz_token_format_valid(token)) {
            return token;
        }
    }
    std::wstring token = token_from_environment();
    return is_listenbrainz_token_format_valid(token) ? token : std::wstring{};
}

} // namespace detail
} // namespace cd404::platform::windows
