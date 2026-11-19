#pragma once

// Headless diagnostics, runnable over SSH while the launcher UI is idle:
//   --radio-ping [n]   send n numbered packets (1/s), print echoes
//   --radio-listen     print every packet with RSSI/SNR, echo pings back
//   --mic-test [s]     print the mic level every 100 ms
//   --codec-loop [s]   mic -> codec2 -> speaker on the same device
//   --bench-codec      codec2 encode+decode CPU cost

#include <csignal>

namespace cpzbm {

// Returns -1 if argv is not a debug mode, else the process exit code.
int runDebugMode(int argc, char** argv, const volatile std::sig_atomic_t* stop_flag);

}  // namespace cpzbm
