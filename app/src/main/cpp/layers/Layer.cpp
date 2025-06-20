
#include "Layer.h"

Layer::Layer(std::string name, int width, int height): layername(name), opacity(1), visible(true), r(0.f), g(0.f), b(0.f), a(1.0f) {
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
       // LOGI("Error creating framebuffer");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

}

void Layer::ClearColor(float r_, float g_, float b_, float a_) {
    r= r_;
    g= g_;
    b= b_;
    a= a_;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}