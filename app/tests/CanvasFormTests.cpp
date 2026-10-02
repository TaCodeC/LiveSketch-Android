// Lo que se configura en la tarjeta de lienzo nuevo: unidades, ppp, candado y escribir
// números (sin interfaz ni GPU).
#include "Test.h"

#include "UI/CanvasForm.h"

#include <cstring>
#include <string>

using Field = CanvasForm::Field;

namespace {

void typeText(CanvasForm& form, const char* text) {
    for (const char* p = text; *p; ++p) {
        form.type(*p);
    }
}

} // namespace

TEST_CASE(canvas_form_starts_full_hd) {
    CanvasForm form;
    CHECK_EQ(form.pixelWidth(), 1920);
    CHECK_EQ(form.pixelHeight(), 1080);
    CHECK(form.unit() == LengthUnit::Pixels);
    CHECK_NEAR(form.ppi(), 72.0f, 1e-6f);
    CHECK(!form.locked());
    CHECK(form.field() == Field::None);
    const CanvasSpec spec = form.spec();
    CHECK_EQ(spec.width, 1920);
    CHECK_EQ(spec.height, 1080);
    CHECK(spec.background.visible);
    CHECK_NEAR(spec.background.color[0], 1.0f, 1e-6f);
    CHECK(spec.name.empty());
}

TEST_CASE(canvas_form_unit_change_keeps_the_size) {
    CanvasForm form;
    form.choose(2480, 3508, LengthUnit::Pixels, 300.0f);
    form.setUnit(LengthUnit::Millimeters);
    CHECK_NEAR(form.width(), 209.97, 0.01);
    CHECK_EQ(form.pixelWidth(), 2480);
    CHECK_EQ(form.pixelHeight(), 3508);
    // De una unidad de papel a otra, exacto y sin cambiar los píxeles.
    for (int i = 0; i < 10; ++i) {
        form.setUnit(LengthUnit::Centimeters);
        form.setUnit(LengthUnit::Inches);
        form.setUnit(LengthUnit::Millimeters);
    }
    CHECK_EQ(form.pixelWidth(), 2480);
    CHECK_EQ(form.pixelHeight(), 3508);
    form.setUnit(LengthUnit::Pixels);
    CHECK_NEAR(form.width(), 2480.0, 1e-9);
    CHECK_NEAR(form.height(), 3508.0, 1e-9);

    // Un A4 elegido en mm sigue midiendo 21 × 29,7 cm.
    form.choose(210, 297, LengthUnit::Millimeters, 300.0f);
    form.setUnit(LengthUnit::Centimeters);
    CHECK_NEAR(form.width(), 21.0, 1e-9);
    CHECK_NEAR(form.height(), 29.7, 1e-9);
}

TEST_CASE(canvas_form_ppi_like_photoshop) {
    // En px, los ppp no cambian los píxeles.
    CanvasForm form;
    form.choose(1920, 1080, LengthUnit::Pixels, 72.0f);
    form.setPpi(300.0f);
    CHECK_EQ(form.pixelWidth(), 1920);
    CHECK_EQ(form.pixelHeight(), 1080);
    // En papel se mantiene el tamaño y cambian los píxeles.
    form.choose(210, 297, LengthUnit::Millimeters, 300.0f);
    CHECK_EQ(form.pixelWidth(), 2480);
    form.setPpi(150.0f);
    CHECK_EQ(form.pixelWidth(), 1240);
    CHECK_EQ(form.pixelHeight(), 1754);
    CHECK_NEAR(form.width(), 210.0, 1e-9);
    // Fuera de los límites se ajusta.
    form.setPpi(0.0f);
    CHECK_NEAR(form.ppi(), canvasspec::kMinPpi, 1e-6f);
    form.setPpi(1.0e6f);
    CHECK_NEAR(form.ppi(), canvasspec::kMaxPpi, 1e-6f);
}

TEST_CASE(canvas_form_lock_keeps_the_ratio) {
    CanvasForm form;
    form.choose(1920, 1080, LengthUnit::Pixels, 72.0f);
    form.setLocked(true);
    form.setWidth(1280);
    CHECK_EQ(form.pixelHeight(), 720);
    form.setHeight(2160);
    CHECK_EQ(form.pixelWidth(), 3840);
    // Al girar, la proporción gira con él.
    form.swap();
    CHECK_EQ(form.pixelWidth(), 2160);
    CHECK_EQ(form.pixelHeight(), 3840);
    form.setWidth(1080);
    CHECK_EQ(form.pixelHeight(), 1920);
    // Sin candado, cada medida va por su lado.
    form.setLocked(false);
    form.setWidth(500);
    CHECK_EQ(form.pixelHeight(), 1920);
    // Elegir otro tamaño con el candado puesto toma su proporción.
    form.setLocked(true);
    form.choose(210, 297, LengthUnit::Millimeters, 300.0f);
    form.setWidth(105);
    CHECK_NEAR(form.height(), 148.5, 1e-9);
}

