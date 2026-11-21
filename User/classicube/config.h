/* Single-player ClassiCube using its portable software rasterizer. */
#define CC_BUILD_MANUAL
#define CC_BUILD_POSIX
#define CC_BUILD_LINUX
#define CC_BUILD_COOPTHREADED
#define CC_BUILD_NOMUSIC
#define CC_BUILD_NOSOUNDS
#define CC_GFX_BACKEND CC_GFX_BACKEND_SOFTGPU
#define CC_AUD_BACKEND CC_AUD_BACKEND_NULL
#include "Core.h"
#undef CC_BUILD_FREETYPE
#undef CC_BUILD_NETWORKING
#undef CC_BUILD_RESOURCES
#undef CC_BUILD_PLUGINS
#undef CUSTOM_MODELS
