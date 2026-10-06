#include "page_fault_handler.h"
#include "log.h"
#include "platform.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>
Log_SetChannel(Common::PageFaultHandler);

#if defined(_WIN32)
#include "windows_headers.h"
#elif defined(__linux__) || defined(__ANDROID__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#define USE_SIGSEGV 1
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <signal.h>
#include <unistd.h>
#define USE_SIGSEGV 1
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
#include <switch.h>
#include "switch_exception_frame.h"
#define USE_LIBNX_EXCEPTION 1
#endif

namespace Common::PageFaultHandler {

struct RegisteredHandler
{
  Callback callback;
  const void* owner;
  void* start_pc;
  uint32_t code_size;
};
static std::vector<RegisteredHandler> m_handlers;
static std::mutex m_handler_lock;
static thread_local bool s_in_handler;

#if defined(CPU_AARCH32)
static bool IsStoreInstruction(const void* ptr)
{
  uint32_t bits;
  std::memcpy(&bits, ptr, sizeof(bits));

  // TODO
  return false;
}

#elif defined(CPU_AARCH64)
static bool IsStoreInstruction(const void* ptr)
{
  uint32_t bits;
  std::memcpy(&bits, ptr, sizeof(bits));

  // Based on vixl's disassembler Instruction::IsStore().
  // if (Mask(LoadStoreAnyFMask) != LoadStoreAnyFixed)
  if ((bits & 0x0a000000) != 0x08000000)
    return false;

  // if (Mask(LoadStorePairAnyFMask) == LoadStorePairAnyFixed)
  if ((bits & 0x3a000000) == 0x28000000)
  {
    // return Mask(LoadStorePairLBit) == 0
    return (bits & (1 << 22)) == 0;
  }

  switch (bits & 0xC4C00000)
  {
    case 0x00000000: // STRB_w
    case 0x40000000: // STRH_w
    case 0x80000000: // STR_w
    case 0xC0000000: // STR_x
    case 0x04000000: // STR_b
    case 0x44000000: // STR_h
    case 0x84000000: // STR_s
    case 0xC4000000: // STR_d
    case 0x04800000: // STR_q
      return true;

    default:
      return false;
  }
}
#endif

#if defined(_WIN32) && (defined(CPU_X64) || defined(CPU_AARCH64))
static PVOID s_veh_handle;

static LONG ExceptionHandler(PEXCEPTION_POINTERS exi)
{
  if (exi->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || s_in_handler)
    return EXCEPTION_CONTINUE_SEARCH;

  s_in_handler = true;

#if defined(_M_AMD64)
  void* const exception_pc = reinterpret_cast<void*>(exi->ContextRecord->Rip);
#elif defined(_M_ARM64)
  void* const exception_pc = reinterpret_cast<void*>(exi->ContextRecord->Pc);
#else
  void* const exception_pc = nullptr;
#endif

  void* const exception_address = reinterpret_cast<void*>(exi->ExceptionRecord->ExceptionInformation[1]);
  bool const is_write = exi->ExceptionRecord->ExceptionInformation[0] == 1;

  std::lock_guard<std::mutex> guard(m_handler_lock);
  for (const RegisteredHandler& rh : m_handlers)
  {
    if (rh.callback(exception_pc, exception_address, is_write) == HandlerResult::ContinueExecution)
    {
      s_in_handler = false;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }

  s_in_handler = false;

  return EXCEPTION_CONTINUE_SEARCH;
}

uint32_t GetHandlerCodeSize()
{
  return 0;
}

#elif defined(USE_SIGSEGV)

static struct sigaction s_old_sigsegv_action;
#if defined(__APPLE__) || defined(__aarch64__)
static struct sigaction s_old_sigbus_action;
#endif

static void SIGSEGVHandler(int sig, siginfo_t* info, void* ctx)
{
  if ((info->si_code != SEGV_MAPERR && info->si_code != SEGV_ACCERR) || s_in_handler)
    return;

#if defined(__linux__) || defined(__ANDROID__)
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);

#if defined(CPU_X64)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(CPU_AARCH32)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.arm_pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#elif defined(CPU_AARCH64)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

#elif defined(__APPLE__)

#if defined(CPU_X64)
  void* const exception_address =
    reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__faultvaddr);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__rip);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__err & 2) != 0;
