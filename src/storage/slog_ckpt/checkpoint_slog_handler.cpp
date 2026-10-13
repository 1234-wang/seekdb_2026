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

#include "share/ob_server_struct.h"
#include "lib/stat/ob_diagnostic_info_guard.h"
#include "checkpoint_slog_handler.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/slog_ckpt/ob_tablet_replay_create_handler.h"
#include "storage/meta_store/storage_meta_service.h"
#include "storage/slog/ob_storage_log.h"
#include "storage/slog/ob_storage_log_reader.h"
#include "storage/slog_ckpt/ob_local_storage_checkpoint_writer.h"
#include "storage/api/storage/runtime/ob_i_server_runtime.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "share/ob_structured_event_logger.h"
#include "storage/compaction/ob_tablet_scheduler.h"

#ifdef _WIN32
#include <windows.h>
#define sleep(sec) Sleep((sec) * 1000)
#endif

namespace oceanbase
{
using namespace share;
using namespace common;
using namespace blocksstable;
namespace storage
{

ObLSCkptMember::ObLSCkptMember()
  : version_(LS_CKPT_MEM_VERSION), length_(0),
    ls_meta_(), tablet_meta_entry_()
{
}

ObLSCkptMember::~ObLSCkptMember()
{
  reset();
}

void ObLSCkptMember::reset()
{
  version_ = LS_CKPT_MEM_VERSION;
  length_ = 0;
  ls_meta_.reset();
  tablet_meta_entry_.reset();
}

bool ObLSCkptMember::is_valid() const
{
  return ls_meta_.is_valid()
         && tablet_meta_entry_.is_valid();
}

int ObLSCkptMember::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  int64_t new_pos = pos;
  const int64_t length = get_serialize_size();

  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len <= 0) || OB_UNLIKELY(pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (LS_CKPT_MEM_VERSION != version_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(length_ > buf_len - pos)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(serialization::encode_i32(buf, buf_len, new_pos, version_))) {
  } else if (new_pos - pos < length && OB_FAIL(serialization::encode_i32(buf, buf_len, new_pos, length))) {
  } else if (new_pos - pos < length && OB_FAIL(ls_meta_.serialize(buf, buf_len, new_pos))) {
  } else if (new_pos - pos < length && OB_FAIL(tablet_meta_entry_.serialize(buf, buf_len, new_pos))) {
  } else if (OB_UNLIKELY(length != new_pos - pos)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    pos = new_pos;
  }

  return ret;
}

int ObLSCkptMember::deserialize(const char *buf, const int64_t buf_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t new_pos = pos;

  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len <= 0) || OB_UNLIKELY(pos < 0) || OB_UNLIKELY(buf_len <= pos)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(serialization::decode_i32(buf, buf_len, new_pos, (int32_t *)&version_))) {
  } else if (OB_FAIL(serialization::decode_i32(buf, buf_len, new_pos, (int32_t *)&length_))) {
  } else if (OB_UNLIKELY(length_ > buf_len - pos)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(LS_CKPT_MEM_VERSION != version_)) {
    ret = OB_NOT_SUPPORTED;
  } else if (new_pos - pos < length_ && OB_FAIL(ls_meta_.deserialize(buf, buf_len, new_pos))) {
  } else if (new_pos - pos < length_ && OB_FAIL(tablet_meta_entry_.deserialize(buf, buf_len, new_pos))) {
  } else if (OB_UNLIKELY(length_ != new_pos - pos)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    pos = new_pos;
  }

  return ret;
}

int64_t ObLSCkptMember::get_serialize_size() const
{
  int64_t size = 0;
  size += serialization::encoded_length_i32(version_);
  size += serialization::encoded_length_i32(length_);
  size += ls_meta_.get_serialize_size();
  size += tablet_meta_entry_.get_serialize_size();
  return size;
}

void CheckpointSlogHandler::ObWriteCheckpointTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  ObCurTraceId::init(GCTX.self_addr());
  if (STORAGE_META_SERVICE.is_started()) {
    if (OB_FAIL(handler_->write_checkpoint(false/*is_force*/))) {
    }
  } else {
    // Must wait for all slog replays to complete before doing ckpt, otherwise some macro blocks may not be marked
    LOG_INFO("slog replay not finish, do not write checkpoint");
  }
}

