//
// Created by Fernando Daniel Martinez Cortes on 12/04/25.
//

#ifndef LIVESKETCH_LAYER_H
#define LIVESKETCH_LAYER_H

#include <GLES3/gl3.h>
#include <android/log.h>
#include <EGL/egl.h>
#define LOG_TAG "layermanager"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#include <string>
class Layer {
public:
    GLuint fbo;
    GLuint texture;
    int opacity;
    bool visible;
    std::string layername;
    Layer(std::string name, int width, int height);
    float r, g, b, a;

    void ClearColor(float r, float g, float b, float a);
};


#endif //LIVESKETCH_LAYER_H
