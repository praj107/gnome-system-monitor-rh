#pragma once

#include <map>
#include <mutex>

#include "procinfo.h"

class ProcList
{
  // TODO: use a set instead
  // sorted by pid. The map has a nice property : it is sorted
  // by pid so this helps a lot when looking for the parent node
  // as ppid is nearly always < pid.
  typedef std::map<pid_t, ProcInfo> List;
  List data;
  std::mutex data_lock;
  public:
  // The last good CPU counter of each pid with the start time of the process
  // it belonged to, so a rebuilt ProcInfo inherits it only if it is the same
  // process; a reused pid starts over.
  struct CpuCounter
  {
    guint64 cpu_ticks;
    guint64 start_time;
  };
  std::map<pid_t, CpuCounter> cpu_times;
  typedef List::iterator Iterator;
  Iterator
  begin ()
  {
    return std::begin (data);
  }
  Iterator
  end ()
  {
    return std::end (data);
  }
  Iterator
  erase (Iterator it)
  {
    std::lock_guard<std::mutex> lg (data_lock);

    cpu_times.erase (it->first);
    return data.erase (it);
  }
  ProcInfo*
  add (pid_t pid)
  {
    return &data.try_emplace (pid, pid).first->second;
  }
  void
  clear ()
  {
    return data.clear ();
  }

  ProcInfo * find (pid_t pid);
};
