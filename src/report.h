// report.h: printing findings and writing the final reports.
#pragma once
#include "recorder.h"

namespace ls {

void report_emit(Finding& f);   // format + print one finding now, then store it (call without State::mu)
void report_final();            // end-of-run summary and report files

}  // namespace ls
