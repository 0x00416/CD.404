#pragma once

#include <span>
#include <string>
#include <vector>

namespace cd404::ui {

// Returns one display label per acquisition source. Live providers supersede
// the cache label, while CD-TEXT remains visible because it is read directly
// from the disc and can contribute alongside online providers.
[[nodiscard]] std::vector<std::wstring> make_metadata_source_labels(
    bool has_cd_text,
    bool loaded_from_cache,
    std::span<const std::wstring> online_sources);

} // namespace cd404::ui
