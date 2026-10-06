#include "memory_arena.h"
#include "common/log.h"
#include "common/string_util.h"
Log_SetChannel(Common::MemoryArena);

#if defined(_WIN32)
#include "common/windows_headers.h"
#elif defined(__ANDROID__)
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/ashmem.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
#include <malloc.h>
#include <switch.h>
#include <vector>
#endif

namespace Common {

#if defined(__SWITCH__) || defined(HAVE_LIBNX)
// The Switch has no shared memory objects, so mmap fastmem is built from
// kernel aliases (as DuckStation's and Dolphin's Switch ports do):
//
// - the arena is heap memory, mapped once with svcMapProcessCodeMemory as a
//   read/write alias in the code region. The heap pages themselves are
//   inaccessible from then on: every view, g_ram included, goes through the
//   alias.
// - each fastmem RAM mirror maps the alias again with svcMapProcessMemory,
//   inside a 4 GB address range reserved for the fastmem base; the rest of
//   that range stays unmapped, so a fastmem access outside RAM faults and is
//   backpatched to the slow path.
// - such a mirror cannot be made read-only, so a page with code in it is
//   unmapped from the mirrors instead (reads fault too, and are backpatched).
//
// Without the syscalls (the loader did not grant them) or when a mapping
// fails, views point straight into the heap and fastmem uses LUT instead.
namespace {
constexpr unsigned SVC_MAP_PROCESS_MEMORY = 0x74;
constexpr unsigned SVC_UNMAP_PROCESS_MEMORY = 0x75;
constexpr unsigned SVC_MAP_PROCESS_CODE_MEMORY = 0x77;
constexpr unsigned SVC_UNMAP_PROCESS_CODE_MEMORY = 0x78;
constexpr size_t SWITCH_PAGE_SIZE = 0x1000;

struct SwitchMirror
{
  uint8_t* addr;
  uint64_t source; // in the alias
  size_t size;
  std::vector<bool> mapped; // per page
};

struct SwitchAlias
{
  uint8_t* addr = nullptr; // the code-region alias of the heap buffer
  uint64_t heap = 0;
  size_t size = 0;
};

SwitchAlias s_alias;
std::vector<SwitchMirror> s_mirrors;
uint8_t* s_fastmem_region = nullptr;
size_t s_fastmem_region_size = 0;
VirtmemReservation* s_fastmem_reservation = nullptr;

bool HasFastmemSyscalls()
{
  return envIsSyscallHinted(SVC_MAP_PROCESS_MEMORY) && envIsSyscallHinted(SVC_UNMAP_PROCESS_MEMORY) &&
         envIsSyscallHinted(SVC_MAP_PROCESS_CODE_MEMORY) && envIsSyscallHinted(SVC_UNMAP_PROCESS_CODE_MEMORY);
}

SwitchMirror* FindMirror(const void* address, size_t length)
{
  const uint8_t* p = static_cast<const uint8_t*>(address);
  for (SwitchMirror& m : s_mirrors)
    if (p >= m.addr && p + length <= m.addr + m.size)
      return &m;
  return nullptr;
}

// Maps (or unmaps) the mirror's pages in [offset, offset + length), a run
// of pages in the same state at a time.
bool SetMirrorPages(SwitchMirror& m, size_t offset, size_t length, bool map)
{
  size_t page = offset / SWITCH_PAGE_SIZE;
  const size_t end = (offset + length + SWITCH_PAGE_SIZE - 1) / SWITCH_PAGE_SIZE;
  while (page < end)
  {
    if (m.mapped[page] == map)
    {
      page++;
      continue;
    }
    size_t run = 1;
    while (page + run < end && m.mapped[page + run] != map)
      run++;
    void* const dst = m.addr + page * SWITCH_PAGE_SIZE;
    const uint64_t src = m.source + page * SWITCH_PAGE_SIZE;
    const Result rc = map ? svcMapProcessMemory(dst, envGetOwnProcessHandle(), src, run * SWITCH_PAGE_SIZE) :
                            svcUnmapProcessMemory(dst, envGetOwnProcessHandle(), src, run * SWITCH_PAGE_SIZE);
    if (R_FAILED(rc))
    {
      Log_ErrorPrintf("%s of %zu pages at %p failed: 0x%x", map ? "svcMapProcessMemory" : "svcUnmapProcessMemory",
                      run, dst, rc);
      return false;
    }
    for (size_t i = 0; i < run; i++)
      m.mapped[page + i] = map;
    page += run;
  }
  return true;
}

void ReleaseMirror(std::vector<SwitchMirror>::iterator it)
{
  SetMirrorPages(*it, 0, it->size, false);
  s_mirrors.erase(it);
}

void ReleaseAlias()
{
  while (!s_mirrors.empty())
    ReleaseMirror(s_mirrors.begin());
  if (!s_alias.addr)
    return;
  const Result rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), reinterpret_cast<uint64_t>(s_alias.addr),
                                              s_alias.heap, s_alias.size);
  if (R_FAILED(rc))
    Log_ErrorPrintf("svcUnmapProcessCodeMemory failed: 0x%x", rc);
  s_alias = {};
}
} // namespace
#endif

