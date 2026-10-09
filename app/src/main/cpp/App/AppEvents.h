#pragma once

#include <SDL3/SDL_stdinc.h>

// Eventos propios de la app. Despiertan el bucle cuando está dormido esperando eventos; los
// empujan un temporizador u otros hilos. Los registra App::init.
namespace appevents {

extern Uint32 wake;           // temporizador de App::wakeAt
extern Uint32 pngSaved;       // terminó un guardado de PNG
extern Uint32 permission;     // respuesta al permiso de almacenamiento (code: 1 si se concedió)
extern Uint32 projectSaved;   // terminó un guardado de proyecto
// El selector de archivos respondió. code: 0, se eligió el archivo de data1; 1, se canceló;
// -1, falló, con el motivo en data1. data1 es de SDL_strdup: lo libera quien lo recibe.
extern Uint32 fileChosen;

// Se puede llamar desde cualquier hilo. `data` pasa a ser del evento (se libera con SDL_free
// si no se pudo encolar).
void push(Uint32 type, Sint32 code = 0, char* data = nullptr);

} // namespace appevents
