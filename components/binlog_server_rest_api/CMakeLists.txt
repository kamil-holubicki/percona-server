MYSQL_ADD_COMPONENT(binlog_server_rest_api
  binlog_server_rest_api.cc
  rest_handlers.cc
  sql_executor.cc
  MODULE_ONLY
  LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto ${CMAKE_DL_LIBS}
)

TARGET_INCLUDE_DIRECTORIES(component_binlog_server_rest_api PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Install the dashboard HTML alongside the component .so
INSTALL(FILES dashboard/index.html
  DESTINATION ${INSTALL_PLUGINDIR}
  RENAME binlog_server_rest_api_dashboard.html
)

# Copy dashboard HTML next to the .so for both build output locations
ADD_CUSTOM_COMMAND(TARGET component_binlog_server_rest_api POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/dashboard/index.html
    ${CMAKE_BINARY_DIR}/plugin_output_directory/binlog_server_rest_api_dashboard.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/dashboard/index.html
    ${CMAKE_BINARY_DIR}/lib/plugin/binlog_server_rest_api_dashboard.html
  COMMENT "Copying dashboard HTML to plugin directories"
)
