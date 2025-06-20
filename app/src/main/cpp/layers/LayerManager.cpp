#include "LayerManager.h"
#include <algorithm>
#include <android/log.h>

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "LayerManager", __VA_ARGS__))

LayerManager::~LayerManager() {
    for (auto layer : layers) {
        delete layer;
    }
    layers.clear();
}

void LayerManager::addLayer() {
    std::string name = "Layer " + std::to_string(layers.size());
    layers.push_back(new Layer(name, layerWidth, layerHeight));
    targetLayer = layers.size() - 1;
    layers.back()->ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
}

void LayerManager::addLayer(int insertPos) {
    LOGI("Adding layer at position %d", insertPos);
    int pos = std::clamp(insertPos + 1, 0, static_cast<int>(layers.size()));
    std::string name = "Layer " + std::to_string(layers.size());
    Layer* newLayer = new Layer(name, layerWidth, layerHeight);
    newLayer->ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    layers.insert(layers.begin() + pos, newLayer);
    targetLayer = pos;
}

void LayerManager::removeLayer(int layerIndex) {
    if (layers.size() > 1 && layerIndex >= 0 && layerIndex < layers.size()) {
        delete layers[layerIndex];
        layers.erase(layers.begin() + layerIndex);
        if (targetLayer >= layers.size())
            targetLayer = layers.size() - 1;
    }
}

void LayerManager::renameLayer(int index, std::string& name) {
    if (index >= 0 && index < layers.size())
        layers[index]->layername = name;
}

void LayerManager::Init(int width, int height, int windowwidth, int windowheight) {
    layerWidth = width;
    layerHeight = height;
    // Activar blending con premultiplied alpha para evitar bordes oscuros
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    windowWidth = windowwidth;
    windowHeight = windowheight;

    scaled = true;
    targetScale = currentScale = glm::vec2(1.0f, 1.0f);
    currentOffset = targetOffset = glm::vec2(0.0f, 0.0f);

    ShaderCompilation();
    initBuffers();
    initFBOS();

    // Capa base opaca blanca
    layers.push_back(new Layer("Base Layer", layerWidth, layerHeight));
    layers[0]->ClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    // Capa secundaria transparente
    addLayer();

    uScaleLoc = glGetUniformLocation(TexturesProgram, "uScale");
    uOffsetLoc = glGetUniformLocation(TexturesProgram, "uOffset");

}

void LayerManager::ShaderCompilation() {
    const char* vertSrc = R"(#version 300 es
precision mediump float;
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;
uniform vec2 uOffset;
uniform vec2 uScale;
out vec2 vTexCoord;
void main() {
    vec2 flippedOffset = vec2(uOffset.x, -uOffset.y);
    vec2 scaledPos = aPos * uScale; 
    vec2 finalPos = scaledPos + flippedOffset;
    gl_Position = vec4(finalPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

    const char* fragSrc = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
uniform sampler2D uTexture;
out vec4 FragColor;
void main() {
    FragColor = texture(uTexture, vTexCoord);
}
)";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertSrc, nullptr);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragSrc, nullptr);
    glCompileShader(fs);
    TexturesProgram = glCreateProgram();
    glAttachShader(TexturesProgram, vs);
    glAttachShader(TexturesProgram, fs);
    glLinkProgram(TexturesProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
}

void LayerManager::RenderToCompositeFBO() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO);
    glViewport(0, 0, layerWidth, layerHeight);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(TexturesProgram);
    glBindVertexArray(vaoFlip);
    for (auto& layer : layers) {
        if (!layer->visible) continue;
        glUniform2f(uScaleLoc, currentScale.x, currentScale.y);
        glUniform2f(uOffsetLoc, currentOffset.x, currentOffset.y);
        glBindTexture(GL_TEXTURE_2D, layer->texture);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void LayerManager::RenderLayersOnScreen() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, windowWidth, windowHeight);
    glUseProgram(TexturesProgram);
    glBindVertexArray(vaoFlip);
    glUniform2f(uScaleLoc, 1.0f, 1.0f);
    glUniform2f(uOffsetLoc, 0.0f, 0.0f);
    glBindTexture(GL_TEXTURE_2D, compositeTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void LayerManager::initBuffers() {
    float quadFlippedY[] = {
            -1.0f,  1.0f, 0.0f, 0.0f,
            1.0f,  1.0f, 1.0f, 0.0f,
            1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 0.0f,
            1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f, -1.0f, 0.0f, 1.0f
    };
    glGenVertexArrays(1, &vaoFlip);
    glBindVertexArray(vaoFlip);
    glGenBuffers(1, &vboFlip);
    glBindBuffer(GL_ARRAY_BUFFER, vboFlip);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadFlippedY), quadFlippedY, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void LayerManager::initFBOS() {
    glGenFramebuffers(1, &compositeFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO);
    glGenTextures(1, &compositeTex);
    glBindTexture(GL_TEXTURE_2D, compositeTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, layerWidth, layerHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, compositeTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGI("Composite FBO not complete!");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}