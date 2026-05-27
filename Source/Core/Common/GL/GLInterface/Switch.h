// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <EGL/egl.h>

#include "Common/GL/GLContext.h"

// Wraps an externally-owned EGL display/surface/context created by the Switch
// frontend (SDL2 against libnx NWindow). Dolphin's OGL backend cannot create
// its own window surface on Horizon OS because there is only one default
// NWindow per process and the frontend already bound it via SDL2.
//
// The frontend passes the live handles through WindowSystemInfo:
//   display_connection -> EGLDisplay
//   render_surface     -> EGLSurface
//   render_window      -> EGLContext
//
// Initialize() adopts those handles, MakeCurrent()/Swap() route through them.
class GLContextSwitch final : public GLContext
{
public:
  ~GLContextSwitch() override;

  bool IsHeadless() const override;

  std::unique_ptr<GLContext> CreateSharedContext() override;

  bool MakeCurrent() override;
  bool ClearCurrent() override;

  void Swap() override;
  void SwapInterval(int interval) override;

  void* GetFuncAddress(const std::string& name) override;

protected:
  bool Initialize(const WindowSystemInfo& wsi, bool stereo, bool core) override;

private:
  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLSurface m_surface = EGL_NO_SURFACE;
  EGLContext m_context = EGL_NO_CONTEXT;
  EGLConfig m_config = nullptr;
  bool m_owns_context = false;
  bool m_owns_surface = false;
};
