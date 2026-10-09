#include "Canvas/ImageAdjust.h"

#include "Gfx/Shader.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Ajustes de color: cada píxel sin premultiplicar, con su alfa. Con la convención de fila
// 0 = arriba, gl_FragCoord son píxeles del lienzo.
constexpr const char* kColorFragment = R"(
precision highp int;
precision highp sampler2D;
uniform sampler2D uLayer;
uniform sampler2D uMask;
uniform int uMaskOn;
uniform int uKind;          // 0 tono, saturación y brillo; 1 balance de color; 2 ruido
uniform vec3 uHsb;          // tono (en vueltas), saturación y brillo, de -1 a 1
uniform vec3 uBalance[3];   // sombras, medios tonos y luces: cian-rojo, magenta-verde, amarillo-azul
uniform vec2 uNoise;        // cantidad y tamaño del grano (px)
uniform uint uSeed;
out vec4 fragColor;

vec3 toHsl(vec3 c) {
    float high = max(c.r, max(c.g, c.b));
    float low = min(c.r, min(c.g, c.b));
    float l = (high + low) * 0.5;
    float d = high - low;
    if (d <= 0.0) {
        return vec3(0.0, 0.0, l);
    }
    float s = l > 0.5 ? d / (2.0 - high - low) : d / (high + low);
    float h;
    if (high == c.r) {
        h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    } else if (high == c.g) {
        h = (c.b - c.r) / d + 2.0;
    } else {
        h = (c.r - c.g) / d + 4.0;
    }
    return vec3(h / 6.0, s, l);
}

