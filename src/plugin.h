#pragma once

#include <clap/clap.h>

namespace aab {

extern const clap_plugin_descriptor_t kDescriptor;

const clap_plugin_t* createPlugin(const clap_host_t* host);

} // namespace aab
