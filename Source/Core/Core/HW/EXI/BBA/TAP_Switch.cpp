// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// TAPNetworkInterface stub for Horizon OS — libnx has no /dev/net/tun and
// no SLAAC user-mode TAP. Provide vtable + method bodies so EXI_DeviceEthernet
// links cleanly. Real Wii BBA pass-through on Switch would need libnx
// nifm + raw sockets, which is M5+ territory.

#include "Core/HW/EXI/EXI_DeviceEthernet.h"

namespace ExpansionInterface
{
bool CEXIETHERNET::TAPNetworkInterface::Activate()
{
  return false;
}

void CEXIETHERNET::TAPNetworkInterface::Deactivate() {}

bool CEXIETHERNET::TAPNetworkInterface::IsActivated()
{
  return false;
}

bool CEXIETHERNET::TAPNetworkInterface::SendFrame(const u8*, u32)
{
  return false;
}

bool CEXIETHERNET::TAPNetworkInterface::RecvInit()
{
  return false;
}

void CEXIETHERNET::TAPNetworkInterface::RecvStart() {}

void CEXIETHERNET::TAPNetworkInterface::RecvStop() {}

// ReadThreadHandler is only declared inside the multi-OS gate
// (Win32/Linux/Apple/FreeBSD/OpenBSD) — Switch is not in that gate, so
// no static-member definition is needed here.
}  // namespace ExpansionInterface