CheckpointSlogHandler::CheckpointSlogHandler()
  : is_inited_(false),
    is_writing_checkpoint_(false),
    last_ckpt_time_(0),
    last_frozen_version_(0),
    slogger_(nullptr),
    lock_(common::ObLatchIds::SLOG_CKPT_LOCK),
    slog_ckpt_lock_(common::ObLatchIds::SLOG_CKPT_LOCK),
    tablet_key_set_(),
    is_copying_tablets_(false),
    ckpt_cursor_(),
    ls_block_handle_(),
    tablet_block_handle_(),
    write_ckpt_timer_(),
    write_ckpt_task_(this),
    replay_tablet_disk_addr_map_()
{
}

int CheckpointSlogHandler::init(ObStorageLogger &slogger)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("CheckpointSlogHandler has inited", K(ret));
  } else if (OB_FAIL(write_ckpt_timer_.init(
      "WriteCkpt", common::ObMemAttr("WriteCkpt")))) {
  } else {
    slogger_ = &slogger;
    is_inited_ = true;
  }
  return ret;
}

int CheckpointSlogHandler::start()
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(write_ckpt_timer_.schedule(write_ckpt_task_,
               ObWriteCheckpointTask::WRITE_CHECKPOINT_INTERVAL_US, true))) {
  }
  return ret;
}

void CheckpointSlogHandler::stop()
{
  if (IS_INIT) {
    write_ckpt_timer_.stop();
  }
  // since the ls service deletes tablets in the stop interface when observer stop,
  // it must ensure that no checkpoint is in progress after stop, otherwise the tablet may be lost
  wait();
}

void CheckpointSlogHandler::wait()
{
  if (IS_INIT) {
    write_ckpt_timer_.wait();
  }
}

void CheckpointSlogHandler::destroy()
{
  if (IS_INIT) {
    write_ckpt_timer_.destroy();
    slogger_ = nullptr;
    ls_block_handle_.reset();
    tablet_block_handle_.reset();
    replay_tablet_disk_addr_map_.destroy();
    tablet_key_set_.destroy();
    ckpt_cursor_.reset();
    is_copying_tablets_ = false;
    is_inited_ = false;
  }
}

int CheckpointSlogHandler::start_replay(const ObServerRuntimeSuperBlock &super_block)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!super_block.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(replay_checkpoint_and_slog(super_block))) {
  }
  LOG_INFO("finish CheckpointSlogHandler replay", K(ret), K(super_block));

  return ret;
}

int CheckpointSlogHandler::replay_checkpoint_and_slog(const ObServerRuntimeSuperBlock &super_block)
{
  int ret = OB_SUCCESS;
  const ObMemAttr mem_attr("RuntimeReplay");
  const int64_t replay_tablet_cnt = 10003;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("CheckpointSlogHandler not init", K(ret));
  } else if (OB_FAIL(replay_tablet_disk_addr_map_.create(replay_tablet_cnt, mem_attr, mem_attr))) {
  } else if (OB_FAIL(replay_checkpoint(super_block))) {
  } else if (OB_FAIL(replay_local_storage_slog(OB_STORAGE_OBJECT_MGR.get_server_super_block().body_.replay_start_point_))) {
  } else {
    replay_tablet_disk_addr_map_.destroy();
  }
  return ret;
}

int CheckpointSlogHandler::replay_checkpoint(const ObServerRuntimeSuperBlock &super_block)
{
  int ret = OB_SUCCESS;
  replay_tablet_disk_addr_map_.reuse();

  if (OB_FAIL(replay_checkpoint_meta(super_block))) {
  }

  LOG_INFO("finish replay runtime checkpoint", K(ret), K(super_block));

  return ret;
}

int CheckpointSlogHandler::replay_checkpoint_meta(const ObServerRuntimeSuperBlock &super_block)
{
  int ret = OB_SUCCESS;
  ObLocalStorageCheckpointReader ls_ckpt_reader;
  ObSArray<MacroBlockId> tablet_block_list(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator("ReplayCKPT"));
  ObSArray<MacroBlockId> ls_block_list(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator("ReplayCKPT"));
  ObLocalStorageCheckpointReader::ObStorageMetaOp replay_ls_op = std::bind(
      &CheckpointSlogHandler::replay_checkpoint_ls,
      this,
      std::placeholders::_1,
      std::placeholders::_2,
      std::placeholders::_3,
      std::ref(tablet_block_list));

  if (OB_FAIL(ls_ckpt_reader.read_single_meta_item(
      super_block.ls_meta_entry_, replay_ls_op, ls_block_list))) {
  } else if (OB_FAIL(ls_block_handle_.add_macro_blocks(ls_block_list))) {
  } else if (OB_FAIL(tablet_block_handle_.add_macro_blocks(tablet_block_list))) {
  }
  return ret;
}

