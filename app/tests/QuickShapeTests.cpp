// Forma rápida sin GL: qué forma se reconoce en un trazo a mano, cómo se ajusta sin soltar,
// la forma perfecta y su contorno.

#include "Test.h"

#include "Canvas/QuickShape.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace quickshape;

constexpr float kPi = 3.14159265358979f;
constexpr float kDegree = kPi / 180.0f;

// Un trazo a mano: el temblor suave (de `wobble` px como mucho) que deja el pulso.
std::vector<glm::vec2> hand(std::vector<glm::vec2> points, float wobble) {
    for (size_t i = 0; i < points.size(); ++i) {
        const float t = static_cast<float>(i);
        const glm::vec2 shake(std::sin(t * 0.37f) + 0.5f * std::sin(t * 1.3f + 1.0f),
                              std::cos(t * 0.29f) + 0.5f * std::sin(t * 0.9f + 2.0f));
        points[i] += shake * (wobble / 1.5f);
    }
    return points;
}

// Puntos cada `step` px de `a` a `b` (sin `b`).
void walk(std::vector<glm::vec2>& out, glm::vec2 a, glm::vec2 b, float step = 2.0f) {
    const int n = std::max(1, static_cast<int>(std::ceil(glm::distance(a, b) / step)));
    for (int i = 0; i < n; ++i) {
        out.push_back(a + (b - a) * (static_cast<float>(i) / static_cast<float>(n)));
    }
}

// Recorre los vértices en orden; cerrado, vuelve al primero y se pasa un poco.
std::vector<glm::vec2> path(const std::vector<glm::vec2>& vertices, bool closed) {
    std::vector<glm::vec2> out;
    for (size_t i = 0; i + 1 < vertices.size(); ++i) {
        walk(out, vertices[i], vertices[i + 1]);
    }
    if (closed) {
        walk(out, vertices.back(), vertices.front());
        walk(out, vertices.front(), vertices.front() + (vertices[1] - vertices.front()) * 0.06f);
    } else {
        out.push_back(vertices.back());
    }
    return out;
}

// Elipse de semiejes `radii` girada `angle`, desde el ángulo `start` recorriendo `sweep`
// (positivo: en el sentido de las agujas del reloj, con la y hacia abajo).
std::vector<glm::vec2> ellipse(glm::vec2 center, glm::vec2 radii, float angle, float start, float sweep) {
    std::vector<glm::vec2> out;
    const int n = std::max(8, static_cast<int>(std::fabs(sweep) * std::max(radii.x, radii.y) / 2.0f));
    const glm::vec2 u(std::cos(angle), std::sin(angle));
    const glm::vec2 v(-u.y, u.x);
    for (int i = 0; i <= n; ++i) {
        const float t = start + sweep * static_cast<float>(i) / static_cast<float>(n);
        out.push_back(center + u * (radii.x * std::cos(t)) + v * (radii.y * std::sin(t)));
    }
    return out;
}

std::vector<glm::vec2> regularPolygon(glm::vec2 center, float radius, int sides, float rotation) {
    std::vector<glm::vec2> out;
    for (int i = 0; i < sides; ++i) {
        const float a = rotation + 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
        out.push_back(center + glm::vec2(std::cos(a), std::sin(a)) * radius);
    }
    return out;
}

float angleOf(glm::vec2 d) { return std::atan2(d.y, d.x); }

// Diferencia entre dos direcciones sin sentido (módulo 180°).
float axisDifference(float a, float b) {
    const float d = std::remainder(a - b, kPi);
    return std::fabs(d);
}

