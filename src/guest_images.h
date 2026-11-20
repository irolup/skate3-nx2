#pragma once
// Registration of the extra recompiled guest modules (the game's EAWebKit library) with the
// function dispatcher, so calls into them reach the recompiled host functions.
#include <cstdint>

#include <rex/image_info.h>
#include <rex/system/function_dispatcher.h>

namespace skate3 {

// Returns the number of functions registered (0 if the module's code range was rejected).
inline uint32_t RegisterGuestModule(rex::runtime::FunctionDispatcher& dispatcher,
                                    const rex::PPCImageInfo& image) {
  if (!dispatcher.InitializeFunctionTable(image.code_base, image.code_size, image.image_base,
                                          image.image_size)) {
    return 0;
  }
  uint32_t registered = 0;
  for (const auto* mapping = image.func_mappings; mapping->guest; ++mapping) {
    if (mapping->host) {
      dispatcher.SetFunction(uint32_t(mapping->guest), mapping->host);
      ++registered;
    }
  }
  return registered;
}

}  // namespace skate3