int CheckpointSlogHandler::replay_checkpoint_ls(
    const ObMetaDiskAddr &addr,
    const char *buf,
    const int64_t buf_len,
    ObIArray<MacroBlockId> &tablet_block_list)
{
  int ret = OB_SUCCESS;
  ObLSCkptMember ls_ckpt_member;
  int64_t pos = 0;
  ObSArray<MacroBlockId> tmp_block_list(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator("ReplayCKPT"));
  ObLocalStorageCheckpointReader tablet_ckpt_reader;
  ObLocalStorageCheckpointReader::ObStorageMetaOp replay_tablet_op =
      std::bind(&CheckpointSlogHandler::replay_checkpoint_tablet,
      this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

  if (OB_FAIL(ls_ckpt_member.deserialize(buf, buf_len, pos))) {
  } else if (OB_FAIL(::oceanbase::share::server_service<::oceanbase::storage::ObLSService>()->replay_create_ls(ls_ckpt_member.ls_meta_))) {
  } else if (OB_FAIL(tablet_ckpt_reader.iter_read_meta_item(
      ls_ckpt_member.tablet_meta_entry_, replay_tablet_op, tmp_block_list))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < tmp_block_list.count(); i++) {
      if (OB_FAIL(tablet_block_list.push_back(tmp_block_list.at(i)))) {
      }
    }
  }
  return ret;
}

int CheckpointSlogHandler::replay_checkpoint_tablet(
    const ObMetaDiskAddr &addr, const char *buf, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  UNUSEDx(addr);
  if (OB_FAIL(inner_replay_deserialize(
      buf, buf_len, false /* allow to overwrite the map's element or not */))) {
  }
  return ret;
}

int CheckpointSlogHandler::replay_local_storage_slog(const common::ObLogCursor &start_point)
{
  int ret = OB_SUCCESS;
  ObLogCursor replay_finish_point;
  ObStorageLogReplayer replayer;
  blocksstable::ObLogFileSpec log_file_spec;
  log_file_spec.retry_write_policy_ = "normal";
  log_file_spec.log_create_policy_ = "normal";
  log_file_spec.log_write_policy_ = "truncate";

  if (OB_FAIL(replayer.init(slogger_->get_dir(), log_file_spec))) {
  } else if (OB_FAIL(replayer.register_redo_module(ObRedoLogMainType::OB_REDO_LOG_LOCAL_STORAGE, this))) {
  } else if (OB_FAIL(replayer.replay(start_point, replay_finish_point))) {
  } else {
    ObTabletReplayCreateHandler handler;
    if (OB_FAIL(handler.init(replay_tablet_disk_addr_map_))) {
    } else if (OB_FAIL(handler.concurrent_replay(::oceanbase::share::server_service<::oceanbase::storage::ObStartupAccelTaskHandler>()))) {
    }
  }
  if (OB_FAIL(ret)) {
    // do nothing
  } else if (OB_FAIL(replayer.replay_over())) {
  } else if (OB_FAIL(slogger_->start_log(replay_finish_point))) {
  }

  LOG_INFO("finish replay runtime slog", K(ret), K(start_point), K(replay_finish_point));

  return ret;
}

int CheckpointSlogHandler::report_slog(
    const ObTabletMapKey &tablet_key,
    const ObMetaDiskAddr &slog_addr)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("CheckpointSlogHandler hasn't been inited", K(ret));
  } else if (OB_UNLIKELY(!slog_addr.is_valid() || !slog_addr.is_file())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("slog_addr is invalid", K(ret), K(slog_addr));
  } else {
    int64_t file_id;
    int64_t offset;
    int64_t size;
    if (is_copying_tablets_) {
      if (OB_UNLIKELY(!ckpt_cursor_.is_valid())) {
        LOG_WARN("checkpoint cursor is invalid", K(ret), K(ckpt_cursor_));
      } else if (OB_FAIL(slog_addr.get_file_addr(file_id, offset, size))) {
      } else if (file_id < ckpt_cursor_.file_id_
          || (file_id == ckpt_cursor_.file_id_ && offset < ckpt_cursor_.offset_)) {
        LOG_INFO("tablet slog cursor is smaller, no need to add it to the set",
          K(ret), K(ckpt_cursor_), K(file_id), K(offset));
      } else if (OB_FAIL(tablet_key_set_.set_refactored(tablet_key, 1 /* whether allow override */))) {
      }
    }
  }
  return ret;
}

