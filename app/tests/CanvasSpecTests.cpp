// Tamaños, unidades y números de la tarjeta de lienzo nuevo (sin GPU).
#include "Test.h"

#include "Canvas/CanvasSpec.h"

#include <cmath>
#include <string>

using namespace canvasspec;

TEST_CASE(canvas_spec_paper_sizes_in_pixels) {
    // A4 a 300 ppp: los 2480 × 3508 de siempre (Photoshop redondea igual).
    CHECK_EQ(toPixels(210.0, LengthUnit::Millimeters, 300.0), 2480);
    CHECK_EQ(toPixels(297.0, LengthUnit::Millimeters, 300.0), 3508);
    CHECK_EQ(toPixels(21.0, LengthUnit::Centimeters, 300.0), 2480);
    CHECK_EQ(toPixels(8.5, LengthUnit::Inches, 300.0), 2550);
    CHECK_EQ(toPixels(11.0, LengthUnit::Inches, 300.0), 3300);
    CHECK_EQ(toPixels(1920.4, LengthUnit::Pixels, 300.0), 1920);
    CHECK_EQ(toPixels(0.0, LengthUnit::Millimeters, 300.0), 0);
    CHECK_EQ(toPixels(-5.0, LengthUnit::Pixels, 72.0), 0);
    CHECK_NEAR(toLength(2480, LengthUnit::Millimeters, 300.0), 209.97, 0.01);
    CHECK_NEAR(toLength(300, LengthUnit::Inches, 300.0), 1.0, 1e-9);
    CHECK_NEAR(toLength(1920, LengthUnit::Pixels, 300.0), 1920.0, 1e-9);
}

TEST_CASE(canvas_spec_unit_round_trip_keeps_pixels) {
    // Pasar de px a otra unidad y volver no cambia los píxeles (la medida no se redondea
    // al guardarla, solo al mostrarla).
    for (const LengthUnit unit : {LengthUnit::Millimeters, LengthUnit::Centimeters, LengthUnit::Inches}) {
        for (const double ppi : {72.0, 96.0, 150.0, 300.0, 600.0, 333.3}) {
            for (int pixels = kMinSide; pixels < 9000; pixels += 97) {
                CHECK_EQ(toPixels(toLength(pixels, unit, ppi), unit, ppi), pixels);
            }
        }
    }
}

TEST_CASE(canvas_spec_converts_between_units) {
    using canvasspec::convertLength;
    // Entre medidas en papel, exacto: no se acumula error al ir y volver.
    CHECK_NEAR(convertLength(210.0, LengthUnit::Millimeters, LengthUnit::Centimeters, 300.0), 21.0, 1e-9);
    CHECK_NEAR(convertLength(8.5, LengthUnit::Inches, LengthUnit::Millimeters, 300.0), 215.9, 1e-9);
    CHECK_NEAR(convertLength(2.54, LengthUnit::Centimeters, LengthUnit::Inches, 72.0), 1.0, 1e-9);
    double length = 297.0;
    for (int i = 0; i < 20; ++i) {
        length = convertLength(length, LengthUnit::Millimeters, LengthUnit::Inches, 300.0);
        length = convertLength(length, LengthUnit::Inches, LengthUnit::Centimeters, 300.0);
        length = convertLength(length, LengthUnit::Centimeters, LengthUnit::Millimeters, 300.0);
    }
    CHECK_NEAR(length, 297.0, 1e-9);
    // A px, al píxel; desde px, lo que miden los píxeles enteros.
    CHECK_NEAR(convertLength(210.0, LengthUnit::Millimeters, LengthUnit::Pixels, 300.0), 2480.0, 0.0);
    CHECK_NEAR(convertLength(2480.0, LengthUnit::Pixels, LengthUnit::Millimeters, 300.0), 2480.0 / 300.0 * 25.4,
               1e-9);
    CHECK_NEAR(convertLength(1920.4, LengthUnit::Pixels, LengthUnit::Inches, 96.0), 20.0, 1e-9);
    CHECK_NEAR(convertLength(1920.0, LengthUnit::Pixels, LengthUnit::Pixels, 96.0), 1920.0, 0.0);
}

