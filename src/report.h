// report.h - turns findings into text (stderr), and writes the final summary, JSON, DOT and HTML.
// OS concepts: the four Coffman conditions are printed as evidence for every deadlock report.
#pragma once
#include "recorder.h"

namespace ls {

void report_emit(Finding& f);   // format + print one finding now, then store it (call without State::mu)
void report_final();            // end-of-run summary and report files

}  // namespace ls