int CheckpointSlogHandler::check_slog(const ObTabletMapKey &tablet_key, bool &has_slog)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("CheckpointSlogHandler hasn't been inited", K(ret));
  } else if (OB_UNLIKELY(!tablet_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    int tmp_ret = tablet_key_set_.exist_refactored(tablet_key);
    if (OB_HASH_EXIST == tmp_ret) {
      has_slog = true;
      LOG_INFO("tablet slog has been written, no need to write checkpoint", K(tmp_ret), K(tablet_key));
    } else if (OB_UNLIKELY(OB_HASH_NOT_EXIST != tmp_ret)) {
      ret = tmp_ret;
      LOG_WARN("fail to check whether tablet slog has been written", K(ret), K(tablet_key));
    } else {
      has_slog = false;
    }
  }
  return ret;
}

int CheckpointSlogHandler::write_checkpoint(bool is_force)
{
  int ret = OB_SUCCESS;
  int64_t alert_interval = ObWriteCheckpointTask::FAIL_WRITE_CHECKPOINT_ALERT_INTERVAL;
  int64_t min_interval = ObWriteCheckpointTask::RETRY_WRITE_CHECKPOINT_MIN_INTERVAL;

  ObIServerRuntime *runtime = share::server_service<ObIServerRuntime>();
  omt::ObServerRuntimeMeta meta;
  ObLogCursor last_cursor;

  HEAP_VARS_3((ObServerRuntimeSuperBlock, super_block), (ObServerRuntimeSuperBlock, last_super_block), (ObLocalStorageCheckpointWriter, local_storage_ckpt_writer)) {
    //Don't compare to MERGE_SCHEDULER_PTR->get_frozen_version(), because we expect to do
    //checkpoint after merge finish.
    // 1) avoid IO traffic between ckpt and major
    // 2) truncate slog totally generated by major tablet
    const int64_t broadcast_version = MERGE_SCHEDULER_PTR->get_frozen_version();
    const int64_t frozen_version = MERGE_SCHEDULER_PTR->get_inner_table_merged_scn();
    const bool is_major_doing = (frozen_version != broadcast_version);
    bool is_writing_checkpoint_set = false;
    const int64_t start_time = ObTimeUtility::current_time();
    int64_t cost_time = 0;

    if (OB_UNLIKELY(!is_inited_)) {
      ret = OB_NOT_INIT;
      LOG_WARN("CheckpointSlogHandler not init", K(ret));
    } else {
      // we can't just return warn, since it will clean copy status before exiting
      // the only way is to wait without timeout
      while (!ATOMIC_BCAS(&is_writing_checkpoint_, false, true)) {
        if (REACH_TIME_INTERVAL(10 * 1000 * 1000)) { // 10s
          LOG_INFO("wait until last checkpoint finished");
        }
        ob_usleep(100 * 1000); // 100ms
      }
      is_writing_checkpoint_set = true;
      last_cursor = OB_STORAGE_OBJECT_MGR.get_server_super_block().body_.replay_start_point_;
      last_super_block = runtime->get_super_block();
    }
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (OB_UNLIKELY(runtime->is_hidden())) {
      LOG_INFO("bootstrap runtime is not checkpointed");
    } else if (OB_UNLIKELY(!last_super_block.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(get_cur_cursor(meta))) {
    } else if (OB_UNLIKELY(!ckpt_cursor_.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (is_force
               || (!is_major_doing && (ckpt_cursor_.file_id_ > last_cursor.file_id_))
               || ((start_time > last_ckpt_time_ + min_interval) // slog is long
                   && !is_major_doing
                   && ckpt_cursor_.newer_than(last_cursor)
                   &&(ckpt_cursor_.log_id_ - last_cursor.log_id_ >= ObWriteCheckpointTask::MIN_WRITE_CHECKPOINT_LOG_CNT))) {
      DEBUG_SYNC(AFTER_CHECKPOINT_GET_CURSOR);
      super_block = last_super_block;
      if (OB_FAIL(local_storage_ckpt_writer.init(this))) {
      } else if (OB_FAIL(local_storage_ckpt_writer.record_meta(super_block.ls_meta_entry_))) {
      }
      clean_copy_status();

      if (OB_SUCC(ret)) {
        meta.super_block_ = super_block;
        if (OB_FAIL(STORAGE_META_SERVICE.publish_checkpoint(meta, ckpt_cursor_))) {
        } else {
          runtime->set_server_super_block(super_block);
        }
      }
      if (OB_FAIL(ret)) {
        // do nothing
      } else if (OB_FAIL(update_tablet_meta_addr_and_block_list(local_storage_ckpt_writer))) {
        LOG_ERROR("fail to update_tablet_meta_addr_and_block_list", K(ret), K(last_super_block));
        // abort if failed, because it cannot be rolled back if partially success.
        // otherwise, updates need to be transactional.
        ob_usleep(1000 * 1000);
        ob_abort();
      } else if (OB_FAIL(slogger_->remove_useless_log_file(ckpt_cursor_.file_id_))) {
      } else {
        last_ckpt_time_ = start_time;
        last_frozen_version_ = frozen_version;
        cost_time = ObTimeUtility::current_time() - start_time;
      }

      int tmp_ret = OB_SUCCESS;
      if (OB_TMP_FAIL(local_storage_ckpt_writer.rollback())) {
      }

      FLOG_INFO("finish write runtime checkpoint", K(ret), K(last_super_block), K(super_block),
          K_(last_ckpt_time), K(start_time), K(broadcast_version),K(frozen_version), K_(last_frozen_version),
          K(is_force), K(cost_time));
      SERVER_EVENT_ADD("storage", "write slog checkpoint",
          "ret", ret, "cursor", ckpt_cursor_, "frozen_version", frozen_version, "cost_time(us)", cost_time);
    }
    clean_copy_status(); // in case fail after get_cur_cursor
    if (is_writing_checkpoint_set) {
      ATOMIC_STORE(&is_writing_checkpoint_, false);
    }
  }
  return ret;
}

int CheckpointSlogHandler::get_cur_cursor(omt::ObServerRuntimeMeta &meta)
{
  int ret = OB_SUCCESS;
  TCWLockGuard guard(slog_ckpt_lock_);
  tablet_key_set_.destroy();
  if (OB_FAIL(share::server_service<ObIServerRuntime>()->capture_checkpoint(*slogger_, meta, ckpt_cursor_))) {
  } else if (OB_FAIL(tablet_key_set_.create(BUCKET_NUM, ObModIds::OB_HASH_BUCKET, ObModIds::OB_HASH_BUCKET))) {
  } else {
    is_copying_tablets_ = true;
  }
  return ret;
}

void CheckpointSlogHandler::clean_copy_status()
{
  TCWLockGuard guard(slog_ckpt_lock_);
  is_copying_tablets_ = false;
  tablet_key_set_.destroy();
}

int CheckpointSlogHandler::update_tablet_meta_addr_and_block_list(
    ObLocalStorageCheckpointWriter &meta_writer)
{
  int ret = OB_SUCCESS;
  ObIArray<MacroBlockId> *meta_block_list = nullptr;
  // Keep the old checkpoint blocks referenced until tablet addresses have been replaced.
  if (OB_FAIL(meta_writer.batch_compare_and_swap_tablet())) {
  }

  TCWLockGuard guard(lock_);
  if (OB_FAIL(ret)) {
  } else {
    do {
      if (OB_FAIL(meta_writer.get_ls_block_list(meta_block_list))) {
      } else if (OB_FAIL(ls_block_handle_.add_macro_blocks(*meta_block_list))) {
      } else if (OB_FAIL(meta_writer.get_tablet_block_list(meta_block_list))) {
      } else if (OB_FAIL(tablet_block_handle_.add_macro_blocks(*meta_block_list))) {
      }
    } while (OB_ALLOCATE_MEMORY_FAILED == ret);
  }

  return ret;
}

int CheckpointSlogHandler::get_meta_block_list(ObIArray<MacroBlockId> &meta_block_list)
{
  int ret = OB_SUCCESS;
  TCRLockGuard guard(lock_);
  meta_block_list.reset();
  const ObIArray<blocksstable::MacroBlockId> &ls_block_list =
    ls_block_handle_.get_meta_block_list();
  const ObIArray<blocksstable::MacroBlockId> &tablet_block_list =
    tablet_block_handle_.get_meta_block_list();

  for (int64_t i = 0; OB_SUCC(ret) && i < ls_block_list.count(); ++i) {
    if (OB_FAIL(meta_block_list.push_back(ls_block_list.at(i)))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tablet_block_list.count(); ++i) {
    if (OB_FAIL(meta_block_list.push_back(tablet_block_list.at(i)))) {
    }
  }
  return ret;
}

int CheckpointSlogHandler::replay(const ObRedoModuleReplayParam &param)
{
  int ret = OB_SUCCESS;
  ObRedoLogMainType main_type = ObRedoLogMainType::OB_REDO_LOG_MAX;
  enum ObRedoLogSubType sub_type;
  ObIRedoModule::parse_cmd(param.cmd_, main_type, sub_type);

  if (OB_UNLIKELY(!param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObRedoLogMainType::OB_REDO_LOG_LOCAL_STORAGE != main_type) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("wrong redo log main type.", K(ret), K(main_type), K(sub_type));
  } else {
    switch (sub_type) {
    case ObRedoLogSubType::OB_REDO_LOG_UPDATE_SERVER_RESOURCES: {
      share::ObServerRuntimeConfig config;
      ObUpdateServerResourcesLog entry(config);
      int64_t pos = 0;
      if (OB_FAIL(entry.deserialize(param.buf_, param.disk_addr_.size(), pos))) {
      } else if (OB_FAIL(share::server_service<ObIServerRuntime>()->replay_server_resources(config))) {
      }
      break;
    }
    case ObRedoLogSubType::OB_REDO_LOG_UPDATE_LS: {
      if (OB_FAIL(inner_replay_update_ls_slog(param))) {
      }
      break;
    }
    case ObRedoLogSubType::OB_REDO_LOG_UPDATE_TABLET: {
      if (OB_FAIL(inner_replay_update_tablet(param))) {
      }
      break;
    }
    case ObRedoLogSubType::OB_REDO_LOG_DELETE_TABLET: {
      if (OB_FAIL(inner_replay_delete_tablet(param))) {
      }
      break;
    }
    case ObRedoLogSubType::OB_REDO_LOG_EMPTY_SHELL_TABLET: {
      if (OB_FAIL(inner_replay_empty_shell_tablet(param))) {
      }
      break;
    }

    default: {
      ret = OB_ERR_SYS;
      LOG_ERROR("wrong redo log subtype", K(ret), K(sub_type), K(param));
    }
    }
  }

  return ret;
}

int CheckpointSlogHandler::inner_replay_update_ls_slog(const ObRedoModuleReplayParam &param)
{
  int ret = OB_SUCCESS;

  ObLSMetaLog slog_entry;
  int64_t pos = 0;
  if (OB_FAIL(slog_entry.deserialize(param.buf_, param.disk_addr_.size(), pos))) {
  } else if (OB_FAIL(::oceanbase::share::server_service<::oceanbase::storage::ObLSService>()->replay_update_ls(slog_entry.get_ls_meta()))) {
  } else {
    LOG_INFO("successfully replay update ls slog", K(param), K(pos));
  }

  return ret;
}

int CheckpointSlogHandler::inner_replay_update_tablet(const ObRedoModuleReplayParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(inner_replay_deserialize(param.buf_, param.data_size_, true /*overwrite if tablet exists*/))) {
  }
  return ret;
}

int CheckpointSlogHandler::inner_replay_deserialize(
    const char *buf,
    const int64_t buf_len,
    bool allow_override /* allow to overwrite the map's element or not */)
{
  int ret = OB_SUCCESS;
  ObUpdateTabletLog slog;
  ObTabletMapKey tablet_key;
  int64_t pos = 0;
  if (OB_FAIL(slog.deserialize(buf, buf_len, pos))) {
  } else if (OB_UNLIKELY(!slog.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("slog is invalid", K(ret), K(slog));
  } else {
    tablet_key.tablet_id_ = slog.tablet_id_;
    if (OB_FAIL(replay_tablet_disk_addr_map_.set_refactored(tablet_key, slog.disk_addr_, allow_override ? 1 : 0))) {
    } else {
      LOG_INFO("Successfully load tablet meta addr for ckpt", K(slog));
    }
  }

  return ret;
}

int CheckpointSlogHandler::inner_replay_delete_tablet(const ObRedoModuleReplayParam &param)
{
  int ret = OB_SUCCESS;

  ObDeleteTabletLog slog_entry;
  int64_t pos = 0;

  if (OB_FAIL(slog_entry.deserialize(param.buf_, param.disk_addr_.size(), pos))) {
  } else {
    const ObTabletMapKey map_key(slog_entry.tablet_id_);
    if (OB_FAIL(replay_tablet_disk_addr_map_.erase_refactored(map_key)) && OB_HASH_NOT_EXIST != ret) {
    } else {
      ret = OB_SUCCESS;
      LOG_INFO("Successfully remove tablet for replay", K(param), K(slog_entry));
    }
  }

  return ret;
}

int CheckpointSlogHandler::inner_replay_empty_shell_tablet(const ObRedoModuleReplayParam &param)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  ObEmptyShellTabletLog slog;

  if (OB_FAIL(slog.deserialize_id(param.buf_, param.disk_addr_.size(), pos))) {
  } else {
    const ObTabletMapKey map_key(slog.tablet_id_);
    if (OB_FAIL(replay_tablet_disk_addr_map_.set_refactored(map_key, param.disk_addr_, 1))) {
    }
  }

  return ret;
}

int CheckpointSlogHandler::parse(
  const int32_t cmd, const char *buf, const int64_t len, FILE *stream)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  ObRedoLogMainType main_type = ObRedoLogMainType::OB_REDO_LOG_LOCAL_STORAGE;
  ObRedoLogSubType sub_type = ObRedoLogSubType::OB_REDO_LOG_INVALID;
  char slog_name[ObStorageLogReplayer::MAX_SLOG_NAME_LEN];

  ObIRedoModule::parse_cmd(cmd, main_type, sub_type);
  if (OB_ISNULL(buf) || OB_ISNULL(stream) || OB_UNLIKELY(len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(ObRedoLogMainType::OB_REDO_LOG_LOCAL_STORAGE != main_type)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("slog type does not match", K(ret), K(main_type), K(sub_type));
  } else if (OB_UNLIKELY(0 > fprintf(stream, "\nlocal storage slog: "))) {
    ret = OB_IO_ERROR;
    LOG_WARN("fail to write runtime slog to stream", K(ret));
  } else {
    switch (sub_type) {
      case ObRedoLogSubType::OB_REDO_LOG_UPDATE_SERVER_RESOURCES: {
        share::ObServerRuntimeConfig config;
        ObUpdateServerResourcesLog entry(config);
        ret = ObStorageLogReplayer::print_slog(buf, len, "update server resources slog: ", entry, stream);
        break;
      }
      case ObRedoLogSubType::OB_REDO_LOG_UPDATE_LS: {
        ObLSMetaLog slog_entry;
        snprintf(slog_name, ObStorageLogReplayer::MAX_SLOG_NAME_LEN, "update ls slog: ");
        if (OB_FAIL(ObStorageLogReplayer::print_slog(buf, len, slog_name, slog_entry, stream))) {
        }
        break;
      }
      case ObRedoLogSubType::OB_REDO_LOG_UPDATE_TABLET: {
        ObUpdateTabletLog slog_entry;
        int64_t pos = 0;
        int32_t length = 0;
        int32_t version = 0;

        snprintf(slog_name, ObStorageLogReplayer::MAX_SLOG_NAME_LEN, "update tablet slog: ");
        if (OB_FAIL(slog_entry.deserialize(buf, len, pos))) {
        } else {
          ObCStringHelper helper;
          if (0 > fprintf(stream, "%s\n version:%d length:%d\n%s\n", slog_name, version, length,
              helper.convert(slog_entry))) {
            ret = OB_IO_ERROR;
            LOG_WARN("Fail to print slog to file.", K(ret));
          }
        }
        break;
      }
      case ObRedoLogSubType::OB_REDO_LOG_DELETE_TABLET: {
        ObTabletID tablet_id;
        ObDeleteTabletLog slog_entry(tablet_id);
        snprintf(slog_name, ObStorageLogReplayer::MAX_SLOG_NAME_LEN, "delete tablet slog: ");
        if (OB_FAIL(ObStorageLogReplayer::print_slog(buf, len, slog_name, slog_entry, stream))) {
        }
        break;
      }

      case ObRedoLogSubType::OB_REDO_LOG_EMPTY_SHELL_TABLET: {
        ObEmptyShellTabletLog slog_entry;
        snprintf(slog_name, ObStorageLogReplayer::MAX_SLOG_NAME_LEN, "empty shell tablet slog: ");
        if (OB_FAIL(slog_entry.deserialize_id(buf, len, pos))) {
        } else {
          ObCStringHelper helper;
          if (0 > fprintf(stream, "%s\n%s\n", slog_name, helper.convert(slog_entry))) {
            ret = OB_IO_ERROR;
            LOG_WARN("Fail to print slog to file.", K(ret));
          }
        }
        break;
      }

      default: {
        ret = OB_ERR_SYS;
        LOG_ERROR("unknown subtype", K(ret), K(sub_type));
      }
    }
  }

  return ret;
}

int CheckpointSlogHandler::replay_over()
{
  return OB_SUCCESS; // nothing to do.
}

int CheckpointSlogHandler::read_from_slog(const ObMetaDiskAddr &addr,
    char *buf, const int64_t buf_len, int64_t &pos)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!addr.is_valid()
               || !addr.is_file()
               || buf_len < addr.size())
               || OB_ISNULL(buf)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid argument", K(ret), K(addr), KP(buf), K(buf_len));
  } else {
    // The reason for retrying, here, is that the current SLOG didn't handle the read and write
    // concurrency for the latest item, and an -4103 error will be returned. At present, the
    // optimized changes for SLOG are relatively large, and we will bypass it in the short term.
    int64_t retry_count = 2;
    do {
      int64_t tmp_pos = pos;
      if (OB_FAIL(ObStorageLogReader::read_log(slogger_->get_dir(), addr, buf_len, buf, tmp_pos))) {
        STORAGE_LOG(WARN, "fail to read slog", K(ret), "logger directory", slogger_->get_dir(), K(addr),
            K(buf_len), KP(buf));
        if (retry_count > 1) {
          sleep(1); // sleep 1s
        }
      } else {
        pos = tmp_pos;
      }
    } while (OB_FAIL(ret) && --retry_count > 0);
  }
  return ret;
}

