#pragma once

// Native player exposes a plain C API; pull it in with managed code switched
// off so the d3d11/dxgi types arrive with C++ linkage.
#pragma managed(push, off)
#include "../MagicStudio.FFmpegPlus.Native/MagicFFmpegPlayer.h"
#pragma managed(pop)

#include <msclr/marshal_cppstd.h>
