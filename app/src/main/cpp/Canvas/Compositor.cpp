#include "Canvas/Compositor.h"

#include <SDL3/SDL_log.h>

namespace {

// Quad que cubre todo el FBO. Fila 0 de la textura = fila 0 del FBO: no hay volteos.
constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Todo en alfa premultiplicado. Los modos de fusión siguen la especificación W3C
// Compositing and Blending: B(Cb, Cs) se calcula sin premultiplicar y el resultado es
//   co = cs·(1 − ab) + cb·(1 − as) + as·ab·B(Cb, Cs),   ao = as + ab·(1 − as).
// Color más oscuro/claro, Luz intensa, Luz focal, Mezcla definida, Restar y Dividir no
// están en la especificación; siguen las fórmulas habituales de Photoshop.
constexpr const char* kFragment = R"(
in vec2 vUV;
uniform sampler2D uLayer;
uniform sampler2D uStroke;
uniform sampler2D uBackdrop;
uniform sampler2D uClip;
uniform float uOpacity;
uniform float uStrokeOpacity;
uniform int uStrokeMode;       // trazo sobre la capa: 0 no hay, 1 pintar, 2 borrar, 3 pintar encima, 4 sustituir
uniform int uClipMode;         // 1: la capa se recorta con el alfa de uClip
uniform int uClipStrokeMode;   // trazo sobre la base del recorte (mismos valores)
uniform int uBlend;            // BlendMode
uniform int uUseBackdrop;      // 1: se mezcla aquí con uBackdrop; 0: mezcla la GPU (modo Normal)
uniform sampler2D uGrain;
uniform float uGrainScale;     // uv del grano por píxel del lienzo
uniform float uGrainDepth;
uniform int uGrainOn;          // grano del trazo: 0 no hay, 1 sobre uStroke, 2 sobre uLayer
uniform sampler2D uSelection;
uniform int uSelectionOn;      // la máscara de la selección multiplica: 1 el trazo, 2 la capa
uniform vec4 uReplaceArea;     // sustituir: zona de la copia de trabajo (x0, y0, x1, y1)
out vec4 fragColor;

// Las coordenadas del FBO son las del lienzo: el grano queda fijo al lienzo.
float grain() {
    return mix(1.0, texture(uGrain, gl_FragCoord.xy * uGrainScale).a, uGrainDepth);
}

vec4 withStroke(vec4 color, int mode) {
    if (mode == 0) {
        return color;
    }
    vec4 stroke = texture(uStroke, vUV);
    if (mode == 4) {
        bool inside = all(greaterThanEqual(gl_FragCoord.xy, uReplaceArea.xy)) &&
                      all(lessThan(gl_FragCoord.xy, uReplaceArea.zw));
        return inside ? stroke : color;
    }
    stroke *= uStrokeOpacity;
    if (uGrainOn == 1) {
        stroke *= grain();
    }
    if (uSelectionOn == 1) {
        stroke *= texture(uSelection, vUV).r;
    }
    if (mode == 1) {
        return stroke + color * (1.0 - stroke.a);
    }
    if (mode == 2) {
        return color * (1.0 - stroke.a);
    }
    return stroke * color.a + color * (1.0 - stroke.a);   // el alfa de la capa no cambia
}

float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }

vec3 clipColor(vec3 c) {
    float l = lum(c);
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    if (n < 0.0) {
        c = l + (c - l) * l / max(l - n, 1e-6);
    }
    if (x > 1.0) {
        c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
    }
    return c;
}

vec3 setLum(vec3 c, float l) { return clipColor(c + (l - lum(c))); }

float sat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }

vec3 setSat(vec3 c, float s) {
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    return x > n ? (c - n) * s / (x - n) : vec3(0.0);
}

float colorBurn(float b, float s) {
    if (b >= 1.0) {
        return 1.0;
    }
    return s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s);
}

float colorDodge(float b, float s) {
    if (b <= 0.0) {
        return 0.0;
    }
    return s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s));
}

float softLight(float b, float s) {
    if (s <= 0.5) {
        return b - (1.0 - 2.0 * s) * b * (1.0 - b);
    }
    float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
    return b + (2.0 * s - 1.0) * (d - b);
}

float vividLight(float b, float s) {
    return s <= 0.5 ? colorBurn(b, 2.0 * s) : colorDodge(b, 2.0 * s - 1.0);
}

