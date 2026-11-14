// lock_graph.h: lock-order graph. Callers must hold State::mu.
#pragma once
#include "recorder.h"

namespace ls {

void lg_note_edges(ThreadState* t, LockInfo* L, Mode want, Todo& todo);
void lg_check(const Todo& todo, std::vector<Finding>& found);
void lg_suggest(const std::vector<int>& nodes, std::vector<int>& order, std::vector<Step>& violations);
std::vector<std::vector<int>> lg_components();

}  // namespace ls
