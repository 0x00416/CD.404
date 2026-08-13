#include "ui_layout.hpp"

#include <algorithm>

namespace cd404::ui::detail {

Layout calculate_layout(
    const float width,
    const float height,
    const float settings_scroll_offset) noexcept
{
    Layout result;
    result.width = width;
    result.height = height;

    const float margin = width < 960.0F ? 20.0F : 24.0F;
    const float top = 84.0F;
    const float transport_height = height < 680.0F ? 104.0F : 124.0F;
    const float transport_top = height - transport_height;
    const float left_width = std::clamp(width * 0.27F, 240.0F, 300.0F);
    const float gap = width < 960.0F ? 28.0F : 40.0F;
    const float cover_size = std::min(
        left_width,
        std::max(220.0F, transport_top - top - 185.0F));
    const float cover_top = top + 52.0F;

    result.refresh_button = D2D1::RectF(width - 160.0F, 12.0F, width - 120.0F, 52.0F);
    result.eject_button = D2D1::RectF(width - 112.0F, 12.0F, width - 72.0F, 52.0F);
    result.settings_button = D2D1::RectF(width - 64.0F, 12.0F, width - 24.0F, 52.0F);
    result.cover = D2D1::RectF(margin, cover_top, margin + cover_size, cover_top + cover_size);
    result.track_list = D2D1::RectF(
        margin + left_width + gap, top + 52.0F, width - margin, transport_top - 12.0F);
    result.progress_hit = D2D1::RectF(
        margin, transport_top + 14.0F, width - margin, transport_top + 34.0F);

    const float center = width * 0.5F;
    const float controls_y = transport_top + transport_height * 0.57F;
    result.previous_button = D2D1::RectF(
        center - 96.0F, controls_y - 20.0F, center - 56.0F, controls_y + 20.0F);
    result.play_button = D2D1::RectF(
        center - 28.0F, controls_y - 28.0F, center + 28.0F, controls_y + 28.0F);
    result.next_button = D2D1::RectF(
        center + 56.0F, controls_y - 20.0F, center + 96.0F, controls_y + 20.0F);
    const float volume_right = width - 24.0F;
    const float volume_left = std::max(width * 0.77F, volume_right - 164.0F);
    result.volume_hit = D2D1::RectF(
        volume_left - 4.0F, controls_y - 16.0F, volume_right + 4.0F, controls_y + 16.0F);

    constexpr float settings_maximum_width = 1'052.0F;
    constexpr float settings_bottom = 736.0F;
    const float settings_shift = -std::max(0.0F, settings_scroll_offset);
    const float settings_width = std::min(
        std::max(0.0F, width - margin * 2.0F),
        settings_maximum_width);
    const float settings_left = (width - settings_width) * 0.5F;
    const float settings_right = settings_left + settings_width;
    result.settings_page = D2D1::RectF(
        settings_left, 84.0F, settings_right, settings_bottom);
    result.settings_back = D2D1::RectF(
        settings_right - 132.0F, 88.0F, settings_right, 128.0F);
    result.settings_diagnostics_export = D2D1::RectF(
        result.settings_back.left - 164.0F, 88.0F,
        result.settings_back.left - 12.0F, 128.0F);
    result.settings_audio_card = D2D1::RectF(
        settings_left,
        148.0F + settings_shift,
        settings_right,
        300.0F + settings_shift);
    result.settings_audio_engine = D2D1::RectF(
        result.settings_audio_card.left + 24.0F,
        result.settings_audio_card.top + 76.0F,
        result.settings_audio_card.left + 260.0F,
        result.settings_audio_card.top + 120.0F);
    result.settings_audio_endpoint = D2D1::RectF(
        result.settings_audio_card.left + 276.0F,
        result.settings_audio_card.top + 76.0F,
        result.settings_audio_card.right - 230.0F,
        result.settings_audio_card.top + 120.0F);
    result.settings_audio_exclusive_toggle = D2D1::RectF(
        result.settings_audio_card.right - 76.0F,
        result.settings_audio_card.top + 58.0F,
        result.settings_audio_card.right - 24.0F,
        result.settings_audio_card.top + 86.0F);
    result.settings_audio_fallback_toggle = D2D1::RectF(
        result.settings_audio_card.right - 76.0F,
        result.settings_audio_card.top + 100.0F,
        result.settings_audio_card.right - 24.0F,
        result.settings_audio_card.top + 128.0F);
    result.settings_listenbrainz_card = D2D1::RectF(
        settings_left,
        316.0F + settings_shift,
        settings_right,
        510.0F + settings_shift);
    result.settings_edit = D2D1::RectF(
        result.settings_listenbrainz_card.left + 24.0F,
        result.settings_listenbrainz_card.top + 112.0F,
        result.settings_listenbrainz_card.right - 224.0F,
        result.settings_listenbrainz_card.top + 156.0F);
    result.settings_clear = D2D1::RectF(
        result.settings_listenbrainz_card.right - 208.0F,
        result.settings_listenbrainz_card.top + 112.0F,
        result.settings_listenbrainz_card.right - 112.0F,
        result.settings_listenbrainz_card.top + 156.0F);
    result.settings_save = D2D1::RectF(
        result.settings_listenbrainz_card.right - 104.0F,
        result.settings_listenbrainz_card.top + 112.0F,
        result.settings_listenbrainz_card.right - 24.0F,
        result.settings_listenbrainz_card.top + 156.0F);
    result.settings_listenbrainz_toggle = D2D1::RectF(
        result.settings_listenbrainz_card.right - 76.0F,
        result.settings_listenbrainz_card.top + 18.0F,
        result.settings_listenbrainz_card.right - 24.0F,
        result.settings_listenbrainz_card.top + 46.0F);
    result.settings_queue_clear = D2D1::RectF(
        result.settings_listenbrainz_card.right - 208.0F,
        result.settings_listenbrainz_card.top + 84.0F,
        result.settings_listenbrainz_card.right - 24.0F,
        result.settings_listenbrainz_card.top + 108.0F);
    result.settings_cddb_card = D2D1::RectF(
        settings_left,
        526.0F + settings_shift,
        settings_right,
        settings_bottom + settings_shift);
    result.settings_cddb_toggle = D2D1::RectF(
        result.settings_cddb_card.right - 76.0F,
        result.settings_cddb_card.top + 18.0F,
        result.settings_cddb_card.right - 24.0F,
        result.settings_cddb_card.top + 46.0F);
    result.settings_cddb_server_edit = D2D1::RectF(
        result.settings_cddb_card.left + 24.0F,
        result.settings_cddb_card.top + 94.0F,
        result.settings_cddb_card.right - 24.0F,
        result.settings_cddb_card.top + 134.0F);
    result.settings_cddb_email_edit = D2D1::RectF(
        result.settings_cddb_card.left + 24.0F,
        result.settings_cddb_card.top + 158.0F,
        result.settings_cddb_card.right - 136.0F,
        result.settings_cddb_card.top + 198.0F);
    result.settings_cddb_save = D2D1::RectF(
        result.settings_cddb_card.right - 120.0F,
        result.settings_cddb_card.top + 158.0F,
        result.settings_cddb_card.right - 24.0F,
        result.settings_cddb_card.top + 198.0F);
    result.settings_autoplay_card = D2D1::RectF(
        settings_left,
        settings_bottom + 16.0F + settings_shift,
        settings_right,
        settings_bottom + 120.0F + settings_shift);
    result.settings_autoplay_repair = D2D1::RectF(
        result.settings_autoplay_card.right - 120.0F,
        result.settings_autoplay_card.top + 46.0F,
        result.settings_autoplay_card.right - 24.0F,
        result.settings_autoplay_card.top + 86.0F);
    result.metadata_button = D2D1::RectF(
        result.track_list.left + 88.0F, 82.0F,
        result.track_list.left + 184.0F, 122.0F);
    result.metadata_page = D2D1::RectF(margin, 84.0F, width - margin, height - 24.0F);
    result.metadata_back = D2D1::RectF(
        width - margin - 132.0F, 88.0F, width - margin, 128.0F);
    const float editor_top = 152.0F;
    const float editor_gap = 16.0F;
    const float editor_half = (width - margin * 2.0F - editor_gap) * 0.5F;
    result.metadata_album_title_edit = D2D1::RectF(
        margin, editor_top, margin + editor_half, editor_top + 42.0F);
    result.metadata_album_artist_edit = D2D1::RectF(
        margin + editor_half + editor_gap, editor_top,
        width - margin, editor_top + 42.0F);
    result.metadata_category_edit = D2D1::RectF(
        margin, editor_top + 66.0F, margin + editor_half, editor_top + 108.0F);
    result.metadata_year_edit = D2D1::RectF(
        margin + editor_half + editor_gap, editor_top + 66.0F,
        width - margin, editor_top + 108.0F);
    result.metadata_track_list = D2D1::RectF(
        margin, editor_top + 132.0F, width - margin, height - 150.0F);
    result.metadata_track_title_edit = D2D1::RectF(
        margin, height - 126.0F, margin + editor_half, height - 84.0F);
    result.metadata_track_artist_edit = D2D1::RectF(
        margin + editor_half + editor_gap, height - 126.0F,
        width - margin, height - 84.0F);
    result.metadata_save = D2D1::RectF(
        margin, height - 68.0F, margin + 118.0F, height - 28.0F);
    result.metadata_test_submit = D2D1::RectF(
        width - margin - 254.0F, height - 68.0F,
        width - margin - 126.0F, height - 28.0F);
    result.metadata_submit = D2D1::RectF(
        width - margin - 112.0F, height - 68.0F,
        width - margin, height - 28.0F);
    return result;
}

} // namespace cd404::ui::detail
