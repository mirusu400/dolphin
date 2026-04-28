// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/MemoryUtil.h"

#include <cstddef>
#include <cstdlib>

#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#ifdef _WIN32
#include <windows.h>
#include "Common/StringUtil.h"
#elif defined(__SWITCH__)
#include <mutex>
#include <unordered_map>
#include <switch.h>
#else
#include <stdio.h>
#include <sys/mman.h>
#if defined(_M_ARM_64) && defined(__APPLE__)
#include <pthread.h>
#endif
#if defined __APPLE__ || defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__
#include <sys/sysctl.h>
#elif defined __HAIKU__
#include <OS.h>
#else
#include <sys/sysinfo.h>
#endif
#endif

#ifdef __SWITCH__
namespace
{
// libnx jit_t scaffolding for Horizon OS. RWX pages are forbidden under
// HOS, so AllocateExecutableMemory routes through jitCreate instead of
// mmap. The map keys recompiled-buffer pointers (rw_addr) to their
// owning Jit handle so FreeMemoryPages can dispatch jitClose, and so
// the W^X scope guards know which handles to transition. See
// docs/jit-memory.md §swap-strategy. The rw->rx alias plumbing inside
// JitArm64Cache is M2 hardware work tracked separately.
struct SwitchJitEntry
{
  ::Jit handle;
  size_t size;
};

std::mutex& SwitchJitMapMutex()
{
  static std::mutex m;
  return m;
}

std::unordered_map<void*, SwitchJitEntry>& SwitchJitMap()
{
  static std::unordered_map<void*, SwitchJitEntry> m;
  return m;
}
}  // namespace
#endif

namespace Common
{
// This is purposely not a full wrapper for virtualalloc/mmap, but it
// provides exactly the primitive operations that Dolphin needs.

void* AllocateExecutableMemory(size_t size)
{
#if defined(_WIN32)
  void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#elif defined(__SWITCH__)
  // Horizon OS forbids RWX pages. Route through libnx jit_t. Size is
  // auto-rounded to 4 KiB inside jitCreate; Dolphin's TOTAL_CODE_SIZE
  // is already a multiple, so no caller-side rounding required.
  SwitchJitEntry entry{};
  entry.size = size;
  Result rc = jitCreate(&entry.handle, size);
  if (R_FAILED(rc))
  {
    PanicAlertFmt("jitCreate failed (rc={:#010x}). The NRO probably lacks "
                  "the JIT kernel capability — launch via hbmenu/hbloader.",
                  rc);
    return nullptr;
  }
  void* ptr = jitGetRwAddr(&entry.handle);
  {
    std::lock_guard<std::mutex> lock(SwitchJitMapMutex());
    SwitchJitMap().emplace(ptr, entry);
  }
#else
  int map_flags = MAP_ANON | MAP_PRIVATE;
#if defined(__APPLE__)
  map_flags |= MAP_JIT;
#endif
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, map_flags, -1, 0);
  if (ptr == MAP_FAILED)
    ptr = nullptr;
#endif

  if (ptr == nullptr)
    PanicAlertFmt("Failed to allocate executable memory");

  return ptr;
}
// This function is used to provide a counter for the JITPageWrite*Execute*
// functions to enable nesting. The static variable is wrapped in a a function
// to allow those functions to be called inside of the constructor of a static
// variable portably.
//
// The variable is thread_local as the W^X mode is specific to each running thread.
static int& JITPageWriteNestCounter()
{
  static thread_local int nest_counter = 0;
  return nest_counter;
}

// Certain platforms (Mac OS on ARM) enforce that a single thread can only have write or
// execute permissions to pages at any given point of time. The two below functions
// are used to toggle between having write permissions or execute permissions.
//
// The default state of these allocations in Dolphin is for them to be executable,
// but not writeable. So, functions that are updating these pages should wrap their
// writes like below:

// JITPageWriteEnableExecuteDisable();
// PrepareInstructionStreamForJIT();
// JITPageWriteDisableExecuteEnable();

// These functions can be nested, in which case execution will only be enabled
// after the call to the JITPageWriteDisableExecuteEnable from the top most
// nesting level. Example:

// [JIT page is in execute mode for the thread]
// JITPageWriteEnableExecuteDisable();
//   [JIT page is in write mode for the thread]
//   JITPageWriteEnableExecuteDisable();
//     [JIT page is in write mode for the thread]
//   JITPageWriteDisableExecuteEnable();
//   [JIT page is in write mode for the thread]
// JITPageWriteDisableExecuteEnable();
// [JIT page is in execute mode for the thread]

