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

#include "common/log/ob_log_cursor.h"
#include "lib/task/ob_timer.h"
#include "share/ob_server_struct.h"
#include "storage/slog_ckpt/ob_linked_macro_block_struct.h"
#include "storage/slog_ckpt/ob_local_storage_checkpoint_reader.h"
#include "storage/meta_mem/ob_tablet_map_key.h"
#include "storage/ob_super_block_struct.h"
#include "storage/meta_store/ob_server_runtime_meta.h"
#include "storage/slog/ob_storage_log_replayer.h"
#include "storage/ls/ob_ls_meta.h"

namespace oceanbase
{

namespace storage
{
class ObServerRuntimeSuperBlock;
struct ObMetaDiskAddr;
class ObLocalStorageCheckpointWriter;
class ObRedoModuleReplayParam;
class ObStorageLogger;
class ObStartupAccelTaskHandler;

struct ObLSCkptMember final
{
public:
  static const int64_t LS_CKPT_MEM_VERSION = 1;
  ObLSCkptMember();
  ~ObLSCkptMember();
  DISALLOW_COPY_AND_ASSIGN(ObLSCkptMember);
  void reset();

  bool is_valid() const;
  int serialize(char *buf, const int64_t len, int64_t &pos) const;
  int deserialize(
      const char *buf,
      const int64_t len,
      int64_t &pos);
  int64_t get_serialize_size() const;
  TO_STRING_KV(K_(version), K_(ls_meta), K_(tablet_meta_entry));

public:
  int32_t version_;
  int32_t length_;
  ObLSMeta ls_meta_;
  blocksstable::MacroBlockId tablet_meta_entry_;
};

class CheckpointSlogHandler : public ObIRedoModule
{
public:
  class ObWriteCheckpointTask : public common::ObTimerTask
  {
  public:
    static const int64_t FAIL_WRITE_CHECKPOINT_ALERT_INTERVAL = 1000L * 1000L * 3600LL;  // 6h
    static const int64_t WRITE_CHECKPOINT_INTERVAL_US = 1000L * 1000L * 60L;             // 1min
    static const int64_t RETRY_WRITE_CHECKPOINT_MIN_INTERVAL = 1000L * 1000L * 300L;     // 5min
    static const int64_t MIN_WRITE_CHECKPOINT_LOG_CNT = 10000; // TODO(fenggu)

    explicit ObWriteCheckpointTask(CheckpointSlogHandler *handler) : handler_(handler)
    {
      disable_timeout_check();
    }
    virtual ~ObWriteCheckpointTask() = default;
    virtual void runTimerTask() override;

  private:
    CheckpointSlogHandler *handler_;
  };
  class ObCkptSlogROptLockGuard
  {
  public:
    [[nodiscard]] explicit ObCkptSlogROptLockGuard(const CheckpointSlogHandler &ckpt_slog_hdl);
    ~ObCkptSlogROptLockGuard();
    inline int get_ret() const { return ret_; }
  private:
    const CheckpointSlogHandler &ckpt_slog_hdl_;
    int ret_;
    int64_t slot_id_;
  private:
    DISALLOW_COPY_AND_ASSIGN(ObCkptSlogROptLockGuard);
  };

  CheckpointSlogHandler();
  ~CheckpointSlogHandler() = default;
  CheckpointSlogHandler(const CheckpointSlogHandler &) = delete;
  CheckpointSlogHandler &operator=(const CheckpointSlogHandler &) = delete;

  int init(ObStorageLogger &slogger);
  int start();
  void stop();
  void wait();
  void destroy();
  int start_replay(const ObServerRuntimeSuperBlock &super_block);

  int report_slog(const ObTabletMapKey &tablet_key, const ObMetaDiskAddr &slog_addr);
  int check_slog(const ObTabletMapKey &tablet_key, bool &has_slog);

  int get_meta_block_list(common::ObIArray<blocksstable::MacroBlockId> &block_list);
  virtual int replay(const ObRedoModuleReplayParam &param) override;
  virtual int replay_over() override;
  int write_checkpoint(bool is_force);
  int read_empty_shell_file(const ObMetaDiskAddr &phy_addr, common::ObArenaAllocator &allocator, char *&buf, int64_t &buf_len);
private:
  int get_cur_cursor(omt::ObServerRuntimeMeta &meta);
  void clean_copy_status();
  virtual int parse(const int32_t cmd, const char *buf, const int64_t len, FILE *stream) override;
  int replay_checkpoint_and_slog(const ObServerRuntimeSuperBlock &super_block);
  int replay_checkpoint(const ObServerRuntimeSuperBlock &super_block);
  int replay_checkpoint_meta(const ObServerRuntimeSuperBlock &super_block);
  int replay_checkpoint_ls(
      const ObMetaDiskAddr &addr,
      const char *buf,
      const int64_t buf_len,
      ObIArray<blocksstable::MacroBlockId> &tablet_block_list);
  int replay_checkpoint_tablet(const ObMetaDiskAddr &addr, const char *buf, const int64_t buf_len);
  int update_tablet_meta_addr_and_block_list(ObLocalStorageCheckpointWriter &ckpt_writer);
  int replay_local_storage_slog(const common::ObLogCursor &start_point);
  int inner_replay_update_ls_slog(const ObRedoModuleReplayParam &param);
  int inner_replay_update_tablet(const ObRedoModuleReplayParam &param);
  int inner_replay_delete_tablet(const ObRedoModuleReplayParam &param);
  int inner_replay_empty_shell_tablet(const ObRedoModuleReplayParam &param);
  int read_from_slog(const ObMetaDiskAddr &phy_addr, char *buf, const int64_t buf_len, int64_t &pos);
  int inner_replay_deserialize(
      const char *buf,
      const int64_t buf_len,
      bool allow_override /* allow to overwrite the map's element or not */);
private:
  const static int64_t BUCKET_NUM = 109;
private:
  typedef common::hash::ObHashMap<ObTabletMapKey, ObMetaDiskAddr> ReplayTabletDiskAddrMap;
  bool is_inited_;
  bool is_writing_checkpoint_;
  int64_t last_ckpt_time_;
  int64_t last_frozen_version_;
  ObStorageLogger *slogger_;
  common::TCRWLock lock_;  // protect block_handle
  common::TCRWLock slog_ckpt_lock_; // protect is_copying_tablets_
  common::hash::ObHashSet<ObTabletMapKey> tablet_key_set_;
  bool is_copying_tablets_;
  ObLogCursor ckpt_cursor_;
  ObMetaBlockListHandle ls_block_handle_;
  ObMetaBlockListHandle tablet_block_handle_;

  common::ObTimer write_ckpt_timer_;
  ObWriteCheckpointTask write_ckpt_task_;
  ReplayTabletDiskAddrMap replay_tablet_disk_addr_map_;
};

}  // end namespace storage
}  // namespace oceanbase
