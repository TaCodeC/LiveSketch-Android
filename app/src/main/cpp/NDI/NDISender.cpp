#include "NDISender.h"
#include <iostream>
#include <android/log.h>
#define LOG_TAG "LiveSketch"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
NDISender::NDISender(const std::string& sourceName)
        : m_sourceName(sourceName)
{

    NDIlib_initialize();

}

NDISender::~NDISender() {
    shutdown();
    NDIlib_destroy();
}

bool NDISender::initialize(int width, int height, bool topDown) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized)
        return true;

    m_width   = width;
    m_height  = height;
    m_topDown = topDown;

    // Creación del sender
    NDIlib_send_create_t createDesc{};
    createDesc.p_ndi_name    = m_sourceName.c_str();
    createDesc.p_groups      = "";
    createDesc.clock_video   = true;
    createDesc.clock_audio   = false;
    // bandwidth y grp ya no existen en SDK v5+

    m_send = NDIlib_send_create(&createDesc);
    if (!m_send) {
        LOGI("NDI send_create FAILED");
        return false;
    } else {
        LOGI("NDI send_create SUCCESS");
    }

    // Configurar frame reutilizable
    m_frame.xres                  = m_width;
    m_frame.yres                  = m_height;
    m_frame.FourCC                = NDIlib_FourCC_type_RGBA;
    m_frame.line_stride_in_bytes  = m_width * 4;
    m_frame.p_data                = nullptr;
    m_frame.frame_rate_N          = 0;
    m_frame.frame_rate_D          = 1001;  // valor por defecto /30fps ~=1000/33
    m_frame.picture_aspect_ratio  = float(m_width) / float(m_height);
    // timestamp se gestiona internamente, no es un miembro en v2_v5

    m_initialized = true;
    return true;
}

void NDISender::sendFrame(const uint8_t* pData) {
    if (!m_initialized || !m_send)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_frame.p_data = const_cast<uint8_t*>(pData);

    // Envío asíncrono y no bloqueante
    NDIlib_send_send_video_async_v2(m_send, &m_frame);
    LOGI("NDI send frame: %d x %d", m_width, m_height);
}

void NDISender::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_send) {
        NDIlib_send_destroy(m_send);
        m_send = nullptr;
    }
    m_initialized = false;
}