float divide(float b, float s) {
    if (s <= 0.0) {
        return b > 0.0 ? 1.0 : 0.0;
    }
    return min(1.0, b / s);
}

vec3 screen(vec3 b, vec3 s) { return b + s - b * s; }

vec3 hardLight(vec3 b, vec3 s) { return mix(2.0 * b * s, screen(b, 2.0 * s - 1.0), step(0.5, s)); }

// B(Cb, Cs): color de la capa `s` sobre el fondo `b`, los dos sin premultiplicar.
vec3 blendColor(vec3 b, vec3 s) {
    int m = uBlend;
    if (m == 1) { return b * s; }                                     // Multiplicar
    if (m == 2) { return min(b, s); }                                 // Oscurecer
    if (m == 3) {                                                     // Subexposición de color
        return vec3(colorBurn(b.r, s.r), colorBurn(b.g, s.g), colorBurn(b.b, s.b));
    }
    if (m == 4) { return max(b + s - 1.0, 0.0); }                     // Subexposición lineal
    if (m == 5) { return lum(s) < lum(b) ? s : b; }                   // Color más oscuro
    if (m == 6) { return max(b, s); }                                 // Aclarar
    if (m == 7) { return screen(b, s); }                              // Trama
    if (m == 8) {                                                     // Sobreexposición de color
        return vec3(colorDodge(b.r, s.r), colorDodge(b.g, s.g), colorDodge(b.b, s.b));
    }
    if (m == 9) { return min(b + s, 1.0); }                           // Añadir
    if (m == 10) { return lum(s) > lum(b) ? s : b; }                  // Color más claro
    if (m == 11) { return hardLight(s, b); }                          // Superponer
    if (m == 12) {                                                    // Luz suave
        return vec3(softLight(b.r, s.r), softLight(b.g, s.g), softLight(b.b, s.b));
    }
    if (m == 13) { return hardLight(b, s); }                          // Luz fuerte
    if (m == 14) {                                                    // Luz intensa
        return vec3(vividLight(b.r, s.r), vividLight(b.g, s.g), vividLight(b.b, s.b));
    }
    if (m == 15) { return clamp(b + 2.0 * s - 1.0, 0.0, 1.0); }       // Luz lineal
    if (m == 16) { return mix(min(b, 2.0 * s), max(b, 2.0 * s - 1.0), step(0.5, s)); }   // Luz focal
    if (m == 17) { return step(1.0 - 0.5 / 255.0, b + s); }           // Mezcla definida
    if (m == 18) { return abs(b - s); }                               // Diferencia
    if (m == 19) { return b + s - 2.0 * b * s; }                      // Exclusión
    if (m == 20) { return max(b - s, 0.0); }                          // Restar
    if (m == 21) {                                                    // Dividir
        return vec3(divide(b.r, s.r), divide(b.g, s.g), divide(b.b, s.b));
    }
    if (m == 22) { return setLum(setSat(s, sat(b)), lum(b)); }        // Tono
    if (m == 23) { return setLum(setSat(b, sat(s)), lum(b)); }        // Saturación
    if (m == 24) { return setLum(s, lum(b)); }                        // Color
    if (m == 25) { return setLum(b, lum(s)); }                        // Luminosidad
    return s;                                                         // Normal
}

void main() {
    vec4 layer = texture(uLayer, vUV);
    if (uGrainOn == 2) {
        layer *= grain();
    }
    if (uSelectionOn == 2) {
        layer *= texture(uSelection, vUV).r;
    }
    vec4 src = withStroke(layer, uStrokeMode) * uOpacity;
    if (uClipMode == 1) {
        src *= withStroke(texture(uClip, vUV), uClipStrokeMode).a;
    }
    if (uUseBackdrop == 0) {
        fragColor = src;
        return;
    }
    vec4 dst = texture(uBackdrop, vUV);
    if (src.a <= 0.0) {
        fragColor = dst;
        return;
    }
    vec3 cs = clamp(src.rgb / src.a, 0.0, 1.0);
    vec3 cb = dst.a > 0.0 ? clamp(dst.rgb / dst.a, 0.0, 1.0) : vec3(0.0);
    vec3 mixed = clamp(blendColor(cb, cs), 0.0, 1.0);
    fragColor = vec4(src.rgb * (1.0 - dst.a) + dst.rgb * (1.0 - src.a) + src.a * dst.a * mixed,
                     src.a + dst.a * (1.0 - src.a));
}
)";

