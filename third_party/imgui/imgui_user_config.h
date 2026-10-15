#pragma once

#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
// the core creates an OpenGL ES 3 context and uses vertex array objects,
// which only the ES3 path of the backend saves and restores
#define IMGUI_IMPL_OPENGL_ES3