int CheckpointSlogHandler::read_empty_shell_file(
    const ObMetaDiskAddr &addr,
    common::ObArenaAllocator &allocator,
    char *&buf,
    int64_t &buf_len)
{
  int ret = OB_SUCCESS;

  ObEmptyShellTabletLog slog;
  int64_t pos = 0;
  if (ObMetaDiskAddr::DiskType::FILE != addr.type()) {
    ret = OB_STATE_NOT_MATCH;
    STORAGE_LOG(WARN, "addr type is not correct", K(ret), K(addr));
  } else if (OB_ISNULL(buf = static_cast<char*>(allocator.alloc(addr.size())))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (FALSE_IT(buf_len = addr.size())) {
  } else if (OB_FAIL(read_from_slog(addr, buf, buf_len, pos))) {
  } else if (OB_FAIL(slog.deserialize_id(buf, buf_len, pos))) {
  } else {
    buf += pos;
    buf_len -= pos;
  }

  return ret;
}

[[nodiscard]] CheckpointSlogHandler::ObCkptSlogROptLockGuard::ObCkptSlogROptLockGuard(
    const CheckpointSlogHandler &ckpt_slog_hdl)
  : ckpt_slog_hdl_(ckpt_slog_hdl),
    ret_(OB_NOT_INIT),
    slot_id_(0)
{
  if (OB_UNLIKELY(OB_SUCCESS != (ret_ = const_cast<TCRWLock&>(ckpt_slog_hdl_.slog_ckpt_lock_).rdlock(INT64_MAX, slot_id_)))) {
    COMMON_LOG_RET(ERROR, ret_, "fail to read lock, ", K_(ret));
  }
}

CheckpointSlogHandler::ObCkptSlogROptLockGuard::~ObCkptSlogROptLockGuard()
{
  if (OB_LIKELY(OB_SUCCESS == ret_)) {
    if (OB_UNLIKELY(OB_SUCCESS != (ret_ = const_cast<TCRWLock&>(ckpt_slog_hdl_.slog_ckpt_lock_).rdunlock(slot_id_)))) {
      COMMON_LOG_RET(WARN, ret_, "Fail to unlock, ", K_(ret));
    }
  }
}

}  // end namespace storage
}  // namespace oceanbase
