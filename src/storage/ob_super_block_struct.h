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

#ifndef OB_STORAGE_SUPER_BLOCK_STRUCT_H_
#define OB_STORAGE_SUPER_BLOCK_STRUCT_H_

#include "common/log/ob_log_cursor.h"
#include "storage/blocksstable/ob_macro_block_id.h"
#include "share/ob_ls_id.h"
#include "common/ob_tablet_id.h"
#include "storage/meta_mem/ob_meta_obj_struct.h"

namespace oceanbase
{
namespace blocksstable {
class ObStorageObjectOpt;
}
namespace storage
{

enum GCTabletType
{
  InvalidType = -1,
  DropTablet = 0,
  CreateAbort = 1,
  DropLS = 2
};

struct ObServerSuperBlockHeader final
{
public:
  static const int32_t SERVER_SUPER_BLOCK_VERSION = 1;
  static const int64_t OB_MAX_SUPER_BLOCK_SIZE = 64 * 1024;

  ObServerSuperBlockHeader();
  ~ObServerSuperBlockHeader() = default;
  bool is_valid() const;
  void reset();
  TO_STRING_KV(K_(version), K_(magic), K_(body_size), K_(body_crc));
  NEED_SERIALIZE_AND_DESERIALIZE;

  int32_t version_;
  int32_t magic_;
  int32_t body_size_;
  int32_t body_crc_;
};

struct ServerSuperBlockBody final
{
public:
  static const int64_t SUPER_BLOCK_BODY_VERSION = 1;

  int64_t create_timestamp_;  // create timestamp
  int64_t modify_timestamp_;  // last modified timestamp
  int64_t macro_block_size_;

  int64_t total_macro_block_count_;
  int64_t total_file_size_;
  // Local recovery cursor and checkpoint entry for the process-wide server runtime.
  common::ObLogCursor replay_start_point_;
  blocksstable::MacroBlockId runtime_meta_entry_;

  ServerSuperBlockBody();
  bool is_valid() const;
  void reset();

  TO_STRING_KV("Type", "ObServerSuperBlockBody",
               K_(create_timestamp),
               K_(modify_timestamp),
               K_(macro_block_size),
               K_(total_macro_block_count),
               K_(total_file_size),
               K_(replay_start_point),
               K_(runtime_meta_entry));

  OB_UNIS_VERSION(SUPER_BLOCK_BODY_VERSION);
};

struct ObServerSuperBlock final
{
public:

  ObServerSuperBlock();
  ~ObServerSuperBlock() = default;

  // represents an entry to an empty linked list, distinguished with the invalid macro block id
  static const blocksstable::MacroBlockId EMPTY_LIST_ENTRY_BLOCK;

  bool is_valid() const;
  void reset();
  NEED_SERIALIZE_AND_DESERIALIZE;
  TO_STRING_KV(K_(header), K_(body));

  OB_INLINE int64_t get_macro_block_size() const
  {
    return body_.macro_block_size_;
  }
  OB_INLINE int64_t get_total_macro_block_count() const
  {
    return body_.total_macro_block_count_;
  }
  OB_INLINE int64_t get_super_block_size() const
  {
    return header_.get_serialize_size() + body_.get_serialize_size();
  }
  int construct_header();
  int format_startup_super_block(const int64_t macro_block_size, const int64_t data_file_size);

  ObServerSuperBlockHeader header_;
  ServerSuperBlockBody body_;
};

struct ObServerRuntimeSuperBlock final
{
public:
  static const int64_t SERVER_RUNTIME_SUPER_BLOCK_VERSION = 4;
  ObServerRuntimeSuperBlock();
  ObServerRuntimeSuperBlock(const bool is_hidden);
  ~ObServerRuntimeSuperBlock() = default;
  ObServerRuntimeSuperBlock(const ObServerRuntimeSuperBlock &other);
  ObServerRuntimeSuperBlock &operator==(const ObServerRuntimeSuperBlock &other) = delete;
  ObServerRuntimeSuperBlock &operator!=(const ObServerRuntimeSuperBlock &other) = delete;
  void reset();
  bool is_valid() const;

  TO_STRING_KV(
               K_(replay_start_point),
               K_(ls_meta_entry),
               K_(is_hidden),
               K_(version));

  OB_UNIS_VERSION(SERVER_RUNTIME_SUPER_BLOCK_VERSION);
public:
  
  // Local recovery state for LS and tablet metadata.
  common::ObLogCursor replay_start_point_;
  blocksstable::MacroBlockId ls_meta_entry_;

  bool is_hidden_;
  int64_t version_;
};

#define IS_EMPTY_BLOCK_LIST(entry_block) (entry_block == oceanbase::storage::ObServerSuperBlock::EMPTY_LIST_ENTRY_BLOCK)
// Due to the design of slog, the log_id_'s initial value must be 1
#define SET_FIRST_VALID_SLOG_CURSOR(cursor) (set_cursor(cursor, 1/*file_id*/, 1/*log_id*/, 0/*offset*/))

}  // end namespace storage
}  // end namespace oceanbase

#endif  // OB_STORAGE_SUPER_BLOCK_STRUCT_H_
