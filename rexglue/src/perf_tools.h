#pragma once

#include <rex/ui/overlay/debug_overlay.h>

// Guest frame rate measured at the game's present function (for the F3 overlay).
rex::ui::FrameStats GetGuestFrameStats();
// Starts the sampling profiler when trialshd_profile = true (Windows only).
void StartProfilerIfEnabled();
// Windows: 1 ms (or finer) timer resolution so short sleeps/waits are not rounded to 15.6 ms.
void SetHighTimerResolution();
