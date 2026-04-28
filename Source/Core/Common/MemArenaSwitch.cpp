// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// MemArena port for Nintendo Switch (Horizon OS).
//
// M2.5 STUB. devkitA64's newlib has no `sys/mman.h` and `shm_open` is
// unavailable; the Unix arena path cannot be reused. The full Switch
// implementation will use libnx `virtmemFindAslr` +
// `svcMapProcessMemory` to back MEM1/MEM2 multi-VA aliasing — see
// `docs/emulated-memory.md` for the design and the three xerpi bugs
// to NOT replicate. That work is M2.5 hardware; this file only gives
// the linker the symbols it expects so M1 can produce an NRO.
//
// Every public method either returns nullptr/false (for view/region
// creators) or is a no-op (for releasers). At runtime Dolphin will
// PanicAlertFmt as soon as it reaches the first emulated-memory
// access — the expected M1 outcome ("links but doesn't run").

#include "Common/MemArena.h"

#include <cstddef>

#include <switch.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

namespace Common
{
namespace
{
// Logged once per uncovered method so a runtime hit is loud but not spammy.
template <const char* Name>
void StubOnce()
{
  static bool warned = false;
  if (!warned)
  {
    warned = true;
    ERROR_LOG_FMT(MEMMAP, "MemArena::{} is a Switch M2.5 stub — emulated memory not yet wired.",
                  Name);
  }
}

constexpr char kGrabName[] = "GrabSHMSegment";
constexpr char kCreateView[] = "CreateView";
constexpr char kReserveRegion[] = "ReserveMemoryRegion";
constexpr char kMapIn[] = "MapInMemoryRegion";
constexpr char kChangeProt[] = "ChangeMappingProtection";
constexpr char kLazyCreate[] = "LazyMemoryRegion::Create";
}  // namespace

MemArena::MemArena() = default;
MemArena::~MemArena() = default;

void MemArena::GrabSHMSegment(size_t size, std::string_view base_name)
{
  StubOnce<kGrabName>();
  m_shm_fd = -1;
}

void MemArena::ReleaseSHMSegment()
{
  m_shm_fd = 0;
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  StubOnce<kCreateView>();
  return nullptr;
}

void MemArena::ReleaseView(void* view, size_t size)
{
  // no-op
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
  StubOnce<kReserveRegion>();
  m_reserved_region = nullptr;
  m_reserved_region_size = 0;
  return nullptr;
}

void MemArena::ReleaseMemoryRegion()
{
  m_reserved_region = nullptr;
  m_reserved_region_size = 0;
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  StubOnce<kMapIn>();
  return nullptr;
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  StubOnce<kChangeProt>();
  return false;
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size)
{
  // no-op
}

size_t MemArena::GetPageSize() const
{
  // 4 KiB is the Horizon page granule for `jit_t` and matches the libnx
  // `virtmemFindCodeMemory(0x1000)` alignment. The 2 MiB heap granule
  // is for `svcSetHeapSize` only and does not apply here.
  return 0x1000;
}

LazyMemoryRegion::LazyMemoryRegion() = default;

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

void* LazyMemoryRegion::Create(size_t size)
{
  StubOnce<kLazyCreate>();
  m_memory = nullptr;
  m_size = 0;
  return nullptr;
}

void LazyMemoryRegion::Clear()
{
  // no-op
}

void LazyMemoryRegion::Release()
{
  m_memory = nullptr;
  m_size = 0;
}

}  // namespace Common
