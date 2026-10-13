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
#ifndef OCEANBASE_STORAGE_META_STORE_OB_LOCAL_STORAGE_META_SERVICE_H_
#define OCEANBASE_STORAGE_META_STORE_OB_LOCAL_STORAGE_META_SERVICE_H_

#include <stdint.h>
#include "share/rc/ob_server_runtime.h"
#include "storage/meta_store/ob_local_storage_meta_persister.h"
#include "storage/blockstore/ob_object_reader_writer.h"
#include "storage/slog_ckpt/checkpoint_slog_handler.h"
#include "storage/slog/ob_storage_logger.h"

namespace oceanbase
{
namespace storage
{
struct ObGCTabletMetaInfoList;
class ObStartupAccelTaskHandler;
class ObLocalStorageMetaService
{
public:
  ObLocalStorageMetaService();
  ~ObLocalStorageMetaService() = default;
  ObLocalStorageMetaService(const ObLocalStorageMetaService &) = delete;
  ObLocalStorageMetaService &operator=(const ObLocalStorageMetaService &) = delete;

  static int server_module_init(ObLocalStorageMetaService *&meta_service);
  int init();
  int start();
  int replay(const ObServerRuntimeSuperBlock &super_block);
  void stop();
  void wait();
  void destroy();
  bool is_started() { return is_started_; }
  ObLocalStorageMetaPersister &get_persister() { return persister_; }
  int get_meta_block_list(ObIArray<blocksstable::MacroBlockId> &meta_block_list);
  int write_checkpoint(bool is_force);
  int read_from_disk(
      const ObMetaDiskAddr &addr,
      common::ObArenaAllocator &allocator,
      char *&buf,
      int64_t &buf_len);
  int read_from_block(
      const ObMetaDiskAddr &addr,
      common::ObArenaAllocator &allocator,
      char *&buf,
      int64_t &buf_len);
  const CheckpointSlogHandler& get_ckpt_slog_hdl() const { return ckpt_slog_handler_; };

  ObObjectReaderWriter &get_object_reader_writer() { return object_rwriter_; }
  ObObjectReaderWriter &get_object_raw_reader_writer() { return object_raw_rwriter_; }

private:
  bool is_inited_;
  bool is_started_;
  CheckpointSlogHandler ckpt_slog_handler_;
  ObLocalStorageMetaPersister persister_;
  ObObjectReaderWriter object_rwriter_;
  ObObjectReaderWriter object_raw_rwriter_;

};

#define LOCAL_STORAGE_META_PERSISTER (::oceanbase::share::server_service<::oceanbase::storage::ObLocalStorageMetaService>()->get_persister())

} // namespace storage
} // namespace oceanbase

#endif // OCEANBASE_STORAGE_META_STORE_OB_LOCAL_STORAGE_META_SERVICE_H_
