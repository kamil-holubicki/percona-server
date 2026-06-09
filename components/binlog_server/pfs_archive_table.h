/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef COMPONENTS_BINLOG_SERVER_PFS_ARCHIVE_TABLE_H
#define COMPONENTS_BINLOG_SERVER_PFS_ARCHIVE_TABLE_H

namespace binlog_server {

class BinlogArchive;

bool register_pfs_archive_table(BinlogArchive *storage);
bool unregister_pfs_archive_table();

}  // namespace binlog_server

#endif