vec3 fromHsl(vec3 hsl) {
    vec3 hue = clamp(abs(mod(hsl.x * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    float chroma = (1.0 - abs(2.0 * hsl.z - 1.0)) * hsl.y;
    return hsl.z + chroma * (hue - 0.5);
}

// Girar el tono, multiplicar la saturación y llevar la luz hacia el blanco o el negro.
vec3 hueSaturation(vec3 c) {
    vec3 hsl = toHsl(c);
    hsl.x = fract(hsl.x + uHsb.x);
    hsl.y = clamp(hsl.y * (1.0 + uHsb.y), 0.0, 1.0);
    hsl.z = uHsb.z >= 0.0 ? hsl.z + (1.0 - hsl.z) * uHsb.z : hsl.z * (1.0 + uHsb.z);
    return fromHsl(hsl);
}

// Como el balance de color de GIMP: cada rango pesa según la luminosidad del píxel, con
// rampas de 0,25 de ancho en 1/3 y 2/3 (los tres pesos suman 1).
float balanceChannel(float value, float l, float shadows, float midtones, float highlights) {
    const float a = 0.25;
    const float b = 0.333;
    const float scale = 0.7;
    float s = shadows * clamp((l - b) / -a + 0.5, 0.0, 1.0);
    float m = midtones * clamp((l - b) / a + 0.5, 0.0, 1.0) * clamp((l + b - 1.0) / -a + 0.5, 0.0, 1.0);
    float h = highlights * clamp((l + b - 1.0) / a + 0.5, 0.0, 1.0);
    return clamp(value + (s + m + h) * scale, 0.0, 1.0);
}

vec3 colorBalance(vec3 c) {
    float l = (max(c.r, max(c.g, c.b)) + min(c.r, min(c.g, c.b))) * 0.5;
    vec3 n = vec3(balanceChannel(c.r, l, uBalance[0].x, uBalance[1].x, uBalance[2].x),
                  balanceChannel(c.g, l, uBalance[0].y, uBalance[1].y, uBalance[2].y),
                  balanceChannel(c.b, l, uBalance[0].z, uBalance[1].z, uBalance[2].z));
    // Conserva la luminosidad: el tono y la saturación nuevos con la luz de antes.
    vec3 hsl = toHsl(n);
    hsl.z = l;
    return fromHsl(hsl);
}

uint scramble(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Valor de -0,5 a 0,5 de un punto de la rejilla: la media de dos al azar (más cerca de 0).
float cell(ivec2 c) {
    uint h = scramble(scramble(uint(c.x) ^ uSeed) ^ uint(c.y));
    uint h2 = scramble(h ^ 0x9e3779b9u);
    return (float(h >> 8) + float(h2 >> 8)) * (1.0 / 33554432.0) - 0.5;
}

// Grano fijo al lienzo. Con más de un píxel, la rejilla se interpola (suave).
float grain(ivec2 p) {
    float size = uNoise.y;
    if (size <= 1.0) {
        return cell(p);
    }
    vec2 q = vec2(p) / size;
    vec2 i = floor(q);
    vec2 f = q - i;
    f = f * f * (3.0 - 2.0 * f);
    ivec2 c = ivec2(i);
    float v = mix(mix(cell(c), cell(c + ivec2(1, 0)), f.x), mix(cell(c + ivec2(0, 1)), cell(c + ivec2(1, 1)), f.x),
                  f.y);
    // Interpolar resta contraste: se compensa de media, así el grano grande se nota igual.
    return v * mix(1.0, 1.346, clamp(size - 1.0, 0.0, 1.0));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 dst = texelFetch(uLayer, p, 0);
    vec4 result = dst;
    if (dst.a > 0.0) {
        vec3 c = clamp(dst.rgb / dst.a, 0.0, 1.0);
        if (uKind == 0) {
            c = hueSaturation(c);
        } else if (uKind == 1) {
            c = colorBalance(c);
        } else {
            c = clamp(c + grain(p) * uNoise.x, 0.0, 1.0);
        }
        result = vec4(c * dst.a, dst.a);
    }
    float m = uMaskOn == 1 ? texelFetch(uMask, p, 0).r : 1.0;
    fragColor = mix(dst, result, m);
}
)";

// Una pasada del desenfoque en una dirección (o, con uRadius = 0, una sola lectura: al
// reducir a la mitad, el filtro bilineal en la esquina común promedia 2×2). Cada lectura
// fuera del centro toma dos texels: el filtro bilineal los mezcla con sus pesos.
constexpr const char* kBlurFragment = R"(
precision highp sampler2D;
uniform sampler2D uSource;
uniform vec4 uMap;      // uv = uMap.xy + gl_FragCoord.xy * uMap.zw
uniform vec4 uLimit;    // uv mínima y máxima: los centros de los texels del borde de la zona con datos
uniform vec2 uStep;     // un texel en la dirección del desenfoque (uv)
uniform float uSigma;   // en texels
uniform int uRadius;
out vec4 fragColor;

vec4 tap(vec2 uv) {
    return texture(uSource, clamp(uv, uLimit.xy, uLimit.zw));
}

void main() {
    vec2 uv = uMap.xy + gl_FragCoord.xy * uMap.zw;
    vec4 sum = tap(uv);
    float total = 1.0;
    float k = -0.5 / max(uSigma * uSigma, 0.0001);
    for (int i = 1; i <= 64; i += 2) {
        if (i > uRadius) {
            break;
        }
        float a = exp(float(i * i) * k);
        float b = i < uRadius ? exp(float((i + 1) * (i + 1)) * k) : 0.0;
        float w = a + b;
        float o = (float(i) * a + float(i + 1) * b) / w;
        sum += (tap(uv + uStep * o) + tap(uv - uStep * o)) * w;
        total += 2.0 * w;
    }
    fragColor = sum / total;
}
)";

// Resultado del desenfoque o de enfocar en el lienzo. uLevel 0: uBlurred tiene solo la
// pasada horizontal, a tamaño completo, y aquí se hace la vertical; 1: es un nivel reducido
// ya desenfocado, que se amplía con una B-spline cúbica (suave: sin la cuadrícula que
// dejaría el filtro bilineal).
constexpr const char* kFinalFragment = R"(
precision highp int;
precision highp sampler2D;
uniform sampler2D uLayer;
uniform sampler2D uBlurred;
uniform sampler2D uMask;
uniform int uMaskOn;
uniform int uMode;        // 0 desenfocar, 1 enfocar
uniform int uLevel;
uniform vec4 uMap;        // uv en uBlurred = uMap.xy + gl_FragCoord.xy * uMap.zw
uniform vec4 uLimit;
uniform vec2 uStep;
uniform float uSigma;
uniform int uRadius;
uniform float uAmount;    // enfocar: cuánto se suma la diferencia
uniform int uAlphaLock;
out vec4 fragColor;

vec4 tap(vec2 uv) {
    return texture(uBlurred, clamp(uv, uLimit.xy, uLimit.zw));
}

vec4 cubic(float v) {
    vec4 n = vec4(1.0, 2.0, 3.0, 4.0) - v;
    vec4 s = n * n * n;
    float x = s.x;
    float y = s.y - 4.0 * s.x;
    float z = s.z - 4.0 * s.y + 6.0 * s.x;
    float w = 6.0 - x - y - z;
    return vec4(x, y, z, w) * (1.0 / 6.0);
}

// B-spline cúbica con cuatro lecturas bilineales.
vec4 bicubic(vec2 uv) {
    vec2 size = vec2(textureSize(uBlurred, 0));
    vec2 texel = uv * size - 0.5;
    vec2 f = fract(texel);
    texel -= f;
    vec4 xc = cubic(f.x);
    vec4 yc = cubic(f.y);
    vec4 c = texel.xxyy + vec2(-0.5, 1.5).xyxy;
    vec4 s = vec4(xc.xz + xc.yw, yc.xz + yc.yw);
    vec4 offset = (c + vec4(xc.yw, yc.yw) / s) / size.xxyy;
    vec4 s0 = tap(offset.xz);
    vec4 s1 = tap(offset.yz);
    vec4 s2 = tap(offset.xw);
    vec4 s3 = tap(offset.yw);
    float sx = s.x / (s.x + s.y);
    float sy = s.z / (s.z + s.w);
    return mix(mix(s3, s2, sx), mix(s1, s0, sx), sy);
}

vec4 blurred() {
    vec2 uv = uMap.xy + gl_FragCoord.xy * uMap.zw;
    if (uLevel == 1) {
        return bicubic(uv);
    }
    vec4 sum = tap(uv);
    float total = 1.0;
    float k = -0.5 / max(uSigma * uSigma, 0.0001);
    for (int i = 1; i <= 64; i += 2) {
        if (i > uRadius) {
            break;
        }
        float a = exp(float(i * i) * k);
        float b = i < uRadius ? exp(float((i + 1) * (i + 1)) * k) : 0.0;
        float w = a + b;
        float o = (float(i) * a + float(i + 1) * b) / w;
        sum += (tap(uv + uStep * o) + tap(uv - uStep * o)) * w;
        total += 2.0 * w;
    }
    return sum / total;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 dst = texelFetch(uLayer, p, 0);
    vec4 b = blurred();
    vec4 result;
    if (uMode == 0) {
        result = b;
        if (uAlphaLock == 1) {
            // El color de lo pintado alrededor, con el alfa que ya tenía la capa.
            result = b.a > 0.0001 ? vec4(clamp(b.rgb / b.a, 0.0, 1.0) * dst.a, dst.a) : dst;
        }
    } else if (uAlphaLock == 1) {
        vec3 c = dst.a > 0.0 ? dst.rgb / dst.a : vec3(0.0);
        vec3 around = b.a > 0.0001 ? b.rgb / b.a : c;
        result = vec4(clamp(c + (c - around) * uAmount, 0.0, 1.0) * dst.a, dst.a);
    } else {
        vec4 s = dst + (dst - b) * uAmount;
        float a = clamp(s.a, 0.0, 1.0);
        result = vec4(clamp(s.rgb, vec3(0.0), vec3(a)), a);
    }
    float m = uMaskOn == 1 ? texelFetch(uMask, p, 0).r : 1.0;
    fragColor = mix(dst, result, m);
}
)";

// Enfocar: la diferencia con una gaussiana de esta sigma (px), multiplicada hasta por esto.
constexpr float kSharpenSigma = 1.2f;
constexpr float kSharpenGain = 3.0f;
// Por debajo, el desenfoque no se nota: la capa queda igual.
constexpr float kMinBlur = 0.3f;
// En el nivel más reducido se desenfoca al menos con esta sigma (en texels): con menos, lo
// que se pierde al reducir se nota (el resultado cambia según dónde cae cada borde en la
// rejilla del nivel). Como mucho, 2^kMaxLevels.
constexpr float kMinLevelSigma = 3.0f;
constexpr int kMaxLevels = 7;
// Los intermedios crecen de esto en esto (px): cambiar la sigma no los vuelve a crear.
constexpr int kBufferStep = 64;

int radiusFor(float sigma) { return std::clamp(static_cast<int>(std::ceil(sigma * 3.0f)), 1, 64); }

struct BlurPlan {
    int levels = 0;       // reducciones a la mitad
    float sigma = 0.0f;   // en texels del nivel
    int radius = 0;
};

// Varianzas (en px² del lienzo) que se suman con `levels` reducciones de S = 2^levels: la
// media de S×S píxeles, (S² - 1)/12; la gaussiana del nivel, sigma²·S²; y la ampliación con
// la B-spline cúbica, S²/3. La del nivel es la que falta para llegar a sigma².
BlurPlan planBlur(float sigma) {
    BlurPlan plan{0, sigma, radiusFor(sigma)};
    for (int levels = 1; levels <= kMaxLevels; ++levels) {
        const float s = static_cast<float>(1 << levels);
        const float variance = sigma * sigma / (s * s) - 5.0f / 12.0f + 1.0f / (12.0f * s * s);
        if (variance < kMinLevelSigma * kMinLevelSigma) {
            break;
        }
        const float levelSigma = std::sqrt(variance);
        plan = {levels, levelSigma, radiusFor(levelSigma)};
    }
    return plan;
}

void copyRect(GLuint source, GLuint target, const IRect& rect) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(rect.x0, rect.y0, rect.x1, rect.y1, rect.x0, rect.y0, rect.x1, rect.y1, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// uv de los centros de los texels del borde de la zona con datos.
void limitOf(int width, int height, int usedWidth, int usedHeight, float out[4]) {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    out[0] = 0.5f / w;
    out[1] = 0.5f / h;
    out[2] = (static_cast<float>(usedWidth) - 0.5f) / w;
    out[3] = (static_cast<float>(usedHeight) - 0.5f) / h;
}

} // namespace

bool AdjustParams::neutral(Adjustment kind) const {
    switch (kind) {
    case Adjustment::HueSaturation:
        return hue == 0.0f && saturation == 0.0f && brightness == 0.0f;
    case Adjustment::ColorBalance:
        for (const auto& range : balance) {
            for (float value : range) {
                if (value != 0.0f) {
                    return false;
                }
            }
        }
        return true;
    case Adjustment::Blur:
        return blur < kMinBlur;
    case Adjustment::Sharpen:
        return sharpen <= 0.0f;
    case Adjustment::Noise:
        return noise <= 0.0f;
    }
    return true;
}

bool ImageAdjust::init() {
    destroy();
    m_colorProgram = gfx::makeProgram("adjust color", kVertex, kColorFragment);
    m_blurProgram = gfx::makeProgram("adjust blur", kVertex, kBlurFragment);
    m_finalProgram = gfx::makeProgram("adjust final", kVertex, kFinalFragment);
    if (!m_colorProgram || !m_blurProgram || !m_finalProgram) {
        destroy();
        return false;
    }
    const GLuint color = m_colorProgram.id();
    m_uColorLayer = glGetUniformLocation(color, "uLayer");
    m_uColorMask = glGetUniformLocation(color, "uMask");
    m_uColorMaskOn = glGetUniformLocation(color, "uMaskOn");
    m_uColorKind = glGetUniformLocation(color, "uKind");
    m_uHsb = glGetUniformLocation(color, "uHsb");
    m_uBalance = glGetUniformLocation(color, "uBalance");
    m_uNoise = glGetUniformLocation(color, "uNoise");
    m_uSeed = glGetUniformLocation(color, "uSeed");

    const GLuint blur = m_blurProgram.id();
    m_uBlurSource = glGetUniformLocation(blur, "uSource");
    m_uBlurMap = glGetUniformLocation(blur, "uMap");
    m_uBlurLimit = glGetUniformLocation(blur, "uLimit");
    m_uBlurStep = glGetUniformLocation(blur, "uStep");
    m_uBlurSigma = glGetUniformLocation(blur, "uSigma");
    m_uBlurRadius = glGetUniformLocation(blur, "uRadius");

    const GLuint final = m_finalProgram.id();
    m_uFinalLayer = glGetUniformLocation(final, "uLayer");
    m_uFinalBlurred = glGetUniformLocation(final, "uBlurred");
    m_uFinalMask = glGetUniformLocation(final, "uMask");
    m_uFinalMaskOn = glGetUniformLocation(final, "uMaskOn");
    m_uFinalMode = glGetUniformLocation(final, "uMode");
    m_uFinalLevel = glGetUniformLocation(final, "uLevel");
    m_uFinalMap = glGetUniformLocation(final, "uMap");
    m_uFinalLimit = glGetUniformLocation(final, "uLimit");
    m_uFinalStep = glGetUniformLocation(final, "uStep");
    m_uFinalSigma = glGetUniformLocation(final, "uSigma");
    m_uFinalRadius = glGetUniformLocation(final, "uRadius");
    m_uFinalAmount = glGetUniformLocation(final, "uAmount");
    m_uFinalAlphaLock = glGetUniformLocation(final, "uAlphaLock");

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
    return true;
}

void ImageAdjust::destroy() {
    dropBuffers();
    m_vbo.reset();
    m_vao.reset();
    m_finalProgram.reset();
    m_blurProgram.reset();
    m_colorProgram.reset();
}

void ImageAdjust::dropBuffers() {
    m_temp = {};
    m_levels.clear();
    m_levelTemp = {};
}

bool ImageAdjust::ensure(Buffer& buffer, int width, int height) {
    buffer.usedWidth = width;
    buffer.usedHeight = height;
    if (buffer.target && buffer.target.width >= width && buffer.target.height >= height) {
        return true;
    }
    const int w = std::max(buffer.target.width, (width + kBufferStep - 1) / kBufferStep * kBufferStep);
    const int h = std::max(buffer.target.height, (height + kBufferStep - 1) / kBufferStep * kBufferStep);
    buffer.target.destroy();
    gfx::clearErrors();
    if (!buffer.target.create(w, h) || !gfx::checkErrors("ImageAdjust::ensure")) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Sin memoria para el desenfoque (%dx%d)", w, h);
        buffer.target.destroy();
        return false;
    }
    return true;
}

