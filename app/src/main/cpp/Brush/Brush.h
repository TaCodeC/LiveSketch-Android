//
// Created by Fernando Daniel Martinez Cortes on 12/04/25.
//

#ifndef LIVESKETCH_BRUSH_H
#define LIVESKETCH_BRUSH_H
#include <android/log.h>  // Incluye la librería para hacer logs en Android


// Brush.h
#include <GLES3/gl3.h>

#include <string>
#include <android/asset_manager.h>
#include <vector>

class Brush {


    GLuint textureID; // ID de la textura del pincel
    GLuint programID; // ID del programa de shader
    GLuint vaoID; // ID del Vertex Array Object
    GLuint vboPos; // ID del Vertex Buffer Object
    GLuint vboUV; // ID del Vertex Buffer Object para UVs
    GLuint brushTextureID; // ID de la textura del pincel
     GLint uBrushColorLoc;
     GLint uBrushTexLoc;
    float rgb[4]; // Color del pincel (rojo, verde, azul, alpha)
    // Tamaño base del pincel, modificable globalmente
    float lastX = -1.0f, lastY = -1.0f; // Última posición válida
    float minSize = 1.f;
    float maxSize = 200.0f;
    const float threshold = .8f; // distancia mínima para interpolar

    const int   COMMIT_BATCH = 100;   // cuántos sub‑puntos dibujar antes de vaciar
    bool isEraser = false;

public:
    void setEraser(bool enabled);
    void setBrush(AAssetManager* mgr, int brushIndex); // Método para establecer la textura del pincel
    void Init();
    void InitBuffers(); // Método para inicializar los buffers
    void compileShaders(); // Método para compilar los shaders
    void Draw(float x, float y, float pressure, int canvasW, int canvasH, GLuint fbo);
    void setBrushSize(float size); // Método para establecer el tamaño del pincel
    void setBrushColor(float r, float g, float b, float a); // Método para establecer el color del pincel
    void DrawInterpolated(float x, float y, float pressure, int canvasW, int canvasH, GLuint fbo);
    bool hasLast = false; // Bandera de si hay una posición previa
    void DrawQuad(float x, float y, float w, float h,
                  int canvasW, int canvasH, GLuint fbo);

    float size;
};



#endif //LIVESKETCH_BRUSH_H
