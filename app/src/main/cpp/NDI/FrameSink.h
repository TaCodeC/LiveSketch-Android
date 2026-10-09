#pragma once

#include <cstdint>
#include <memory>

// Destino de los frames del lienzo: el emisor NDI real o uno falso en las pruebas.
// Todos los métodos se llaman desde el hilo de envío, nunca desde el hilo de GL.
class FrameSink {
public:
    virtual ~FrameSink() = default;

    virtual bool open(int width, int height) = 0;
    // `rgba`: RGBA8 sin premultiplicar, filas de arriba abajo, `stride` bytes por fila.
    // Puede bloquear; al volver, el buffer ya no se usa.
    virtual void send(const uint8_t* rgba, int width, int height, int stride) = 0;
    // Receptores conectados ahora mismo (0 si no se sabe).
    virtual int connections() = 0;
    virtual void close() = 0;
};

// Emisor NDI real con el nombre de fuente dado, o nullptr si la app se compiló sin NDI.
std::unique_ptr<FrameSink> makeNdiSink(const char* sourceName);
