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
#pragma once

#include <stdint.h>
#include "storage/slog_ckpt/ob_linked_macro_block_struct.h"
#include "storage/meta_store/ob_server_runtime_meta.h"
#include "lib/lock/ob_tc_rwlock.h"
#include "storage/slog/ob_storage_logger.h"
#include "share/ob_i_local_device_space_provider.h"

namespace oceanbase
{
namespace storage
{
class ObIServerRuntime;
class StorageMetaService : public share::ObILocalDeviceSpaceProvider
{
public:
  static StorageMetaService &get_instance();
  int init(ObIServerRuntime &server_runtime);
  int start();
  void stop();
  void wait();
  void destroy();
  int write_resources(const share::ObServerRuntimeConfig &config);
  bool is_started() const { return ATOMIC_LOAD(&is_started_); }

  int get_meta_block_list(ObIArray<blocksstable::MacroBlockId> &meta_block_list);
  int get_reserved_size(int64_t &reserved_size) const override;
  int write_checkpoint(bool is_force);
  ObStorageLogger &get_slogger() { return slogger_; }
  int publish_checkpoint(const omt::ObServerRuntimeMeta &meta, const common::ObLogCursor &cursor);

  static constexpr int64_t SLOG_RESERVED_DISK_SIZE = 128 * 1024 * 1024L; // 128M

private:
  StorageMetaService();
  ~StorageMetaService() = default;
  StorageMetaService(const StorageMetaService &) = delete;
  StorageMetaService &operator=(const StorageMetaService &) = delete;

  int recover();
  int get_using_disk_space(int64_t &using_space) const;
  int check_log_disk(const char *data_dir, const char *log_dir);

private:
  bool is_inited_;
  bool is_started_;
  ObStorageLogger slogger_;
  ObIServerRuntime *server_runtime_;
  common::TCRWLock checkpoint_lock_;
  ObMetaBlockListHandle runtime_block_handle_;
  bool need_reserved_;
};

#define STORAGE_META_SERVICE (oceanbase::storage::StorageMetaService::get_instance())

} // namespace storage
} // namespace oceanbase
