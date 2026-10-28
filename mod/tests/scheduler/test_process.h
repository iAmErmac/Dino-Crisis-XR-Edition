#ifndef DC_TEST_PROCESS_H
#define DC_TEST_PROCESS_H

#ifdef _MSC_VER
#include <crtdbg.h>
#include <stdlib.h>
#include <windows.h>
static void test_process_init(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
}
#else
static void test_process_init(void) {}
#endif

#endif
