/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "gtid_renumberer.h"

namespace binlog_server {
namespace rewrite {

void reset(LogicalClockState &s) { s.last_local_sequence_number = 0; }

RewriteResult fix_logical_clock(LogicalClockState &, unsigned char,
                                const char *, unsigned long, bool,
                                std::vector<unsigned char> &) {
  // NOT YET IMPLEMENTED: When rewrite mode coalesces multiple source binlog
  // segments into a single local file, sequence_number and last_committed
  // must be renumbered so the parallel applier's dependency tracking remains
  // correct. GTID identifiers (uuid:gno) are never modified -- cross-file
  // continuity comes from synthesized PREVIOUS_GTIDS_LOG events built from
  // the accumulated GTID set.
  return RewriteResult{nullptr, 0, false};
}

}  // namespace rewrite
}  // namespace binlog_server
