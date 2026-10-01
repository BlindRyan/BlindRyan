// Exports the CLAP entry point. clap-wrapper compiles this file once for
// each plugin format (CLAP, VST3) and links it to the shared static library.

#include "entry_impl.h"

extern "C" {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

extern CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    rar_entry_init,
    rar_entry_deinit,
    rar_entry_get_factory,
};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}
