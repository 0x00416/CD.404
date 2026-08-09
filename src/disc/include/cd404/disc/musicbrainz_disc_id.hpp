#pragma once

#include <cd404/disc/toc.hpp>

#include <optional>
#include <string>

namespace cd404::disc {

struct MusicBrainzDiscIdentity final {
    std::string disc_id;
    std::string toc;
};

// Produces the standard MusicBrainz SHA-1/modified-Base64 identity. A mixed
// session is rejected until the platform supplies its audio-session lead-out;
// using the following data-track offset would create a plausible but wrong ID.
[[nodiscard]] std::optional<MusicBrainzDiscIdentity>
make_musicbrainz_disc_identity(const Toc& toc);

} // namespace cd404::disc
