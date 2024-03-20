#include "config.h"
#include <assert.h>
#include "common.h"

static constexpr char VAR_TELEMETRY_ROOT_DIR[] = "telemetry_root_dir";
static constexpr char VAR_SCRAPE_INTERVAL[] = "scrape_interval";
static constexpr char VAR_GRACE_INTERVAL[] = "grace_interval";
static constexpr char VAR_HISTORY_KEEP_INTERVAL[] = "history_keep_interval";

static constexpr char TELEMETRY_ROOT_DIR_DEFAULT[] =
    "/usr/local/percona/telemetry/ps";

static constexpr int SCRAPE_INTERVAL_DEFAULT = 60 * 60 * 24 * 1;  // 1 day
static constexpr int SCRAPE_INTERVAL_MIN = 10;
static constexpr int SCRAPE_INTERVAL_MAX = 60 * 60 * 24 * 7;  // 1 week

static constexpr int GRACE_INTERVAL_DEFAULT = 60 * 60 * 24 * 1;  // 1 day
static constexpr int GRACE_INTERVAL_MIN = 20;
static constexpr int GRACE_INTERVAL_MAX = 60 * 60 * 24 * 2;  // 2 days

static constexpr int HISTORY_KEEP_INTERVAL_DEFAULT =
    60 * 60 * 24 * 7;  // 7 days
static constexpr int HISTORY_KEEP_INTERVAL_MIN = 60;
static constexpr int HISTORY_KEEP_INTERVAL_MAX = HISTORY_KEEP_INTERVAL_DEFAULT;

Config::Config(SERVICE_TYPE(component_sys_variable_register) &
                   var_register_service,
               SERVICE_TYPE(component_sys_variable_unregister) &
                   var_unregister_service)
    : var_register_service_(var_register_service),
      var_unregister_service_(var_unregister_service),
      telemetry_root_dir_value_(nullptr) {}

bool Config::init() {
  STR_CHECK_ARG(str) telemetry_root_dir_arg;
  telemetry_root_dir_arg.def_val =
      const_cast<char *>(TELEMETRY_ROOT_DIR_DEFAULT);
  if (var_register_service_.register_variable(
          CURRENT_COMPONENT_NAME_STR, VAR_TELEMETRY_ROOT_DIR,
          PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY,
          "Root path of the telemetry data for all mysqld servers", nullptr,
          nullptr, &telemetry_root_dir_arg, &telemetry_root_dir_value_)) {
    return true;
  }

  INTEGRAL_CHECK_ARG(uint)
  scrape_interval_arg, grace_interval_arg, history_keep_interval_arg;
  scrape_interval_arg.def_val = SCRAPE_INTERVAL_DEFAULT;
  scrape_interval_arg.min_val = SCRAPE_INTERVAL_MIN;
  scrape_interval_arg.max_val = SCRAPE_INTERVAL_MAX;
  scrape_interval_arg.blk_sz = 0;
  if (var_register_service_.register_variable(
          CURRENT_COMPONENT_NAME_STR, VAR_SCRAPE_INTERVAL,
          PLUGIN_VAR_INT | PLUGIN_VAR_READONLY, "Telemetry scrape interval",
          nullptr, nullptr, &scrape_interval_arg, &scrape_interval_value_)) {
    return true;
  }

  grace_interval_arg.def_val = GRACE_INTERVAL_DEFAULT;
  grace_interval_arg.min_val = GRACE_INTERVAL_MIN;
  grace_interval_arg.max_val = GRACE_INTERVAL_MAX;
  grace_interval_arg.blk_sz = 0;
  if (var_register_service_.register_variable(
          CURRENT_COMPONENT_NAME_STR, VAR_GRACE_INTERVAL,
          PLUGIN_VAR_INT | PLUGIN_VAR_READONLY, "Telemetry grace interval",
          nullptr, nullptr, &grace_interval_arg, &grace_interval_value_)) {
    return true;
  }

  history_keep_interval_arg.def_val = HISTORY_KEEP_INTERVAL_DEFAULT;
  history_keep_interval_arg.min_val = HISTORY_KEEP_INTERVAL_MIN;
  history_keep_interval_arg.max_val = HISTORY_KEEP_INTERVAL_MAX;
  history_keep_interval_arg.blk_sz = 0;
  if (var_register_service_.register_variable(
          CURRENT_COMPONENT_NAME_STR, VAR_HISTORY_KEEP_INTERVAL,
          PLUGIN_VAR_INT | PLUGIN_VAR_READONLY,
          "Telemetry history keep interval", nullptr, nullptr,
          &history_keep_interval_arg, &history_keep_interval_value_)) {
    return true;
  }

  return false;
}

bool Config::deinit() {
  bool res = false;
  if (var_unregister_service_.unregister_variable(CURRENT_COMPONENT_NAME_STR,
                                                  VAR_TELEMETRY_ROOT_DIR)) {
    res = true;
  }
  if (var_unregister_service_.unregister_variable(CURRENT_COMPONENT_NAME_STR,
                                                  VAR_SCRAPE_INTERVAL)) {
    res = true;
  }
  if (var_unregister_service_.unregister_variable(CURRENT_COMPONENT_NAME_STR,
                                                  VAR_GRACE_INTERVAL)) {
    res = true;
  }
  if (var_unregister_service_.unregister_variable(CURRENT_COMPONENT_NAME_STR,
                                                  VAR_HISTORY_KEEP_INTERVAL)) {
    res = true;
  }

  return res;
}

const std::string &Config::telemetry_storage_dir_path() const noexcept {
  assert(telemetry_root_dir_value_);
  // it is read-only value
  static std::string telemetry_root_dir_value_str(telemetry_root_dir_value_);
  return telemetry_root_dir_value_str;
}

int Config::scrape_interval() const noexcept { return scrape_interval_value_; }

int Config::grace_interval() const noexcept { return grace_interval_value_; }

int Config::history_keep_interval() const noexcept {
  return history_keep_interval_value_;
}

int Config::unconditional_history_cleanup_interval() const noexcept {
  return HISTORY_KEEP_INTERVAL_MAX;
}