bool ImageAdjust::draw(const gfx::RenderTarget& target, const gfx::RenderTarget& layer, const IRect& rect,
                       Adjustment kind, const AdjustParams& params, bool alphaLock, GLuint mask) {
    const IRect area = rect.intersected(IRect::ofSize(std::min(target.width, layer.width),
                                                      std::min(target.height, layer.height)));
    if (area.empty() || !m_colorProgram) {
        return true;
    }
    glDisable(GL_SCISSOR_TEST);
    if (params.neutral(kind)) {
        copyRect(layer.fbo.id(), target.fbo.id(), area);
        return true;
    }
    switch (kind) {
    case Adjustment::HueSaturation:
    case Adjustment::ColorBalance:
    case Adjustment::Noise:
        drawColor(target, layer, area, kind, params, mask);
        return true;
    case Adjustment::Blur:
        return drawBlurred(target, layer, area, params.blur, false, 0.0f, alphaLock, mask);
    case Adjustment::Sharpen:
        return drawBlurred(target, layer, area, kSharpenSigma, true, std::clamp(params.sharpen, 0.0f, 1.0f) * kSharpenGain,
                           alphaLock, mask);
    }
    return true;
}

void ImageAdjust::drawColor(const gfx::RenderTarget& target, const gfx::RenderTarget& layer, const IRect& rect,
                            Adjustment kind, const AdjustParams& params, GLuint mask) {
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo.id());
    glViewport(0, 0, target.width, target.height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(rect.x0, rect.y0, rect.width(), rect.height());
    glDisable(GL_BLEND);

    // Todas las unidades llevan una textura válida (WebGL avisa si no, aunque no se lea).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, layer.texture.id());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mask != 0 ? mask : layer.texture.id());
    glUseProgram(m_colorProgram.id());
    glUniform1i(m_uColorLayer, 0);
    glUniform1i(m_uColorMask, 1);
    glUniform1i(m_uColorMaskOn, mask != 0 ? 1 : 0);
    int code = 0;
    if (kind == Adjustment::ColorBalance) {
        code = 1;
    } else if (kind == Adjustment::Noise) {
        code = 2;
    }
    glUniform1i(m_uColorKind, code);
    glUniform3f(m_uHsb, std::clamp(params.hue, -1.0f, 1.0f) * 0.5f, std::clamp(params.saturation, -1.0f, 1.0f),
                std::clamp(params.brightness, -1.0f, 1.0f));
    float balance[9];
    for (int range = 0; range < 3; ++range) {
        for (int axis = 0; axis < 3; ++axis) {
            balance[range * 3 + axis] = std::clamp(params.balance[range][axis], -1.0f, 1.0f);
        }
    }
    glUniform3fv(m_uBalance, 3, balance);
    glUniform2f(m_uNoise, std::clamp(params.noise, 0.0f, 1.0f), std::max(params.noiseSize, 1.0f));
    glUniform1ui(m_uSeed, params.seed);
    glBindVertexArray(m_vao.id());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    for (GLenum unit : {GL_TEXTURE1, GL_TEXTURE0}) {
        glActiveTexture(unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ImageAdjust::blurPass(Buffer& buffer, GLuint source, int sourceWidth, int sourceHeight, int limitWidth,
                           int limitHeight, const float map[4], float stepX, float stepY, float sigma) {
    glBindFramebuffer(GL_FRAMEBUFFER, buffer.target.fbo.id());
    glViewport(0, 0, buffer.usedWidth, buffer.usedHeight);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    float limit[4];
    limitOf(sourceWidth, sourceHeight, limitWidth, limitHeight, limit);
    glUniform4f(m_uBlurMap, map[0], map[1], map[2], map[3]);
    glUniform4f(m_uBlurLimit, limit[0], limit[1], limit[2], limit[3]);
    glUniform2f(m_uBlurStep, stepX, stepY);
    glUniform1f(m_uBlurSigma, sigma);
    glUniform1i(m_uBlurRadius, sigma > 0.0f ? radiusFor(sigma) : 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

bool ImageAdjust::drawBlurred(const gfx::RenderTarget& target, const gfx::RenderTarget& layer, const IRect& rect,
                              float sigma, bool sharpen, float amount, bool alphaLock, GLuint mask) {
    if (sigma < kMinBlur) {
        copyRect(layer.fbo.id(), target.fbo.id(), rect);
        return true;
    }
    const BlurPlan plan = planBlur(sigma);
    // Lo que se lee alrededor de `rect`: el radio de la gaussiana y, con reducciones, la
    // media de cada texel y la B-spline de la ampliación. Fuera del lienzo se repite su
    // borde. A tamaño completo ya lo hace leer la capa; reduciendo, los niveles cubren
    // también lo de fuera (la primera reducción lee la capa con el borde repetido), así la
    // gaussiana del nivel ve junto al borde lo mismo que a tamaño completo.
    const int margin = plan.levels == 0 ? plan.radius : (plan.radius + 3) << plan.levels;
    IRect ext{rect.x0 - margin, rect.y0 - margin, rect.x1 + margin, rect.y1 + margin};
    if (plan.levels == 0) {
        ext = ext.intersected(IRect::ofSize(layer.width, layer.height));
    }
    const float cw = static_cast<float>(layer.width);
    const float ch = static_cast<float>(layer.height);
    const float x0 = static_cast<float>(ext.x0);
    const float y0 = static_cast<float>(ext.y0);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(m_blurProgram.id());
    glUniform1i(m_uBlurSource, 0);
    glBindVertexArray(m_vao.id());

    const Buffer* blurred = nullptr;
    float finalMap[4];
    if (plan.levels == 0) {
        // Horizontal leyendo la capa; la vertical, en la pasada final.
        if (!ensure(m_temp, ext.width(), ext.height())) {
            glBindVertexArray(0);
            glUseProgram(0);
            glEnable(GL_BLEND);
            return false;
        }
        const float map[4] = {x0 / cw, y0 / ch, 1.0f / cw, 1.0f / ch};
        blurPass(m_temp, layer.texture.id(), layer.width, layer.height, layer.width, layer.height, map, 1.0f / cw, 0.0f,
                 plan.sigma);
        blurred = &m_temp;
        const float tw = static_cast<float>(m_temp.target.width);
        const float th = static_cast<float>(m_temp.target.height);
        finalMap[0] = -x0 / tw;
        finalMap[1] = -y0 / th;
        finalMap[2] = 1.0f / tw;
        finalMap[3] = 1.0f / th;
    } else {
        if (static_cast<int>(m_levels.size()) < plan.levels) {
            m_levels.resize(static_cast<size_t>(plan.levels));
        }
        // Reducciones: cada texel lee la esquina común de 2×2 del nivel anterior.
        bool ok = true;
        GLuint source = layer.texture.id();
        int sourceW = layer.width;
        int sourceH = layer.height;
        int usedW = layer.width;
        int usedH = layer.height;
        int w = ext.width();
        int h = ext.height();
        for (int i = 0; i < plan.levels && ok; ++i) {
            w = (w + 1) / 2;
            h = (h + 1) / 2;
            Buffer& level = m_levels[static_cast<size_t>(i)];
            if (!ensure(level, w, h)) {
                ok = false;
                break;
            }
            float map[4];
            if (i == 0) {
                map[0] = x0 / cw;
                map[1] = y0 / ch;
                map[2] = 2.0f / cw;
                map[3] = 2.0f / ch;
            } else {
                map[0] = 0.0f;
                map[1] = 0.0f;
                map[2] = 2.0f / static_cast<float>(sourceW);
                map[3] = 2.0f / static_cast<float>(sourceH);
            }
            blurPass(level, source, sourceW, sourceH, usedW, usedH, map, 0.0f, 0.0f, 0.0f);
            source = level.target.texture.id();
            sourceW = level.target.width;
            sourceH = level.target.height;
            usedW = w;
            usedH = h;
        }
        Buffer& last = m_levels[static_cast<size_t>(plan.levels - 1)];
        if (!ok || !ensure(m_levelTemp, w, h)) {
            glBindVertexArray(0);
            glUseProgram(0);
            glEnable(GL_BLEND);
            return false;
        }
        const float lw = static_cast<float>(last.target.width);
        const float lh = static_cast<float>(last.target.height);
        const float tw = static_cast<float>(m_levelTemp.target.width);
        const float tht = static_cast<float>(m_levelTemp.target.height);
        const float mapLast[4] = {0.0f, 0.0f, 1.0f / lw, 1.0f / lh};
        blurPass(m_levelTemp, last.target.texture.id(), last.target.width, last.target.height, w, h, mapLast, 1.0f / lw,
                 0.0f, plan.sigma);
        const float mapTemp[4] = {0.0f, 0.0f, 1.0f / tw, 1.0f / tht};
        blurPass(last, m_levelTemp.target.texture.id(), m_levelTemp.target.width, m_levelTemp.target.height, w, h,
                 mapTemp, 0.0f, 1.0f / tht, plan.sigma);
        blurred = &last;
        // Ampliación: el texel j del nivel cubre los píxeles [j·S, (j+1)·S) desde ext.
        const float s = static_cast<float>(1 << plan.levels);
        finalMap[0] = -x0 / (s * lw);
        finalMap[1] = -y0 / (s * lh);
        finalMap[2] = 1.0f / (s * lw);
        finalMap[3] = 1.0f / (s * lh);
    }

    // Pasada final en el lienzo, solo dentro de `rect`.
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo.id());
    glViewport(0, 0, target.width, target.height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(rect.x0, rect.y0, rect.width(), rect.height());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, layer.texture.id());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurred->target.texture.id());
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mask != 0 ? mask : layer.texture.id());
    glUseProgram(m_finalProgram.id());
    glUniform1i(m_uFinalLayer, 0);
    glUniform1i(m_uFinalBlurred, 1);
    glUniform1i(m_uFinalMask, 2);
    glUniform1i(m_uFinalMaskOn, mask != 0 ? 1 : 0);
    glUniform1i(m_uFinalMode, sharpen ? 1 : 0);
    glUniform1i(m_uFinalLevel, plan.levels == 0 ? 0 : 1);
    glUniform4f(m_uFinalMap, finalMap[0], finalMap[1], finalMap[2], finalMap[3]);
    float limit[4];
    limitOf(blurred->target.width, blurred->target.height, blurred->usedWidth, blurred->usedHeight, limit);
    glUniform4f(m_uFinalLimit, limit[0], limit[1], limit[2], limit[3]);
    glUniform2f(m_uFinalStep, 0.0f, 1.0f / static_cast<float>(blurred->target.height));
    glUniform1f(m_uFinalSigma, plan.sigma);
    glUniform1i(m_uFinalRadius, plan.levels == 0 ? plan.radius : 0);
    glUniform1f(m_uFinalAmount, amount);
    glUniform1i(m_uFinalAlphaLock, alphaLock ? 1 : 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    for (GLenum unit : {GL_TEXTURE2, GL_TEXTURE1, GL_TEXTURE0}) {
        glActiveTexture(unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}
