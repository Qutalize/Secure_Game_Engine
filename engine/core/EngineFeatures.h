#pragma once
// Old all/fps/range builds keep their existing API. Selected builds use generated gates.
#ifdef SELECTED_RUNTIME
#include "SelectedFeatures.h"
#else
#define ENGINE_MODEL_GEOMETRY 0
#define ENGINE_MODEL_ANIMATION 0
#define ENGINE_MODEL_CONVEX 0
#define ENGINE_MODEL_STATIC 0
#define ENGINE_MODEL_DEFORMING 0
#define ENGINE_RAGDOLL 0
#define ENGINE_DEFORMATION_CCD 0
#define ENGINE_RT_MESH 0
#define ENGINE_RIGID_BODY 0
#define ENGINE_RAY_TRACING 0
#define ENGINE_RT_HARDWARE 0
#define ENGINE_RT_SHADOWS 0
#define ENGINE_RT_REFLECTIONS 0
#define ENGINE_DISTRIBUTION_SIGNATURE 0
#define ENGINE_RENDER 1
#define ENGINE_COLLISION 1
#define ENGINE_AUDIO 1
#define ENGINE_TEXTURES 1
#define ENGINE_TEXTURE_PIPELINE 1
#define ENGINE_PBR 1
#define ENGINE_SHADOWS 1
#define ENGINE_SKINNING 1
#endif
