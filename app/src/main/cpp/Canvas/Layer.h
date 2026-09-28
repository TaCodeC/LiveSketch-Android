#pragma once

#include "Gfx/GLObjects.h"

#include <cstdint>
#include <string>

// Una capa del lienzo. Es dueña de su textura y su FBO: al destruirse los libera.
struct Layer {
    uint32_t id = 0;        // estable mientras la capa exista (IDs de ImGui, archivos)
    std::string name;
    bool visible = true;
    float opacity = 1.0f;   // 0..1, se aplica al componer
    uint64_t revision = 0;  // sube cada vez que cambian sus píxeles (miniaturas)
    gfx::RenderTarget target;
};
