#pragma once
#include "types.h"

#if defined(__SWITCH__) || defined(HAVE_LIBNX)
#include <switch.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#endif

class JitCodeBuffer
{
public:
  JitCodeBuffer();
  ~JitCodeBuffer();

  bool IsValid() const { return (m_code_ptr != nullptr); }

  bool Allocate(uint32_t size = 64 * 1024 * 1024, uint32_t far_code_size = 0);
  bool Initialize(void* buffer, uint32_t size, uint32_t far_code_size = 0, uint32_t guard_size = 0);
  void Destroy();
  void Reset();

#if defined(__SWITCH__) || defined(HAVE_LIBNX)
  ALWAYS_INLINE uint8_t* GetCodePointer() const { return m_rx_ptr; }
#else
  ALWAYS_INLINE uint8_t* GetCodePointer() const { return m_code_ptr; }
#endif
  ALWAYS_INLINE uint32_t GetTotalSize() const { return m_total_size; }

  ALWAYS_INLINE uint8_t* GetFreeCodePointer() const { return m_free_code_ptr; }
  ALWAYS_INLINE uint32_t GetFreeCodeSpace() const { return static_cast<uint32_t>(m_code_size - m_code_used); }
  void ReserveCode(uint32_t size);
  void CommitCode(uint32_t length);

  ALWAYS_INLINE uint8_t* GetFreeFarCodePointer() const { return m_free_far_code_ptr; }
  ALWAYS_INLINE uint32_t GetFreeFarCodeSpace() const { return static_cast<uint32_t>(m_far_code_size - m_far_code_used); }
  void CommitFarCode(uint32_t length);

  ALWAYS_INLINE uint8_t* GetFreeRxCodePointer() const
  {
#if defined(__SWITCH__) || defined(HAVE_LIBNX)
    return m_rx_ptr + (m_free_code_ptr - m_code_ptr);
#else
    return m_free_code_ptr;
#endif
  }

  ALWAYS_INLINE uint8_t* GetFreeFarRxCodePointer() const
  {
#if defined(__SWITCH__) || defined(HAVE_LIBNX)
    return m_rx_ptr + (m_free_far_code_ptr - m_code_ptr);
#else
    return m_free_far_code_ptr;
#endif
  }

  /// Adjusts the free code pointer to the specified alignment, padding with bytes.
  /// Assumes alignment is a power-of-two.
  void Align(uint32_t alignment, uint8_t padding_value);

  /// Flushes the instruction cache on the host for the specified range.
  static void FlushInstructionCache(void* address, uint32_t size);

  /// Switch: Toggles write protection on the JIT space.
#if defined(__SWITCH__) || defined(HAVE_LIBNX)
  void WriteProtect(bool enabled);
  /// For Apple Silicon - Toggles write protection on the JIT space.
  //
  /// macOS only. The two calls behind it, pthread_jit_write_protect_supported_np
  /// and pthread_jit_write_protect_np, are API_UNAVAILABLE(ios, tvos): the
  /// embedded platforms have no per-thread W^X toggle, so there is nothing to
  /// call and the no-op below is the whole story there.
#elif defined(__APPLE__) && defined(__aarch64__) && TARGET_OS_OSX
  static void WriteProtect(bool enabled);
#else
  ALWAYS_INLINE static void WriteProtect(bool enabled) {}
#endif

private:
  uint8_t* m_code_ptr = nullptr;
  uint8_t* m_free_code_ptr = nullptr;
  uint32_t m_code_size = 0;
  uint32_t m_code_reserve_size = 0;
  uint32_t m_code_used = 0;

  uint8_t* m_far_code_ptr = nullptr;
  uint8_t* m_free_far_code_ptr = nullptr;
  uint32_t m_far_code_size = 0;
  uint32_t m_far_code_used = 0;

  uint32_t m_total_size = 0;
  uint32_t m_guard_size = 0;
  uint32_t m_old_protection = 0;
  bool m_owns_buffer = false;

#if defined(__SWITCH__) || defined(HAVE_LIBNX)
public:
  // On Switch, GetCodePointer() returns the RX alias so logical usage (offsets/calling) works.
  // m_code_ptr will point to RW alias for internal writing logic.
  ALWAYS_INLINE uint8_t* GetRxPointer() const { return m_rx_ptr; }

  // Converts an RX address to its corresponding RW address for writing.
  // Returns nullptr if address is outside the JIT buffer range.
  ALWAYS_INLINE void* RxToRw(void* rx_addr) const
  {
    uint8_t* rx = static_cast<uint8_t*>(rx_addr);
    if (rx >= m_rx_ptr && rx < m_rx_ptr + m_total_size)
      return m_code_ptr + (rx - m_rx_ptr);
    return nullptr;
  }

private:
  Jit m_jit = {};
  uint8_t* m_rx_ptr = nullptr;
#endif
};