// Color liso (rellenar, vaciar, invertir, escalar).
constexpr const char* kColorFragment = R"(
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    fragColor = uColor;
}
)";

// Color liso multiplicado por la máscara de la selección. El destino es del tamaño del
// lienzo: su píxel es el de la máscara.
constexpr const char* kMaskedColorFragment = R"(
uniform vec4 uColor;
uniform sampler2D uMask;
out vec4 fragColor;
void main() {
    fragColor = uColor * texelFetch(uMask, ivec2(gl_FragCoord.xy), 0).r;
}
)";

// Filtros de una capa, mezclados con el original según la máscara de la selección
// (colorspace::kGlsl va delante).
constexpr const char* kFilterFragment = R"(
in vec2 vUV;
uniform sampler2D uSource;
uniform sampler2D uMask;
uniform int uMaskOn;
uniform int uFilter;       // 0: invertir; 1: cambiar de perfil de color
out vec4 fragColor;
void main() {
    vec4 c = texture(uSource, vUV);
    vec4 f = c;
    if (uFilter == 0) {
        f = vec4(c.a - c.rgb, c.a);   // premultiplicado: 1 − color pasa a alfa − color
    } else if (uFilter == 1) {
        f = gamutConvertPremultiplied(c);
    }
    float m = uMaskOn == 1 ? texture(uMask, vUV).r : 1.0;
    fragColor = mix(c, f, m);
}
)";

// Unidades de textura del programa del compositor.
constexpr GLint kLayerUnit = 0;
constexpr GLint kStrokeUnit = 1;
constexpr GLint kBackdropUnit = 2;
constexpr GLint kClipUnit = 3;
constexpr GLint kGrainUnit = 4;
constexpr GLint kSelectionUnit = 5;
constexpr GLint kUnitCount = 6;

int strokeMode(const StrokePreview* stroke) {
    if (!stroke) {
        return 0;
    }
    switch (stroke->mode) {
    case StrokePreview::Mode::Paint:
        return 1;
    case StrokePreview::Mode::Erase:
        return 2;
    case StrokePreview::Mode::PaintAtop:
        return 3;
    case StrokePreview::Mode::Replace:
        return 4;
    }
    return 0;
}

void blit(GLuint source, GLuint target, const IRect& rect) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(rect.x0, rect.y0, rect.x1, rect.y1, rect.x0, rect.y0, rect.x1, rect.y1, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
}

} // namespace

