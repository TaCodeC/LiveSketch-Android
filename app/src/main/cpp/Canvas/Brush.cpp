#include "Canvas/Brush.h"

#include "Canvas/BrushTips.h"
#include "Gfx/Shader.h"
#include "IO/Assets.h"
#include "ThirdParty/stb_image.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

static_assert(sizeof(Dab) == 6 * sizeof(float), "los sellos se suben tal cual");

// Píxeles alrededor de la punta redonda para su antialias.
constexpr float kRoundPad = 1.0f;

// Unidades de textura del sello húmedo.
constexpr GLint kWetPatchUnit = 0;
constexpr GLint kWetTipUnit = 1;
constexpr GLint kWetSelectionUnit = 2;
constexpr GLint kWetGrainUnit = 3;
constexpr GLint kWetOriginalUnit = 4;
constexpr GLint kWetCoverageUnit = 5;
constexpr GLint kWetUnitCount = 6;
// Con todo el arrastre, sobre pintura queda esta parte de la pintura del pincel sin poner.
constexpr float kWetPickup = 0.9f;

uint32_t mixBits(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aCorner;   // esquina del quad, de (-1,-1) a (1,1)
layout(location = 1) in vec4 aDab;      // por sello: centro x, centro y, radio, alfa
layout(location = 2) in float aAngle;   // por sello: giro en radianes
uniform vec2 uCanvasSize;
uniform float uRoundness;   // la punta se aplana a lo alto
uniform float uPad;         // píxeles de más alrededor de la punta
uniform float uFlip;        // 1: imagen invertida (pinceles de antes)
out vec2 vLocal;            // píxeles desde el centro, sin aplanar: un círculo de radio vRadius
out vec2 vUV;
out float vAlpha;
out float vRadius;
void main() {
    float r = aDab.z;
    vec2 halfSize = vec2(r, r * uRoundness) + uPad;
    vec2 local = aCorner * halfSize;
    float c = cos(aAngle);
    float s = sin(aAngle);
    vec2 p = aDab.xy + vec2(local.x * c - local.y * s, local.x * s + local.y * c);
    gl_Position = vec4(p / uCanvasSize * 2.0 - 1.0, 0.0, 1.0);
    vLocal = vec2(local.x, local.y / uRoundness);
    vec2 uv = local / vec2(r, r * uRoundness);
    vUV = vec2(uv.x, mix(uv.y, -uv.y, uFlip)) * 0.5 + 0.5;
    vAlpha = aDab.w;
    vRadius = r;
}
)";

constexpr const char* kFragment = R"(
in vec2 vLocal;
in vec2 vUV;
in float vAlpha;
in float vRadius;
uniform int uRound;          // 1: punta redonda calculada aquí
uniform float uHardness;
uniform sampler2D uTip;
uniform vec3 uColor;
out vec4 fragColor;
void main() {
    float a;
    if (uRound == 1) {
        float d = length(vLocal);
        // Borde con antialias de un píxel de pantalla y caída suave según la dureza.
        float edge = clamp((vRadius - d) / max(fwidth(d), 1e-4) + 0.5, 0.0, 1.0);
        float soft = (1.0 - uHardness) * vRadius;
        float t = soft > 1e-3 ? clamp((vRadius - d) / soft, 0.0, 1.0) : 1.0;
        a = edge * t * t * (3.0 - 2.0 * t);
    } else {
        a = texture(uTip, vUV).a;
    }
    a *= vAlpha;
    fragColor = vec4(uColor * a, a);
}
)";

constexpr const char* kQuadVertex = R"(
layout(location = 0) in vec2 aCorner;
void main() {
    gl_Position = vec4(aCorner, 0.0, 1.0);
}
)";

// Factor del grano en cada píxel, fijo a las coordenadas del destino.
constexpr const char* kGrainFragment = R"(
uniform sampler2D uGrain;
uniform float uGrainScale;   // uv del grano por píxel
uniform float uGrainDepth;
out vec4 fragColor;
void main() {
    fragColor = vec4(0.0, 0.0, 0.0, mix(1.0, texture(uGrain, gl_FragCoord.xy * uGrainScale).a, uGrainDepth));
}
)";

