// Plugin factory and entry functions.

#include "entry_impl.h"

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

bool rar_entry_init(const char*)
{
    return true;
}

void rar_entry_deinit()
{
    aab::Gui::unregisterWindowClass();
}

const void* rar_entry_get_factory(const char* factoryId)
{
    return std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &factory;
}
