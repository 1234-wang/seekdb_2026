/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "seekdb_engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>

#define REQUIRE(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(int argc, char **argv)
{
  int error = 123;
  REQUIRE(seekdb_engine_close(nullptr) == 0);
  REQUIRE(seekdb_engine_open(-1, nullptr, &error) == nullptr && error != 0);
  REQUIRE(seekdb_engine_open(1, nullptr, &error) == nullptr && error != 0);
  const char *null_argument[] = {nullptr};
  REQUIRE(seekdb_engine_open(1, null_argument, &error) == nullptr && error != 0);
  const int previous_optind = optind;
  optind = 37;
  const char *invalid[] = {"--not-a-seekdb-option"};
  for (int i = 0; i < 3; ++i) {
    REQUIRE(seekdb_engine_open(1, invalid, &error) == nullptr && error != 0);
  }
  REQUIRE(optind == 37);
  optind = previous_optind;
  const char *help[] = {"--help"};
  REQUIRE(seekdb_engine_open(1, help, &error) == nullptr && error != 0);
  const char *missing[] = {"--port"};
  REQUIRE(seekdb_engine_open(1, missing, &error) == nullptr && error != 0);
  if (argc == 2 && std::strcmp(argv[1], "startup_failure") == 0) {
    char directory[] = "/tmp/seekdb-engine-failure-XXXXXX";
    REQUIRE(mkdtemp(directory) != nullptr);
    char original[4096];
    REQUIRE(getcwd(original, sizeof(original)) != nullptr);
    const char *args[] = {"--base-dir", directory,
        "--parameter", "unknown_sdk_test_parameter=1"};
    auto engine = seekdb_engine_open(sizeof(args) / sizeof(args[0]), args, &error);
    if (engine != nullptr) { seekdb_engine_close(engine); }
    REQUIRE(engine == nullptr && error != 0);
    char restored[4096];
    REQUIRE(getcwd(restored, sizeof(restored)) != nullptr);
    REQUIRE(std::strcmp(original, restored) == 0);
    REQUIRE(access((std::string(directory) + "/run/seekdb.pid").c_str(), F_OK) != 0);
  }
  if (argc == 2 && std::strcmp(argv[1], "lifetime") == 0) {
    char directory[] = "/tmp/seekdb-engine-XXXXXX";
    REQUIRE(mkdtemp(directory) != nullptr);
    char original[4096];
    REQUIRE(getcwd(original, sizeof(original)) != nullptr);
    struct sigaction before, after;
    REQUIRE(sigaction(SIGTERM, nullptr, &before) == 0);
    const char *args[] = {"--base-dir", directory, "--embedded", "--nodaemon",
        "--parameter", "memory_budget=1G", "--parameter", "log_disk_size=2G",
        "--parameter", "mysql_port_mode=disabled", "--parameter", "cpu_count=2"};
    auto engine = seekdb_engine_open(sizeof(args) / sizeof(args[0]), args, &error);
    if (engine == nullptr) { std::fprintf(stderr, "open failed: %d (%s)\n", error, directory); }
    REQUIRE(engine != nullptr && error == 0);
    REQUIRE(sigaction(SIGTERM, nullptr, &after) == 0);
    REQUIRE(before.sa_handler == after.sa_handler);
    REQUIRE(seekdb_engine_open(sizeof(args) / sizeof(args[0]), args, &error) == nullptr && error != 0);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(client >= 0);
    timeval timeout{10, 0};
    REQUIRE(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    std::snprintf(endpoint.sun_path, sizeof(endpoint.sun_path), "%s/run/sql.sock", directory);
    REQUIRE(connect(client, reinterpret_cast<sockaddr *>(&endpoint), sizeof(endpoint)) == 0);
    unsigned char greeting[5];
    REQUIRE(recv(client, greeting, sizeof(greeting), MSG_WAITALL) == sizeof(greeting));
    REQUIRE(greeting[3] == 0 && greeting[4] == 10);
    REQUIRE(close(client) == 0);
    // Let background package loading start before exercising shutdown.
    sleep(6);
    error = seekdb_engine_close(engine);
    if (error != 0) { std::fprintf(stderr, "close failed: %d\n", error); }
    REQUIRE(error == 0);
    char restored[4096];
    REQUIRE(getcwd(restored, sizeof(restored)) != nullptr);
    REQUIRE(std::strcmp(original, restored) == 0);
    const std::string pid = std::string(directory) + "/run/seekdb.pid";
    REQUIRE(access(pid.c_str(), F_OK) != 0);
    // Until core singleton reuse is supported, fail explicitly instead of
    // reinitializing destroyed providers or terminating the host.
    REQUIRE(seekdb_engine_open(sizeof(args) / sizeof(args[0]), args, &error) == nullptr && error != 0);
    std::printf("Engine test data retained at %s\n", directory);
  }
  std::puts("SEEKDB_ENGINE_TEST_COMPLETED");
  return 0;
}
