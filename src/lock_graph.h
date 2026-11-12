// lock_graph.h - the global lock-order graph (edge H->L: L requested while H held).
// OS concepts: deadlock detection on a resource graph, DFS cycle search, prevention by ordering.
// Every function here expects State::mu to be held by the caller.
#pragma once
#include "recorder.h"

namespace ls {

void lg_note_edges(ThreadState* t, LockInfo* L, Mode want, Todo& todo);
void lg_check(const Todo& todo, std::vector<Finding>& found);
void lg_suggest(const std::vector<int>& nodes, std::vector<int>& order, std::vector<Step>& violations);
std::vector<std::vector<int>> lg_components();

}  // namespace ls
