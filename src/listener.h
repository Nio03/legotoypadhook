#pragma once
#include <cstdint>
#include "locator.h"

// Starts the loopback TCP listener on a background thread. Safe to call once.
// `log` may be null. Returns false if the symbols are incomplete.
bool listener_start(const ToypadSymbols& symbols, void (*log)(const char*), uint16_t port);
void listener_stop();