bool Compositor::init(int width, int height) {
    destroy();

    m_program = gfx::makeProgram("compositor", kVertex, kFragment);
    m_colorProgram = gfx::makeProgram("compositor-color", kVertex, kColorFragment);
    m_maskedColorProgram = gfx::makeProgram("compositor-masked-color", kVertex, kMaskedColorFragment);
    m_filterProgram = gfx::makeProgram("compositor-filter", kVertex, kFilterFragment, colorspace::kGlsl);
    if (!m_program || !m_colorProgram || !m_maskedColorProgram || !m_filterProgram) {
        destroy();
        return false;
    }
    const GLuint id = m_program.id();
    m_uLayer = glGetUniformLocation(id, "uLayer");
    m_uStroke = glGetUniformLocation(id, "uStroke");
    m_uBackdrop = glGetUniformLocation(id, "uBackdrop");
    m_uClip = glGetUniformLocation(id, "uClip");
    m_uOpacity = glGetUniformLocation(id, "uOpacity");
    m_uStrokeOpacity = glGetUniformLocation(id, "uStrokeOpacity");
    m_uStrokeMode = glGetUniformLocation(id, "uStrokeMode");
    m_uClipMode = glGetUniformLocation(id, "uClipMode");
    m_uClipStrokeMode = glGetUniformLocation(id, "uClipStrokeMode");
    m_uBlend = glGetUniformLocation(id, "uBlend");
    m_uUseBackdrop = glGetUniformLocation(id, "uUseBackdrop");
    m_uGrain = glGetUniformLocation(id, "uGrain");
    m_uGrainScale = glGetUniformLocation(id, "uGrainScale");
    m_uGrainDepth = glGetUniformLocation(id, "uGrainDepth");
    m_uGrainOn = glGetUniformLocation(id, "uGrainOn");
    m_uSelectionOn = glGetUniformLocation(id, "uSelectionOn");
    m_uReplaceArea = glGetUniformLocation(id, "uReplaceArea");
    m_uColor = glGetUniformLocation(m_colorProgram.id(), "uColor");
    m_uMaskedColor = glGetUniformLocation(m_maskedColorProgram.id(), "uColor");
    m_uFilterSource = glGetUniformLocation(m_filterProgram.id(), "uSource");
    m_uFilterMask = glGetUniformLocation(m_filterProgram.id(), "uMask");
    m_uFilterMaskOn = glGetUniformLocation(m_filterProgram.id(), "uMaskOn");
    m_uFilterKind = glGetUniformLocation(m_filterProgram.id(), "uFilter");
    m_uFilterGamut.locate(m_filterProgram);

    glUseProgram(id);
    glUniform1i(m_uLayer, kLayerUnit);
    glUniform1i(m_uStroke, kStrokeUnit);
    glUniform1i(m_uBackdrop, kBackdropUnit);
    glUniform1i(m_uClip, kClipUnit);
    glUniform1i(m_uGrain, kGrainUnit);
    glUniform1i(glGetUniformLocation(id, "uSelection"), kSelectionUnit);
    glUseProgram(m_maskedColorProgram.id());
    glUniform1i(glGetUniformLocation(m_maskedColorProgram.id(), "uMask"), 0);
    glUseProgram(m_filterProgram.id());
    glUniform1i(m_uFilterSource, 0);
    glUniform1i(m_uFilterMask, 1);
    glUseProgram(0);

    const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    m_vao = gfx::VertexArray::create();
    m_vbo = gfx::Buffer::create();
    glBindVertexArray(m_vao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo.id());
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return m_composite.create(width, height);
}

void Compositor::destroy() {
    m_scratch.destroy();
    m_scratchFailed = false;
    m_composite.destroy();
    m_vbo.reset();
    m_vao.reset();
    m_filterProgram.reset();
    m_maskedColorProgram.reset();
    m_colorProgram.reset();
    m_program.reset();
}

bool Compositor::ensureScratch() {
    if (m_scratch) {
        return true;
    }
    if (m_scratchFailed) {
        return false;
    }
    if (!m_scratch.create(m_composite.width, m_composite.height)) {
        // Se vuelve a intentar al recrear el compositor; mientras, esas capas se ven en Normal.
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Sin memoria para los modos de fusión: se muestran en modo Normal");
        m_scratchFailed = true;
        return false;
    }
    return true;
}

void Compositor::bindCanvasPass(GLuint target, const IRect& rect) {
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, m_composite.width, m_composite.height);
    // Con la convención de fila 0 = arriba, las coordenadas del FBO coinciden con las del lienzo.
    glEnable(GL_SCISSOR_TEST);
    glScissor(rect.x0, rect.y0, rect.width(), rect.height());
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(m_vao.id());
}

