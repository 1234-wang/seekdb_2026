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
#define USING_LOG_PREFIX STORAGE

#ifndef _WIN32
#include <sys/statvfs.h>
#endif
#include "share/rc/ob_server_runtime.h"
#include "storage/api/storage/runtime/ob_i_server_runtime.h"
#include "storage/meta_store/storage_meta_service.h"
#include "share/ob_force_print_log.h"
#include "share/ob_local_device.h"
#include "storage/ob_file_system_router.h"
#include "storage/slog_ckpt/runtime_checkpoint_reader.h"
#include "storage/slog/ob_storage_log.h"
#include "storage/slog_ckpt/ob_linked_macro_block_writer.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/ls/ob_ls.h"
#include "storage/blocksstable/ob_block_manager.h"
#include "storage/meta_store/ob_local_storage_meta_service.h"
namespace oceanbase
{
namespace storage
{

StorageMetaService &StorageMetaService::get_instance()
{
  static StorageMetaService instance_;
  return instance_;
}
StorageMetaService::StorageMetaService()
  : is_inited_(false),
    is_started_(false),
    slogger_(),
    server_runtime_(nullptr),
    checkpoint_lock_(common::ObLatchIds::SLOG_CKPT_LOCK),
    runtime_block_handle_(),
    need_reserved_(false) {}

int StorageMetaService::init(ObIServerRuntime &server_runtime)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(check_log_disk(
        OB_FILE_SYSTEM_ROUTER.get_sstable_dir(),
        OB_FILE_SYSTEM_ROUTER.get_slog_dir()))) {
  } else if (OB_FAIL(slogger_.init(
        OB_FILE_SYSTEM_ROUTER.get_slog_dir(),
        ObLogConstants::MAX_LOG_FILE_SIZE,
        OB_FILE_SYSTEM_ROUTER.get_slog_file_spec()))) {
  } else {
    server_runtime_ = &server_runtime;
    is_inited_ = true;
  }
  return ret;
}

int StorageMetaService::start()
{
  int ret = OB_SUCCESS;
  const int64_t start_time = ObTimeUtility::current_time();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(slogger_.start())) {
  } else if (OB_FAIL(recover())) {
  } else {
    ATOMIC_STORE(&is_started_, true);
  }
  const int64_t cost_time_us = ObTimeUtility::current_time() - start_time;
  FLOG_INFO("finish start server storage meta service", K(ret), K(cost_time_us));
  return ret;
}

void StorageMetaService::stop()
{
  if (IS_INIT) {
    ATOMIC_STORE(&is_started_, false);
    auto *local = share::server_service<ObLocalStorageMetaService>();
    if (OB_NOT_NULL(local)) {
      local->stop();
      local->wait();
    }
  }
}
void StorageMetaService::wait()
{
  if (IS_INIT) {
    slogger_.stop();
    slogger_.wait();
  }
}
void StorageMetaService::destroy()
{
  stop();
  wait();
  slogger_.destroy();
  runtime_block_handle_.reset();
  server_runtime_ = nullptr;
  need_reserved_ = false;
  is_inited_ = false;
}

int StorageMetaService::get_meta_block_list(ObIArray<blocksstable::MacroBlockId> &blocks)
{
  common::TCRLockGuard guard(checkpoint_lock_);
  return blocks.assign(runtime_block_handle_.get_meta_block_list());
}

int StorageMetaService::get_reserved_size(int64_t &reserved_size) const
{
  int ret = OB_SUCCESS;
  reserved_size = 0;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (need_reserved_) {
    int64_t used_size = 0;
    if (OB_FAIL(get_using_disk_space(used_size))) {
    } else {
      reserved_size = std::max(static_cast<int64_t>(0), SLOG_RESERVED_DISK_SIZE - used_size);
    }
  }
  return ret;
}

int StorageMetaService::write_checkpoint(bool is_force)
{
  int ret = OB_SUCCESS;
  auto *local = share::server_service<ObLocalStorageMetaService>();
  if (!is_started() || OB_ISNULL(local)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(local->write_checkpoint(is_force))) {
  }
  return ret;
}

int StorageMetaService::check_log_disk(
    const char *data_dir,
    const char *log_dir)
{
  int ret = OB_SUCCESS;
  need_reserved_ = false;
  if (OB_ISNULL(data_dir) || OB_ISNULL(log_dir)) {
    ret = OB_INVALID_ARGUMENT;
#ifdef _WIN32
  } else {
    UNUSEDx(data_dir, log_dir);
  }
#else
  } else {
    struct statvfs data_svfs;
    struct statvfs log_svfs;
    if (OB_UNLIKELY(0 != statvfs(data_dir, &data_svfs))) {
      ret = OB_IO_ERROR;
    } else if (OB_UNLIKELY(0 != statvfs(log_dir, &log_svfs))) {
      ret = OB_IO_ERROR;
      LOG_WARN("fail to get slog directory vfs", K(ret), K(log_dir));
    } else if (OB_UNLIKELY(0 >= log_svfs.f_bavail)) {
      ret = OB_DISK_ERROR;
      LOG_ERROR("slog disk is full", K(ret), K(log_dir), K(log_svfs.f_bavail));
    } else {
      need_reserved_ = (data_svfs.f_fsid == log_svfs.f_fsid);
    }
  }