// Allows a thread to write to executable memory, but not execute the data.
void JITPageWriteEnableExecuteDisable()
{
#if defined(_M_ARM_64) && defined(__APPLE__)
  if (JITPageWriteNestCounter() == 0)
  {
    pthread_jit_write_protect_np(0);
  }
#elif defined(__SWITCH__)
  if (JITPageWriteNestCounter() == 0)
  {
    // Stub-only: transition every registered handle. M2 hardware work
    // refines this with per-allocation tracking and lifts the rw->rx
    // alias translation into the dispatcher path. Single-core JIT
    // (the M2 default) makes the broad transition safe; dual-core
    // JIT (M6) needs per-handle locking — see docs/jit-memory.md
    // §thread-affinity.
    std::lock_guard<std::mutex> lock(SwitchJitMapMutex());
    for (auto& [rw_ptr, entry] : SwitchJitMap())
      jitTransitionToWritable(&entry.handle);
  }
#endif
  JITPageWriteNestCounter()++;
}
// Allows a thread to execute memory allocated for execution, but not write to it.
void JITPageWriteDisableExecuteEnable()
{
  JITPageWriteNestCounter()--;

  // Sanity check the NestCounter to identify underflow
  // This can indicate the calls to JITPageWriteDisableExecuteEnable()
  // are not matched with previous calls to JITPageWriteEnableExecuteDisable()
  if (JITPageWriteNestCounter() < 0)
    PanicAlertFmt("JITPageWriteNestCounter() underflowed");

#if defined(_M_ARM_64) && defined(__APPLE__)
  if (JITPageWriteNestCounter() == 0)
  {
    pthread_jit_write_protect_np(1);
  }
#elif defined(__SWITCH__)
  if (JITPageWriteNestCounter() == 0)
  {
    std::lock_guard<std::mutex> lock(SwitchJitMapMutex());
    for (auto& [rw_ptr, entry] : SwitchJitMap())
      jitTransitionToExecutable(&entry.handle);
  }
#endif
}

void* AllocateMemoryPages(size_t size)
{
#ifdef _WIN32
  void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
#elif defined(__SWITCH__)
  // No mmap on devkitA64. Page-aligned heap allocation is fine for
  // the non-JIT raw-memory callers (config/staging buffers etc.) —
  // emulated MEM1/MEM2 multi-VA aliasing goes through MemArena which
  // has its own Switch arm. aligned_alloc requires size to be a
  // multiple of alignment.
  size = (size + 0xFFF) & ~static_cast<size_t>(0xFFF);
  void* ptr = std::aligned_alloc(0x1000, size);
#else
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

  if (ptr == MAP_FAILED)
    ptr = nullptr;
#endif

  if (ptr == nullptr)
    PanicAlertFmt("Failed to allocate raw memory");

  return ptr;
}

void* AllocateAlignedMemory(size_t size, size_t alignment)
{
#ifdef _WIN32
  void* ptr = _aligned_malloc(size, alignment);
#else
  void* ptr = nullptr;
  if (posix_memalign(&ptr, alignment, size) != 0)
    ERROR_LOG_FMT(MEMMAP, "Failed to allocate aligned memory");
#endif

  if (ptr == nullptr)
    PanicAlertFmt("Failed to allocate aligned memory");

  return ptr;
}

bool FreeMemoryPages(void* ptr, size_t size)
{
  if (ptr)
  {
#ifdef _WIN32
    if (!VirtualFree(ptr, 0, MEM_RELEASE))
    {
      PanicAlertFmt("FreeMemoryPages failed!\nVirtualFree: {}", GetLastErrorString());
      return false;
    }
#elif defined(__SWITCH__)
    // JIT pointers go through jitClose. Non-JIT allocations from
    // AllocateMemoryPages still flow to munmap on Switch's newlib mmap.
    bool was_jit = false;
    {
      std::lock_guard<std::mutex> lock(SwitchJitMapMutex());
      auto it = SwitchJitMap().find(ptr);
      if (it != SwitchJitMap().end())
      {
        Result rc = jitClose(&it->second.handle);
        SwitchJitMap().erase(it);
        was_jit = true;
        if (R_FAILED(rc))
        {
          PanicAlertFmt("jitClose failed (rc={:#010x})", rc);
          return false;
        }
      }
    }
    if (!was_jit)
    {
      // Counterpart of AllocateMemoryPages's aligned_alloc.
      std::free(ptr);
    }
#else
    if (munmap(ptr, size) != 0)
    {
      PanicAlertFmt("FreeMemoryPages failed!\nmunmap: {}", LastStrerrorString());
      return false;
    }
#endif
  }
  return true;
}