void Compositor::drawPass(const Pass& pass) {
    // Todas las unidades que usa el programa llevan una textura válida. Ninguna puede ser
    // la del FBO de destino, aunque el shader no la lea: WebGL lo rechaza (bucle de
    // realimentación). La capa nunca es un destino, así que rellena las que sobran.
    const StrokePreview* stroke = pass.stroke ? pass.stroke : pass.clipStroke;
    const StrokeGrain* grain = nullptr;
    int grainOn = 0;
    if (pass.layerGrain && pass.layerGrain->active()) {
        grain = pass.layerGrain;
        grainOn = 2;
    } else if (stroke && stroke->grain.active()) {
        grain = &stroke->grain;
        grainOn = 1;
    }
    const GLuint textures[kUnitCount] = {
        pass.layer,
        stroke ? stroke->texture : pass.layer,
        pass.backdrop ? pass.backdrop : pass.layer,
        pass.clip ? pass.clip : pass.layer,
        grain ? grain->texture : pass.layer,
        pass.selection ? pass.selection : pass.layer,
    };
    for (GLint unit = 0; unit < kUnitCount; ++unit) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, textures[unit]);
    }

    glUseProgram(m_program.id());
    glUniform1f(m_uOpacity, pass.opacity);
    glUniform1f(m_uStrokeOpacity, stroke ? stroke->opacity : 1.0f);
    glUniform1i(m_uStrokeMode, strokeMode(pass.stroke));
    glUniform1i(m_uClipMode, pass.clip ? 1 : 0);
    glUniform1i(m_uClipStrokeMode, pass.stroke ? 0 : strokeMode(pass.clipStroke));
    glUniform1i(m_uBlend, static_cast<int>(pass.blend));
    glUniform1i(m_uUseBackdrop, pass.backdrop ? 1 : 0);
    glUniform1i(m_uGrainOn, grainOn);
    glUniform1f(m_uGrainScale, grain ? grain->scale : 1.0f);
    glUniform1f(m_uGrainDepth, grain ? grain->depth : 0.0f);
    glUniform1i(m_uSelectionOn, pass.selection ? pass.selectionOn : 0);
    const IRect area = stroke ? stroke->area : IRect{};
    glUniform4f(m_uReplaceArea, static_cast<float>(area.x0), static_cast<float>(area.y0), static_cast<float>(area.x1),
                static_cast<float>(area.y1));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::drawColor(const float rgba[4], GLuint mask) {
    if (mask) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mask);
        glUseProgram(m_maskedColorProgram.id());
        glUniform4fv(m_uMaskedColor, 1, rgba);
    } else {
        glUseProgram(m_colorProgram.id());
        glUniform4fv(m_uColor, 1, rgba);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::finishPass() {
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    for (GLint unit = kUnitCount - 1; unit >= 0; --unit) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Compositor::compose(const LayerStack& layers, const CanvasBackground& background, const IRect& rect,
                         const StrokePreview* preview) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }
    const StrokePreview* stroke = preview && preview->texture ? preview : nullptr;
    const int active = layers.activeIndex();

    // Destino con el compuesto hasta ahora. Una capa en modo Normal se mezcla sobre él con
    // la GPU; una en otro modo se escribe en el otro destino leyéndolo como fondo, y los
    // papeles se intercambian.
    const gfx::RenderTarget* current = &m_composite;
    bindCanvasPass(current->fbo.id(), area);
    if (background.visible) {
        glClearColor(background.color[0], background.color[1], background.color[2], 1.0f);
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    for (int i = 0; i < layers.count(); ++i) {
        const Layer& layer = layers.at(i);
        if (!layer.visible || layer.opacity <= 0.0f) {
            continue;
        }
        Pass pass;
        pass.layer = layer.target.texture.id();
        pass.opacity = layer.opacity;
        pass.blend = layer.blend;
        pass.stroke = stroke && i == active ? stroke : nullptr;
        const int base = layers.clipBase(i);
        if (base >= 0) {
            const Layer& clipLayer = layers.at(base);
            if (!clipLayer.visible) {
                continue;   // sin su base a la vista, una capa recortada no se ve
            }
            pass.clip = clipLayer.target.texture.id();
            pass.clipStroke = stroke && base == active ? stroke : nullptr;
        }
        // El trazo solo llega a lo seleccionado (la copia de trabajo ya lo tiene en cuenta).
        if ((pass.stroke || pass.clipStroke) && stroke->selection && stroke->mode != StrokePreview::Mode::Replace) {
            pass.selection = stroke->selection;
            pass.selectionOn = 1;
        }

        if (layer.blend != BlendMode::Normal && ensureScratch()) {
            const gfx::RenderTarget* next = current == &m_composite ? &m_scratch : &m_composite;
            pass.backdrop = current->texture.id();
            glBindFramebuffer(GL_FRAMEBUFFER, next->fbo.id());
            glDisable(GL_BLEND);
            drawPass(pass);
            glEnable(GL_BLEND);
            current = next;
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, current->fbo.id());
            drawPass(pass);
        }
    }

    if (current != &m_composite) {
        blit(current->fbo.id(), m_composite.fbo.id(), area);
    }
    finishPass();
}

