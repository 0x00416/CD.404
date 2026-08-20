#pragma once

#include <filesystem>

namespace cd404::platform::windows {

inline constexpr wchar_t kBundledFontFamily[] = L"Noto Sans CJK SC";
inline constexpr wchar_t kBundledFontFileName[] = L"NotoSansCJKsc-VF.ttf";

class BundledFontRegistration final {
public:
    BundledFontRegistration();
    explicit BundledFontRegistration(std::filesystem::path font_path);
    ~BundledFontRegistration();

    BundledFontRegistration(const BundledFontRegistration&) = delete;
    BundledFontRegistration& operator=(const BundledFontRegistration&) = delete;
    BundledFontRegistration(BundledFontRegistration&&) = delete;
    BundledFontRegistration& operator=(BundledFontRegistration&&) = delete;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
    int loaded_resources_{};
};

[[nodiscard]] std::filesystem::path bundled_font_path();

} // namespace cd404::platform::windows
