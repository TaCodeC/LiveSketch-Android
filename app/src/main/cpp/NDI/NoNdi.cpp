// Compilación sin el SDK de NDI: no hay emisor.
#include "NDI/FrameSink.h"

std::unique_ptr<FrameSink> makeNdiSink(const char*) {
    return nullptr;
}
