// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// MemArena port for Nintendo Switch (Horizon OS).
//
// Backs Dolphin's emulated MEM1 / MEM2 / Wii-VC heap via libnx virtmem +
// svcMapProcessMemory. MEM1 ends up multi-VA-aliased into the fastmem
// region the JIT will branch through.
//
// Adapted from xerpi/dolphin-switch's MemArenaSwitch.cpp with three
// fixes documented in docs/emulated-memory.md §xerpi-bugs:
//   1. ReserveMemoryRegion reserves the *destination* VA range (the
//      fastmem window), not the SHM source pool.
//   2. UnmapFromMemoryRegion erases m_maps, not m_views.
//   3. The process handle obtained via svcGetInfo(InfoType 65001) is
//      closed in the destructor.
//
// Single-instance assumption: Dolphin constructs one MemArena per
// emulation session. State lives in static file-scope variables so
// the Dolphin MemArena.h private layout (per-OS) does not need
// extending.

#include "Common/MemArena.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <switch.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

namespace Common
{
namespace
{
struct ViewInfo
{
  VirtmemReservation* resv = nullptr;
  s64 offset = 0;
  std::size_t size = 0;
};

struct MapInfo
{
  s64 offset = 0;
  std::size_t size = 0;
};

// Process handle returned by svcGetInfo(InfoType 65001) — required for
// svcMapProcessMemory / svcMapProcessCodeMemory. Acquired in
// GrabSHMSegment, closed in MemArena destructor (bug-fix #3).
Handle g_proc_handle = INVALID_HANDLE;

// "Physical" backing — heap-allocated 4 KiB-aligned region remapped into
// a code-memory virtual range so subsequent svcMapProcessMemory calls
// against the code-view succeed.
void* g_phys_alloc = nullptr;          // raw aligned_alloc result
void* g_code_view = nullptr;           // virtmem'd code-memory view of g_phys_alloc
std::size_t g_shm_size = 0;

// Reservation covering the fastmem window (ReserveMemoryRegion).
VirtmemReservation* g_region_resv = nullptr;
void* g_reserved_region = nullptr;
std::size_t g_reserved_region_size = 0;

// View tables. CreateView populates m_views; MapInMemoryRegion populates
// m_maps. Each is keyed by the VA pointer returned to the caller.
std::mutex g_mu;
std::unordered_map<void*, ViewInfo> g_views;
std::unordered_map<void*, MapInfo> g_maps;

Handle GetCurProcHandle()
{
  if (g_proc_handle != INVALID_HANDLE)
    return g_proc_handle;
  u64 value = 0;
  // InfoType 65001 = MesosphereCurrentProcess — Atmosphère / mesosphere
  // expose this so we can svcMapProcessMemory into our own address space.
  Result rc = svcGetInfo(&value, static_cast<InfoType>(65001), INVALID_HANDLE, 0);
  if (R_FAILED(rc))
  {
    ERROR_LOG_FMT(MEMMAP, "svcGetInfo(InfoType 65001) failed: rc={:#010x}", rc);
    return INVALID_HANDLE;
  }
  g_proc_handle = static_cast<Handle>(value);
  return g_proc_handle;
}
}  // namespace

MemArena::MemArena() = default;

MemArena::~MemArena()
{
  if (g_proc_handle != INVALID_HANDLE)
  {
    svcCloseHandle(g_proc_handle);
    g_proc_handle = INVALID_HANDLE;
  }
}

void MemArena::GrabSHMSegment(size_t size, std::string_view /*base_name*/)
{
  std::lock_guard<std::mutex> lock(g_mu);

  Handle proc = GetCurProcHandle();
  if (proc == INVALID_HANDLE)
  {
    PanicAlertFmt("MemArenaSwitch: GrabSHMSegment failed — no process handle.");
    return;
  }

  size = (size + 0xFFF) & ~static_cast<std::size_t>(0xFFF);
  g_shm_size = size;

  g_phys_alloc = std::aligned_alloc(0x1000, size);
  if (!g_phys_alloc)
  {
    PanicAlertFmt("MemArenaSwitch: aligned_alloc({} bytes) failed.", size);
    return;
  }

  virtmemLock();
  g_code_view = virtmemFindCodeMemory(size, 0x1000);
  virtmemUnlock();
  if (!g_code_view)
  {
    PanicAlertFmt("MemArenaSwitch: virtmemFindCodeMemory failed.");
    std::free(g_phys_alloc);
    g_phys_alloc = nullptr;
    return;
  }

  Result rc = svcMapProcessCodeMemory(proc, reinterpret_cast<u64>(g_code_view),
                                      reinterpret_cast<u64>(g_phys_alloc), size);
  if (R_FAILED(rc))
  {
    PanicAlertFmt("MemArenaSwitch: svcMapProcessCodeMemory failed: rc={:#010x}", rc);
    std::free(g_phys_alloc);
    g_phys_alloc = nullptr;
    g_code_view = nullptr;
    return;
  }

  rc = svcSetProcessMemoryPermission(proc, reinterpret_cast<u64>(g_code_view), size, Perm_Rw);
  if (R_FAILED(rc))
  {
    PanicAlertFmt("MemArenaSwitch: svcSetProcessMemoryPermission failed: rc={:#010x}", rc);
    return;
  }

  m_shm_fd = 1;  // sentinel — non-zero signals "have SHM"
}

void MemArena::ReleaseSHMSegment()
{
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_phys_alloc)
    return;

