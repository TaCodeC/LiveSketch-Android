#include "App/CanvasProject.h"

#include <cstring>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace canvasproject {
namespace {

// FNV-1a de 64 bits.
class Hash {
public:
    template <typename T>
    void add(T value) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        bytes(&value, sizeof(value));
    }
    void add(bool value) { add(static_cast<uint8_t>(value ? 1 : 0)); }
    void add(std::string_view text) {
        add(static_cast<uint64_t>(text.size()));
        bytes(text.data(), text.size());
    }
    uint64_t value() const { return m_value != 0 ? m_value : 1; }

private:
    void bytes(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            m_value = (m_value ^ p[i]) * 1099511628211ull;
        }
    }
    uint64_t m_value = 14695981039346656037ull;
};

const zip::Entry* findEntry(const std::vector<zip::Entry>& entries, std::string_view name) {
    for (const zip::Entry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

const SavedLayer* Record::find(uint32_t id) const {
    for (const SavedLayer& layer : layers) {
        if (layer.id == id) {
            return &layer;
        }
    }
    return nullptr;
}

uint64_t compositeKey(const Canvas& canvas) {
    Hash hash;
    hash.add(canvas.width());
    hash.add(canvas.height());
    const CanvasInfo& info = canvas.info();
    hash.add(info.profile);
    hash.add(info.ppi);
    const CanvasBackground& background = canvas.background();
    for (const float channel : background.color) {
        hash.add(channel);
    }
    hash.add(background.visible);
    const LayerStack& layers = canvas.layers();
    hash.add(layers.count());
    for (int i = 0; i < layers.count(); ++i) {
        const Layer& layer = layers.at(i);
        hash.add(layer.id);
        hash.add(layer.revision);
        hash.add(layer.visible);
        hash.add(layer.opacity);
        hash.add(layer.blend);
        hash.add(layer.clipping);
    }
    return hash.value();
}

project::Document document(const Canvas& canvas) {
    project::Document document;
    document.width = canvas.width();
    document.height = canvas.height();
    document.info = canvas.info();
    document.background = canvas.background();
    const LayerStack& layers = canvas.layers();
    document.layers.resize(static_cast<size_t>(layers.count()));
    for (int i = 0; i < layers.count(); ++i) {
        const Layer& layer = layers.at(i);
        project::LayerInfo& out = document.layers[static_cast<size_t>(i)];
        out.name = layer.name;
        out.visible = layer.visible;
        out.opacity = layer.opacity;
        out.blend = layer.blend;
        out.alphaLock = layer.alphaLock;
        out.clipping = layer.clipping;
        out.reference = layer.reference;
    }
    document.activeLayer = layers.activeIndex();
    document.guide = canvas.guide();
    return document;
}

bool save(Canvas& canvas, project::Document document, const project::Target& target, const std::string& app,
          const Record& previous, project::Saver& saver, std::function<void()> onFinished, Pending* pending) {
    const LayerStack& layers = canvas.layers();
    const int count = layers.count();
    if (!canvas.ready() || document.layers.size() != static_cast<size_t>(count)) {
        return false;
    }
    Pending out;
    out.record.layers.resize(static_cast<size_t>(count));

    // Las capas que no cambiaron desde el archivo anterior tienen la caja de entonces; las
    // demás se miden.
    std::vector<const SavedLayer*> kept(static_cast<size_t>(count), nullptr);
    bool copyAny = false;
    for (int i = 0; i < count; ++i) {
        const Layer& layer = layers.at(i);
        const SavedLayer* saved = previous.path.empty() ? nullptr : previous.find(layer.id);
        project::LayerInfo& info = document.layers[static_cast<size_t>(i)];
        if (saved && saved->revision == layer.revision) {
            kept[static_cast<size_t>(i)] = saved;
            info.rect = saved->rect;
            copyAny = copyAny || !saved->rect.empty();
        } else {
            info.rect = canvas.layerContent(i);
        }
        SavedLayer& record = out.record.layers[static_cast<size_t>(i)];
        record.id = layer.id;
        record.revision = layer.revision;
        record.rect = info.rect;
    }
    // Con algo a medias, el compuesto enseña lo que aún no está en las capas: vale para este
    // archivo, pero no se sabría cuándo vuelve a valer.
    const uint64_t key = canvas.unsettled() ? 0 : compositeKey(canvas);
    const bool copyComposite = !previous.path.empty() && key == previous.composite && !previous.thumbnail.name.empty() &&
                               !previous.merged.name.empty();
    out.record.composite = key;
    std::vector<IRect> rects;
    rects.reserve(static_cast<size_t>(count));
    for (const project::LayerInfo& info : document.layers) {
        rects.push_back(info.rect);
    }
    if (!saver.begin(std::move(document), target, app, copyAny || copyComposite ? previous.path : std::string())) {
        return false;
    }
    const bool reusing = saver.reusing();

    // Cada capa recortada a lo pintado, en cuanto quepa en la memoria de lo que espera a
    // comprimirse (con un byte más por fila: el PNG se filtra ahí mismo).
    std::string error;
    auto read = [&canvas, &saver, &error](int index, const IRect& rect, std::vector<uint8_t>& pixels) {
        const size_t row = static_cast<size_t>(rect.width()) * 4;
        const size_t rows = static_cast<size_t>(rect.height());
        saver.reserve((row + 1) * rows);
        try {
            pixels.reserve((row + 1) * rows);
            pixels.resize(row * rows);
        } catch (const std::bad_alloc&) {
            error = "no hay memoria para leer el dibujo";
            return false;
        }
        if (!canvas.readRegion(index, rect, pixels.data())) {
            error = "no se pudo leer el dibujo de la memoria de gráficos";
            return false;
        }
        return true;
    };
    for (int i = 0; i < count; ++i) {
        const IRect& rect = rects[static_cast<size_t>(i)];
        const SavedLayer* saved = kept[static_cast<size_t>(i)];
        if (rect.empty()) {
            saver.addLayer(i, {});
        } else if (saved && reusing) {
            saver.reuseLayer(i, saved->entry);
            ++out.copiedLayers;
        } else {
            std::vector<uint8_t> pixels;
            if (!read(i, rect, pixels)) {
                saver.abort(error, std::move(onFinished));
                return false;
            }
            saver.addLayer(i, std::move(pixels));
            ++out.encodedLayers;
        }
    }
    if (copyComposite && reusing) {
        saver.finishReusing(previous.thumbnail, previous.merged, std::move(onFinished));
        out.copiedComposite = true;
    } else {
        std::vector<uint8_t> composite;
        if (!read(-1, IRect::ofSize(canvas.width(), canvas.height()), composite)) {
            saver.abort(error, std::move(onFinished));
            return false;
        }
        saver.finish(std::move(composite), std::move(onFinished));
    }
    if (pending) {
        *pending = std::move(out);
    }
    return true;
}

Record saved(const Pending& pending, const project::Saver::Result& result) {
    Record record;
    if (!result.ok) {
        return record;
    }
    record.path = result.path;
    record.layers.reserve(pending.record.layers.size());
    for (size_t i = 0; i < pending.record.layers.size(); ++i) {
        SavedLayer layer = pending.record.layers[i];
        if (!layer.rect.empty()) {
            const zip::Entry* entry = findEntry(result.entries, project::layerEntry(static_cast<int>(i)));
            if (!entry) {
                continue;   // no debería pasar: la próxima vez se comprime
            }
            layer.entry = *entry;
        }
        record.layers.push_back(std::move(layer));
    }
    const zip::Entry* thumbnail = findEntry(result.entries, project::kThumbnailEntry);
    const zip::Entry* merged = findEntry(result.entries, project::kMergedEntry);
    if (thumbnail && merged) {
        record.composite = pending.record.composite;
        record.thumbnail = *thumbnail;
        record.merged = *merged;
    }
    return record;
}

std::optional<SavedLayer> openedLayer(const Canvas& canvas, const project::Loader& loader, int index, bool loaded) {
    const project::Document& document = loader.document();
    if (!loaded || !canvas.layers().validIndex(index) || index < 0 ||
        static_cast<size_t>(index) >= document.layers.size()) {
        return std::nullopt;
    }
    const project::LayerInfo& info = document.layers[static_cast<size_t>(index)];
    const Layer& layer = canvas.layers().at(index);
    SavedLayer saved;
    saved.id = layer.id;
    saved.revision = layer.revision;
    saved.rect = info.rect;
    if (!info.rect.empty()) {
        std::optional<zip::Entry> entry = loader.entry(info.file);
        if (!entry) {
            return std::nullopt;
        }
        saved.entry = std::move(*entry);
    }
    return saved;
}

Record opened(const Canvas& canvas, const project::Loader& loader, const std::string& path,
              std::vector<SavedLayer> layers, bool complete) {
    Record record;
    record.path = path;
    record.layers = std::move(layers);
    const project::Document& document = loader.document();
    const CanvasInfo& info = canvas.info();
    // El dibujo entero del archivo vale si el lienzo es el que dice el archivo: todas sus capas
    // y los datos que lleva su PNG.
    if (complete && record.layers.size() == static_cast<size_t>(canvas.layers().count()) &&
        info.ppi == document.info.ppi && info.profile == document.info.profile && !canvas.unsettled()) {
        std::optional<zip::Entry> thumbnail = loader.entry(project::kThumbnailEntry);
        std::optional<zip::Entry> merged = loader.entry(project::kMergedEntry);
        if (thumbnail && merged) {
            record.composite = compositeKey(canvas);
            record.thumbnail = std::move(*thumbnail);
            record.merged = std::move(*merged);
        }
    }
    return record;
}

} // namespace canvasproject