#endif
  return ret;
}

int StorageMetaService::get_using_disk_space(int64_t &using_space) const
{
  return slogger_.get_using_disk_space(using_space);
}

int StorageMetaService::recover()
{
  int ret = OB_SUCCESS;
  RuntimeCheckpointReader reader;
  omt::ObServerRuntimeMeta meta;
  bool exists = false;
  const ObServerSuperBlock &super_block = OB_STORAGE_OBJECT_MGR.get_server_super_block();
  if (OB_FAIL(reader.read_checkpoint(super_block))) {
  } else if (OB_FAIL(reader.get_runtime_meta(meta, exists))) {
  } else if (!exists) {
    // An uncommitted bootstrap has no recoverable runtime yet.
    ret = slogger_.start_log(super_block.body_.replay_start_point_);
  } else if (OB_FAIL(runtime_block_handle_.add_macro_blocks(reader.get_meta_block_list()))) {
  } else if (OB_FAIL(server_runtime_->create_runtime(meta))) {
  } else if (OB_FAIL(share::server_service<ObLocalStorageMetaService>()->replay(meta.super_block_))) {
  } else {
    server_runtime_->set_synced();
  }
  if (OB_SUCC(ret) && OB_FAIL(OB_SERVER_BLOCK_MGR.first_mark_device())) {
  }
  if (OB_SUCC(ret) && exists) {
    ObLS *ls = nullptr;
    ObLSService *ls_service = share::server_service<ObLSService>();
    if (OB_FAIL(ls_service->get_ls(ls))) {
    } else if (OB_FAIL(ls->finish_storage_meta_replay())) {
    } else if (OB_FAIL(ls_service->gc_ls_after_replay_slog())) {
    } else if (OB_FAIL(ls_service->online_ls())) {
    }
  }
  LOG_INFO("finish unified storage recovery", K(ret), K(exists));
  return ret;
}

int StorageMetaService::publish_checkpoint(
    const omt::ObServerRuntimeMeta &meta, const common::ObLogCursor &cursor)
{
  int ret = OB_SUCCESS;
  ObLinkedMacroBlockItemWriter writer;
  common::ObArenaAllocator allocator("RuntimeCkpt");
  blocksstable::MacroBlockId entry;
  const int64_t size = meta.get_serialize_size();
  char *buf = nullptr;
  int64_t pos = 0;
  if (!meta.is_valid() || !cursor.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(meta.serialize(buf, size, pos))) {
  } else if (OB_FAIL(writer.init(false, common::ObMemAttr("RuntimeCkpt")))) {
  } else if (OB_FAIL(writer.write_item(buf, pos, nullptr))) {
  } else if (OB_FAIL(writer.close())) {
  } else if (OB_FAIL(writer.get_entry_block(entry))) {
  } else if (OB_FAIL(LOCAL_DEVICE_INSTANCE.fsync_block())) {
  } else if (OB_FAIL(OB_STORAGE_OBJECT_MGR.update_super_block(cursor, entry))) {
    // Publication may have reached disk. Do not release blocks or roll back tablet refs.
    LOG_ERROR("checkpoint publication failed; stopping before releasing referenced blocks", K(ret));
    ob_abort();
  } else {
    common::TCWLockGuard guard(checkpoint_lock_);
    if (OB_FAIL(runtime_block_handle_.add_macro_blocks(writer.get_meta_block_list()))) {
      LOG_ERROR("cannot retain published checkpoint blocks", K(ret));
      ob_abort();
    }
  }
  return ret;
}

int StorageMetaService::write_resources(const share::ObServerRuntimeConfig &config)
{
  int ret = OB_SUCCESS;
  ObUpdateServerResourcesLog entry(*const_cast<share::ObServerRuntimeConfig *>(&config));
  ObStorageLogParam param;
  param.data_ = &entry;
  param.cmd_ = ObIRedoModule::gen_cmd(ObRedoLogMainType::OB_REDO_LOG_LOCAL_STORAGE,
                                    ObRedoLogSubType::OB_REDO_LOG_UPDATE_SERVER_RESOURCES);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(slogger_.write_log(param))) {
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
