#pragma once

// What a stack for overflow reports costs a thread, and which threads have one.
namespace eacp::CrashReporting::Tests
{
int runThreadCreationBench();
int runThreadMemoryMeasurement();
int runThreadStackCheck();
int overflowANewStdThread();
int runSmallStackThreadCheck();
} // namespace eacp::CrashReporting::Tests