// Cada punto esperado tiene uno de `actual` a menos de `tolerance`.
bool sameVertices(const std::vector<glm::vec2>& actual, const std::vector<glm::vec2>& expected, float tolerance) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (const glm::vec2& e : expected) {
        const bool found = std::any_of(actual.begin(), actual.end(),
                                       [&](const glm::vec2& a) { return glm::distance(a, e) <= tolerance; });
        if (!found) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE(quickshape_recognizes_lines) {
    std::vector<glm::vec2> stroke;
    walk(stroke, {20.0f, 30.0f}, {260.0f, 150.0f});
    stroke.push_back({260.0f, 150.0f});
    stroke = hand(stroke, 1.5f);
    const Shape line = recognize(stroke);
    REQUIRE(line.kind == Kind::Line);
    REQUIRE(line.points.size() == 2);
    // De donde empezó a donde terminó el trazo (26,6°: no se endereza).
    CHECK_NEAR(glm::distance(line.points[0], stroke.front()), 0.0f, 0.01f);
    CHECK_NEAR(glm::distance(line.points[1], stroke.back()), 0.0f, 0.05f);
    CHECK_EQ(std::string(name(line)), std::string("Línea"));
    CHECK(!line.closed());

    // Casi horizontal: horizontal del todo, con el mismo largo.
    std::vector<glm::vec2> flat;
    walk(flat, {20.0f, 100.0f}, {220.0f, 104.0f});
    flat.push_back({220.0f, 104.0f});
    const Shape level = recognize(flat);
    REQUIRE(level.kind == Kind::Line);
    CHECK_NEAR(level.points[1].y, level.points[0].y, 1e-3f);
    CHECK_NEAR(glm::distance(level.points[0], level.points[1]), glm::distance(flat.front(), flat.back()), 0.05f);

    // Con la vista girada 30°, una línea casi horizontal en la pantalla se endereza con ella.
    Options turned;
    turned.screenRight = {std::cos(30.0f * kDegree), std::sin(30.0f * kDegree)};
    std::vector<glm::vec2> tilted;
    const glm::vec2 end = glm::vec2(20.0f, 20.0f) + glm::vec2(std::cos(31.5f * kDegree), std::sin(31.5f * kDegree)) * 200.0f;
    walk(tilted, {20.0f, 20.0f}, end);
    tilted.push_back(end);
    const Shape screenLevel = recognize(tilted, turned);
    REQUIRE(screenLevel.kind == Kind::Line);
    CHECK_NEAR(angleOf(screenLevel.points[1] - screenLevel.points[0]), 30.0f * kDegree, 1e-3f);
    // Sin girar la vista se queda como está.
    const Shape free = recognize(tilted);
    REQUIRE(free.kind == Kind::Line);
    CHECK_NEAR(angleOf(free.points[1] - free.points[0]), 31.5f * kDegree, 1e-3f);
}

TEST_CASE(quickshape_ignores_short_strokes_and_scribbles) {
    std::vector<glm::vec2> tiny;
    walk(tiny, {10.0f, 10.0f}, {20.0f, 12.0f});
    tiny.push_back({20.0f, 12.0f});
    CHECK(recognize(tiny).kind == Kind::None);
    CHECK(recognize(std::vector<glm::vec2>{{5.0f, 5.0f}}).kind == Kind::None);
    CHECK(recognize(std::vector<glm::vec2>{}).kind == Kind::None);
    // Más corto que el mínimo que se pide.
    std::vector<glm::vec2> line;
    walk(line, {0.0f, 0.0f}, {60.0f, 0.0f});
    line.push_back({60.0f, 0.0f});
    Options strict;
    strict.minLength = 80.0f;
    CHECK(recognize(line, strict).kind == Kind::None);
    CHECK(recognize(line).kind == Kind::Line);

    // Una espiral no se parece a ninguna forma.
    std::vector<glm::vec2> spiral;
    for (int i = 0; i <= 600; ++i) {
        const float t = static_cast<float>(i) / 600.0f;
        const float a = t * 3.0f * 2.0f * kPi;
        const float r = 20.0f + 100.0f * t;
        spiral.push_back(glm::vec2(200.0f, 200.0f) + glm::vec2(std::cos(a), std::sin(a)) * r);
    }
    CHECK(recognize(spiral).kind == Kind::None);
    // Un garabato de ida y vuelta tampoco.
    std::vector<glm::vec2> zigzag;
    for (int i = 0; i < 12; ++i) {
        walk(zigzag, {20.0f + 15.0f * static_cast<float>(i), i % 2 == 0 ? 20.0f : 120.0f},
             {35.0f + 15.0f * static_cast<float>(i), i % 2 == 0 ? 120.0f : 20.0f});
    }
    CHECK(recognize(zigzag).kind == Kind::None);
}

TEST_CASE(quickshape_recognizes_circles_and_ellipses) {
    const std::vector<glm::vec2> round = hand(ellipse({200.0f, 200.0f}, {100.0f, 100.0f}, 0.0f, 0.3f, 2.0f * kPi + 0.15f), 1.5f);
    const Shape circle = recognize(round);
    REQUIRE(circle.kind == Kind::Circle);
    CHECK(circle.closed());
    CHECK(glm::distance(circle.center, glm::vec2(200.0f, 200.0f)) < 3.0f);
    CHECK_NEAR(std::fabs(circle.radii.x), 100.0f, 3.0f);
    CHECK_NEAR(std::fabs(circle.radii.y), 100.0f, 3.0f);
    CHECK_EQ(std::string(name(circle)), std::string("Círculo"));

    const std::vector<glm::vec2> oval = hand(ellipse({300.0f, 220.0f}, {150.0f, 70.0f}, 0.5f, 0.0f, 2.0f * kPi + 0.1f), 1.5f);
    const Shape e = recognize(oval);
    REQUIRE(e.kind == Kind::Ellipse);
    CHECK(glm::distance(e.center, glm::vec2(300.0f, 220.0f)) < 4.0f);
    const float a = std::fabs(e.radii.x);
    const float b = std::fabs(e.radii.y);
    CHECK_NEAR(std::max(a, b), 150.0f, 5.0f);
    CHECK_NEAR(std::min(a, b), 70.0f, 5.0f);
    const float major = a >= b ? e.angle : e.angle + kPi * 0.5f;
    CHECK(axisDifference(major, 0.5f) < 3.0f * kDegree);
    CHECK_EQ(std::string(name(e)), std::string("Elipse"));

    // Casi derecha: se endereza con los ejes del lienzo.
    const Shape straight = recognize(ellipse({300.0f, 220.0f}, {150.0f, 70.0f}, 4.0f * kDegree, 0.0f, 2.0f * kPi + 0.1f));
    REQUIRE(straight.kind == Kind::Ellipse);
    CHECK(axisDifference(straight.angle, 0.0f) < 1e-4f || axisDifference(straight.angle, kPi * 0.5f) < 1e-4f);
}

TEST_CASE(quickshape_closed_outline_starts_where_the_stroke_started) {
    // En los dos sentidos: el contorno empieza donde el trazo y va hacia el mismo lado.
    for (const float sweep : {2.0f * kPi + 0.12f, -(2.0f * kPi + 0.12f)}) {
        const std::vector<glm::vec2> stroke = ellipse({250.0f, 250.0f}, {160.0f, 80.0f}, 0.4f, 1.1f, sweep);
        const Shape e = recognize(stroke);
        REQUIRE(e.kind == Kind::Ellipse);
        CHECK_EQ(e.radii.y < 0.0f, sweep < 0.0f);
        const std::vector<glm::vec2> out = outline(e);
        REQUIRE(out.size() > 20);
        CHECK(glm::distance(out.front(), stroke.front()) < 3.0f);
        CHECK(glm::distance(out.front(), out.back()) < 1e-3f);
        // Al principio va por donde iba el trazo.
        const glm::vec2 drawn = stroke[stroke.size() / 20] - stroke.front();
        const glm::vec2 traced = out[out.size() / 20] - out.front();
        CHECK(glm::dot(glm::normalize(drawn), glm::normalize(traced)) > 0.9f);
    }
    // El rectángulo también, desde la esquina más cercana.
    const std::vector<glm::vec2> corners = {{340.0f, 100.0f}, {100.0f, 100.0f}, {100.0f, 220.0f}, {340.0f, 220.0f}};
    const Shape rect = recognize(path(corners, true));
    REQUIRE(rect.kind == Kind::Rectangle);
    CHECK(glm::distance(rect.points[0], corners[0]) < 3.0f);
    CHECK(glm::distance(rect.points[1], corners[1]) < 3.0f);
}

TEST_CASE(quickshape_recognizes_rectangles_and_squares) {
    const std::vector<glm::vec2> corners = {{100.0f, 100.0f}, {340.0f, 100.0f}, {340.0f, 220.0f}, {100.0f, 220.0f}};
    const Shape rect = recognize(hand(path(corners, true), 1.2f));
    REQUIRE(rect.kind == Kind::Rectangle);
    CHECK(sameVertices(rect.points, corners, 4.0f));
    CHECK_EQ(std::string(name(rect)), std::string("Rectángulo"));
    // Derecho del todo (los lados, horizontales y verticales).
    CHECK_NEAR(rect.points[0].y, rect.points[1].y, 1e-3f);
    CHECK_NEAR(rect.points[1].x, rect.points[2].x, 1e-3f);

    // Perfecto: un cuadrado con el mismo centro y del lado medio.
    const Shape square = regular(rect);
    REQUIRE(square.kind == Kind::Rectangle);
    CHECK(square.regular);
    CHECK_EQ(std::string(name(square)), std::string("Cuadrado"));
    const float side = (glm::distance(rect.points[0], rect.points[1]) + glm::distance(rect.points[1], rect.points[2])) * 0.5f;
    for (size_t i = 0; i < 4; ++i) {
        CHECK_NEAR(glm::distance(square.points[i], square.points[(i + 1) % 4]), side, 0.01f);
    }
    glm::vec2 middle(0.0f);
    for (const glm::vec2& p : square.points) {
        middle += p * 0.25f;
    }
    CHECK(glm::distance(middle, rect.center) < 0.01f);

    // Casi cuadrado: ya sale cuadrado.
    const std::vector<glm::vec2> nearly = {{100.0f, 100.0f}, {300.0f, 100.0f}, {300.0f, 305.0f}, {100.0f, 305.0f}};
    const Shape almost = recognize(path(nearly, true));
    REQUIRE(almost.kind == Kind::Rectangle);
    CHECK_EQ(std::string(name(almost)), std::string("Cuadrado"));

    // Girado 20°: sigue siendo un rectángulo, girado igual.
    std::vector<glm::vec2> turned;
    const glm::vec2 c(250.0f, 250.0f);
    for (const glm::vec2& p : corners) {
        const glm::vec2 d = p - glm::vec2(220.0f, 160.0f);
        const float a = 20.0f * kDegree;
        turned.push_back(c + glm::vec2(d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a)));
    }
    const Shape tilted = recognize(hand(path(turned, true), 1.0f));
    REQUIRE(tilted.kind == Kind::Rectangle);
    CHECK(sameVertices(tilted.points, turned, 4.0f));
}