// Borrowed from Dolphin
#ifdef __ANDROID__
#define ASHMEM_DEVICE "/dev/ashmem"

static int AshmemCreateFileMapping(const char* name, size_t size)
{
  // ASharedMemory path - works on API >= 26 and falls through on API < 26:

  // We can't call ASharedMemory_create the normal way without increasing the
  // minimum version requirement to API 26, so we use dlopen/dlsym instead
  static void* libandroid = dlopen("libandroid.so", RTLD_LAZY | RTLD_LOCAL);
  static auto shared_memory_create =
    reinterpret_cast<int (*)(const char*, size_t)>(dlsym(libandroid, "ASharedMemory_create"));
  if (shared_memory_create)
    return shared_memory_create(name, size);

  // /dev/ashmem path - works on API < 29:

  int fd, ret;
  fd = open(ASHMEM_DEVICE, O_RDWR);
  if (fd < 0)
    return fd;

  // We don't really care if we can't set the name, it is optional
  ioctl(fd, ASHMEM_SET_NAME, name);

  ret = ioctl(fd, ASHMEM_SET_SIZE, size);
  if (ret < 0)
  {
    close(fd);
    Log_ErrorPrintf("Ashmem returned error: 0x%08x", ret);
    return ret;
  }
  return fd;
}
#endif

MemoryArena::MemoryArena() = default;

MemoryArena::~MemoryArena()
{
  Destroy();
}

