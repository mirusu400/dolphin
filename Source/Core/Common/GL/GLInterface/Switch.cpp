// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLInterface/Switch.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Common/Logging/Log.h"

// Swap-pipeline heartbeat exposed to the Switch frontend so the main loop
// can detect "no frame swapped for N seconds" — the leading hypothesis for
// the late-boot freeze. Updated on every eglSwapBuffers regardless of
// success so a stalled compositor still increments the timestamp and a
// stalled video thread shows up as a frozen counter.
namespace
{
std::atomic<std::uint64_t> g_swap_count{0};
std::atomic<std::uint64_t> g_last_swap_us{0};

std::uint64_t SteadyNowUs()
{
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
}  // namespace

extern "C" void DolphinSwitchGetSwapStats(std::uint64_t* out_count,
                                          std::uint64_t* out_last_us)
{
  if (out_count)
    *out_count = g_swap_count.load(std::memory_order_relaxed);
  if (out_last_us)
    *out_last_us = g_last_swap_us.load(std::memory_order_relaxed);
}

GLContextSwitch::~GLContextSwitch()
{
  // Release the EGL binding from whichever thread is running this destructor.
  // Dolphin creates a throwaway GLContext during PopulateBackendInfo to probe
  // backend caps, then drops it. Without this ClearCurrent the SDL context
  // stays bound to the probe thread, so the real Initialize() on the Emu
  // thread later trips EGL_BAD_ACCESS. eglMakeCurrent(NO_*) on a thread that
  // does not hold the binding is a harmless no-op, so we always call it.
  if (m_display != EGL_NO_DISPLAY)
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (m_owns_surface && m_surface != EGL_NO_SURFACE && m_display != EGL_NO_DISPLAY)
    eglDestroySurface(m_display, m_surface);
  if (m_owns_context && m_context != EGL_NO_CONTEXT && m_display != EGL_NO_DISPLAY)
    eglDestroyContext(m_display, m_context);
}

bool GLContextSwitch::IsHeadless() const
{
  return false;
}

bool GLContextSwitch::Initialize(const WindowSystemInfo& wsi, bool stereo, bool core)
{
  m_display = static_cast<EGLDisplay>(wsi.display_connection);
  m_surface = static_cast<EGLSurface>(wsi.render_surface);
  m_context = static_cast<EGLContext>(wsi.render_window);

  if (m_display == EGL_NO_DISPLAY || m_surface == EGL_NO_SURFACE ||
      m_context == EGL_NO_CONTEXT)
  {
    ERROR_LOG_FMT(VIDEO, "GLContextSwitch: frontend did not pass valid EGL handles");
    return false;
  }

  m_opengl_mode = Mode::OpenGLES;
  m_owns_context = false;

  EGLint config_id = 0;
  if (!eglQueryContext(m_display, m_context, EGL_CONFIG_ID, &config_id))
  {
    ERROR_LOG_FMT(VIDEO, "GLContextSwitch: eglQueryContext(EGL_CONFIG_ID) failed: {:#06x}",
                  eglGetError());
    return false;
  }

  EGLint num_configs = 0;
  if (!eglGetConfigs(m_display, nullptr, 0, &num_configs) || num_configs <= 0)
  {
    ERROR_LOG_FMT(VIDEO, "GLContextSwitch: eglGetConfigs(count) failed: {:#06x}", eglGetError());
    return false;
  }

  std::vector<EGLConfig> configs(static_cast<size_t>(num_configs));
  if (!eglGetConfigs(m_display, configs.data(), num_configs, &num_configs))
  {
    ERROR_LOG_FMT(VIDEO, "GLContextSwitch: eglGetConfigs(list) failed: {:#06x}", eglGetError());
    return false;
  }

  for (EGLint i = 0; i < num_configs; ++i)
  {
    EGLint id = 0;
    if (eglGetConfigAttrib(m_display, configs[i], EGL_CONFIG_ID, &id) && id == config_id)
    {
      m_config = configs[i];
      break;
    }
  }
  if (!m_config)
  {
    ERROR_LOG_FMT(VIDEO, "GLContextSwitch: could not match EGLConfig id {}", config_id);
    return false;
  }

  if (!MakeCurrent())
  {
    ERROR_LOG_FMT(VIDEO, "GLContextSwitch: initial MakeCurrent failed: {:#06x}", eglGetError());
    return false;
  }

  EGLint w = 0, h = 0;
  eglQuerySurface(m_display, m_surface, EGL_WIDTH, &w);
  eglQuerySurface(m_display, m_surface, EGL_HEIGHT, &h);
  m_backbuffer_width = static_cast<u32>(w);
  m_backbuffer_height = static_cast<u32>(h);
  return true;
}

std::unique_ptr<GLContext> GLContextSwitch::CreateSharedContext()
{
  // Async shader workers must NOT share the main NWindow surface: EGL only
  // allows one current binding per surface, so making the worker context
  // current on the main surface would steal it from the video thread and
  // return EGL_BAD_ACCESS.
  //
  // Mesa nouveau on Switch picks a config with EGL_SURFACE_TYPE=EGL_WINDOW_BIT
  // only — pbuffer surfaces fail with EGL_BAD_MATCH (0x3009). Prefer
  // EGL_KHR_surfaceless_context (Mesa supports it on GLES3); fall back to a
  // 1x1 pbuffer only if surfaceless is unavailable.
  const char* exts = eglQueryString(m_display, EGL_EXTENSIONS);
  const bool surfaceless =
      exts && std::string(exts).find("EGL_KHR_surfaceless_context") != std::string::npos;

  EGLSurface shared_surface = EGL_NO_SURFACE;
  bool owns_surface = false;
  if (!surfaceless)
  {
    const EGLint pbuf_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    shared_surface = eglCreatePbufferSurface(m_display, m_config, pbuf_attribs);
    if (shared_surface == EGL_NO_SURFACE)
    {
      ERROR_LOG_FMT(VIDEO,
                    "GLContextSwitch: no surfaceless ext and "
                    "eglCreatePbufferSurface failed: {:#06x}",
                    eglGetError());
      return nullptr;
    }
    owns_surface = true;
  }

  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext shared = eglCreateContext(m_display, m_config, m_context, ctx_attribs);
  if (shared == EGL_NO_CONTEXT)
  {
    ERROR_LOG_FMT(VIDEO, "GLContextSwitch: eglCreateContext(shared) failed: {:#06x}",
                  eglGetError());
    if (owns_surface)
      eglDestroySurface(m_display, shared_surface);
    return nullptr;
  }

  INFO_LOG_FMT(VIDEO, "GLContextSwitch::CreateSharedContext ok (surfaceless={})", surfaceless);

  auto out = std::make_unique<GLContextSwitch>();
  out->m_display = m_display;
  out->m_surface = shared_surface;
  out->m_context = shared;
  out->m_config = m_config;
  out->m_owns_context = true;
  out->m_owns_surface = owns_surface;
  out->m_opengl_mode = m_opengl_mode;
  out->m_is_shared = true;
  return out;
}

bool GLContextSwitch::MakeCurrent()
{
  INFO_LOG_FMT(VIDEO,
               "GLContextSwitch::MakeCurrent disp={} surf={} ctx={} "
               "(was current: ctx={} surf={})",
               fmt::ptr(m_display), fmt::ptr(m_surface), fmt::ptr(m_context),
               fmt::ptr(eglGetCurrentContext()),
               fmt::ptr(eglGetCurrentSurface(EGL_DRAW)));
  const EGLBoolean ok =
      eglMakeCurrent(m_display, m_surface, m_surface, m_context);
  if (!ok)
  {
    ERROR_LOG_FMT(VIDEO, "GLContextSwitch::MakeCurrent eglMakeCurrent failed: {:#06x}",
                  eglGetError());
  }
  return ok == EGL_TRUE;
}

bool GLContextSwitch::ClearCurrent()
{
  return eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
}

void GLContextSwitch::Swap()
{
  static unsigned counter = 0;
  ++counter;
  const EGLBoolean ok = eglSwapBuffers(m_display, m_surface);
  g_swap_count.fetch_add(1, std::memory_order_relaxed);
  g_last_swap_us.store(SteadyNowUs(), std::memory_order_relaxed);
  if (counter <= 5 || (counter % 60) == 0)
  {
    INFO_LOG_FMT(VIDEO, "GLContextSwitch::Swap #{} ok={} err={:#06x} surf={}",
                 counter, ok, ok ? 0 : eglGetError(), fmt::ptr(m_surface));
  }
}

void GLContextSwitch::SwapInterval(int interval)
{
  eglSwapInterval(m_display, interval);
}

void* GLContextSwitch::GetFuncAddress(const std::string& name)
{
  return reinterpret_cast<void*>(eglGetProcAddress(name.c_str()));
}