// Un sello de mezcla húmeda, sobre la zona de su caja (con scissor). Lo que hay en la
// capa sale de la copia de la zona (uPatch, sin filtrar en el píxel mismo); lo que arrastra
// el pincel es lo que había a uDelta por detrás, donde estaba el sello anterior:
//   base = lo que hay, mezclado con lo arrastrado según el arrastre
//   final = base, mezclada con el color según la pintura
// cada una en la medida de la punta (su forma, el grano y la selección).
// Cada sello mueve lo que hay una fracción (uDrag por su medida) de lo que avanza: lo
// arrastrado va a esa fracción de la velocidad del pincel, así que recorre lo mismo con
// los sellos más juntos o más separados (solo cambia cuánto se emborrona).
// La pintura sigue a la cobertura del trazo (la segunda salida, como el alfa de un trazo
// normal): lo que un sello sube la cobertura, por uPaint, es la pintura que pone. Así una
// pasada deja uPaint de lo que dejaría el mismo pincel sin mezcla húmeda, con cualquier
// punta y espaciado. Sobre la pintura que ya había, el arrastre rebaja la pintura
// (uPickup): el pincel mezcla lo que hay con su color en vez de taparlo.
// Todo en alfa premultiplicado; los resultados se cuantizan a 8 bits con un tramado al
// azar para que, con poca fuerza, los cambios pequeños no se pierdan al redondear.
constexpr const char* kWetFragment = R"(
// El azar necesita enteros de 32 bits y las copias toda su precisión (por defecto, los
// enteros y las texturas del fragment shader pueden ser de 16 bits o menos).
precision highp int;
precision highp sampler2D;
uniform sampler2D uPatch;
uniform sampler2D uCoveragePatch;   // la cobertura del trazo en la misma zona (en el rojo)
uniform vec4 uPatchRect;     // zona copiada, en píxeles del lienzo: x0, y0, x1, y1
uniform vec2 uPatchTexel;    // 1 / tamaño de la textura de la copia
uniform int uCoverageOn;
uniform vec2 uCenter;
uniform float uRadius;
uniform float uAngle;
uniform float uRoundness;
uniform float uFlip;
uniform int uRound;
uniform float uHardness;
uniform sampler2D uTip;
uniform float uAlpha;        // alfa del sello (flujo y presión), el de un pincel normal
uniform vec2 uDelta;         // del sello anterior a este
uniform float uDrag;         // arrastre con la punta entera
uniform float uPaint;        // pintura de una pasada (0..1)
uniform float uPickup;       // sobre pintura, cuánto la mezcla en vez de taparla (0..1)
uniform sampler2D uOriginal; // la capa antes del trazo: dónde había pintura
uniform int uOriginalOn;
uniform vec3 uColor;
uniform int uAlphaLock;
uniform sampler2D uSelection;
uniform int uSelectionOn;
uniform sampler2D uGrain;
uniform int uGrainOn;
uniform float uGrainScale;
uniform float uGrainDepth;
uniform uint uSeed;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragCoverage;

vec4 patchAt(vec2 p) {
    p = clamp(p, uPatchRect.xy + 0.5, uPatchRect.zw - 0.5);
    return textureLod(uPatch, (p - uPatchRect.xy) * uPatchTexel, 0.0);
}

// Cobertura de la punta, como la del sello normal (ver kFragment).
float coverage(vec2 p) {
    vec2 d = p - uCenter;
    float c = cos(uAngle);
    float s = sin(uAngle);
    vec2 local = vec2(d.x * c + d.y * s, -d.x * s + d.y * c);
    if (uRound == 1) {
        float dist = length(vec2(local.x, local.y / uRoundness));
        float edge = clamp((uRadius - dist) / max(fwidth(dist), 1e-4) + 0.5, 0.0, 1.0);
        float soft = (1.0 - uHardness) * uRadius;
        float t = soft > 1e-3 ? clamp((uRadius - dist) / soft, 0.0, 1.0) : 1.0;
        return edge * t * t * (3.0 - 2.0 * t);
    }
    vec2 uv = local / vec2(uRadius, uRadius * uRoundness);
    float a = texture(uTip, vec2(uv.x, mix(uv.y, -uv.y, uFlip)) * 0.5 + 0.5).a;
    return abs(uv.x) <= 1.0 && abs(uv.y) <= 1.0 ? a : 0.0;
}

// Azar de 0 a 1 para cada píxel y sello.
float dither(vec2 p, uint seed) {
    uvec2 q = uvec2(p);
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u ^ seed;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h >> 8) * (1.0 / 16777216.0);
}

