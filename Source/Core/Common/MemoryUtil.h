// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Common
{
void* AllocateExecutableMemory(size_t size);

// Returns the executable alias of a writable JIT pointer. On platforms
// with unified RWX pages this is the identity function. On Horizon OS
// the writable view (returned by AllocateExecutableMemory) and the
// executable view live at different virtual addresses, so any caller
// that hands a JIT pointer to host CPU control flow (e.g. casting it to
// a function pointer, or BR-ing to it from generated code) must
// translate first. Pointers not owned by an active JIT allocation are
// returned unchanged.
void* JITWriteToExecAddress(void* rw_ptr);

// Returns rx_alias - rw_alias for the JIT allocation containing the
// given writable pointer, in bytes (may be negative). Returns 0 on
// platforms with unified RWX pages, or if the pointer is not owned by
// an active JIT allocation. Useful when codegen needs to embed the
// delta as an immediate operand so a single dispatcher path can patch
// rw addresses into rx targets at runtime.
std::intptr_t JITRxRwOffset(void* rw_ptr);

// These two functions control the executable/writable state of the W^X memory
// allocations. More detailed documentation about them is in the .cpp file.
// In general where applicable the ScopedJITPageWriteAndNoExecute wrapper
// should be used to prevent bugs from not pairing up the calls properly.

// Allows a thread to write to executable memory, but not execute the data.
void JITPageWriteEnableExecuteDisable();
// Allows a thread to execute memory allocated for execution, but not write to it.
void JITPageWriteDisableExecuteEnable();
// RAII Wrapper around JITPageWrite*Execute*(). When this is in scope the thread can
// write to executable memory but not execute it.
struct ScopedJITPageWriteAndNoExecute
{
  ScopedJITPageWriteAndNoExecute() { JITPageWriteEnableExecuteDisable(); }
  ~ScopedJITPageWriteAndNoExecute() { JITPageWriteDisableExecuteEnable(); }
};
void* AllocateMemoryPages(size_t size);
bool FreeMemoryPages(void* ptr, size_t size);
void* AllocateAlignedMemory(size_t size, size_t alignment);
void FreeAlignedMemory(void* ptr);
bool ReadProtectMemory(void* ptr, size_t size);
bool WriteProtectMemory(void* ptr, size_t size, bool executable = false);
bool UnWriteProtectMemory(void* ptr, size_t size, bool allowExecute = false);
size_t MemPhysical();

}  // namespace Common