void* MemoryArena::FindBaseAddressForMapping(size_t size)
{
  void* base_address;
#if defined(_WIN32)
  base_address = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
  if (base_address)
    VirtualFree(base_address, 0, MEM_RELEASE);
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  base_address = mmap(nullptr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (base_address)
    munmap(base_address, size);
#elif defined(__ANDROID__)
  base_address = mmap(nullptr, size, PROT_NONE, MAP_ANON | MAP_SHARED, -1, 0);
  if (base_address)
    munmap(base_address, size);
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  // the range stays reserved (the mirrors are mapped into it), until the
  // arena is destroyed
  base_address = nullptr;
  if (s_fastmem_region && s_fastmem_region_size >= size)
    return s_fastmem_region;
  if (s_alias.addr)
  {
    virtmemLock();
    base_address = virtmemFindAslr(size, SWITCH_PAGE_SIZE);
    if (base_address)
    {
      s_fastmem_reservation = virtmemAddReservation(base_address, size);
      if (!s_fastmem_reservation)
        base_address = nullptr;
    }
    virtmemUnlock();
    if (base_address)
    {
      s_fastmem_region = static_cast<uint8_t*>(base_address);
      s_fastmem_region_size = size;
      Log_InfoPrintf("Fastmem region: %zu bytes at %p", size, base_address);
    }
  }
#else
  base_address = nullptr;
#endif

  if (!base_address)
  {
    Log_ErrorPrintf("Failed to get base address for memory mapping of size %zu", size);
    return nullptr;
  }

  return base_address;
}

bool MemoryArena::IsValid() const
{
#if defined(_WIN32)
  return m_file_handle != nullptr;
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  return m_buffer != nullptr;
#else
  return m_shmem_fd >= 0;
#endif
}

static std::string GetFileMappingName()
{
#if defined(_WIN32)
  const unsigned pid = GetCurrentProcessId();
#elif defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  const unsigned pid = static_cast<unsigned>(getpid());
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  const unsigned pid = 0;
#else
#error Unknown platform.
#endif

  const std::string ret(StringUtil::StdStringFromFormat("swanstation_%u_%p", pid, ((void*)&GetFileMappingName)));

  Log_InfoPrintf("File mapping name: %s", ret.c_str());
  return ret;
}

bool MemoryArena::Create(size_t size, bool writable, bool executable)
{
  if (IsValid())
    Destroy();

  const std::string file_mapping_name(GetFileMappingName());

#if defined(_WIN32)
  const DWORD protect = (writable ? (executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) : PAGE_READONLY);
  m_file_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, protect, static_cast<uint32_t>(size >> 16 >> 16), static_cast<uint32_t>(size),
                                     file_mapping_name.c_str());
  if (!m_file_handle)
  {
    Log_ErrorPrintf("CreateFileMapping failed: %u", GetLastError());
    return false;
  }

  m_size = size;
  m_writable = writable;
  m_executable = executable;
  return true;
#elif defined(__ANDROID__)
  m_shmem_fd = AshmemCreateFileMapping(file_mapping_name.c_str(), size);
  if (m_shmem_fd < 0)
  {
    Log_ErrorPrintf("AshmemCreateFileMapping failed: %d %d", m_shmem_fd, errno);
    return false;
  }

  m_size = size;
  m_writable = writable;
  m_executable = executable;
  return true;
#elif defined(__linux__)
  m_shmem_fd = shm_open(file_mapping_name.c_str(), O_CREAT | O_EXCL | (writable ? O_RDWR : O_RDONLY), 0600);
  if (m_shmem_fd < 0)
  {
    Log_ErrorPrintf("shm_open failed: %d", errno);
    return false;
  }

  // we're not going to be opening this mapping in other processes, so remove the file
  shm_unlink(file_mapping_name.c_str());

  // ensure it's the correct size
  if (ftruncate(m_shmem_fd, static_cast<off_t>(size)) < 0)
  {
    Log_ErrorPrintf("ftruncate(%zu) failed: %d", size, errno);
    return false;
  }

  m_size = size;
  m_writable = writable;
  m_executable = executable;
  return true;
#elif defined(__APPLE__) || defined(__FreeBSD__)
#if defined(__APPLE__)
  m_shmem_fd = shm_open(file_mapping_name.c_str(), O_CREAT | O_EXCL | (writable ? O_RDWR : O_RDONLY), 0600);
#else
  m_shmem_fd = shm_open(SHM_ANON, O_CREAT | O_EXCL | (writable ? O_RDWR : O_RDONLY), 0600);
#endif

  if (m_shmem_fd < 0)
  {
    Log_ErrorPrintf("shm_open failed: %d", errno);
    return false;
  }

#ifdef __APPLE__
  // we're not going to be opening this mapping in other processes, so remove the file
  shm_unlink(file_mapping_name.c_str());
#endif

  // ensure it's the correct size
  if (ftruncate(m_shmem_fd, static_cast<off_t>(size)) < 0)
  {
    Log_ErrorPrintf("ftruncate(%zu) failed: %d", size, errno);
    return false;
  }

  m_size = size;
  m_writable = writable;
  m_executable = executable;
  return true;

#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  m_buffer = memalign(0x1000, size);
  if (!m_buffer)
  {
    Log_ErrorPrintf("memalign(%zu) failed", size);
    return false;
  }
  std::memset(m_buffer, 0, size);
  m_size = size;
  m_writable = writable;
  m_executable = executable;

  if (!HasFastmemSyscalls())
  {
    Log_WarningPrintf("The memory mapping syscalls are not available: no mmap fastmem");
    return true;
  }
  virtmemLock();
  void* alias = virtmemFindCodeMemory(size, SWITCH_PAGE_SIZE);
  Result rc = alias ? svcMapProcessCodeMemory(envGetOwnProcessHandle(), reinterpret_cast<uint64_t>(alias),
                                              reinterpret_cast<uint64_t>(m_buffer), size) :
                      MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  if (R_SUCCEEDED(rc))
  {
    rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), reinterpret_cast<uint64_t>(alias), size, Perm_Rw);
    if (R_FAILED(rc))
      svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), reinterpret_cast<uint64_t>(alias),
                                reinterpret_cast<uint64_t>(m_buffer), size);
  }
  virtmemUnlock();
  if (R_FAILED(rc))
  {
    Log_WarningPrintf("Mapping the memory arena's alias failed (0x%x): no mmap fastmem", rc);
    return true;
  }
  s_alias.addr = static_cast<uint8_t*>(alias);
  s_alias.heap = reinterpret_cast<uint64_t>(m_buffer);
  s_alias.size = size;
  return true;
#else
  return false;
#endif
}