TEST_CASE(canvas_form_typing_replaces_then_appends) {
    CanvasForm form;
    form.begin(Field::Width);
    CHECK(form.field() == Field::Width);
    CHECK_EQ(form.text(), std::string("1920"));
    CHECK(form.fresh());
    // La primera tecla sustituye lo que había; cada tecla cambia ya el tamaño.
    form.type('3');
    CHECK_EQ(form.text(), std::string("3"));
    CHECK(!form.fresh());
    CHECK_EQ(form.pixelWidth(), 3);
    typeText(form, "840");
    CHECK_EQ(form.text(), std::string("3840"));
    CHECK_EQ(form.pixelWidth(), 3840);
    // En px no hay decimales.
    form.type(',');
    CHECK_EQ(form.text(), std::string("3840"));
    form.erase();
    CHECK_EQ(form.text(), std::string("384"));
    CHECK(form.commit());
    CHECK(form.field() == Field::None);
    CHECK_EQ(form.pixelWidth(), 384);
    CHECK_EQ(form.pixelHeight(), 1080);
}

TEST_CASE(canvas_form_typing_decimals) {
    CanvasForm form;
    form.choose(210, 297, LengthUnit::Millimeters, 300.0f);
    form.begin(Field::Width);
    CHECK_EQ(form.text(), std::string("210"));
    CHECK(form.acceptsComma());
    CHECK_EQ(form.decimals(), 1);
    // Una coma al empezar pone el cero delante; el punto también vale.
    form.type('.');
    CHECK_EQ(form.text(), std::string("0,"));
    typeText(form, "57");
    CHECK_EQ(form.text(), std::string("0,5"));   // en mm, un decimal
    form.type(',');
    CHECK_EQ(form.text(), std::string("0,5"));   // una sola coma
    CHECK_NEAR(form.width(), 0.5, 1e-9);
    form.cancel();
    CHECK_NEAR(form.width(), 210.0, 1e-9);

    // Sin ceros delante.
    form.begin(Field::Height);
    typeText(form, "0042");
    CHECK_EQ(form.text(), std::string("42"));
    // Como mucho nueve caracteres.
    typeText(form, "123456789");
    CHECK_EQ(form.text().size(), size_t{9});
    form.commit();

    // En pulgadas, dos decimales; en los ppp, uno.
    form.setUnit(LengthUnit::Inches);
    form.begin(Field::Width);
    typeText(form, "8,505");
    CHECK_EQ(form.text(), std::string("8,50"));   // el tercer decimal no entra
    form.commit();
    CHECK_NEAR(form.width(), 8.5, 1e-9);
    form.begin(Field::Ppi);
    CHECK_EQ(form.decimals(), 1);
    typeText(form, "96,55");
    CHECK_EQ(form.text(), std::string("96,5"));
    CHECK_NEAR(form.ppi(), 96.5f, 1e-4f);
    form.commit();
}

TEST_CASE(canvas_form_invalid_number_restores) {
    CanvasForm form;
    form.begin(Field::Height);
    form.erase();   // la primera tecla, borrar, lo deja vacío
    CHECK(form.text().empty());
    CHECK(!form.commit());
    CHECK_EQ(form.pixelHeight(), 1080);
    // Un cero no vale.
    form.begin(Field::Width);
    form.type('0');
    CHECK(!form.commit());
    CHECK_EQ(form.pixelWidth(), 1920);
    // Escape deshace lo que se veía mientras se escribía.
    form.setLocked(true);
    form.begin(Field::Width);
    typeText(form, "960");
    CHECK_EQ(form.pixelHeight(), 540);
    form.cancel();
    CHECK_EQ(form.pixelWidth(), 1920);
    CHECK_EQ(form.pixelHeight(), 1080);
    // Sin tocar nada, terminar deja el valor como estaba.
    form.begin(Field::Ppi);
    CHECK(form.commit());
    CHECK_NEAR(form.ppi(), 72.0f, 1e-6f);
}

TEST_CASE(canvas_form_switching_fields_commits) {
    CanvasForm form;
    form.begin(Field::Width);
    typeText(form, "1280");
    // Empezar otro campo, elegir un tamaño o cambiar de unidad deja escrito el anterior.
    form.begin(Field::Height);
    CHECK_EQ(form.pixelWidth(), 1280);
    CHECK(form.field() == Field::Height);
    typeText(form, "720");
    form.setUnit(LengthUnit::Centimeters);
    CHECK(form.field() == Field::None);
    CHECK_EQ(form.pixelHeight(), 720);
    CHECK_EQ(form.pixelWidth(), 1280);
}

TEST_CASE(canvas_form_spec_and_saved_size) {
    CanvasForm form;
    form.choose(8.5, 11, LengthUnit::Inches, 300.0f);
    form.background = CanvasForm::Background::Transparent;
    std::strcpy(form.name, "  Portada  ");
    CanvasSpec spec = form.spec();
    CHECK_EQ(spec.width, 2550);
    CHECK_EQ(spec.height, 3300);
    CHECK_NEAR(spec.ppi, 300.0f, 1e-6f);
    CHECK(spec.unit == LengthUnit::Inches);
    CHECK(!spec.background.visible);
    CHECK_EQ(spec.name, std::string("Portada"));

    form.background = CanvasForm::Background::Color;
    spec = form.spec();
    CHECK(spec.background.visible);
    CHECK_NEAR(spec.background.color[0], form.color[0], 1e-6f);
    CHECK_NEAR(spec.background.color[2], form.color[2], 1e-6f);

    // El tamaño guardado vuelve igual.
    canvasspec::SavedSize back;
    REQUIRE(canvasspec::fromLine(canvasspec::toLine(form.size()), &back));
    CHECK(back == form.size());
}