void main() {
    vec2 p = gl_FragCoord.xy;   // las coordenadas del FBO son las del lienzo
    vec4 dst = texelFetch(uPatch, ivec2(p - uPatchRect.xy), 0);
    float before = uCoverageOn == 1 ? texelFetch(uCoveragePatch, ivec2(p - uPatchRect.xy), 0).r : 0.0;
    float shape = coverage(p);
    if (uGrainOn == 1) {
        shape *= mix(1.0, texture(uGrain, p * uGrainScale).a, uGrainDepth);
    }
    if (uSelectionOn == 1) {
        shape *= texelFetch(uSelection, ivec2(p), 0).r;
    }
    shape = clamp(shape, 0.0, 1.0);

    vec4 base = mix(dst, patchAt(p - uDelta), clamp(uDrag * shape, 0.0, 1.0));
    float after = before + clamp(shape * uAlpha, 0.0, 1.0) * (1.0 - before);
    // Solo cuenta la pintura de antes: la del propio trazo no le quita pintura al pincel.
    float under = uOriginalOn == 1 ? texelFetch(uOriginal, ivec2(p), 0).a : 0.0;
    float amount = uPaint * (1.0 - uPickup * under);
    // Lleva lo pintado de `amount * before` a `amount * after`, como la opacidad de un trazo.
    float paint = amount * (after - before) / max(1.0 - amount * before, 1e-4);
    vec4 result = mix(base, vec4(uColor, 1.0), clamp(paint, 0.0, 1.0));
    if (uAlphaLock == 1) {
        result = result.a > 0.5 / 255.0 ? vec4(result.rgb / result.a * dst.a, dst.a) : dst;
    }
    // El valor cuantizado se escribe a un cuarto de su escalón: así el driver lo guarda igual
    // redondee o trunque.
    vec4 q = (floor(clamp(result, 0.0, 1.0) * 255.0 + (0.002 + 0.996 * dither(p, uSeed))) + 0.25) / 255.0;
    q.rgb = min(q.rgb, vec3(q.a));
    float covered = (floor(after * 255.0 + (0.002 + 0.996 * dither(p, uSeed ^ 0x68e31da4u))) + 0.25) / 255.0;
    fragColor = shape > 0.0 ? q : dst;
    fragCoverage = vec4(shape > 0.0 ? max(covered, before) : before, 0.0, 0.0, 1.0);
}
)";

// Textura blanca con `coverage` en el alfa. Con mipmaps (las puntas se ven muy pequeñas).
gfx::Texture makeAlphaTexture(const std::vector<uint8_t>& coverage, int width, int height, bool repeat,
                              bool mipmaps) {
    std::vector<uint8_t> rgba(coverage.size() * 4, 255);
    for (size_t i = 0; i < coverage.size(); ++i) {
        rgba[i * 4 + 3] = coverage[i];
    }
    gfx::Texture texture = gfx::Texture::create();
    glBindTexture(GL_TEXTURE_2D, texture.id());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

bool isClassic(BrushTip tip) {
    return tip == BrushTip::Classic0 || tip == BrushTip::Classic1 || tip == BrushTip::Classic2 ||
           tip == BrushTip::Classic3;
}

// brush0.png ... brush3.png. La cobertura es rojo × alfa (lo que leía la primera versión
// de su textura premultiplicada). Sin mipmaps: se ven como siempre.
gfx::Texture loadClassic(BrushTip tip) {
    const int index = static_cast<int>(tip) - static_cast<int>(BrushTip::Classic0);
    const std::string name = "brush" + std::to_string(index) + ".png";
    const std::vector<uint8_t> file = io::loadAsset(name.c_str());
    if (file.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo leer el pincel %s", name.c_str());
        return {};
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels,
                                            STBI_rgb_alpha);
    if (!pixels) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PNG inválido %s: %s", name.c_str(), stbi_failure_reason());
        return {};
    }
    std::vector<uint8_t> coverage(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (size_t i = 0; i < coverage.size(); ++i) {
        coverage[i] = static_cast<uint8_t>((pixels[i * 4] * pixels[i * 4 + 3] + 127) / 255);
    }
    stbi_image_free(pixels);
    return makeAlphaTexture(coverage, width, height, false, false);
}

} // namespace

