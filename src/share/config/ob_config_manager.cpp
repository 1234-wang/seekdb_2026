/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SHARE


#include "ob_config_manager.h"
#include "share/ob_sql_client_decorator.h"
#include "share/config/ob_config_rpc_types.h"
#include "config_bridge.h"
#include "local_bridge.h"

namespace oceanbase
{
namespace obcall
{

OB_SERIALIZE_MEMBER(ObAdminSetConfigItem, name_, value_, comment_, is_reset_);

} // namespace obcall

namespace common
{
namespace
{
int report_error(const char *operation, config::Status &error)
{
  int ret = OB_INVALID_CONFIG;
  LOG_ERROR("config operation failed", K(operation), "detail", error.message.c_str(),
            "commit_uncertain", error.commit_uncertain);
  if (error.commit_uncertain != 0) {
    LOG_USER_ERROR(OB_INVALID_CONFIG,
                   "configuration commit outcome is uncertain; restart to recover storage");
  } else {
    LOG_USER_ERROR(OB_INVALID_CONFIG, error.message.c_str());
  }
  return ret;
}

}

ObConfigManager::~ObConfigManager()
{
}

int ObConfigManager::init()
{
  int ret = OB_SUCCESS;
  if (!local::is_open()) {
    ret = OB_NOT_INIT;
  } else {
    inited_ = true;
  }
  return ret;
}

int ObConfigManager::reload_config()
{
  return reload_config_func_();
}

int ObConfigManager::update_local()
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
  } else {
    auto error = config::load_active(!enable_static_effect_);
    if (!error.ok) {
      ret = report_error("load active", error);
    } else {
      LOG_INFO("read config success");
    }
  }
  return ret;
}

int ObConfigManager::got_version()
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
  } else {
    if (OB_FAIL(update_local())) {
    } else {
      LOG_INFO("loaded new config synchronously");
    }
  }
  return ret;
}

int ObConfigManager::save_internal_state(const char *name, const char *value)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(name) || OB_ISNULL(value)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!inited_) {
    ret = OB_NOT_INIT;
  } else {
    auto error = config::update_internal_state(name, value);
    if (!error.ok) {
      ret = report_error("save internal state", error);
    }
  }
  return ret;
}

int ObConfigManager::update_checked(const char *name, const char *value, bool reset,
                                    const config::CheckCallback &callback)
{
  int ret = OB_SUCCESS;
  if (nullptr == name) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!config::parameter_exists(rust::Str(name))) {
    ret = OB_ERR_SYS_CONFIG_UNKNOWN;
  } else if (!inited_) {
    ret = OB_NOT_INIT;
  } else if (!reset && nullptr == value) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    auto error = config::update_checked(name, reset ? "" : value, reset, callback);
    if (!error.ok) {
      ret = report_error(reset ? "checked reset" : "checked save", error);
    }
  }
  return ret;
}

int ObConfigManager::save_configs()
{
  int ret = OB_SUCCESS;
  auto error = config::save_bootstrap();
  if (!error.ok) {
    ret = OB_INVALID_CONFIG;
    LOG_ERROR("failed to save startup parameters", K(ret), "detail", error.message.c_str(),
              "commit_uncertain", error.commit_uncertain);
  }
  return ret;
}

} // namespace common
} // namespace oceanbase