void MemoryArena::Destroy()
{
#if defined(_WIN32)
  if (m_file_handle)
  {
    CloseHandle(m_file_handle);
    m_file_handle = nullptr;
  }
#elif defined(__linux__) || defined(__FreeBSD__)
  if (m_shmem_fd > 0)
  {
    close(m_shmem_fd);
    m_shmem_fd = -1;
  }
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  ReleaseAlias(); // the heap is accessible again once its alias is gone
  if (s_fastmem_reservation)
  {
    virtmemLock();
    virtmemRemoveReservation(s_fastmem_reservation);
    virtmemUnlock();
    s_fastmem_reservation = nullptr;
    s_fastmem_region = nullptr;
    s_fastmem_region_size = 0;
  }
  if (m_buffer)
  {
    free(m_buffer);
    m_buffer = nullptr;
  }
#endif
}

std::optional<MemoryArena::View> MemoryArena::CreateView(size_t offset, size_t size, bool writable, bool executable,
                                                         void* fixed_address)
{
  void* base_pointer = CreateViewPtr(offset, size, writable, executable, fixed_address);
  if (!base_pointer)
    return std::nullopt;

  return View(this, base_pointer, offset, size, writable);
}

std::optional<MemoryArena::View> MemoryArena::CreateReservedView(size_t size, void* fixed_address /*= nullptr*/)
{
  void* base_pointer = CreateReservedPtr(size, fixed_address);
  if (!base_pointer)
    return std::nullopt;

  return View(this, base_pointer, View::RESERVED_REGION_OFFSET, size, false);
}

void* MemoryArena::CreateViewPtr(size_t offset, size_t size, bool writable, bool executable,
                                 void* fixed_address /*= nullptr*/)
{
  void* base_pointer;
#if defined(_WIN32)
  const DWORD desired_access = FILE_MAP_READ | (writable ? FILE_MAP_WRITE : 0) | (executable ? FILE_MAP_EXECUTE : 0);
  base_pointer =
    MapViewOfFileEx(m_file_handle, desired_access, static_cast<uint32_t>(offset >> 16 >> 16), static_cast<uint32_t>(offset), size, fixed_address);
  if (!base_pointer)
    return nullptr;
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  const int flags = (fixed_address != nullptr) ? (MAP_SHARED | MAP_FIXED) : MAP_SHARED;
  const int prot = PROT_READ | (writable ? PROT_WRITE : 0) | (executable ? PROT_EXEC : 0);
  base_pointer = mmap(fixed_address, size, prot, flags, m_shmem_fd, static_cast<off_t>(offset));
  if (base_pointer == reinterpret_cast<void*>(-1))
    return nullptr;
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  if (offset + size > m_size)
    return nullptr;
  if (!fixed_address)
  {
    // a plain view: through the alias when there is one (the heap pages are
    // not accessible then)
    base_pointer = (s_alias.addr ? s_alias.addr : static_cast<uint8_t*>(m_buffer)) + offset;
  }
  else
  {
    // a fastmem mirror at the address asked for
    if (!s_alias.addr)
      return nullptr;
    SwitchMirror mirror{static_cast<uint8_t*>(fixed_address), reinterpret_cast<uint64_t>(s_alias.addr) + offset,
                        size, std::vector<bool>((size + SWITCH_PAGE_SIZE - 1) / SWITCH_PAGE_SIZE, false)};
    if (!SetMirrorPages(mirror, 0, size, true))
    {
      SetMirrorPages(mirror, 0, size, false);
      return nullptr;
    }
    s_mirrors.push_back(std::move(mirror));
    base_pointer = fixed_address;
  }
#else
  return nullptr;
#endif

  m_num_views.fetch_add(1);
  return base_pointer;
}

bool MemoryArena::FlushViewPtr(void* address, size_t size)
{
#if defined(_WIN32)
  return FlushViewOfFile(address, size);
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  return (msync(address, size, 0) >= 0);
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  // No OS flush needed for simple RAM.
  return true;
#else
  return false;
#endif
}

bool MemoryArena::ReleaseViewPtr(void* address, size_t size)
{
  bool result;
#if defined(_WIN32)
  result = static_cast<bool>(UnmapViewOfFile(address));
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  result = (munmap(address, size) >= 0);
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  // a mirror is unmapped; a plain view lives as long as the arena's alias
  result = true;
  for (auto it = s_mirrors.begin(); it != s_mirrors.end(); ++it)
  {
    if (it->addr == address)
    {
      ReleaseMirror(it);
      break;
    }
  }
#else
  result = false;
#endif

  if (!result)
  {
    Log_ErrorPrintf("Failed to unmap previously-created view at %p", address);
    return false;
  }

  m_num_views.fetch_sub(1);
  return true;
}