bool Brush::init() {
    destroy();

    m_program = gfx::makeProgram("brush", kVertex, kFragment);
    m_grainProgram = gfx::makeProgram("brush-grain", kQuadVertex, kGrainFragment);
    m_wetProgram = gfx::makeProgram("brush-wet", kQuadVertex, kWetFragment);
    if (!m_program || !m_grainProgram || !m_wetProgram) {
        destroy();
        return false;
    }
    const GLuint id = m_program.id();
    m_uCanvasSize = glGetUniformLocation(id, "uCanvasSize");
    m_uRoundness = glGetUniformLocation(id, "uRoundness");
    m_uPad = glGetUniformLocation(id, "uPad");
    m_uFlip = glGetUniformLocation(id, "uFlip");
    m_uRound = glGetUniformLocation(id, "uRound");
    m_uHardness = glGetUniformLocation(id, "uHardness");
    m_uColor = glGetUniformLocation(id, "uColor");
    m_uTip = glGetUniformLocation(id, "uTip");
    m_uGrain = glGetUniformLocation(m_grainProgram.id(), "uGrain");
    m_uGrainScale = glGetUniformLocation(m_grainProgram.id(), "uGrainScale");
    m_uGrainDepth = glGetUniformLocation(m_grainProgram.id(), "uGrainDepth");

    const GLuint wet = m_wetProgram.id();
    m_wet.patchRect = glGetUniformLocation(wet, "uPatchRect");
    m_wet.coverageOn = glGetUniformLocation(wet, "uCoverageOn");
    m_wet.patchTexel = glGetUniformLocation(wet, "uPatchTexel");
    m_wet.center = glGetUniformLocation(wet, "uCenter");
    m_wet.radius = glGetUniformLocation(wet, "uRadius");
    m_wet.angle = glGetUniformLocation(wet, "uAngle");
    m_wet.roundness = glGetUniformLocation(wet, "uRoundness");
    m_wet.flip = glGetUniformLocation(wet, "uFlip");
    m_wet.round = glGetUniformLocation(wet, "uRound");
    m_wet.hardness = glGetUniformLocation(wet, "uHardness");
    m_wet.alpha = glGetUniformLocation(wet, "uAlpha");
    m_wet.delta = glGetUniformLocation(wet, "uDelta");
    m_wet.drag = glGetUniformLocation(wet, "uDrag");
    m_wet.paint = glGetUniformLocation(wet, "uPaint");
    m_wet.pickup = glGetUniformLocation(wet, "uPickup");
    m_wet.originalOn = glGetUniformLocation(wet, "uOriginalOn");
    m_wet.color = glGetUniformLocation(wet, "uColor");
    m_wet.alphaLock = glGetUniformLocation(wet, "uAlphaLock");
    m_wet.selectionOn = glGetUniformLocation(wet, "uSelectionOn");
    m_wet.grainOn = glGetUniformLocation(wet, "uGrainOn");
    m_wet.grainScale = glGetUniformLocation(wet, "uGrainScale");
    m_wet.grainDepth = glGetUniformLocation(wet, "uGrainDepth");
    m_wet.seed = glGetUniformLocation(wet, "uSeed");
    glUseProgram(wet);
    glUniform1i(glGetUniformLocation(wet, "uPatch"), kWetPatchUnit);
    glUniform1i(glGetUniformLocation(wet, "uTip"), kWetTipUnit);
    glUniform1i(glGetUniformLocation(wet, "uSelection"), kWetSelectionUnit);
    glUniform1i(glGetUniformLocation(wet, "uGrain"), kWetGrainUnit);
    glUniform1i(glGetUniformLocation(wet, "uOriginal"), kWetOriginalUnit);
    glUniform1i(glGetUniformLocation(wet, "uCoveragePatch"), kWetCoverageUnit);
    glUseProgram(0);

    const float corners[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    m_vao = gfx::VertexArray::create();
    m_quadVao = gfx::VertexArray::create();
    m_cornerVbo = gfx::Buffer::create();
    m_instanceVbo = gfx::Buffer::create();

    glBindVertexArray(m_vao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_cornerVbo.id());
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo.id());
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Dab), nullptr);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Dab), reinterpret_cast<const void*>(4 * sizeof(float)));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(m_quadVao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_cornerVbo.id());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void Brush::destroy() {
    for (auto& tip : m_tips) {
        tip.reset();
    }
    m_tipFailed.fill(false);
    for (auto& grain : m_grains) {
        grain.reset();
    }
    m_instanceVbo.reset();
    m_cornerVbo.reset();
    m_quadVao.reset();
    m_vao.reset();
    m_wetPatch.reset();
    m_wetCoveragePatch.reset();
    m_wetFbo.reset();
    m_wetPatchWidth = 0;
    m_wetPatchHeight = 0;
    m_wetProgram.reset();
    m_grainProgram.reset();
    m_program.reset();
}

