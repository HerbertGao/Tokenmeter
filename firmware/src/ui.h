#pragma once
#include "core.h"
void uiBegin();
void uiUpdate(const meter::Snapshot* snapshot, uint64_t elapsedSeconds, bool connected);
void uiTouch(bool pressed, int x, int y);
bool uiPoll();
void uiCancelTouch();
void uiHome();
