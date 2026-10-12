/*
 * Copyright (c) 2026 OceanBase.
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

#include <cstdio>
#include <gtest/gtest.h>
#include "lib/worker.h"
#include "lib/thread/protected_stack_allocator.h"
#ifdef SEEKDB_SERVER_LIFECYCLE_TEST
#include <curl/curl.h>
#include "share/ob_encryption_util.h"
#endif

int main(int argc, char **argv)
{
#ifdef SEEKDB_SERVER_LIFECYCLE_TEST
  // OpenSSL allocator configuration and Curl are host-process resources. Set
  // them up once, before any database open, in the production startup order.
  if (oceanbase::common::OB_SUCCESS != oceanbase::share::ObEncryptionUtil::init_ssl_malloc()
      || CURLE_OK != curl_global_init(CURL_GLOBAL_ALL)) {
    std::fputs("Failed to initialize host crypto resources\n", stderr);
    return 2;
  }
#endif
  ::testing::InitGoogleTest(&argc, argv);
  oceanbase::lib::ObStackHeaderGuard stack_header_guard;
  oceanbase::lib::Worker worker;
  oceanbase::lib::Worker::set_worker_to_thread_local(&worker);
  const int result = RUN_ALL_TESTS();
  const bool ran_tests = ::testing::UnitTest::GetInstance()->test_to_run_count() != 0;
#ifdef SEEKDB_SERVER_LIFECYCLE_TEST
  curl_global_cleanup();
#endif
  oceanbase::lib::Worker::set_worker_to_thread_local(nullptr);
  // The outer runner checks this marker as well as the exit status. Calling
  // _Exit(0) during shutdown must not make an incomplete test pass.
  std::puts("SEEKDB_LIFECYCLE_TESTS_COMPLETED");
  std::fflush(stdout);
  return ran_tests ? result : 2;
}
