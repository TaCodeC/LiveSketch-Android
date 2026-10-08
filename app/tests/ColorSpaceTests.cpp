#include "Test.h"

#include "Gfx/ColorSpace.h"
#include "UI/ColorManage.h"

#include <imgui.h>

#include <cmath>
#include <random>

namespace {

constexpr ColorProfile kSrgb = ColorProfile::Srgb;
constexpr ColorProfile kP3 = ColorProfile::DisplayP3;

ImDrawVert vertex(ImU32 color) {
    ImDrawVert v{};
    v.col = color;
    return v;
}

// El color de un vértice como "r g b a" (para que un fallo diga qué salió).
std::string rgba(ImU32 color) {
    std::ostringstream out;
    out << ((color >> IM_COL32_R_SHIFT) & 0xFF) << ' ' << ((color >> IM_COL32_G_SHIFT) & 0xFF) << ' '
        << ((color >> IM_COL32_B_SHIFT) & 0xFF) << ' ' << ((color >> IM_COL32_A_SHIFT) & 0xFF);
    return out.str();
}

} // namespace

TEST_CASE(color_profiles_convert_keeping_the_look) {
    CHECK(colorspace::between(kSrgb, kSrgb).identity());
    CHECK(!colorspace::between(kSrgb, kP3).identity());
    CHECK(colorspace::between(kSrgb, kP3).key() != colorspace::between(kP3, kSrgb).key());
    CHECK(colorspace::between(kP3, kP3).key() != colorspace::between(kSrgb, kSrgb).key());
    CHECK_EQ(std::string(colorspace::name(kSrgb)), std::string("sRGB"));
    CHECK_EQ(std::string(colorspace::name(kP3)), std::string("Display P3"));

    // Las dos matrices son inversas.
    const colorspace::Transform to = colorspace::between(kSrgb, kP3);
    const colorspace::Transform back = colorspace::between(kP3, kSrgb);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += back.matrix[i * 3 + k] * to.matrix[k * 3 + j];
            }
            CHECK_NEAR(sum, i == j ? 1.0f : 0.0f, 1e-6);
        }
    }

    // El rojo de sRGB en P3 (lo que da Little CMS con el perfil de Display P3).
    float red[3] = {1.0f, 0.0f, 0.0f};
    colorspace::convert(to, red);
    CHECK_NEAR(red[0], 0.9175f, 1e-4);
    CHECK_NEAR(red[1], 0.2003f, 1e-4);
    CHECK_NEAR(red[2], 0.1386f, 1e-4);
    // El de P3 no cabe en sRGB: se queda en su borde.
    float p3Red[3] = {1.0f, 0.0f, 0.0f};
    colorspace::convert(back, p3Red);
    CHECK_NEAR(p3Red[0], 1.0f, 1e-6);
    CHECK_NEAR(p3Red[1], 0.0f, 1e-6);
    CHECK_NEAR(p3Red[2], 0.0f, 1e-6);
    // Uno que cabe: lo mismo que convierte Chrome al pintar un PNG en P3 en un lienzo sRGB.
    float orange[3] = {204.0f / 255.0f, 128.0f / 255.0f, 77.0f / 255.0f};
    colorspace::convert(back, orange);
    CHECK_EQ(std::lround(orange[0] * 255.0f), 217);
    CHECK_EQ(std::lround(orange[1] * 255.0f), 123);
    CHECK_EQ(std::lround(orange[2] * 255.0f), 65);

    // Los grises son el mismo número en los dos perfiles.
    for (const float gray : {0.0f, 0.18f, 0.5f, 1.0f}) {
        float rgb[3] = {gray, gray, gray};
        colorspace::convert(to, rgb);
        CHECK_EQ(rgb[0], gray);
        CHECK_EQ(rgb[1], gray);
        CHECK_EQ(rgb[2], gray);
    }

    // sRGB cabe en P3: ir y volver deja el color como estaba.
    std::mt19937 random(11);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (int n = 0; n < 500; ++n) {
        const float original[3] = {unit(random), unit(random), unit(random)};
        float rgb[3] = {original[0], original[1], original[2]};
        colorspace::convert(to, rgb);
        colorspace::convert(back, rgb);
        for (int i = 0; i < 3; ++i) {
            CHECK_NEAR(rgb[i], original[i], 1e-4);
        }
    }

    // La curva de transferencia y su inversa.
    for (int i = 0; i <= 255; ++i) {
        const float encoded = static_cast<float>(i) / 255.0f;
        CHECK_NEAR(colorspace::toEncoded(colorspace::toLinear(encoded)), encoded, 1e-5);
    }
}