TEST_CASE(quickshape_recognizes_triangles_and_polygons) {
    const std::vector<glm::vec2> triangle = {{200.0f, 60.0f}, {320.0f, 260.0f}, {80.0f, 260.0f}};
    const Shape t = recognize(hand(path(triangle, true), 1.2f));
    REQUIRE(t.kind == Kind::Triangle);
    CHECK(sameVertices(t.points, triangle, 5.0f));
    CHECK_EQ(std::string(name(t)), std::string("Triángulo"));

    // Perfecto: equilátero, con el mismo centro y el primer vértice hacia el mismo lado.
    const Shape equilateral = regular(t);
    REQUIRE(equilateral.points.size() == 3);
    const float side = glm::distance(equilateral.points[0], equilateral.points[1]);
    CHECK_NEAR(glm::distance(equilateral.points[1], equilateral.points[2]), side, 0.01f);
    CHECK_NEAR(glm::distance(equilateral.points[2], equilateral.points[0]), side, 0.01f);
    glm::vec2 middle(0.0f);
    for (const glm::vec2& p : equilateral.points) {
        middle += p / 3.0f;
    }
    CHECK(glm::distance(middle, t.center) < 0.01f);
    CHECK(glm::dot(glm::normalize(equilateral.points[0] - t.center), glm::normalize(t.points[0] - t.center)) > 0.999f);

    const std::vector<glm::vec2> pentagon = regularPolygon({300.0f, 300.0f}, 120.0f, 5, 0.2f);
    const Shape p = recognize(hand(path(pentagon, true), 1.0f));
    REQUIRE(p.kind == Kind::Polygon);
    CHECK(sameVertices(p.points, pentagon, 5.0f));
    CHECK_EQ(std::string(name(p)), std::string("Pentágono"));

    // Un rombo no es un rectángulo: es un cuadrilátero.
    const std::vector<glm::vec2> rhombus = {{200.0f, 80.0f}, {300.0f, 250.0f}, {200.0f, 420.0f}, {100.0f, 250.0f}};
    const Shape r = recognize(path(rhombus, true));
    REQUIRE(r.kind == Kind::Polygon);
    CHECK(sameVertices(r.points, rhombus, 4.0f));
    CHECK_EQ(std::string(name(r)), std::string("Cuadrilátero"));
}