#elif defined(CPU_AARCH64)
  void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__far);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

#elif defined(__FreeBSD__)

#if defined(CPU_X64)
  void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_addr);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_rip);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_err & 2) != 0;
#elif defined(CPU_AARCH64)
  void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__far);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

#endif

  std::lock_guard<std::mutex> guard(m_handler_lock);
  for (const RegisteredHandler& rh : m_handlers)
  {
    if (rh.callback(exception_pc, exception_address, is_write) == HandlerResult::ContinueExecution)
    {
      s_in_handler = false;
      return;
    }
  }

  // call old signal handler
#if !defined(__APPLE__) && !defined(__aarch64__)
  const struct sigaction& sa = s_old_sigsegv_action;
#else
  const struct sigaction& sa = (sig == SIGBUS) ? s_old_sigbus_action : s_old_sigsegv_action;
#endif
  if (sa.sa_flags & SA_SIGINFO)
    sa.sa_sigaction(sig, info, ctx);
  else if (sa.sa_handler == SIG_DFL)
    signal(sig, SIG_DFL);
  else if (sa.sa_handler == SIG_IGN)
    return;
  else
    sa.sa_handler(sig);
}

uint32_t GetHandlerCodeSize()
{
  return 0;
}

#elif defined(USE_LIBNX_EXCEPTION)

// Switch/libnx exception handler implementation
// switch_exception_entry.S replaces libnx's __libnx_exception_entry: the
// kernel calls it on a fault with the exception frame, and it calls
// switch_exception_handler below on a dedicated stack. A fastmem fault is
// backpatched (the code at pc is rewritten) and the thread resumes at the
// same pc; anything else is returned to the kernel unhandled, for a crash
// report of the original fault.

extern "C" {
alignas(16) uint8_t switch_exception_stack_top[0x8000];
uint32_t switch_exception_handler(Result reason, ExceptionFrameA64* frame, uint64_t fp);
}

static bool s_libnx_handler_installed = false;
// Not thread_local, and no lock taken: thread-local storage reads as null
// inside the exception entry (a guard there faulted at address 0), and only
// the CPU thread faults on purpose. The handlers only change while the
// recompiler starts or stops, outside any fault.
static bool s_switch_in_handler = false;

static bool HandleSwitchFault(ExceptionFrameA64* frame)
{
  // a fault inside a handler is not one to handle again
  if (!s_libnx_handler_installed || s_switch_in_handler)
    return false;

  // Only data aborts (from this EL or a lower one) can be fastmem accesses;
  // anything else (a jump to a bad address, an undefined instruction) is not
  // ours, and reading the instruction at pc could fault again there.
  const uint32_t exception_class = frame->esr >> 26;
  if (exception_class != 0x24 && exception_class != 0x25)
    return false;

  void* const exception_pc = reinterpret_cast<void*>(frame->pc);
  void* const fault_address = reinterpret_cast<void*>(frame->far);
  const bool is_write = (frame->esr & (1u << 6)) != 0; // ISS.WnR

  s_switch_in_handler = true;
  bool handled = false;
  for (const RegisteredHandler& rh : m_handlers)
  {
    if (rh.callback(exception_pc, fault_address, is_write) == HandlerResult::ContinueExecution)
    {
      handled = true;
      break;
    }
  }
  s_switch_in_handler = false;
  return handled;
}

