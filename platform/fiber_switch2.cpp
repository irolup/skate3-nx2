// Fibers for Horizon (AArch64): no ucontext, so a hand-written context switch.
#include <rex/platform.h>
#include <rex/thread/fiber.h>

#include <cassert>
#include <cstring>

extern "C" void rex_fiber_switch(void* from_ctx, void* to_ctx);

// void rex_fiber_switch(Context* from, Context* to)
// Saves x19-x30, sp, d8-d15 into *from; restores them from *to and returns
// into the restored x30.
asm(R"(
.text
.global rex_fiber_switch
.type rex_fiber_switch, %function
rex_fiber_switch:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    stp d8,  d9,  [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ldr x9, [x1, #96]
    mov sp, x9
    ldp d8,  d9,  [x1, #104]
    ldp d10, d11, [x1, #120]
    ldp d12, d13, [x1, #136]
    ldp d14, d15, [x1, #152]
    ret
)");

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size + 16);
  std::memset(&f->context_, 0, sizeof(f->context_));
  uintptr_t top = reinterpret_cast<uintptr_t>(f->stack_.data() + f->stack_.size());
  top &= ~uintptr_t(15);
  f->context_.sp = top;
  // x30 (index 11) is the return address of the first switch.
  f->context_.x[11] = reinterpret_cast<uint64_t>(&Fiber::Trampoline);
  return f;
}

void Fiber::Trampoline() {
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
  // A fiber entry must not return.
  __builtin_trap();
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_switch(&from->context_, &target->context_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  delete this;
}

}  // namespace rex::thread
