//
// Created by Fernando Daniel Martinez Cortes on 12/04/25.
//

#ifndef LIVESKETCH_LAYERMANAGER_H
#define LIVESKETCH_LAYERMANAGER_H

#include <vector>
#include "Layer.h" // Incluye la definición de la clase layer
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define LOG_TAG "layermanager"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))

class LayerManager {
private:
    GLuint TexturesProgram; // Programa de texturas
    GLuint vaoNormal, vboNormal; // VAO y VBO para la capa normal
    GLuint vaoFlip, vboFlip; // VAO y VBO para la capa invertida
    GLfloat uOffsetLoc;
    GLfloat uScaleLoc;
    // Alto de la pantalla
    void initFBOS();
public:
    std::vector<Layer*> layers; // Vector de punteros a capas

    LayerManager() = default; // Constructor por defecto
    ~LayerManager(); // Destructor
    int targetLayer; // Capa objetivo
    void addLayer(); // Agregar una capa
    void addLayer(int insertPos);
    void removeLayer(int layerIndex); // Eliminar la capa superior\

    void renameLayer(int index, std::string& name); // Renombrar la capa
    void setLayerVisibility(int index, bool visible); // Establecer la visibilidad de la capa
    void setLayerOpacity(int index, float opacity); // Establecer la opacidad de la capa
    void Init(int layerwidth, int layerheight, int windowwidth, int windowheight); // Inicializar el gestor de capas
    void ShaderCompilation();
    void RenderToCompositeFBO();
    void RenderLayersOnScreen();
    void initBuffers();
    void RenderLayers(bool* renderDirtyFlag);
    bool scaled;
    glm::vec2 targetScale;
    glm::vec2 currentScale;
    glm::vec2 currentOffset; // Offset actual
    glm::vec2 targetOffset; // Offset objetivo
// Composite FBO
    GLuint compositeFBO = 0;
    GLuint compositeTex = 0;

    int layerWidth;
    int layerHeight;
    int windowWidth;
    int windowHeight;

};


#endif //LIVESKETCH_LAYERMANAGER_H