TEST_CASE(color_converter8_rounds_like_the_float_conversion) {
    std::mt19937 random(5);
    for (const ColorProfile from : {kSrgb, kP3}) {
        for (const ColorProfile to : {kSrgb, kP3}) {
            const colorspace::Converter8& converter = colorspace::converter8(from, to);
            CHECK(converter.transform().from == from);
            CHECK(converter.transform().to == to);
            CHECK(&converter == &colorspace::converter8(from, to));
            int mismatches = 0;
            for (int n = 0; n < 4000; ++n) {
                uint8_t c[3] = {static_cast<uint8_t>(random()), static_cast<uint8_t>(random()),
                                static_cast<uint8_t>(random())};
                if (n < 256) {
                    c[0] = c[1] = c[2] = static_cast<uint8_t>(n);   // todos los grises
                }
                float rgb[3] = {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f};
                colorspace::convert(converter.transform(), rgb);
                converter.apply(c[0], c[1], c[2]);
                for (int i = 0; i < 3; ++i) {
                    const long expected = std::lround(rgb[i] * 255.0f);
                    // Justo en la mitad entre dos valores, el redondeo puede caer a cualquier lado.
                    CHECK(std::abs(static_cast<long>(c[i]) - expected) <= 1);
                    mismatches += c[i] != expected ? 1 : 0;
                }
                if (n < 256) {
                    CHECK(c[0] == n && c[1] == n && c[2] == n);
                }
            }
            CHECK(mismatches <= 4);
        }
    }
}

TEST_CASE(ui_colors_are_converted_to_the_display_profile) {
    ImDrawData data;

    // Pantalla en P3 con un lienzo P3: la interfaz (sRGB) se convierte y los colores del
    // dibujo ya son de P3. El alfa no cambia.
    {
        ImDrawList list(nullptr);
        ui::gamut::beginFrame(kP3, kP3);
        list.VtxBuffer.push_back(vertex(IM_COL32(255, 0, 0, 200)));
        {
            const ui::gamut::Scope scope(&list);
            list.VtxBuffer.push_back(vertex(IM_COL32(255, 0, 0, 255)));
            list.VtxBuffer.push_back(vertex(IM_COL32(10, 200, 30, 128)));
        }
        list.VtxBuffer.push_back(vertex(IM_COL32(128, 128, 128, 64)));
        {
            // Con un perfil dado (las muestras de la tarjeta de lienzo nuevo).
            const ui::gamut::Scope scope(&list, kSrgb);
            list.VtxBuffer.push_back(vertex(IM_COL32(255, 0, 0, 255)));
        }
        data.CmdLists.clear();
        data.CmdLists.push_back(&list);
        ui::gamut::convert(&data);
        CHECK_EQ(rgba(list.VtxBuffer[0].col), std::string("234 51 35 200"));
        CHECK_EQ(rgba(list.VtxBuffer[1].col), std::string("255 0 0 255"));
        CHECK_EQ(rgba(list.VtxBuffer[2].col), std::string("10 200 30 128"));
        CHECK_EQ(rgba(list.VtxBuffer[3].col), std::string("128 128 128 64"));
        CHECK_EQ(rgba(list.VtxBuffer[4].col), std::string("234 51 35 255"));
    }

    // Pantalla sRGB con un lienzo P3: la interfaz se queda como está y los colores del
    // dibujo pasan a sRGB.
    {
        ImDrawList list(nullptr);
        ImDrawList other(nullptr);
        ui::gamut::beginFrame(kSrgb, kP3);
        list.VtxBuffer.push_back(vertex(IM_COL32(255, 0, 0, 255)));
        {
            const ui::gamut::Scope scope(&other);
            other.VtxBuffer.push_back(vertex(IM_COL32(204, 128, 77, 255)));
        }
        {
            const ui::gamut::Scope scope(&list);
            list.VtxBuffer.push_back(vertex(IM_COL32(204, 128, 77, 255)));
            list.VtxBuffer.push_back(vertex(IM_COL32(255, 0, 0, 255)));
        }
        {
            const ui::gamut::Scope scope(&list);   // vacío: no cuenta
        }
        list.VtxBuffer.push_back(vertex(IM_COL32(204, 128, 77, 255)));
        data.CmdLists.clear();
        data.CmdLists.push_back(&list);
        data.CmdLists.push_back(&other);
        ui::gamut::convert(&data);
        CHECK_EQ(rgba(list.VtxBuffer[0].col), std::string("255 0 0 255"));
        CHECK_EQ(rgba(list.VtxBuffer[1].col), std::string("217 123 65 255"));
        CHECK_EQ(rgba(list.VtxBuffer[2].col), std::string("255 0 0 255"));
        CHECK_EQ(rgba(list.VtxBuffer[3].col), std::string("204 128 77 255"));
        CHECK_EQ(rgba(other.VtxBuffer[0].col), std::string("217 123 65 255"));
    }

    // Todo en sRGB: no cambia nada.
    {
        ImDrawList list(nullptr);
        ui::gamut::beginFrame(kSrgb, kSrgb);
        {
            const ui::gamut::Scope scope(&list);
            list.VtxBuffer.push_back(vertex(IM_COL32(204, 128, 77, 255)));
        }
        list.VtxBuffer.push_back(vertex(IM_COL32(1, 2, 3, 4)));
        data.CmdLists.clear();
        data.CmdLists.push_back(&list);
        ui::gamut::convert(&data);
        CHECK_EQ(rgba(list.VtxBuffer[0].col), std::string("204 128 77 255"));
        CHECK_EQ(rgba(list.VtxBuffer[1].col), std::string("1 2 3 4"));
    }
    data.CmdLists.clear();
    ui::gamut::beginFrame(kSrgb, kSrgb);
}
