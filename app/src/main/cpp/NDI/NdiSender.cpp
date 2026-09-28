// Emisor NDI con el SDK oficial. Solo se compila con LIVESKETCH_WITH_NDI.
#include "NDI/FrameSink.h"

#include <cstddef>   // las cabeceras de NDI usan NULL sin incluir nada que lo defina

#include <Processing.NDI.Lib.h>
#include <SDL3/SDL_log.h>

#include <string>

namespace {

class NdiSender final : public FrameSink {
public:
    explicit NdiSender(std::string name) : m_name(std::move(name)) {}
    ~NdiSender() override { close(); }

    bool open(int width, int height) override {
        close();
        // Se inicializa una vez por proceso; el SDK lo libera al salir.
        static const bool initialized = NDIlib_initialize();
        if (!initialized) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NDI: esta CPU no está soportada");
            return false;
        }

        NDIlib_send_create_t settings;
        settings.p_ndi_name = m_name.c_str();
        settings.p_groups = nullptr;
        // El ritmo lo marca la app (hasta 30 fps y un reenvío por segundo sin cambios).
        settings.clock_video = false;
        settings.clock_audio = false;
        m_send = NDIlib_send_create(&settings);
        if (!m_send) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NDI: no se pudo crear el emisor");
            return false;
        }

        m_frame.xres = width;
        m_frame.yres = height;
        m_frame.FourCC = NDIlib_FourCC_type_RGBA;
        m_frame.frame_rate_N = 30;
        m_frame.frame_rate_D = 1;
        m_frame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
        m_frame.frame_format_type = NDIlib_frame_format_type_progressive;
        m_frame.timecode = NDIlib_send_timecode_synthesize;
        return true;
    }

    void send(const uint8_t* rgba, int width, int height, int stride) override {
        if (!m_send) {
            return;
        }
        m_frame.xres = width;
        m_frame.yres = height;
        m_frame.line_stride_in_bytes = stride;
        m_frame.p_data = const_cast<uint8_t*>(rgba);
        // Envío síncrono: al volver, NDI ya no usa el buffer.
        NDIlib_send_send_video_v2(m_send, &m_frame);
    }

    int connections() override { return m_send ? NDIlib_send_get_no_connections(m_send, 0) : 0; }

    void close() override {
        if (m_send) {
            NDIlib_send_destroy(m_send);
            m_send = nullptr;
        }
    }

private:
    std::string m_name;
    NDIlib_send_instance_t m_send = nullptr;
    NDIlib_video_frame_v2_t m_frame;
};

} // namespace

std::unique_ptr<FrameSink> makeNdiSink(const char* sourceName) {
    return std::make_unique<NdiSender>(sourceName);
}
