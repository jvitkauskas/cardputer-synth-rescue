// The SD card: mounted whenever one is needed, so it may go in at any time.
#pragma once
#include <SD.h>

extern bool sd_ok;
// Mounts the card (again).
bool sd_mount();
// A mounted card that still answers, or one mounted now.
bool sd_ready();
