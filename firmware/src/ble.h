#pragma once
#include "core.h"

bool bleBegin();
// Main thread only; swaps current/staging only after full validation.
bool blePoll(meter::Snapshot*& current, meter::Snapshot*& staging, uint64_t& receivedUs);
bool bleConnected();
