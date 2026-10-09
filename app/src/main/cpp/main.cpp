// Punto de entrada. SDL llama a estas cuatro funciones (callbacks de SDL3): en Android
// desde el hilo nativo que crea SDLActivity, en escritorio desde el hilo principal.
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include "App/App.h"

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    auto* app = new App();
    *appstate = app;
    return app->init(argc, argv);
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    return static_cast<App*>(appstate)->event(*event);
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    return static_cast<App*>(appstate)->iterate();
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    auto* app = static_cast<App*>(appstate);
    if (app) {
        app->quit();
        delete app;
    }
}
