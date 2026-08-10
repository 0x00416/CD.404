#include <cd404/ui/metadata_source_model.hpp>

#include <algorithm>

namespace cd404::ui {

std::vector<std::wstring> make_metadata_source_labels(
    const bool has_cd_text,
    const bool loaded_from_cache,
    const std::span<const std::wstring> online_sources)
{
    std::vector<std::wstring> labels;
    const auto append_unique = [&labels](const std::wstring& source) {
        if (!source.empty() &&
            std::ranges::find(labels, source) == labels.end()) {
            labels.push_back(source);
        }
    };

    if (has_cd_text) {
        append_unique(L"CD-TEXT");
    }
    for (const auto& source : online_sources) {
        append_unique(source);
    }
    if (online_sources.empty() && loaded_from_cache) {
        append_unique(L"Local");
    }
    return labels;
}

} // namespace cd404::ui