TEST_CASE(canvas_spec_numbers_use_decimal_comma) {
    CHECK_EQ(formatNumber(29.7, 1), std::string("29,7"));
    CHECK_EQ(formatNumber(21.0, 1), std::string("21"));
    CHECK_EQ(formatNumber(8.5, 2), std::string("8,5"));
    CHECK_EQ(formatNumber(209.97, 1), std::string("210"));
    CHECK_EQ(formatNumber(1.0 / 3.0, 2), std::string("0,33"));
    CHECK_EQ(formatNumber(100.0, 2), std::string("100"));
    CHECK_EQ(formatNumber(-0.001, 1), std::string("0"));
    CHECK_EQ(formatSize(21.0, 29.7, LengthUnit::Centimeters), std::string("21 × 29,7 cm"));
    CHECK_EQ(formatSize(1920.0, 1080.0, LengthUnit::Pixels), std::string("1920 × 1080 px"));
    CHECK_EQ(paperSize(2480, 3508, LengthUnit::Centimeters, 300.0), std::string("21 × 29,7 cm"));
    CHECK_EQ(formatPpi(300.0), std::string("300 ppp"));
    CHECK_EQ(formatPpi(96.5), std::string("96,5 ppp"));
    CHECK_EQ(formatBytes(size_t{2480} * 3508 * 4), std::string("33 MB"));
    CHECK_EQ(formatBytes(size_t{1280} * 720 * 4), std::string("3,5 MB"));
    CHECK_EQ(formatBytes(size_t{3} * 1024 * 1024 * 1024 / 2), std::string("1,5 GB"));
    CHECK_EQ(formatBytes(size_t{48} * 1024 + 300), std::string("48 KB"));
    CHECK_EQ(formatBytes(200), std::string("1 KB"));
    CHECK_EQ(formatBytes(0), std::string("0 KB"));
    CHECK_EQ(formatBytes(size_t{970} * 1024), std::string("970 KB"));
    CHECK_EQ(formatBytes(size_t{990} * 1024), std::string("1 MB"));

    double value = 0.0;
    CHECK(parseNumber("29,7", &value));
    CHECK_NEAR(value, 29.7, 1e-12);
    CHECK(parseNumber(" 29.7 ", &value));
    CHECK_NEAR(value, 29.7, 1e-12);
    CHECK(parseNumber(",5", &value));
    CHECK_NEAR(value, 0.5, 1e-12);
    CHECK(parseNumber("1920", &value));
    CHECK_EQ(value, 1920.0);
    CHECK(!parseNumber("", &value));
    CHECK(!parseNumber(",", &value));
    CHECK(!parseNumber("1,2,3", &value));
    CHECK(!parseNumber("-4", &value));
    CHECK(!parseNumber("12a", &value));
    CHECK(!parseNumber("1e5", &value));
}

TEST_CASE(canvas_spec_layer_limits_and_sizes) {
    // A4 a 300 ppp: 23 capas en el presupuesto de 768 MB.
    CHECK_EQ(layerLimit(2480, 3508), 23);
    CHECK_EQ(layerLimit(1920, 1080), 64);
    CHECK_EQ(layerLimit(3840, 2160), 24);
    CHECK_EQ(layerLimit(16384, 16384), kMinLayerLimit);
    CHECK(sizeProblem(2480, 3508, 4096) == SizeProblem::None);
    CHECK(sizeProblem(3508, 4961, 4096) == SizeProblem::TooWide);
    CHECK(sizeProblem(3508, 4961, 8192) == SizeProblem::None);
    CHECK(sizeProblem(4, 100, 4096) == SizeProblem::TooSmall);
    CHECK(sizeProblem(8000, 8000, 16384) == SizeProblem::TooManyPixels);
    // 65536 × 65536 son 2³² píxeles: con size_t de 32 bits (Android armeabi-v7a, la web)
    // daría 0 y pasaría por bueno.
    CHECK(sizeProblem(65536, 65536, 0) == SizeProblem::TooManyPixels);
    CHECK(layerBytes(65536, 65536) >= kLayerMemoryBudget);
    CHECK_EQ(layerLimit(65536, 65536), kMinLayerLimit);
    // El más grande que se admite deja sitio para las capas mínimas.
    CHECK(layerBytes(7000, 7000) * kMinLayerLimit <= kLayerMemoryBudget);
    CHECK(sizeProblem(7000, 7000, 16384) == SizeProblem::None);
}

TEST_CASE(canvas_spec_presets_are_valid) {
    for (int c = 0; c < kPresetCategoryCount; ++c) {
        const PresetCategory category = static_cast<PresetCategory>(c);
        CHECK(categoryName(category)[0] != '\0');
        CHECK(categoryShortName(category)[0] != '\0');
        for (const Preset& preset : presets(category)) {
            if (preset.width == 0.0) {
                continue;   // la pantalla del dispositivo
            }
            const int width = toPixels(preset.width, preset.unit, preset.ppi);
            const int height = toPixels(preset.height, preset.unit, preset.ppi);
            CHECK(sizeProblem(width, height, 0) == SizeProblem::None);
        }
    }
    CHECK(presets(PresetCategory::Saved).empty());
    // Los papeles se reconocen por su tamaño, en las dos orientaciones.
    CHECK_EQ(std::string(paperName(2480, 3508, 300.0)), std::string("A4"));
    CHECK_EQ(std::string(paperName(3508, 2480, 300.0)), std::string("A4"));
    CHECK_EQ(std::string(paperName(2550, 3300, 300.0)), std::string("Carta"));
    CHECK_EQ(std::string(paperName(1240, 1754, 150.0)), std::string("A4"));
    CHECK(paperName(1920, 1080, 72.0) == nullptr);
    CHECK_EQ(std::string(orientationName(2480, 3508)), std::string("Vertical"));
    CHECK_EQ(std::string(orientationName(1920, 1080)), std::string("Horizontal"));
    CHECK_EQ(std::string(orientationName(1080, 1080)), std::string("Cuadrado"));
}

