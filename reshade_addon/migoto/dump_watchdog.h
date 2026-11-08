#pragma once
// ===========================================================================
//  dump_watchdog.h -- OURS. PORT-LEAF. Where the frame-analysis dump is, and a thread that says so
//  in the log when it stops moving (a hung capture can be read from the log, no debugger attached).
// ===========================================================================
namespace MigotoPort {

void DumpStage(const char* stage, unsigned long long detail = 0);
const char* DumpStageName(); // the last DumpStage, for a failure report
unsigned long long DumpStageDetail();
// Started with the capture, stopped at its end. Logs "DUMP WATCHDOG" every 5 s of no stage change.
void DumpWatchdogStart();
void DumpWatchdogStop();
void DumpLogFrames(const char* tag, void* const* frames, unsigned n);
void LogThreadStack(unsigned long tid, const char* tag);

}