TEST_CASE(quickshape_recognizes_arcs_and_polylines) {
    // Media circunferencia por arriba (la y va hacia abajo).
    const std::vector<glm::vec2> half = hand(ellipse({200.0f, 200.0f}, {120.0f, 120.0f}, 0.0f, kPi, kPi), 1.0f);
    const Shape arc = recognize(half);
    REQUIRE(arc.kind == Kind::Arc);
    REQUIRE(arc.points.size() == 3);
    CHECK(glm::distance(arc.points[0], half.front()) < 0.01f);
    CHECK(glm::distance(arc.points[2], half.back()) < 0.01f);
    CHECK(glm::distance(arc.points[1], glm::vec2(200.0f, 80.0f)) < 4.0f);
    const std::vector<glm::vec2> curve = outline(arc);
    for (const glm::vec2& q : curve) {
        CHECK(q.y <= 202.0f);
    }

    const std::vector<glm::vec2> corner = {{50.0f, 50.0f}, {250.0f, 50.0f}, {250.0f, 200.0f}};
    const Shape polyline = recognize(hand(path(corner, false), 1.0f));
    REQUIRE(polyline.kind == Kind::Polyline);
    CHECK(sameVertices(polyline.points, corner, 4.0f));
    CHECK(!polyline.closed());
    // Perfectos no cambian.
    CHECK(regular(arc).points == arc.points);
    CHECK(regular(polyline).points == polyline.points);
}

