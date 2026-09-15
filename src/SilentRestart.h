#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)

// Restart after USB mass-storage handoff. On X4 Pro this first returns
// the shared USB PHY from OTG/MSC to the hardware Serial/JTAG block.
void restartToHomeAfterStorageHandoff();
