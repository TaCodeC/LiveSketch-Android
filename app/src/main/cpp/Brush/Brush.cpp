#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "Brush.h"
#include <vector>
#include <android/log.h>

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "Brush", __VA_ARGS__))

void Brush::Init() {
    compileShaders();
    InitBuffers();
    size = 10.0f;
    rgb[0] = rgb[1] = rgb[2] = 0.f;
    rgb[3] = 1.f;
    uBrushTexLoc   = glGetUniformLocation(programID, "uBrushTexture");
    uBrushColorLoc = glGetUniformLocation(programID, "uBrushColor");
}

void Brush::setBrushSize(float newSize) {
    size = newSize;
}

void Brush::setBrushColor(float r, float g, float b, float a) {
    rgb[0] = r;
    rgb[1] = g;
    rgb[2] = b;
    rgb[3] = a;
}

void Brush::InitBuffers() {
    glGenVertexArrays(1, &vaoID);
    glGenBuffers(1, &vboPos);
    glGenBuffers(1, &vboUV);

    glBindVertexArray(vaoID);
    // Positions VBO
    glBindBuffer(GL_ARRAY_BUFFER, vboPos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*8, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    // UVs VBO
    float texCoords[] = {0.f,0.f, 1.f,0.f, 0.f,1.f, 1.f,1.f};
    glBindBuffer(GL_ARRAY_BUFFER, vboUV);
    glBufferData(GL_ARRAY_BUFFER, sizeof(texCoords), texCoords, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindVertexArray(0);
    glGenTextures(1, &brushTextureID);
}

void Brush::compileShaders() {
    const char* vsSrc = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

    const char* fsSrc = R"(#version 300 es
precision mediump float;
in vec2 vUV;
uniform sampler2D uBrushTexture;
uniform vec4 uBrushColor; // rgb base, a = presión
out vec4 FragColor;
void main() {
    float mask = texture(uBrushTexture, vUV).r;
    float alpha = uBrushColor.a * mask;
    vec3 rgbp = uBrushColor.rgb * alpha;
    FragColor = vec4(rgbp, alpha);
}
)";


    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vsSrc, nullptr);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fsSrc, nullptr);
    glCompileShader(fs);

    programID = glCreateProgram();
    glAttachShader(programID, vs);
    glAttachShader(programID, fs);
    glLinkProgram(programID);
    glDeleteShader(vs);
    glDeleteShader(fs);
}

void Brush::setBrush(AAssetManager* mgr, int brushIndex) {
    std::string filename = "brush" + std::to_string(brushIndex) + ".png";
    AAsset* asset = AAssetManager_open(mgr, filename.c_str(), AASSET_MODE_BUFFER);
    if (!asset) return;
    size_t len = AAsset_getLength(asset);
    std::vector<unsigned char> buf(len);
    AAsset_read(asset, buf.data(), len);
    AAsset_close(asset);

    int w,h,ch;
    unsigned char* data = stbi_load_from_memory(buf.data(), len, &w, &h, &ch, STBI_rgb_alpha);
    if (!data) return;

    // Premultiplicar la textura en CPU:
    int pixels = w*h;

    for (int i = 0; i < pixels; ++i) {
        float a = data[i*4+3] / 255.0f;
        data[i*4+0] = static_cast<unsigned char>(data[i*4+0] * a);
        data[i*4+1] = static_cast<unsigned char>(data[i*4+1] * a);
        data[i*4+2] = static_cast<unsigned char>(data[i*4+2] * a);
    }

    glBindTexture(GL_TEXTURE_2D, brushTextureID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    stbi_image_free(data);
}

void Brush::Draw(float pixelX, float pixelY, float pressure,
                 int canvasW, int  canvasH, GLuint fbo) {
    float brushSize = std::clamp(pressure * size, minSize, maxSize);
    float nx = (pixelX / canvasW) * 2.f - 1.f;
    float ny = 1.f - (pixelY / canvasH) * 2.f;
    float hw = (brushSize / canvasW) * 2.f;
    float hh = (brushSize / canvasH) * 2.f;
    float verts[] = { nx-hw, ny-hh, nx+hw, ny-hh, nx-hw, ny+hh, nx+hw, ny+hh };

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glUseProgram(programID);
    glUniform1i(uBrushTexLoc, 0);
    // color base: rgb sin cambiar, alpha = presión
    if (isEraser) {
        glUniform4f(uBrushColorLoc, 0.0f, 0.0f, 0.0f, pressure);
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glUniform4f(uBrushColorLoc, rgb[0], rgb[1], rgb[2], pressure/2);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    glBindVertexArray(vaoID);
    glBindBuffer(GL_ARRAY_BUFFER, vboPos);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, brushTextureID);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    LOGI("ERRASER: " "%d", isEraser);
}

void Brush::DrawInterpolated(float x, float y, float pressure,
                             int canvasW, int canvasH, GLuint fbo) {
    if (!hasLast) {
        Draw(x, y, pressure, canvasW, canvasH, fbo);
        lastX = x; lastY = y; hasLast = true;
        return;
    }
    float dx = x - lastX, dy = y - lastY;
    float dist = sqrtf(dx*dx + dy*dy);
    int steps = static_cast<int>(dist / threshold);
    for (int i = 1; i <= steps; ++i) {
        float t = i / static_cast<float>(steps);
        Draw(lastX + dx*t, lastY + dy*t, pressure, canvasW, canvasH, fbo);
    }
    lastX = x; lastY = y;
}

void Brush::setEraser(bool enabled) {
    isEraser = enabled;

}