TEST_CASE(quickshape_adjust_follows_the_pointer) {
    Shape line;
    line.kind = Kind::Line;
    line.points = {{0.0f, 0.0f}, {100.0f, 0.0f}};
    const Shape moved = adjust(line, {100.0f, 0.0f}, {150.0f, 80.0f});
    CHECK(glm::distance(moved.points[0], glm::vec2(0.0f, 0.0f)) < 1e-4f);
    CHECK(glm::distance(moved.points[1], glm::vec2(150.0f, 80.0f)) < 1e-3f);

    // Los cerrados crecen y giran alrededor de su centro.
    Shape circle;
    circle.kind = Kind::Circle;
    circle.center = {100.0f, 100.0f};
    circle.radii = {50.0f, 50.0f};
    const Shape bigger = adjust(circle, {150.0f, 100.0f}, {100.0f, 200.0f});
    CHECK_NEAR(bigger.radii.x, 100.0f, 1e-3f);
    CHECK_NEAR(bigger.angle, 0.0f, 1e-6f);   // el círculo no gira

    Shape square;
    square.kind = Kind::Rectangle;
    square.center = {100.0f, 100.0f};
    square.points = {{50.0f, 50.0f}, {150.0f, 50.0f}, {150.0f, 150.0f}, {50.0f, 150.0f}};
    const float turn = 30.0f * kDegree;
    const glm::vec2 from(150.0f, 150.0f);
    const glm::vec2 d = from - square.center;
    const glm::vec2 to = square.center + glm::vec2(d.x * std::cos(turn) - d.y * std::sin(turn),
                                                   d.x * std::sin(turn) + d.y * std::cos(turn));
    const Shape turned = adjust(square, from, to);
    CHECK(glm::distance(turned.points[2], to) < 1e-3f);
    CHECK_NEAR(glm::distance(turned.points[0], turned.points[1]), 100.0f, 1e-3f);
    // Un giro pequeño no tuerce la forma al cambiar su tamaño.
    const Shape scaled = adjust(square, from, {152.0f, 153.0f});
    CHECK_NEAR(scaled.points[0].y, scaled.points[1].y, 1e-4f);
    CHECK(scaled.points[2].x > 150.0f);

    // Arco: el final va al puntero y la curvatura se mantiene.
    Shape arc;
    arc.kind = Kind::Arc;
    arc.points = {{0.0f, 0.0f}, {50.0f, -20.0f}, {100.0f, 0.0f}};
    const Shape longer = adjust(arc, {100.0f, 0.0f}, {200.0f, 0.0f});
    CHECK(glm::distance(longer.points[2], glm::vec2(200.0f, 0.0f)) < 1e-4f);
    CHECK(glm::distance(longer.points[1], glm::vec2(100.0f, -40.0f)) < 1e-3f);
}

