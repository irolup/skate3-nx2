// No host fault interception on Horizon (see platform notes): handlers are
// registered but never invoked. Write-watch relies on explicit invalidation.
#include <rex/exception_handler.h>

namespace rex::arch {
namespace {
struct Entry { ExceptionHandler::Handler fn = nullptr; void* data = nullptr; };
Entry g_handlers[8];
}  // namespace

void ExceptionHandler::Install(Handler fn, void* data) {
  for (auto& e : g_handlers) {
    if (!e.fn) { e.fn = fn; e.data = data; return; }
  }
}
void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (auto& e : g_handlers) {
    if (e.fn == fn && e.data == data) { e = {}; return; }
  }
}
}  // namespace rex::arch
