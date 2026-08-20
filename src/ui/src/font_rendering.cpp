#include <cd404/ui/font_rendering.hpp>

#include <wrl/client.h>

namespace cd404::ui {

HRESULT configure_text_rendering(
    ID2D1RenderTarget& render_target,
    IDWriteFactory& write_factory)
{
    Microsoft::WRL::ComPtr<IDWriteRenderingParams> system_parameters;
    HRESULT result = write_factory.CreateRenderingParams(
        system_parameters.GetAddressOf());
    if (FAILED(result)) {
        return result;
    }

    Microsoft::WRL::ComPtr<IDWriteRenderingParams> natural_parameters;
    result = write_factory.CreateCustomRenderingParams(
        system_parameters->GetGamma(),
        system_parameters->GetEnhancedContrast(),
        0.0F,
        DWRITE_PIXEL_GEOMETRY_FLAT,
        kTextRenderingMode,
        natural_parameters.GetAddressOf());
    if (FAILED(result)) {
        return result;
    }

    render_target.SetTextAntialiasMode(kInterfaceTextAntialiasMode);
    render_target.SetTextRenderingParams(natural_parameters.Get());
    return S_OK;
}

} // namespace cd404::ui
