#pragma once

#include "config.h"

#include <windows.h>

#include <cstdint>

bool MGS4ModLoader_Install(HMODULE gameModule, void* textBegin, uintptr_t textSize,
                          const ModLoaderConfig& config);
