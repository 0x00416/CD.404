#include "ui_layout.hpp"

#include <algorithm>

namespace cd404::ui::detail {

Layout calculate_layout(const float width, const float height) noexcept
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

    result.settings_page = D2D1::RectF(margin, 84.0F, width - margin, height - 24.0F);
    result.settings_back = D2D1::RectF(width - margin - 132.0F, 88.0F, width - margin, 128.0F);
    result.settings_diagnostics_export = D2D1::RectF(
        result.settings_back.left - 164.0F, 88.0F,
        result.settings_back.left - 12.0F, 128.0F);
    result.settings_audio_card = D2D1::RectF(margin, 148.0F, width - margin, 300.0F);
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
    result.settings_listenbrainz_card = D2D1::RectF(margin, 316.0F, width - margin, 510.0F);
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
        result.settings_listenbrainz_card.top + 22.0F,
        result.settings_listenbrainz_card.right - 24.0F,
        result.settings_listenbrainz_card.top + 50.0F);
    result.settings_queue_clear = D2D1::RectF(
        result.settings_listenbrainz_card.right - 208.0F,
        result.settings_listenbrainz_card.top + 66.0F,
        result.settings_listenbrainz_card.right - 24.0F,
        result.settings_listenbrainz_card.top + 102.0F);
    return result;
}

} // namespace cd404::ui::detail
