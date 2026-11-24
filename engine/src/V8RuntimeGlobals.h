#pragma once

#include <v8.h>

namespace hydra::runtime_globals {

// Installs the native helpers that back the web-platform globals (URL,
// URLSearchParams) on `globalThis.__hydraNative`. The bootstrap script captures
// them in a closure and deletes the global, so page code never sees them.
void installNatives(v8::Isolate *isolate, v8::Local<v8::Context> context);

// The script that defines the browser globals the SSR runtime provides. It
// must run after installNatives() and before the SSR bundle.
const char *bootstrapSource();

}  // namespace hydra::runtime_globals
