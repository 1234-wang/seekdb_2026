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

#include "ob_task_define.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/oblog/ob_log.h"
#include <numeric>
#include <new>

using namespace oceanbase::lib;
using namespace oceanbase::common;

namespace oceanbase {
namespace common {
void allow_next_syslog(int64_t count)
{
  share::ObTaskController::get().allow_next_syslog(count);
}
} // common

namespace share {

class ObLogRateLimiter : public common::ObSyslogSimpleRateLimiter
{
  friend class ObTaskController;
public:
  ObLogRateLimiter()
  {
  }

  bool is_force_allows() const override
  {
    return OB_UNLIKELY(allows_ > 0);
  }
  void reset_force_allows() override
  {
    if (is_force_allows()) {
      allows_--;
    }
  }

  int do_acquire(int64_t permits, int log_level, int errcode) override {
    return ObSyslogSimpleRateLimiter::do_acquire(permits, log_level, errcode);
  }

private:
  RLOCAL_STATIC(int64_t, allows_);
};

_RLOCAL(int64_t, ObLogRateLimiter::allows_);

namespace
{
struct ProcessLogLimiters
{
  ProcessLogLimiters()
  {
    limiters_[toUType(ObTaskType::GENERIC)].set_name("ObTaskType::GENERIC");
    limiters_[toUType(ObTaskType::USER_REQUEST)].set_name("ObTaskType::USER_REQUEST");
    limiters_[toUType(ObTaskType::DATA_MAINTAIN)].set_name("ObTaskType::DATA_MAINTAIN");
    limiters_[toUType(ObTaskType::SCHEMA)].set_name("ObTaskType::SCHEMA");
  }
  ObLogRateLimiter limiters_[toUType(ObTaskType::MAX)];
};

ProcessLogLimiters &process_log_limiters()
{
  // Like ROOT_CONTEXT, these fixed process facilities outlive database close
  // and host TLS. Raw storage deliberately does not register an atexit
  // destructor; there are five nodes, with no allocation on subsequent opens.
  alignas(ProcessLogLimiters) static unsigned char storage[sizeof(ProcessLogLimiters)];
  static ProcessLogLimiters *const limiters = new (storage) ProcessLogLimiters();
  return *limiters;
}
} // namespace

// class ObTaskController
ObTaskController ObTaskController::instance_;

ObTaskController::ObTaskController()
    : limiters_(), rate_pctgs_(), log_rate_limit_(LOG_RATE_LIMIT),
      rates_inited_(false), active_(false)
{}

ObTaskController::~ObTaskController()
{
  destroy();
}

int ObTaskController::init()
{
  int ret = OB_SUCCESS;
  if (active_) {
    return OB_INIT_TWICE;
  }
  if (!rates_inited_) {
    ProcessLogLimiters &process = process_log_limiters();
    for (int i = 0; i < MAX_TASK_ID; ++i) {
      limiters_[i] = &process.limiters_[i];
    }
#define LOG_PCTG(ID, PCTG)                      \
    do {                                        \
      set_log_rate_pctg<ID>(PCTG);              \
    } while (0)

    // Set percentage of each task here.
    // @NOTE: Inquire @yongle.xh before any change.
    LOG_PCTG(ObTaskType::GENERIC, 100.0);  // default limiter
    LOG_PCTG(ObTaskType::USER_REQUEST, 100.0);  // default limiter
    LOG_PCTG(ObTaskType::DATA_MAINTAIN, 100.0);  // default limiter
    LOG_PCTG(ObTaskType::SCHEMA, 100.0);  // default limiter

#undef LOG_PCTG

    calc_log_rate();
    rates_inited_ = true;
  }
  ObLogger::set_default_limiter(*limiters_[toUType(ObTaskType::GENERIC)]);
  ObLogger::set_tl_type(0);
  active_ = true;
  return ret;
}

void ObTaskController::stop()
{
}

void ObTaskController::wait()
{
}

void ObTaskController::destroy()
{
  stop();
  wait();
  if (active_) {
    ObLogger::clear_log_limiters();
    active_ = false;
  }
  // Borrowed pointers may already be in use on a host thread. The fixed
  // process limiters remain alive; generation invalidation resets later TLS
  // lookups without adding reference counting to the logging hot path.
}

void ObTaskController::switch_task(ObTaskType task_id)
{
  ObLogger::set_tl_limiter(*limiters_[toUType(task_id)]);
  ObLogger::set_tl_type(static_cast<int32_t>(toUType(task_id)));
}

void ObTaskController::allow_next_syslog(int64_t count)
{
  ObLogRateLimiter::allows_ += count;
}

void ObTaskController::set_log_rate_limit(int64_t limit)
{
  if (limit != log_rate_limit_) {
    log_rate_limit_ = limit;
    calc_log_rate();
  }
}

void ObTaskController::set_diag_per_error_limit(int64_t cnt)
{
  UNUSED(cnt);
}

void ObTaskController::calc_log_rate()
{
  const double total = std::accumulate(
      rate_pctgs_, rate_pctgs_ + MAX_TASK_ID, .0);
  for (int i = 0; total > 0 && i < MAX_TASK_ID; i++) {
    if (nullptr != limiters_[i]) {
      limiters_[i]->set_rate(
          static_cast<int64_t>(
              rate_pctgs_[i]/total * static_cast<double>(log_rate_limit_)));
    }
  }
}

ObTaskController &ObTaskController::get()
{
  return instance_;
}

ObTaskController::RateLimiter *ObTaskController::get_limiter(ObTaskType id)
{
  return limiters_[toUType(id)];
};

}  // share
}  // oceanbase
