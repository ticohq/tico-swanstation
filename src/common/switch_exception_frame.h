#pragma once

// The registers the Switch kernel hands a user exception handler, as
// __libnx_exception_entry receives them in x1 (from DuckStation's Switch port).
// The kernel restores the thread from this frame on svcReturnFromException.

#include <cstdint>

struct ExceptionFrameA64
{
  uint64_t x[9];
  uint64_t lr, sp, pc;
  uint32_t pstate, afsr0, afsr1, esr;
  uint64_t far;
};

static_assert(sizeof(ExceptionFrameA64) == 0x78, "the kernel's exception frame layout");
