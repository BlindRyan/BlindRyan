// CLAP entry point and plugin factory.

#include "gui.h"
#include "plugin.h"

#include <cstring>

namespace {

const clap_plugin_factory_t factory = {
    [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
    [](const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t* {
        return index == 0 ? &aab::kDescriptor : nullptr;
    },
    [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t* {
        if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, aab::kDescriptor.id))
            return nullptr;
        return aab::createPlugin(host);
    },
};

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    [](const char*) { return true; },
    [] { aab::Gui::unregisterWindowClass(); },
    [](const char* id) -> const void* {
        return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &factory;
    },
};
