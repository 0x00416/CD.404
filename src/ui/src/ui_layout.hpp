#pragma once

#include <d2d1.h>

namespace cd404::ui::detail {

struct Layout final {
    float width{};
    float height{};
    D2D1_RECT_F refresh_button{};
    D2D1_RECT_F eject_button{};
    D2D1_RECT_F vu_button{};
    D2D1_RECT_F settings_button{};
    D2D1_RECT_F cover{};
    D2D1_RECT_F track_list{};
    D2D1_RECT_F progress_hit{};
    D2D1_RECT_F previous_button{};
    D2D1_RECT_F play_button{};
    D2D1_RECT_F next_button{};
    D2D1_RECT_F volume_hit{};
    D2D1_RECT_F settings_page{};
    D2D1_RECT_F settings_audio_card{};
    D2D1_RECT_F settings_audio_engine{};
    D2D1_RECT_F settings_audio_endpoint{};
    D2D1_RECT_F settings_audio_exclusive_toggle{};
    D2D1_RECT_F settings_audio_fallback_toggle{};
    D2D1_RECT_F settings_listenbrainz_card{};
    D2D1_RECT_F settings_back{};
    D2D1_RECT_F settings_diagnostics_export{};
    D2D1_RECT_F settings_edit{};
    D2D1_RECT_F settings_save{};
    D2D1_RECT_F settings_clear{};
    D2D1_RECT_F settings_listenbrainz_toggle{};
    D2D1_RECT_F settings_queue_clear{};
    D2D1_RECT_F settings_cddb_card{};
    D2D1_RECT_F settings_cddb_server_edit{};
    D2D1_RECT_F settings_cddb_email_edit{};
    D2D1_RECT_F settings_cddb_save{};
    D2D1_RECT_F settings_cddb_toggle{};
    D2D1_RECT_F settings_autoplay_card{};
    D2D1_RECT_F settings_autoplay_repair{};
    D2D1_RECT_F metadata_button{};
    D2D1_RECT_F metadata_page{};
    D2D1_RECT_F metadata_back{};
    D2D1_RECT_F metadata_album_title_edit{};
    D2D1_RECT_F metadata_album_artist_edit{};
    D2D1_RECT_F metadata_category_edit{};
    D2D1_RECT_F metadata_year_edit{};
    D2D1_RECT_F metadata_track_list{};
    D2D1_RECT_F metadata_track_title_edit{};
    D2D1_RECT_F metadata_track_artist_edit{};
    D2D1_RECT_F metadata_save{};
    D2D1_RECT_F metadata_test_submit{};
    D2D1_RECT_F metadata_submit{};
};

[[nodiscard]] Layout calculate_layout(
    float width,
    float height,
    float settings_scroll_offset = 0.0F) noexcept;

} // namespace cd404::ui::detail
