#pragma once

#include "Canvas/CanvasSpec.h"

#include <string>

// Lo que se configura en la tarjeta de lienzo nuevo, sin la interfaz: el tamaño en su
// unidad, los ppp, el candado de proporción, el fondo, el nombre y el número que se está
// escribiendo.
//
// Las medidas se guardan sin redondear en su unidad, como en Photoshop: en px, cambiar los
// ppp conserva los píxeles; en mm, cm o pulgadas, conserva el tamaño en papel (y cambian
// los píxeles). Pasar de una unidad de papel a otra es exacto, así que ir y volver no
// acumula errores.
class CanvasForm {
public:
    enum class Field { None, Width, Height, Ppi };
    enum class Background { White, Color, Transparent };

    // --- Tamaño ---
    double width() const { return m_width; }   // en unit()
    double height() const { return m_height; }
    LengthUnit unit() const { return m_unit; }
    float ppi() const { return m_ppi; }
    // Lo que mide el lienzo que sale.
    int pixelWidth() const;
    int pixelHeight() const;

    // Un tamaño de la tarjeta (o uno guardado), con su unidad y sus ppp.
    void choose(double width, double height, LengthUnit unit, float ppi);
    // Cambia de unidad sin cambiar el tamaño.
    void setUnit(LengthUnit unit);
    void setPpi(float ppi);
    // Con el candado puesto, la otra medida sigue la proporción.
    void setWidth(double width);
    void setHeight(double height);
    // Vertical <-> horizontal.
    void swap();
    bool locked() const { return m_locked; }
    void setLocked(bool locked);

    // --- Escribir un número ---
    // Mientras se escribe, cada tecla cambia ya el tamaño (el resumen lo enseña); Intro
    // lo deja y Escape vuelve a como estaba.
    Field field() const { return m_field; }
    // Lo escrito (con coma decimal). Al empezar es el valor actual, que la primera tecla
    // sustituye (`fresh`).
    const std::string& text() const { return m_text; }
    bool fresh() const { return m_fresh; }
    // Empieza a escribir en `field` (el que se estuviera escribiendo se aplica antes).
    void begin(Field field);
    // Un dígito o la coma (también vale el punto).
    void type(char c);
    void erase();
    // Termina: si lo escrito no vale (vacío o cero), vuelve a como estaba y devuelve false.
    bool commit();
    void cancel();
    // En px no hay decimales; en las otras unidades y en los ppp, sí.
    bool acceptsComma() const;
    // Cuántos decimales admite el campo que se escribe.
    int decimals() const;

    // --- Lo demás ---
    Background background = Background::White;
    float color[3] = {243.0f / 255.0f, 237.0f / 255.0f, 226.0f / 255.0f};   // el de Background::Color
    char name[64] = {};
    int category = 0;   // canvasspec::PresetCategory que se ve

    // El lienzo que sale (sin comprobar si cabe en el dispositivo).
    CanvasSpec spec() const;
    // El tamaño para "Mis tamaños" y para recordarlo.
    canvasspec::SavedSize size() const;

private:
    void applyText();
    void restore();

    double m_width = 1920.0;
    double m_height = 1080.0;
    LengthUnit m_unit = LengthUnit::Pixels;
    float m_ppi = 72.0f;
    bool m_locked = false;
    double m_ratio = 1920.0 / 1080.0;   // ancho / alto mientras el candado está puesto

    Field m_field = Field::None;
    std::string m_text;
    bool m_fresh = false;
    double m_before[3] = {0.0, 0.0, 0.0};   // ancho, alto y ppp al empezar a escribir
};