GLuint Brush::tipTexture(BrushTip tip) {
    const auto index = static_cast<size_t>(tip);
    if (index >= m_tips.size()) {
        return 0;
    }
    if (!m_tips[index] && !m_tipFailed[index]) {
        if (isClassic(tip)) {
            m_tips[index] = loadClassic(tip);
        } else {
            m_tips[index] = makeAlphaTexture(brushtips::makeTip(tip), brushtips::kTipSize, brushtips::kTipSize, false,
                                             true);
        }
        m_tipFailed[index] = !m_tips[index];
    }
    return m_tips[index].id();
}

GLuint Brush::grainTexture(BrushGrain grain) {
    const auto index = static_cast<size_t>(grain);
    if (index >= m_grains.size() || grain == BrushGrain::None) {
        return 0;
    }
    if (!m_grains[index]) {
        m_grains[index] = makeAlphaTexture(brushtips::makeGrain(grain), brushtips::kGrainSize, brushtips::kGrainSize,
                                           true, true);
    }
    return m_grains[index].id();
}

bool Brush::prepare(const BrushParams& params) {
    if (!m_program) {
        return false;
    }
    if (params.grain != BrushGrain::None) {
        grainTexture(params.grain);
    }
    return params.tip == BrushTip::Round || tipTexture(params.tip) != 0;
}

IRect Brush::bounds(std::span<const Dab> dabs) {
    IRect rect;
    for (const Dab& dab : dabs) {
        // Un círculo del radio cubre la punta con cualquier giro (la redondez solo la
        // aplana), más el margen del antialias de la redonda.
        rect.unite(IRect::around(dab.x, dab.y, dab.radius + kRoundPad));
    }
    return rect;
}

IRect Brush::draw(GLuint target, int width, int height, std::span<const Dab> dabs, const BrushParams& params,
                  const float color[3]) {
    if (dabs.empty() || !m_program || !prepare(params)) {
        return {};
    }
    const bool round = params.tip == BrushTip::Round;

    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);   // viewport del lienzo, no el de la ventana
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    if (params.buildUp == BrushBuildUp::Uniform) {
        // Cada píxel se queda con el sello más opaco: repasar no oscurece.
        glBlendEquation(GL_MAX);
    } else {
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    glUseProgram(m_program.id());
    glUniform2f(m_uCanvasSize, static_cast<float>(width), static_cast<float>(height));
    glUniform1f(m_uRoundness, std::clamp(params.roundness, 0.1f, 1.0f));
    glUniform1f(m_uPad, round ? kRoundPad : 0.0f);
    glUniform1f(m_uFlip, isClassic(params.tip) ? 1.0f : 0.0f);
    glUniform1i(m_uRound, round ? 1 : 0);
    glUniform1f(m_uHardness, std::clamp(params.hardness, 0.0f, 1.0f));
    glUniform3f(m_uColor, color[0], color[1], color[2]);
    glUniform1i(m_uTip, 0);
    glActiveTexture(GL_TEXTURE0);
    // La redonda no lee la textura, pero la unidad necesita una válida.
    glBindTexture(GL_TEXTURE_2D, tipTexture(round ? BrushTip::Round : params.tip));

    glBindVertexArray(m_vao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo.id());
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(dabs.size_bytes()), dabs.data(), GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(dabs.size()));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return bounds(dabs).intersected(IRect::ofSize(width, height));
}

IRect Brush::wetBounds(std::span<const Dab> dabs, const WetCursor& cursor) {
    IRect rect;
    float fromX = cursor.x;
    float fromY = cursor.y;
    bool started = cursor.started;
    for (const Dab& dab : dabs) {
        const float reach = dab.radius + kRoundPad;
        rect.unite(IRect::around(dab.x, dab.y, reach));
        if (started) {
            rect.unite(IRect::around(fromX, fromY, reach));
        }
        fromX = dab.x;
        fromY = dab.y;
        started = true;
    }
    return rect;
}