void FreeAlignedMemory(void* ptr)
{
  if (ptr)
  {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
  }
}

bool ReadProtectMemory(void* ptr, size_t size)
{
#ifdef _WIN32
  DWORD oldValue;
  if (!VirtualProtect(ptr, size, PAGE_NOACCESS, &oldValue))
  {
    PanicAlertFmt("ReadProtectMemory failed!\nVirtualProtect: {}", GetLastErrorString());
    return false;
  }
#elif defined(__SWITCH__)
  // svcSetMemoryPermission is restricted on Horizon OS for unprivileged
  // code; treat memory protection toggles as best-effort no-ops. This
  // is the same approach Apple-Silicon takes for MAP_JIT pages.
#else
  if (mprotect(ptr, size, PROT_NONE) != 0)
  {
    PanicAlertFmt("ReadProtectMemory failed!\nmprotect: {}", LastStrerrorString());
    return false;
  }
#endif
  return true;
}

bool WriteProtectMemory(void* ptr, size_t size, bool allowExecute)
{
#ifdef _WIN32
  DWORD oldValue;
  if (!VirtualProtect(ptr, size, allowExecute ? PAGE_EXECUTE_READ : PAGE_READONLY, &oldValue))
  {
    PanicAlertFmt("WriteProtectMemory failed!\nVirtualProtect: {}", GetLastErrorString());
    return false;
  }
#elif defined(__SWITCH__)
  // see ReadProtectMemory — best-effort no-op.
#elif !(defined(_M_ARM_64) && defined(__APPLE__))
  // MacOS 11.2 on ARM does not allow for changing the access permissions of pages
  // that were marked executable, instead it uses the protections offered by MAP_JIT
  // for write protection.
  if (mprotect(ptr, size, allowExecute ? (PROT_READ | PROT_EXEC) : PROT_READ) != 0)
  {
    PanicAlertFmt("WriteProtectMemory failed!\nmprotect: {}", LastStrerrorString());
    return false;
  }
#endif
  return true;
}

bool UnWriteProtectMemory(void* ptr, size_t size, bool allowExecute)
{
#ifdef _WIN32
  DWORD oldValue;
  if (!VirtualProtect(ptr, size, allowExecute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &oldValue))
  {
    PanicAlertFmt("UnWriteProtectMemory failed!\nVirtualProtect: {}", GetLastErrorString());
    return false;
  }
#elif defined(__SWITCH__)
  // see ReadProtectMemory — best-effort no-op.
#elif !(defined(_M_ARM_64) && defined(__APPLE__))
  // MacOS 11.2 on ARM does not allow for changing the access permissions of pages
  // that were marked executable, instead it uses the protections offered by MAP_JIT
  // for write protection.
  if (mprotect(ptr, size,
               allowExecute ? (PROT_READ | PROT_WRITE | PROT_EXEC) : PROT_WRITE | PROT_READ) != 0)
  {
    PanicAlertFmt("UnWriteProtectMemory failed!\nmprotect: {}", LastStrerrorString());
    return false;
  }
#endif
  return true;
}

size_t MemPhysical()
{
#ifdef _WIN32
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  GlobalMemoryStatusEx(&memInfo);
  return memInfo.ullTotalPhys;
#elif defined __APPLE__ || defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__
  int mib[2];
  size_t physical_memory;
  mib[0] = CTL_HW;
#ifdef __APPLE__
  mib[1] = HW_MEMSIZE;
#elif defined __FreeBSD__
  mib[1] = HW_REALMEM;
#elif defined __OpenBSD__ || defined __NetBSD__
  mib[1] = HW_PHYSMEM64;
#endif
  size_t length = sizeof(size_t);
  sysctl(mib, 2, &physical_memory, &length, nullptr, 0);
  return physical_memory;
#elif defined __HAIKU__
  system_info sysinfo;
  get_system_info(&sysinfo);
  return static_cast<size_t>(sysinfo.max_pages * B_PAGE_SIZE);
#else
  struct sysinfo memInfo;
  sysinfo(&memInfo);
  return (size_t)memInfo.totalram * memInfo.mem_unit;
#endif
}

}  // namespace Common
