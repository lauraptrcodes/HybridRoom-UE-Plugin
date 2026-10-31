#pragma once

// In Unreal definiert UBT HYBRIDACOUSTICSCORE_API als DLLEXPORT (im Kern-Modul)
// bzw. DLLIMPORT (in abhängigen Modulen). Außerhalb von UE (CMake/Tests) bleibt FDTD_API leer.
#if defined(HYBRIDACOUSTICSCORE_API)
#include "HAL/Platform.h"            // definiert DLLEXPORT / DLLIMPORT
#define FDTD_API HYBRIDACOUSTICSCORE_API
#else
#define FDTD_API
#endif