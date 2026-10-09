#include "Test.h"

#include "Canvas/LayerStack.h"

namespace {

// Pila de 8×8 con las capas nombradas, de abajo arriba.
LayerStack makeStack(std::initializer_list<const char*> names) {
    LayerStack stack;
    stack.reset(8, 8);
    for (const char* name : names) {
        stack.insert(stack.count(), name);
    }
    return stack;
}

} // namespace

TEST_CASE(layers_insert_names_and_active) {
    LayerStack stack;
    stack.reset(8, 8);
    REQUIRE(stack.insert(0, "Fondo") != nullptr);
    REQUIRE(stack.insert(1, "") != nullptr);
    CHECK_EQ(stack.count(), 2);
    CHECK_EQ(stack.at(1).name, std::string("Capa 1"));
    CHECK_EQ(stack.activeIndex(), 1);   // la capa nueva queda activa

    stack.insert(2, "");
    CHECK_EQ(stack.at(2).name, std::string("Capa 2"));
    // Los nombres automáticos reutilizan el primer número libre.
    stack.rename(1, "Boceto");
    stack.insert(3, "");
    CHECK_EQ(stack.at(3).name, std::string("Capa 1"));

    // Los ids son únicos y estables.
    CHECK(stack.at(0).id != stack.at(1).id);
    CHECK_EQ(stack.indexOf(stack.at(2).id), 2);
    CHECK_EQ(stack.indexOf(9999), -1);

    // Una capa nueva es transparente y no ensucia el compuesto.
    CHECK(stack.dirty().empty());
    CHECK_EQ(test::pixelAt(test::readTarget(stack.at(3).target), 8, 4, 4), (test::Pixel{0, 0, 0, 0}));

    // Un nombre vacío no se acepta.
    stack.rename(0, "");
    CHECK_EQ(stack.at(0).name, std::string("Fondo"));
}

TEST_CASE(layers_remove_keeps_active_layer) {
    LayerStack stack = makeStack({"A", "B", "C", "D"});
    stack.setActive(2);   // C
    const uint32_t c = stack.at(2).id;

    CHECK(stack.remove(0));   // borrar una de abajo mueve el índice, no la capa activa
    CHECK_EQ(stack.activeIndex(), 1);
    CHECK_EQ(stack.active().id, c);

    CHECK(stack.remove(stack.activeIndex()));   // borrar la activa pasa a la de abajo
    CHECK_EQ(stack.active().name, std::string("B"));

    CHECK(stack.remove(0));   // B era la de abajo del todo: queda D
    CHECK_EQ(stack.count(), 1);
    CHECK_EQ(stack.active().name, std::string("D"));
    CHECK(!stack.remove(0));  // nunca se queda vacía
    CHECK(!stack.remove(5));
}

TEST_CASE(layers_move_keeps_active_layer) {
    LayerStack stack = makeStack({"A", "B", "C", "D"});
    stack.setActive(1);   // B
    stack.clearDirty();

    CHECK(stack.move(0, 3));   // A arriba del todo
    CHECK_EQ(stack.at(3).name, std::string("A"));
    CHECK_EQ(stack.at(0).name, std::string("B"));
    CHECK_EQ(stack.active().name, std::string("B"));
    CHECK(!stack.dirty().empty());

    CHECK(stack.move(0, 2));   // la activa también se puede mover
    CHECK_EQ(stack.activeIndex(), 2);
    CHECK_EQ(stack.active().name, std::string("B"));

    CHECK(!stack.move(1, 1));
    CHECK(!stack.move(-1, 2));
    CHECK(!stack.move(0, 4));
}

TEST_CASE(layers_available_name) {
    LayerStack stack = makeStack({"Fondo", "Fondo copia"});
    CHECK_EQ(stack.availableName("Tinta"), std::string("Tinta"));
    CHECK_EQ(stack.availableName("Fondo copia"), std::string("Fondo copia 2"));
    stack.insert(2, "Fondo copia 2");
    CHECK_EQ(stack.availableName("Fondo copia"), std::string("Fondo copia 3"));
}

TEST_CASE(layers_properties_mark_dirty) {
    LayerStack stack = makeStack({"A", "B"});
    stack.clearDirty();

    stack.setOpacity(1, 0.5f);
    CHECK_EQ(stack.at(1).opacity, 0.5f);
    CHECK_EQ(stack.dirty().width(), 8);
    CHECK_EQ(stack.dirty().height(), 8);

    stack.clearDirty();
    stack.setOpacity(1, 0.5f);   // sin cambio: no hay que recomponer
    CHECK(stack.dirty().empty());
    stack.setOpacity(1, 3.0f);
    CHECK_EQ(stack.at(1).opacity, 1.0f);

    stack.clearDirty();
    stack.setVisible(0, false);
    CHECK(!stack.at(0).visible);
    CHECK(!stack.dirty().empty());

    // Lo que se marca fuera del lienzo se recorta.
    stack.clearDirty();
    stack.markDirty({-5, 6, 3, 20});
    CHECK_EQ(stack.dirty().x0, 0);
    CHECK_EQ(stack.dirty().y0, 6);
    CHECK_EQ(stack.dirty().x1, 3);
    CHECK_EQ(stack.dirty().y1, 8);
}