void* MemoryArena::CreateReservedPtr(size_t size, void* fixed_address /*= nullptr*/)
{
  void* base_pointer;
#if defined(_WIN32)
  base_pointer = VirtualAlloc(fixed_address, size, MEM_RESERVE, PAGE_NOACCESS);
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  const int flags =
    (fixed_address != nullptr) ? (MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED) : (MAP_PRIVATE | MAP_ANONYMOUS);
  base_pointer = mmap(fixed_address, size, PROT_NONE, flags, -1, 0);
  if (base_pointer == reinterpret_cast<void*>(-1))
    return nullptr;
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  // inside the fastmem region the range is already reserved, and stays
  // unmapped so that accesses there fault
  const uint8_t* p = static_cast<const uint8_t*>(fixed_address);
  if (p && s_fastmem_region && p >= s_fastmem_region && p + size <= s_fastmem_region + s_fastmem_region_size)
    base_pointer = fixed_address;
  else if (!fixed_address)
    base_pointer = memalign(0x1000, size);
  else
    return nullptr;
#else
  return nullptr;
#endif

  m_num_views.fetch_add(1);
  return base_pointer;
}

bool MemoryArena::ReleaseReservedPtr(void* address, size_t size)
{
  bool result;
#if defined(_WIN32)
  result = static_cast<bool>(VirtualFree(address, 0, MEM_RELEASE));
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  result = (munmap(address, size) >= 0);
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  const uint8_t* p = static_cast<const uint8_t*>(address);
  if (!(s_fastmem_region && p >= s_fastmem_region && p < s_fastmem_region + s_fastmem_region_size))
    free(address);
  result = true;
#else
  result = false;
#endif

  if (!result)
  {
    Log_ErrorPrintf("Failed to release previously-created view at %p", address);
    return false;
  }

  m_num_views.fetch_sub(1);
  return true;
}

bool MemoryArena::SetPageProtection(void* address, size_t length, bool readable, bool writable, bool executable)
{
#if defined(_WIN32)
  static constexpr DWORD protection_table[2][2][2] = {
    {{PAGE_NOACCESS, PAGE_EXECUTE}, {PAGE_WRITECOPY, PAGE_EXECUTE_WRITECOPY}},
    {{PAGE_READONLY, PAGE_EXECUTE_READ}, {PAGE_READWRITE, PAGE_EXECUTE_READWRITE}}};

  DWORD old_protect;
  return static_cast<bool>(
    VirtualProtect(address, length, protection_table[readable][writable][executable], &old_protect));
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__FreeBSD__)
  const int prot = (readable ? PROT_READ : 0) | (writable ? PROT_WRITE : 0) | (executable ? PROT_EXEC : 0);
  return (mprotect(address, length, prot) >= 0);
#elif defined(__SWITCH__) || defined(HAVE_LIBNX)
  // a mirror's pages are mapped while writable and unmapped otherwise (a
  // process-memory mapping cannot be read-only); other memory is left as is
  (void)readable;
  (void)executable;
  SwitchMirror* mirror = FindMirror(address, length);
  if (!mirror)
    return true;
  return SetMirrorPages(*mirror, static_cast<size_t>(static_cast<uint8_t*>(address) - mirror->addr), length,
                        writable);
#else
  return false;
#endif
}

MemoryArena::View::View(MemoryArena* parent, void* base_pointer, size_t arena_offset, size_t mapping_size,
                        bool writable)
  : m_parent(parent), m_base_pointer(base_pointer), m_arena_offset(arena_offset), m_mapping_size(mapping_size),
    m_writable(writable)
{
}

MemoryArena::View::View(View&& view)
  : m_parent(view.m_parent), m_base_pointer(view.m_base_pointer), m_arena_offset(view.m_arena_offset),
    m_mapping_size(view.m_mapping_size)
{
  view.m_parent = nullptr;
  view.m_base_pointer = nullptr;
  view.m_arena_offset = 0;
  view.m_mapping_size = 0;
}

MemoryArena::View::~View()
{
  if (m_parent)
  {
    if (m_arena_offset != RESERVED_REGION_OFFSET)
    {
      if (m_writable)
        m_parent->FlushViewPtr(m_base_pointer, m_mapping_size);
      m_parent->ReleaseViewPtr(m_base_pointer, m_mapping_size);
    }
    else
    {
      m_parent->ReleaseReservedPtr(m_base_pointer, m_mapping_size);
    }
  }
}
} // namespace Common