TEST_CASE(quickshape_regular_line_turns_in_steps) {
    Shape line;
    line.kind = Kind::Line;
    line.points = {{0.0f, 0.0f}, {100.0f, 40.0f}};
    const Shape snapped = regular(line);
    CHECK(snapped.regular);
    CHECK_NEAR(angleOf(snapped.points[1] - snapped.points[0]), 15.0f * kDegree, 1e-4f);
    CHECK_NEAR(glm::distance(snapped.points[0], snapped.points[1]), glm::length(glm::vec2(100.0f, 40.0f)), 1e-3f);
    // Al ajustarla sigue yendo de 15 en 15°.
    const Shape moved = adjust(snapped, snapped.points[1], {50.0f, 100.0f});
    const float angle = angleOf(moved.points[1] - moved.points[0]);
    CHECK_NEAR(std::remainder(angle, 15.0f * kDegree), 0.0f, 1e-4f);

    // Elipse perfecta: un círculo del radio medio, en el mismo sentido.
    Shape e;
    e.kind = Kind::Ellipse;
    e.center = {0.0f, 0.0f};
    e.radii = {120.0f, -60.0f};
    e.start = 0.7f;
    const Shape c = regular(e);
    CHECK(c.kind == Kind::Circle);
    CHECK_NEAR(c.radii.x, 90.0f, 1e-4f);
    CHECK_NEAR(c.radii.y, -90.0f, 1e-4f);
    // Empieza hacia el mismo lado que la elipse.
    const glm::vec2 first = outline(e).front();
    const glm::vec2 circleFirst = outline(c).front();
    CHECK(glm::dot(glm::normalize(first), glm::normalize(circleFirst)) > 0.9999f);
}

TEST_CASE(quickshape_outline_stays_on_the_shape) {
    Shape circle;
    circle.kind = Kind::Circle;
    circle.center = {300.0f, 300.0f};
    circle.radii = {200.0f, 200.0f};
    circle.start = 0.3f;
    const float tolerance = 0.15f;
    const std::vector<glm::vec2> out = outline(circle, tolerance);
    REQUIRE(out.size() > 12);
    CHECK(glm::distance(out.front(), out.back()) < 1e-6f);
    for (size_t i = 0; i < out.size(); ++i) {
        CHECK_NEAR(glm::distance(out[i], circle.center), 200.0f, 0.01f);
        if (i > 0) {
            // El medio de cada cuerda, a menos de la tolerancia de la curva.
            const glm::vec2 mid = (out[i] + out[i - 1]) * 0.5f;
            CHECK(200.0f - glm::distance(mid, circle.center) <= tolerance + 0.01f);
        }
    }
    // Las cerradas de lados rectos terminan en su primer vértice.
    Shape triangle;
    triangle.kind = Kind::Triangle;
    triangle.points = {{0.0f, 0.0f}, {10.0f, 0.0f}, {0.0f, 10.0f}};
    const std::vector<glm::vec2> closed = outline(triangle);
    REQUIRE(closed.size() == 4);
    CHECK(closed.front() == closed.back());
    CHECK(outline(Shape{}).empty());
}