void Compositor::draw(GLuint target, GLuint source, float opacity, Blend blend, const IRect& rect,
                      const StrokeGrain* grain, GLuint mask) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }

    bindCanvasPass(target, area);
    switch (blend) {
    case Blend::Over:
        break;
    case Blend::Erase:
        // destino *= 1 - alfa de la fuente
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case Blend::Atop:
        // color: fuente·alfa del destino + destino·(1 - alfa de la fuente); el alfa no cambia
        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        break;
    case Blend::Mask:
        // destino *= alfa de la fuente
        glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
        break;
    }
    Pass pass;
    pass.layer = source;
    pass.opacity = opacity;
    pass.layerGrain = grain;
    pass.selection = mask;
    pass.selectionOn = 2;
    drawPass(pass);
    finishPass();
}

void Compositor::scale(GLuint target, float factor, const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }

    bindCanvasPass(target, area);
    // destino *= factor (los cuatro canales)
    glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
    const float color[4] = {0.0f, 0.0f, 0.0f, factor};
    drawColor(color);
    finishPass();
}

void Compositor::merge(const gfx::RenderTarget& lower, const Layer& upper, GLuint clip, const gfx::RenderTarget& scratch,
                       const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }

    // La mezcla lee la capa de abajo como fondo, así que se escribe en `scratch` y se copia.
    bindCanvasPass(scratch.fbo.id(), area);
    glDisable(GL_BLEND);
    Pass pass;
    pass.layer = upper.target.texture.id();
    pass.opacity = upper.opacity;
    pass.blend = upper.blend;
    pass.clip = clip;
    pass.backdrop = lower.texture.id();
    drawPass(pass);

    blit(scratch.fbo.id(), lower.fbo.id(), area);
    glBindFramebuffer(GL_FRAMEBUFFER, scratch.fbo.id());
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    finishPass();
}

void Compositor::fill(GLuint target, const float rgb[3], Fill mode, const IRect& rect, GLuint mask) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }

    bindCanvasPass(target, area);
    // Con máscara, el color va encima según ella (m): destino·(1 − m) + color·m.
    if (mode == Fill::Replace) {
        if (!mask) {
            glBlendFunc(GL_ONE, GL_ZERO);
        }
    } else if (mask) {
        // color·m·alfa del destino + destino·(1 − m); el alfa no cambia
        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    } else {
        // color · alfa del destino; el alfa no cambia
        glBlendFuncSeparate(GL_DST_ALPHA, GL_ZERO, GL_ZERO, GL_ONE);
    }
    const float color[4] = {rgb[0], rgb[1], rgb[2], 1.0f};
    drawColor(color, mask);
    finishPass();
}

void Compositor::clear(GLuint target, const IRect& rect, GLuint mask) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }

    bindCanvasPass(target, area);
    if (!mask) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        // destino *= 1 − m
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        drawColor(color, mask);
    }
    finishPass();
}

void Compositor::invert(GLuint target, const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }

    bindCanvasPass(target, area);
    // Sin premultiplicar, el color pasa a 1 − c; premultiplicado, a alfa − c:
    // color = 1·alfa del destino − destino; alfa = el del destino.
    glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    drawColor(white);
    finishPass();
}

void Compositor::filter(const gfx::RenderTarget& target, Filter filter, GLuint mask, const gfx::RenderTarget& scratch,
                        const IRect& rect) {
    runFilter(target, static_cast<int>(filter), mask, scratch, rect, colorspace::Transform{});
}

void Compositor::convertColors(const gfx::RenderTarget& target, const colorspace::Transform& transform,
                               const gfx::RenderTarget& scratch, const IRect& rect) {
    if (!transform.identity()) {
        runFilter(target, 1, 0, scratch, rect, transform);
    }
}

void Compositor::runFilter(const gfx::RenderTarget& target, int kind, GLuint mask, const gfx::RenderTarget& scratch,
                           const IRect& rect, const colorspace::Transform& gamut) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_filterProgram) {
        return;
    }

    // El filtro lee la capa, así que se escribe en `scratch` y se copia.
    bindCanvasPass(scratch.fbo.id(), area);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, target.texture.id());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mask ? mask : target.texture.id());
    glUseProgram(m_filterProgram.id());
    glUniform1i(m_uFilterMaskOn, mask ? 1 : 0);
    glUniform1i(m_uFilterKind, kind);
    m_uFilterGamut.set(gamut);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    blit(scratch.fbo.id(), target.fbo.id(), area);
    glBindFramebuffer(GL_FRAMEBUFFER, scratch.fbo.id());
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    finishPass();
}