// Returns what switch_exception_entry.S passes to svcReturnFromException: 0
// resumes the thread; 0xF801 (as libnx's own entry does) marks the exception
// unhandled, so the kernel ends the process and Atmosphere reports the
// original fault, with the registers it happened with.
extern "C" uint32_t switch_exception_handler(Result reason, ExceptionFrameA64* frame, uint64_t fp)
{
  (void)fp;
  if (HandleSwitchFault(frame))
    return 0;
  if (s_libnx_handler_installed)
  {
    Log_ErrorPrintf("Unhandled exception 0x%x: class 0x%02X (esr %08X) at pc %p, address %p, lr %p", reason,
                    frame->esr >> 26, frame->esr, reinterpret_cast<void*>(frame->pc),
                    reinterpret_cast<void*>(frame->far), reinterpret_cast<void*>(frame->lr));
  }
  return 0xF801;
}

uint32_t GetHandlerCodeSize()
{
  return 0;
}

#else

uint32_t GetHandlerCodeSize()
{
  return 0;
}

#endif

bool InstallHandler(const void* owner, void* start_pc, uint32_t code_size, Callback callback)
{
  bool was_empty;
  {
    std::lock_guard<std::mutex> guard(m_handler_lock);
    if (std::find_if(m_handlers.begin(), m_handlers.end(),
                     [owner](const RegisteredHandler& rh) { return rh.owner == owner; }) != m_handlers.end())
    {
      return false;
    }

    was_empty = m_handlers.empty();
  }

  if (was_empty)
  {
#if defined(_WIN32) && (defined(CPU_X64) || defined(CPU_AARCH64))
    s_veh_handle = AddVectoredExceptionHandler(1, ExceptionHandler);
    if (!s_veh_handle)
    {
      Log_ErrorPrint("Failed to add vectored exception handler");
      return false;
    }
#elif defined(USE_SIGSEGV)
    struct sigaction sa = {};
    sa.sa_sigaction = SIGSEGVHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &s_old_sigsegv_action) < 0)
    {
      Log_ErrorPrintf("sigaction(SIGSEGV) failed: %d", errno);
      return false;
    }
#if defined(__APPLE__) || defined(__aarch64__)
    if (sigaction(SIGBUS, &sa, &s_old_sigbus_action) < 0)
    {
      Log_ErrorPrintf("sigaction(SIGBUS) failed: %d", errno);
      return false;
    }
#endif

#elif defined(USE_LIBNX_EXCEPTION)
    // switch_exception_entry.S is always the process's exception entry; it
    // only hands faults on while a handler is installed.
    s_libnx_handler_installed = true;
    Log_InfoPrint("Installed libnx exception handler for fastmem");

#else
    return false;
#endif
  }

  m_handlers.push_back(RegisteredHandler{callback, owner, start_pc, code_size});
  return true;
}

bool RemoveHandler(const void* owner)
{
  std::lock_guard<std::mutex> guard(m_handler_lock);
  auto it = std::find_if(m_handlers.begin(), m_handlers.end(),
                         [owner](const RegisteredHandler& rh) { return rh.owner == owner; });
  if (it == m_handlers.end())
    return false;

  m_handlers.erase(it);

  if (m_handlers.empty())
  {
#if defined(_WIN32) && (defined(CPU_X64) || defined(CPU_AARCH64))
    RemoveVectoredExceptionHandler(s_veh_handle);
    s_veh_handle = nullptr;
#elif defined(USE_SIGSEGV)
    // restore old signal handler
#if defined(__APPLE__) || defined(__aarch64__)
    if (sigaction(SIGBUS, &s_old_sigbus_action, nullptr) < 0)
    {
      Log_ErrorPrintf("sigaction(SIGBUS) failed: %d", errno);
      return false;
    }
    s_old_sigbus_action = {};
#endif

    if (sigaction(SIGSEGV, &s_old_sigsegv_action, nullptr) < 0)
    {
      Log_ErrorPrintf("sigaction(SIGSEGV) failed: %d", errno);
      return false;
    }

    s_old_sigsegv_action = {};

#elif defined(USE_LIBNX_EXCEPTION)
    // faults are no longer handed on (see InstallHandler)
    s_libnx_handler_installed = false;
    Log_InfoPrint("Removed libnx exception handler");

#else
    return false;
#endif
  }

  return true;
}

} // namespace Common::PageFaultHandler
