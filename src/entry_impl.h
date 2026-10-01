#pragma once

// The plugin's entry functions, built into the shared static library so the
// CLAP and VST3 builds can each export them in their own way.

#include <clap/clap.h>

bool rar_entry_init(const char* pluginPath);
void rar_entry_deinit();
const void* rar_entry_get_factory(const char* factoryId);
