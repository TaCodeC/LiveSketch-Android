// NDIThreadSender.h
#ifndef LIVESKETCH_NDITHREADSENDER_H
#define LIVESKETCH_NDITHREADSENDER_H
#pragma once

#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include "NDI/NDISender.h"

// Inicia un hilo de captura NDI (solo declaraciones)
void StartNDIThread(NDISender* sender,
                    int width, int height,
                    EGLDisplay display,
                    EGLContext sharedContext,
                    EGLConfig config,
                    GLuint compositeFBO,
                    GLuint pboId);

// Señala al broadcast thread que debe capturar y enviar el siguiente frame
void NotifyNDIDirty();

// Copia manual al buffer si es necesario antes del envío
void UpdateNDIBuffer(const uint8_t* data);

// Detiene el hilo de broadcast y libera recursos
void StopNDIThread();

#endif // LIVESKETCH_NDITHREADSENDER_H
