// Copyright (c) 2026 OceanBase.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "observer/in_process/seekdb.h"
#include "rust/in_process.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace oceanbase::in_process {
// Exercise the real Rust startup and handle lifecycle with only the server
// boundary replaced by a Unix socket that supplies the readiness greeting.
int32_t run_server(rust::Str, rust::Slice<const rust::String>, rust::Fn<void()> report_running)
{
  const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strcpy(address.sun_path, "run/sql.sock");
  if (listener < 0 || bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      listen(listener, 1) != 0) {
    return -4002;
  }
  report_running();
  const int client = accept(listener, nullptr, nullptr);
  const unsigned char greeting[] = {1, 0, 0, 0, 10};
  if (client < 0 || write(client, greeting, sizeof(greeting)) != sizeof(greeting)) {
    return -4002;
  }
  close(client);
  close(listener);
  for (;;) {
    pause();
  }
}
} // namespace oceanbase::in_process

TEST(InProcessApi, InvalidInputsClearTheOutputHandle)
{
  EXPECT_EQ(SEEKDB_INVALID_ARGUMENT, seekdb_open(nullptr, nullptr, nullptr));
  EXPECT_STREQ("out_handle must not be NULL", seekdb_last_open_error());
  const char invalid_utf8[] = {'/', static_cast<char>(0xff), '\0'};
  for (const char *directory : {static_cast<const char *>(nullptr), "relative", invalid_utf8}) {
    SeekdbHandle handle = reinterpret_cast<SeekdbHandle>(1);
    EXPECT_EQ(SEEKDB_INVALID_ARGUMENT, seekdb_open(directory, nullptr, &handle));
    EXPECT_EQ(nullptr, handle);
    EXPECT_NE('\0', seekdb_last_open_error()[0]);
  }
  SeekdbConnectionOptions options{};
  EXPECT_EQ(SEEKDB_INVALID_ARGUMENT, seekdb_connection_options(nullptr, &options));
  EXPECT_EQ(SEEKDB_INVALID_ARGUMENT, seekdb_close(nullptr));
}

TEST(InProcessApi, RejectsMalformedAndUnsupportedParameters)
{
  const char bad_utf8[] = {static_cast<char>(0xff), '\0'};
  const char *cases[][3] = {
      {"port", nullptr, nullptr},
      {"", "value", nullptr},
      {"key", bad_utf8, nullptr},
      {"port", "3306", nullptr},
      {"mysql_port_mode", "fixed", nullptr},
  };
  for (auto &parameters : cases) {
    SeekdbHandle handle = reinterpret_cast<SeekdbHandle>(1);
    EXPECT_EQ(SEEKDB_INVALID_ARGUMENT, seekdb_open("/unused", parameters, &handle));
    EXPECT_EQ(nullptr, handle);
    EXPECT_NE('\0', seekdb_last_open_error()[0]);
  }
}

TEST(InProcessApi, LastOpenErrorIsThreadLocal)
{
  ASSERT_EQ(SEEKDB_INVALID_ARGUMENT, seekdb_open(nullptr, nullptr, nullptr));
  const std::string message = seekdb_last_open_error();
  std::thread other([] {
    EXPECT_STREQ("", seekdb_last_open_error());
    SeekdbHandle handle = nullptr;
    EXPECT_EQ(SEEKDB_INVALID_ARGUMENT, seekdb_open("relative", nullptr, &handle));
    EXPECT_STREQ("the directory must be an absolute path", seekdb_last_open_error());
  });
  other.join();
  EXPECT_EQ(message, seekdb_last_open_error());
}

static void check_handle_lifecycle(const char *directory)
{
  SeekdbHandle first = nullptr;
  ASSERT_EQ(SEEKDB_SUCCESS, seekdb_open(directory, nullptr, &first)) << seekdb_last_open_error();
  ASSERT_NE(nullptr, first);
  EXPECT_STREQ("", seekdb_last_open_error());
  SeekdbConnectionOptions options{};
  ASSERT_EQ(SEEKDB_SUCCESS, seekdb_connection_options(first, &options));
  EXPECT_STREQ(SEEKDB_CONNECTION_TRANSPORT_UNIX_SOCKET, options.transport);
  EXPECT_EQ(std::string(directory) + "/run/sql.sock", options.unix_socket);
  EXPECT_EQ(0U, options.port);
  EXPECT_EQ(nullptr, options.host);
  EXPECT_EQ(nullptr, options.named_pipe);
  EXPECT_STREQ("root", options.user);
  EXPECT_EQ(SEEKDB_INVALID_ARGUMENT, seekdb_connection_options(first, nullptr));

  SeekdbHandle second = nullptr;
  ASSERT_EQ(SEEKDB_SUCCESS, seekdb_open(directory, nullptr, &second));
  EXPECT_EQ(SEEKDB_SUCCESS, seekdb_close(second));
  EXPECT_EQ(std::string(directory) + "/run/sql.sock", options.unix_socket);
  EXPECT_EQ(SEEKDB_SUCCESS, seekdb_close(first));

  ASSERT_EQ(SEEKDB_SUCCESS, seekdb_open(directory, nullptr, &first));
  EXPECT_EQ(SEEKDB_SUCCESS, seekdb_close(first));
  const std::string other = std::string(directory) + "/other";
  EXPECT_EQ(SEEKDB_INTERNAL_ERROR, seekdb_open(other.c_str(), nullptr, &second));
  EXPECT_EQ(nullptr, second);
  EXPECT_STREQ("another directory is already open in this process", seekdb_last_open_error());
}

TEST(InProcessApi, HandlesOwnTheirPathsAndReuseTheEngine)
{
  char directory[] = "/tmp/seekdb-api-XXXXXX";
  ASSERT_NE(nullptr, mkdtemp(directory));
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    check_handle_lifecycle(directory);
    _exit(::testing::Test::HasFailure() ? 1 : 0);
  }
  int status = 0;
  EXPECT_EQ(child, waitpid(child, &status, 0));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