  if (g_proc_handle != INVALID_HANDLE && g_code_view)
  {
    svcUnmapProcessCodeMemory(g_proc_handle, reinterpret_cast<u64>(g_code_view),
                              reinterpret_cast<u64>(g_phys_alloc), g_shm_size);
  }
  std::free(g_phys_alloc);
  g_phys_alloc = nullptr;
  g_code_view = nullptr;
  g_shm_size = 0;
  m_shm_fd = 0;
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  std::lock_guard<std::mutex> lock(g_mu);
  Handle proc = GetCurProcHandle();
  if (proc == INVALID_HANDLE || !g_code_view)
    return nullptr;

  size = (size + 0xFFF) & ~static_cast<std::size_t>(0xFFF);

  virtmemLock();
  void* view = virtmemFindAslr(size, 0x1000);
  VirtmemReservation* resv = view ? virtmemAddReservation(view, size) : nullptr;
  virtmemUnlock();
  if (!view || !resv)
  {
    NOTICE_LOG_FMT(MEMMAP, "MemArenaSwitch: CreateView ASLR alloc failed.");
    return nullptr;
  }

  Result rc = svcMapProcessMemory(view, proc,
                                  reinterpret_cast<u64>(g_code_view) + offset, size);
  if (R_FAILED(rc))
  {
    NOTICE_LOG_FMT(MEMMAP,
                   "MemArenaSwitch: svcMapProcessMemory failed (view={:#x} src={:#x} size={:#x} rc={:#010x})",
                   reinterpret_cast<u64>(view),
                   reinterpret_cast<u64>(g_code_view) + offset, size, rc);
    virtmemLock();
    virtmemRemoveReservation(resv);
    virtmemUnlock();
    return nullptr;
  }

  g_views.emplace(view, ViewInfo{resv, offset, size});
  return view;
}

void MemArena::ReleaseView(void* view, size_t /*size*/)
{
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_views.find(view);
  if (it == g_views.end())
    return;

  if (g_proc_handle != INVALID_HANDLE)
  {
    svcUnmapProcessMemory(view, g_proc_handle,
                          reinterpret_cast<u64>(g_code_view) + it->second.offset,
                          it->second.size);
  }
  virtmemLock();
  virtmemRemoveReservation(it->second.resv);
  virtmemUnlock();
  g_views.erase(it);
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
  std::lock_guard<std::mutex> lock(g_mu);

  memory_size = (memory_size + 0xFFF) & ~static_cast<std::size_t>(0xFFF);

  virtmemLock();
  void* dst = virtmemFindAslr(memory_size, 0x1000);
  // Bug-fix #1: reserve the *destination* VA range, not the SHM source.
  g_region_resv = dst ? virtmemAddReservation(dst, memory_size) : nullptr;
  virtmemUnlock();
  if (!dst || !g_region_resv)
  {
    PanicAlertFmt("MemArenaSwitch: ReserveMemoryRegion failed (size={:#x})", memory_size);
    return nullptr;
  }

  g_reserved_region = dst;
  g_reserved_region_size = memory_size;
  return static_cast<u8*>(dst);
}

