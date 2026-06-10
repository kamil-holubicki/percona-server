/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_REST_API_HANDLERS_H
#define BINLOG_SERVER_REST_API_HANDLERS_H

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

namespace rest_api {

void register_handlers(httplib::Server &svr,
                       const std::string &plugin_dir);

}  // namespace rest_api

#endif