TEST_CASE(canvas_spec_saved_sizes_round_trip) {
    const SavedSize sizes[] = {
        {1920.0, 1080.0, LengthUnit::Pixels, 72.0f},
        {210.0, 297.0, LengthUnit::Millimeters, 300.0f},
        {8.5, 11.0, LengthUnit::Inches, 600.0f},
        {12.34, 5.6, LengthUnit::Centimeters, 96.5f},
    };
    for (const SavedSize& size : sizes) {
        SavedSize read;
        REQUIRE(fromLine(toLine(size), &read));
        CHECK_NEAR(read.width, size.width, 1e-6);
        CHECK_NEAR(read.height, size.height, 1e-6);
        CHECK(read.unit == size.unit);
        CHECK_NEAR(read.ppi, size.ppi, 1e-3);
    }
    CHECK_EQ(savedName(sizes[1]), std::string("210 × 297 mm"));
    CHECK_EQ(savedName(sizes[2]), std::string("8,5 × 11 pulg"));
    SavedSize read;
    CHECK(!fromLine("", &read));
    CHECK(!fromLine("1920 1080 px", &read));
    CHECK(!fromLine("1920 1080 px 72 extra", &read));
    CHECK(!fromLine("1920 1080 ft 72", &read));
    CHECK(!fromLine("1920 1080 px 0", &read));
    CHECK(!fromLine("2 2 px 72", &read));            // demasiado pequeño
    CHECK(!fromLine("90000 90000 px 72", &read));    // demasiado grande
}

TEST_CASE(canvas_spec_dates_durations_and_counts) {
    const LocalTime now{2026, 10, 2, 21, 5};
    CHECK_EQ(formatDate({2026, 10, 2, 19, 30}, now), std::string("Hoy, 19:30"));
    CHECK_EQ(formatDate({2026, 10, 1, 8, 5}, now), std::string("Ayer, 8:05"));
    // De este año, con la hora; de otro, con el año.
    CHECK_EQ(formatDate({2026, 9, 30, 23, 59}, now), std::string("30 sept, 23:59"));
    CHECK_EQ(formatDate({2026, 1, 1, 0, 0}, now), std::string("1 ene, 0:00"));
    CHECK_EQ(formatDate({2025, 1, 15, 7, 45}, now), std::string("15 ene 2025"));
    CHECK_EQ(formatDate({2025, 12, 31, 23, 0}, now), std::string("31 dic 2025"));
    // Ayer, cambiando de mes y de año (y con el 29 de febrero).
    CHECK_EQ(formatDate({2026, 12, 31, 0, 0}, {2027, 1, 1, 9, 0}), std::string("Ayer, 0:00"));
    CHECK_EQ(formatDate({2024, 2, 29, 12, 0}, {2024, 3, 1, 9, 0}), std::string("Ayer, 12:00"));
    CHECK_EQ(formatDate({2025, 2, 28, 12, 0}, {2025, 3, 1, 9, 0}), std::string("Ayer, 12:00"));
    // Una fecha futura (el reloj cambió) no es hoy ni ayer.
    CHECK_EQ(formatDate({2026, 10, 3, 10, 0}, now), std::string("3 oct, 10:00"));

    CHECK_EQ(formatDuration(0.0), std::string("Menos de 1 min"));
    CHECK_EQ(formatDuration(59.9), std::string("Menos de 1 min"));
    CHECK_EQ(formatDuration(60.0), std::string("1 min"));
    CHECK_EQ(formatDuration(12 * 60 + 59), std::string("12 min"));
    CHECK_EQ(formatDuration(3600 + 5 * 60), std::string("1 h 5 min"));
    CHECK_EQ(formatDuration(3 * 3600 + 30), std::string("3 h"));
    CHECK_EQ(formatDuration(std::nan("")), std::string("Menos de 1 min"));

    CHECK_EQ(formatCount(0), std::string("0"));
    CHECK_EQ(formatCount(1234), std::string("1234"));
    CHECK_EQ(formatCount(12345), std::string("12 345"));
    CHECK_EQ(formatCount(100000), std::string("100 000"));
    CHECK_EQ(formatCount(1234567), std::string("1 234 567"));
}

TEST_CASE(canvas_spec_drawing_time) {
    // El primer trazo cuenta lo que dura.
    CHECK_NEAR(strokeSeconds(10.0, 12.5, -1.0), 2.5, 1e-9);
    // Una pausa corta antes del trazo también cuenta; una larga, no.
    CHECK_NEAR(strokeSeconds(20.0, 21.0, 12.5), 8.5, 1e-9);
    CHECK_NEAR(strokeSeconds(51.0, 52.0, 21.0), 31.0, 1e-9);   // justo kDrawingPause
    CHECK_NEAR(strokeSeconds(100.0, 101.0, 21.0), 1.0, 1e-9);
    // Relojes sin sentido: nada, o solo el trazo.
    CHECK_NEAR(strokeSeconds(5.0, 4.0, -1.0), 0.0, 1e-9);
    CHECK_NEAR(strokeSeconds(5.0, 6.0, 9.0), 1.0, 1e-9);
    CHECK_NEAR(strokeSeconds(std::nan(""), 6.0, -1.0), 0.0, 1e-9);
}
