#pragma once

#include <cmath>

using std::isnan;

#ifdef __SWITCH__
// devkitA64's newlib lacks the BSD/GNU extension `timegm`. implot.cpp:920
// uses it for UTC-anchored time math. newlib's `mktime` defaults to the
// process timezone (UTC when none is set, which is the default on
// Horizon OS), so on the Switch path `timegm == mktime` is a faithful
// shim for implot's purposes.
#include <ctime>
inline ::time_t timegm(::tm* ptm) { return ::mktime(ptm); }
#endif
