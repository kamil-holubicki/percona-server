/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_GTID_RENUMBERER_H
#define BINLOG_SERVER_GTID_RENUMBERER_H

#include <cstdint>
#include <vector>

namespace binlog_server {
namespace rewrite {

/// Per-channel logical-clock rewriting state.
///
/// When archive rewrite mode is active (rewrite_file_size > 0), multiple
/// source binlog segments may be coalesced into a single local file. MySQL
/// resets sequence_number to 1 at each source rotation, which would produce
/// non-monotone values in a merged file. The rewriter fixes sequence_number
/// and last_committed fields so the parallel applier dependency tracking
/// remains correct.
///
/// IMPORTANT: GTID identifiers (uuid:gno) are NEVER modified. Cross-file
/// GTID continuity is provided by synthesizing PREVIOUS_GTIDS_LOG events
/// from the accumulated GTID set at each local file boundary.
struct LogicalClockState {
  std::uint64_t last_local_sequence_number{0};
};

/// Reset logical clock state (called on local file rotation).
void reset(LogicalClockState &s);

/// Result of a rewrite attempt on a GTID event.
struct RewriteResult {
  const char *buf{nullptr};
  unsigned long len{0};
  bool rewritten{false};
};

/// NOT YET IMPLEMENTED: Fix sequence_number and last_committed in a GTID
/// event for coalesced archive files. Currently returns {nullptr, 0, false}.
RewriteResult fix_logical_clock(LogicalClockState &s,
                                unsigned char event_type,
                                const char *event_buf, unsigned long event_len,
                                bool has_checksum,
                                std::vector<unsigned char> &scratch);

}  // namespace rewrite
}  // namespace binlog_server

#endif /* BINLOG_SERVER_GTID_RENUMBERER_H */
