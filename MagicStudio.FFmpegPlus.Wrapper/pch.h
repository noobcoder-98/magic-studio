#pragma once

// Pull in native code without managed interference.
#pragma managed(push, off)
#include "../MagicStudio.FFmpegPlus.Native/pch.h"
#include "../MagicStudio.FFmpegPlus.Native/MagicPlayer/MediaDecoder.h"
#pragma managed(pop)

#include <msclr/marshal_cppstd.h>
