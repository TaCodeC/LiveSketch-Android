//
// Created by Fernando Daniel Martinez Cortes on 21/04/25.
//

#ifndef LIVESKETCH_NDISENDER_H
#define LIVESKETCH_NDISENDER_H


// NDISender.h
#pragma once
#include <Processing.NDI.Lib.h>
#include <string>
#include <atomic>
#include <mutex>

class NDISender {
public:
    explicit NDISender(const std::string& sourceName);
    ~NDISender();

    // Inicializa el sender NDI con ancho y alto de video
    bool initialize(int width, int height, bool topDown = true);

    // Envía un frame RGBA; pData apunta a un buffer con stride = width*4 bytes
    void sendFrame(const uint8_t* pData);

    // Cierra el sender y libera recursos
    void shutdown();

private:
    // Prohibir copia
    NDISender(const NDISender&) = delete;
    NDISender& operator=(const NDISender&) = delete;

    NDIlib_send_instance_t  m_send         = nullptr;
    std::string             m_sourceName;
    int                     m_width        = 0;
    int                     m_height       = 0;
    bool                    m_topDown      = true;
    std::atomic<bool>       m_initialized{false};
    std::mutex              m_mutex;
    NDIlib_video_frame_v2_t m_frame{};
};


#endif //LIVESKETCH_NDISENDER_H
