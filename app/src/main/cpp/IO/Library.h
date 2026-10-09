#pragma once

#include "Canvas/CanvasSpec.h"
#include "IO/Json.h"
#include "IO/ProjectFile.h"
#include "IO/Tasks.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Los proyectos de la app: un archivo .lvskt por proyecto en una carpeta suya, dentro de la
// de datos de la app. El archivo se llama con un identificador que no cambia (el nombre del
// lienzo va dentro), así que renombrar un proyecto no lo mueve. Mirar la carpeta y renombrar,
// duplicar, copiar y borrar proyectos se hace en otro hilo (en la web, entre frames).
namespace library {

// Lo que se enseña de un proyecto sin abrirlo.
struct Summary {
    std::string path;
    uint64_t bytes = 0;            // lo que ocupa
    int64_t fileTime = 0;          // fecha del archivo (SDL_Time): si cambia, se vuelve a leer
    bool readable = false;         // se pudo leer su descripción
    std::string error;             // si no, por qué
    std::string name;              // el del lienzo (vacío si no tiene)
    int width = 0;
    int height = 0;
    ColorProfile profile = ColorProfile::Srgb;
    int layers = 0;
    int64_t modified = 0;          // último cambio del dibujo (si no lo dice, la fecha del archivo)
    // La miniatura sin premultiplicar, en el perfil del lienzo (vacía si no tiene).
    std::vector<uint8_t> thumbnail;
    int thumbnailWidth = 0;
    int thumbnailHeight = 0;
};

// Lee lo que se enseña del proyecto `path`. Desde cualquier hilo.
Summary summarize(const std::string& path);

// Copia el proyecto `from` a `to` con su document.json cambiado por `edit` (lo demás, tal
// cual): se escribe al lado y se cambia de nombre, así que `to` se sustituye entero o no
// cambia, aunque sea el mismo archivo. False si no se pudo (motivo en `error`).
bool rewrite(const std::string& from, const std::string& to, const std::function<bool(json::Value&)>& edit,
             std::string* error);
// Copia un archivo tal cual a `to`: una ruta (que se sustituye igual de seguro) o, en Android,
// la URI de un documento (content://), que se escribe directamente. O a un archivo nuevo de
// `to.folder` llamado `to.stem` (ver io::createFile). Deja en `written` el que se escribió.
bool copyFile(const std::string& from, const project::Target& to, std::string* written, std::string* error);

class Library {
public:
    // Una operación que terminó.
    struct Done {
        enum class Kind { Rename, Duplicate, Remove, Copy };
        Kind kind = Kind::Rename;
        bool ok = false;
        std::string path;     // el proyecto
        std::string result;   // el archivo nuevo (duplicar) o la copia (copiar)
        std::string error;    // por qué falló
    };

    Library() = default;
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    // Se llama, desde cualquier hilo, cuando hay algo nuevo que recoger con update() o
    // takeDone() (para despertar el bucle de la app). Antes de open().
    void setOnChange(std::function<void()> onChange) { m_onChange = std::move(onChange); }
    // Usa la carpeta `folder` (terminada en '/'; se crea si no existe) y borra lo que dejaron
    // guardados cortados a medias. False si no se puede.
    bool open(const std::string& folder);
    bool ready() const { return !m_folder.empty(); }
    const std::string& folder() const { return m_folder; }
    // Ruta para un proyecto nuevo (aún no existe): la fecha, la hora y algo al azar.
    std::string newPath() const;
    // Si `path` es un proyecto de la carpeta.
    bool contains(const std::string& path) const;

    // Vuelve a mirar la carpeta en segundo plano (lo que no cambió no se vuelve a leer).
    void refresh();
    // Cada frame. En la web, trabaja aquí durante `budgetMs` como mucho. Devuelve true si la
    // lista cambió desde la última vez.
    bool update(uint64_t budgetMs);
    // Los proyectos, el último que cambió primero.
    const std::vector<Summary>& items() const { return m_items; }
    const Summary* find(const std::string& path) const;
    // Aún no se ha mirado la carpeta.
    bool loading() const { return !m_listed; }
    // Una operación en marcha con este proyecto.
    bool busy(const std::string& path) const;
    // Alguna operación en marcha (o la lista).
    bool working() const;

    // Operaciones en segundo plano, en orden; lo que pasó llega por takeDone().
    void rename(const std::string& path, std::string name);
    // Una copia con otro nombre, que pasa a ser el último que cambió.
    void duplicate(const std::string& path, std::string name);
    void remove(const std::string& path);
    void copy(const std::string& path, const project::Target& to);
    std::vector<Done> takeDone();
    // Espera a que termine todo (también la lista).
    void wait();

private:
    struct Scan;
    void startScan();
    void run(Done done, std::function<bool(Done&)> work);
    void changed() const {
        if (m_onChange) {
            m_onChange();
        }
    }

    std::function<void()> m_onChange;
    std::string m_folder;
    std::unique_ptr<io::TaskPool> m_pool;
    std::vector<Summary> m_items;
    bool m_listed = false;

    mutable std::mutex m_mutex;
    std::shared_ptr<Scan> m_scan;          // la que está en marcha
    bool m_rescan = false;                 // pedir otra al terminar esta
    bool m_scanned = false;                // hay una lista nueva sin recoger
    std::vector<Summary> m_scannedItems;
    std::vector<std::string> m_busy;       // proyectos con una operación en marcha
    std::vector<Done> m_done;
    int m_jobs = 0;
};

} // namespace library