void MemArena::ReleaseMemoryRegion()
{
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_region_resv)
  {
    virtmemLock();
    virtmemRemoveReservation(g_region_resv);
    virtmemUnlock();
    g_region_resv = nullptr;
  }
  g_reserved_region = nullptr;
  g_reserved_region_size = 0;
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  std::lock_guard<std::mutex> lock(g_mu);
  Handle proc = GetCurProcHandle();
  if (proc == INVALID_HANDLE || !g_code_view)
    return nullptr;

  size = (size + 0xFFF) & ~static_cast<std::size_t>(0xFFF);

  Result rc = svcMapProcessMemory(base, proc,
                                  reinterpret_cast<u64>(g_code_view) + offset, size);
  if (R_FAILED(rc))
  {
    NOTICE_LOG_FMT(MEMMAP,
                   "MemArenaSwitch: MapInMemoryRegion svcMapProcessMemory failed: rc={:#010x}",
                   rc);
    return nullptr;
  }

  // Best-effort write-protection toggle. Failures are non-fatal.
  if (!writeable)
  {
    Result wp = svcSetProcessMemoryPermission(proc, reinterpret_cast<u64>(base),
                                              size, Perm_R);
    if (R_FAILED(wp))
      WARN_LOG_FMT(MEMMAP,
                   "MemArenaSwitch: svcSetProcessMemoryPermission(R) failed: rc={:#010x}", wp);
  }

  g_maps.emplace(base, MapInfo{offset, size});
  return base;
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  std::lock_guard<std::mutex> lock(g_mu);
  Handle proc = GetCurProcHandle();
  if (proc == INVALID_HANDLE)
    return false;
  size = (size + 0xFFF) & ~static_cast<std::size_t>(0xFFF);
  Result rc = svcSetProcessMemoryPermission(proc, reinterpret_cast<u64>(view), size,
                                            writeable ? Perm_Rw : Perm_R);
  return R_SUCCEEDED(rc);
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t /*size*/)
{
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_maps.find(view);
  if (it == g_maps.end())
    return;

  if (g_proc_handle != INVALID_HANDLE)
  {
    svcUnmapProcessMemory(view, g_proc_handle,
                          reinterpret_cast<u64>(g_code_view) + it->second.offset,
                          it->second.size);
  }
  // Bug-fix #2: erase from m_maps, not m_views.
  g_maps.erase(it);
}

size_t MemArena::GetPageSize() const
{
  return 0x1000;
}

// ----------------------------------------------------------------------------
// LazyMemoryRegion — flat anonymous mapping (no aliasing, no SHM). Used for
// shader caches and similar bulk buffers. aligned_alloc is the simplest
// backing on Switch; the kernel demand-pages on access.
// ----------------------------------------------------------------------------

LazyMemoryRegion::LazyMemoryRegion() = default;

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

void* LazyMemoryRegion::Create(size_t size)
{
  if (m_memory)
    return nullptr;
  if (size == 0)
    return nullptr;
  size = (size + 0xFFF) & ~static_cast<std::size_t>(0xFFF);
  m_memory = std::aligned_alloc(0x1000, size);
  if (!m_memory)
  {
    NOTICE_LOG_FMT(MEMMAP, "LazyMemoryRegion::Create({} bytes) failed.", size);
    return nullptr;
  }
  m_size = size;
  return m_memory;
}

void LazyMemoryRegion::Clear()
{
  if (m_memory)
    std::memset(m_memory, 0, m_size);
}

void LazyMemoryRegion::Release()
{
  if (m_memory)
  {
    std::free(m_memory);
    m_memory = nullptr;
    m_size = 0;
  }
}

}  // namespace Common