bool Brush::ensureWetPatch(int width, int height) {
    if (m_wetPatch && m_wetCoveragePatch && width <= m_wetPatchWidth && height <= m_wetPatchHeight) {
        return true;
    }
    // Crece con margen: no se rehace a cada sello de un pincel que cambia de tamaño.
    const int w = std::max({width, m_wetPatchWidth, 64});
    const int h = std::max({height, m_wetPatchHeight, 64});
    const int newWidth = width > m_wetPatchWidth ? std::max(w, width + width / 2) : w;
    const int newHeight = height > m_wetPatchHeight ? std::max(h, height + height / 2) : h;
    gfx::clearErrors();
    gfx::Texture textures[2];
    for (gfx::Texture& texture : textures) {
        texture = gfx::Texture::create();
        glBindTexture(GL_TEXTURE_2D, texture.id());
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, newWidth, newHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!gfx::checkErrors("Brush::ensureWetPatch")) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Sin memoria para la mezcla húmeda (%dx%d)", newWidth, newHeight);
        return false;
    }
    m_wetPatch = std::move(textures[0]);
    m_wetCoveragePatch = std::move(textures[1]);
    m_wetPatchWidth = newWidth;
    m_wetPatchHeight = newHeight;
    return true;
}

IRect Brush::drawWet(const gfx::RenderTarget& target, const gfx::RenderTarget* coverage, std::span<const Dab> dabs,
                     const BrushParams& params, const WetMix& mix, WetCursor& cursor) {
    if (dabs.empty() || !m_wetProgram || !target || !prepare(params)) {
        return {};
    }
    if (coverage && (!*coverage || coverage->width < target.width || coverage->height < target.height)) {
        coverage = nullptr;
    }
    if (!m_wetFbo) {
        m_wetFbo = gfx::Framebuffer::create();
    }
    const IRect canvas = IRect::ofSize(target.width, target.height);
    const bool round = params.tip == BrushTip::Round;

    // La copia de la zona de cada sello: las texturas se hacen antes, del tamaño del mayor.
    {
        int width = 0;
        int height = 0;
        WetCursor probe = cursor;
        for (const Dab& dab : dabs) {
            IRect patch = wetBounds(std::span<const Dab>(&dab, 1), probe).intersected(canvas);
            probe.x = dab.x;
            probe.y = dab.y;
            probe.started = true;
            width = std::max(width, patch.width());
            height = std::max(height, patch.height());
        }
        if (width == 0 || !ensureWetPatch(width, height)) {
            return {};
        }
    }

    // El destino y su cobertura en un mismo FBO: cada sello escribe los dos.
    glBindFramebuffer(GL_FRAMEBUFFER, m_wetFbo.id());
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture.id(), 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, coverage ? coverage->texture.id() : 0,
                           0);
    const GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, coverage ? GLenum{GL_COLOR_ATTACHMENT1} : GLenum{GL_NONE}};
    glDrawBuffers(2, buffers);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "FBO de la mezcla húmeda incompleto");
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return {};
    }
    glViewport(0, 0, target.width, target.height);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glUseProgram(m_wetProgram.id());
    glUniform2f(m_wet.patchTexel, 1.0f / static_cast<float>(m_wetPatchWidth), 1.0f / static_cast<float>(m_wetPatchHeight));
    glUniform1i(m_wet.coverageOn, coverage ? 1 : 0);
    glUniform1f(m_wet.roundness, std::clamp(params.roundness, 0.1f, 1.0f));
    glUniform1f(m_wet.flip, isClassic(params.tip) ? 1.0f : 0.0f);
    glUniform1i(m_wet.round, round ? 1 : 0);
    glUniform1f(m_wet.hardness, std::clamp(params.hardness, 0.0f, 1.0f));
    glUniform3f(m_wet.color, mix.color[0], mix.color[1], mix.color[2]);
    // Difuminar arrastra todo; su trazo lleva flujo 1 (ver Canvas::beginStroke).
    const float pull = mix.smudge ? 1.0f : std::clamp(params.wetPull, 0.0f, 1.0f);
    const float flow = mix.smudge ? 1.0f : std::max(params.flow, 0.01f);
    glUniform1f(m_wet.pickup, mix.smudge ? 0.0f : kWetPickup * pull);
    glUniform1i(m_wet.originalOn, mix.original ? 1 : 0);
    glUniform1i(m_wet.alphaLock, mix.alphaLock ? 1 : 0);
    glUniform1i(m_wet.selectionOn, mix.selection ? 1 : 0);
    const bool grain = mix.grain != 0 && mix.grainDepth > 0.0f;
    glUniform1i(m_wet.grainOn, grain ? 1 : 0);
    glUniform1f(m_wet.grainScale, mix.grainScale);
    glUniform1f(m_wet.grainDepth, std::clamp(mix.grainDepth, 0.0f, 1.0f));
    // Las unidades que no se usan llevan la copia: nunca una textura del destino (WebGL lo
    // rechaza aunque el shader no la lea).
    const GLuint units[kWetUnitCount] = {m_wetPatch.id(),
                                         tipTexture(round ? BrushTip::Round : params.tip),
                                         mix.selection ? mix.selection : m_wetPatch.id(),
                                         grain ? mix.grain : m_wetPatch.id(),
                                         mix.original ? mix.original : m_wetPatch.id(),
                                         m_wetCoveragePatch.id()};
    for (GLint unit = 0; unit < kWetUnitCount; ++unit) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, units[unit] ? units[unit] : m_wetPatch.id());
    }
    glBindVertexArray(m_quadVao.id());

    IRect touched;
    for (const Dab& dab : dabs) {
        const float fromX = cursor.started ? cursor.x : dab.x;
        const float fromY = cursor.started ? cursor.y : dab.y;
        const IRect patch = wetBounds(std::span<const Dab>(&dab, 1), cursor).intersected(canvas);
        cursor.x = dab.x;
        cursor.y = dab.y;
        cursor.started = true;
        const uint32_t index = cursor.count++;
        const IRect box = IRect::around(dab.x, dab.y, dab.radius + kRoundPad).intersected(canvas);
        if (box.empty() || !(dab.alpha > 0.0f) || !(mix.strength > 0.0f)) {
            continue;
        }
        // Lo que hay ahora (con los sellos anteriores) donde lee este, y su cobertura.
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + kWetPatchUnit));
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, patch.x0, patch.y0, patch.width(), patch.height());
        if (coverage) {
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + kWetCoverageUnit));
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, patch.x0, patch.y0, patch.width(), patch.height());
            glReadBuffer(GL_COLOR_ATTACHMENT0);
        }

        glUniform4f(m_wet.patchRect, static_cast<float>(patch.x0), static_cast<float>(patch.y0),
                    static_cast<float>(patch.x1), static_cast<float>(patch.y1));
        glUniform2f(m_wet.center, dab.x, dab.y);
        glUniform1f(m_wet.radius, dab.radius);
        glUniform1f(m_wet.angle, dab.angle);
        glUniform1f(m_wet.alpha, std::clamp(dab.alpha, 0.0f, 1.0f));
        glUniform2f(m_wet.delta, dab.x - fromX, dab.y - fromY);
        // El arrastre depende de lo que apoya el pincel (la presión), no del flujo de pintura.
        const float contact = std::clamp(dab.alpha / flow, 0.0f, 1.0f);
        glUniform1f(m_wet.drag, std::clamp(pull * contact * mix.strength, 0.0f, 1.0f));
        const float paint = mix.smudge ? 0.0f : brushes::wetPaint(params, mix.radius, dab.distance) * mix.strength;
        glUniform1f(m_wet.paint, std::clamp(paint, 0.0f, 1.0f));
        glUniform1ui(m_wet.seed, mixBits(index * 0x9E3779B1U + 0x85EBCA77U));
        glScissor(box.x0, box.y0, box.width(), box.height());
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        touched.unite(box);
    }

    glBindVertexArray(0);
    for (GLint unit = kWetUnitCount - 1; unit >= 0; --unit) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    // Suelta las texturas: el FBO no las mantiene vivas.
    const GLenum single[2] = {GL_COLOR_ATTACHMENT0, GL_NONE};
    glDrawBuffers(2, single);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return touched;
}

void Brush::applyGrain(GLuint target, int width, int height, const BrushParams& params, const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(width, height));
    const GLuint grain = grainTexture(params.grain);
    if (area.empty() || !grain || params.grainDepth <= 0.0f || !m_grainProgram) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(area.x0, area.y0, area.width(), area.height());
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ZERO, GL_SRC_ALPHA);   // destino *= factor del grano

    glUseProgram(m_grainProgram.id());
    glUniform1i(m_uGrain, 0);
    glUniform1f(m_uGrainScale, 1.0f / (static_cast<float>(brushtips::kGrainSize) * std::max(params.grainScale, 0.01f)));
    glUniform1f(m_uGrainDepth, std::clamp(params.grainDepth, 0.0f, 1.0f));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, grain);
    glBindVertexArray(m_quadVao.id());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
