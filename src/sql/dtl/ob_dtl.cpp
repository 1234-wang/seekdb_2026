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

#define USING_LOG_PREFIX SQL_DTL
#include "ob_dtl.h"
#include "sql/dtl/ob_dtl_local_channel.h"
#include "sql/dtl/ob_dtl_channel_watcher.h"

using namespace oceanbase::lib;
using namespace oceanbase::common;

namespace oceanbase {
namespace sql {
namespace dtl {

////////////////////////////////////////////////////////////////////////////
int ObDtlChannelManager::insert_channel(uint64_t hash_val, uint64_t chid, ObDtlChannel *&chan)
{
  int ret = OB_SUCCESS;
  ObLockGuard<ObSpinLock> lock_guard(spin_lock_);
  if (OB_FAIL(hash_table_.insert_channel(hash_val, chid, chan))) {
  }
  return ret;
}

int ObDtlChannelManager::remove_channel(uint64_t hash_val, uint64_t chid, ObDtlChannel *&ch)
{
  int ret = OB_SUCCESS;
  ObLockGuard<ObSpinLock> lock_guard(spin_lock_);
  if (OB_FAIL(hash_table_.remove_channel(hash_val, chid, ch))) {
  }
  return ret;
}

int ObDtlChannelManager::get_channel(uint64_t hash_val, uint64_t chid, ObDtlChannel *&ch)
{
  int ret = OB_SUCCESS;
  ObLockGuard<ObSpinLock> lock_guard(spin_lock_);
  if (OB_FAIL(hash_table_.get_channel(hash_val, chid, ch))) {
  }
  return ret;
}

int ObDtlChannelManager::foreach_refactored(int64_t interval, std::function<int(ObDtlChannel *ch)> op)
{
  int ret = OB_SUCCESS;
  int64_t bucket_num = hash_table_.get_bucket_num();
  ObLockGuard<ObSpinLock> lock_guard(spin_lock_);
  for (int64_t i = idx_; i < bucket_num && OB_SUCC(ret); i += interval) {
    if (OB_FAIL(hash_table_.foreach_refactored(i, op))) {
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////
ObDtlHashTable::~ObDtlHashTable()
{
  destroy();
}

void ObDtlHashTable::destroy()
{
  if (nullptr != bucket_cells_) {
    for (int64_t i = 0; i < bucket_num_; ++i) {
      bucket_cells_[i].~ObDtlHashTableCell();
    }
    allocator_.free(bucket_cells_);
    bucket_cells_ = nullptr;
  }
  allocator_.reset();
  bucket_num_ = 0;
}

int ObDtlHashTable::init(int64_t bucket_num)
{
  int ret = OB_SUCCESS;
  if (nullptr != bucket_cells_ || allocator_.is_inited()) {
    ret = OB_INIT_TWICE;
  } else if (bucket_num <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected bucket number", K(bucket_num));
  } else {
    ObMemAttr attr("SqlDtlMgr");
    if (OB_FAIL(allocator_.init(lib::ObMallocAllocator::get_instance(),
                               OB_MALLOC_NORMAL_BLOCK_SIZE, attr))) {
    } else {
      allocator_.set_label("SqlDtlMgr");
      bucket_cells_ = reinterpret_cast<ObDtlHashTableCell*>(
          allocator_.alloc(bucket_num * sizeof(ObDtlHashTableCell)));
      if (nullptr == bucket_cells_) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (int64_t i = 0; i < bucket_num; ++i) {
          new (bucket_cells_ + i) ObDtlHashTableCell();
        }
        bucket_num_ = bucket_num;
      }
    }
    if (OB_FAIL(ret)) {
      destroy();
    }
  }
  return ret;
}

int ObDtlHashTable::insert_channel(uint64_t hash_val, uint64_t chid, ObDtlChannel *&chan)
{
  int ret = OB_SUCCESS;
  if (nullptr == bucket_cells_) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t nth_cell = hash_val % bucket_num_;
    if (OB_FAIL(bucket_cells_[nth_cell].insert_channel(chid, chan))) {
    }
  }
  return ret;
}

int ObDtlHashTable::remove_channel(uint64_t hash_val, uint64_t chid, ObDtlChannel *&ch)
{
  int ret = OB_SUCCESS;
  if (nullptr == bucket_cells_) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t nth_cell = hash_val % bucket_num_;
    if (OB_FAIL(bucket_cells_[nth_cell].remove_channel(chid, ch))) {
    }
  }
  return ret;
}

int ObDtlHashTable::get_channel(uint64_t hash_val, uint64_t chid, ObDtlChannel *&ch)
{
  int ret = OB_SUCCESS;
  if (nullptr == bucket_cells_) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t nth_cell = hash_val % bucket_num_;
    if (OB_FAIL(bucket_cells_[nth_cell].get_channel(chid, ch))) {
    }
  }
  return ret;
}

int ObDtlHashTable::foreach_refactored(int64_t nth_cell, std::function<int(ObDtlChannel *ch)> op)
{
  int ret = OB_SUCCESS;
  if (0 > nth_cell || bucket_num_ <= nth_cell) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    if (OB_FAIL(bucket_cells_[nth_cell].foreach_refactored(op))) {
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////
int ObDtlHashTableCell::foreach_refactored(std::function<int(ObDtlChannel *ch)> op)
{
  int ret = OB_SUCCESS;
  if (0 < chan_list_.get_size()) {
    DLIST_FOREACH_X(node, chan_list_, OB_SUCC(ret)) {
      if (OB_FAIL(op(node))) {
      }
    }
  }
  return ret;
}

int ObDtlHashTableCell::insert_channel(uint64_t chid, ObDtlChannel *&chan)
{
  int ret = OB_SUCCESS;
  // first find channel by chid
  ObDtlChannel *ch = nullptr;
  if (0 < chan_list_.get_size()) {
    DLIST_FOREACH(node, chan_list_) {
      if (node->get_id() == chid) {
        ch = node;
        break;
      }
    }
  }
  if (OB_NOT_NULL(ch)) {
    ret = OB_HASH_EXIST;
  } else if (!chan_list_.add_last(chan)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObDtlHashTableCell::remove_channel(uint64_t chid, ObDtlChannel *&ch)
{
  int ret = OB_SUCCESS;
  ch = nullptr;
  if (0 < chan_list_.get_size()) {
    DLIST_FOREACH_REMOVESAFE_X(node, chan_list_, OB_SUCC(ret)) {
      if (node->get_id() == chid) {
        ObDtlChannel *tmp = chan_list_.remove(node);
        if (nullptr == tmp) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ch = node;
        }
        break;
      }
    }
  }
  if (OB_ISNULL(ch)) {
    ret = OB_HASH_NOT_EXIST;
  }
  return ret;
}

int ObDtlHashTableCell::get_channel(uint64_t chid, ObDtlChannel *&ch)
{
  int ret = OB_SUCCESS;
  ch = nullptr;
  if (0 < chan_list_.get_size()) {
    DLIST_FOREACH_X(node, chan_list_, OB_SUCC(ret)) {
      if (node->get_id() == chid) {
        ch = node;
        ch->pin();
        break;
      }
    }
  }
  if (nullptr == ch) {
    ret = OB_HASH_NOT_EXIST;
  }
  return ret;
}
////////////////////////////////////////////////////////////////////////////
ObDtl::ObDtl()
    : is_inited_(false),
      allocator_("SqlDtlMgr"),
      dfc_server_(),
      hash_table_(),
      ch_mgrs_(nullptr),
      constructed_manager_count_(0),
      owned_channel_count_(0)
{
}

ObDtl::~ObDtl()
{
  IGNORE_RETURN destroy();
}

int ObDtl::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_ || nullptr != ch_mgrs_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(dfc_server_.init())) {
  } else if (OB_FAIL(hash_table_.init(BUCKET_NUM))) {
  } else if (OB_ISNULL(ch_mgrs_ = reinterpret_cast<ObDtlChannelManager*>(
      allocator_.alloc(sizeof(ObDtlChannelManager) * HASH_CNT)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int64_t i = 0; i < HASH_CNT; ++i) {
      new (ch_mgrs_ + i) ObDtlChannelManager(i, hash_table_);
      ++constructed_manager_count_;
    }
    is_inited_ = true;
  }
  if (OB_FAIL(ret) && !is_inited_) {
    reset_resources();
  }
  return ret;
}

int ObDtl::check_channels_drained() const
{
  // Count includes channels removed from the hash but still owned by operators,
  // and channels waiting for a get_channel pin to be returned before deletion.
  return 0 == ATOMIC_LOAD(&owned_channel_count_) ? OB_SUCCESS : OB_EAGAIN;
}

int ObDtl::destroy()
{
  int ret = check_channels_drained();
  if (OB_SUCC(ret)) {
    // The singleton does not own orphaned channels. A registry entry must be
    // unlinked by its SQL/PX owner before its hash-cell storage can be reset.
    for (int64_t i = 0; i < constructed_manager_count_ && OB_SUCC(ret); ++i) {
      ret = ch_mgrs_[i].foreach_refactored(HASH_CNT,
          [](ObDtlChannel *) { return OB_EAGAIN; });
    }
    if (OB_SUCC(ret)) {
      reset_resources();
    }
  }
  return ret;
}

void ObDtl::reset_resources()
{
  for (int64_t i = 0; i < constructed_manager_count_; ++i) {
    ch_mgrs_[i].~ObDtlChannelManager();
  }
  constructed_manager_count_ = 0;
  ch_mgrs_ = nullptr;
  hash_table_.destroy();
  allocator_.reset();
  dfc_server_.destroy();
  is_inited_ = false;
}
// Remove the channel from the registry by id and destroy it after readers release their pins.
int ObDtl::destroy_channel(uint64_t chid)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObDtlChannel *chan = nullptr;
    uint64_t hash_val = get_hash_value(chid);
    ObDtlChannelManager *ch_mgr = nullptr;
    if (OB_FAIL(get_dtl_channel_manager(hash_val, ch_mgr))) {
    } else if (nullptr == ch_mgr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ch_mgr->remove_channel(hash_val, chid, chan))) {
    } else if (nullptr != chan) {
      chan->unpin();
      // spin until there's no reference of this channel.
      while (chan->get_pins() != 0) {
        // Here we add a sleep mainly to yield the cpu
        // In the sysbench px join scenario, this occupies 10% of the CPU. After yielding the CPU, it can improve by about 10%
        // sql: select  /*+ use_px */t1.pad,t2.pad,t3.pad from sbtest1 t1,sbtest5 t2,sbtest4 t3
        //         where t1.id = 503100 and t1.id=t2.id and t2.id=t3.id
        // plan:
        // |0 |NESTED-LOOP JOIN     |        |1        |147 |
        // |1 | NESTED-LOOP JOIN    |        |1        |100 |
        // |2 |  EXCHANGE IN DISTR  |        |1        |53  |
        // |3 |   EXCHANGE OUT DISTR|:EX10000|1        |52  |
        // |4 |    TABLE GET        |t1      |1        |52  |
        // |5 |  TABLE GET          |t2      |1        |47  |
        // |6 | TABLE GET           |t3      |1        |47  |
        // sleep(100): cpu .0% // cannot see
        // sleep(50 ): cpu .87%
        // sleep(10 ): cpu .88%
        ob_usleep<ObWaitEventIds::DTL_DESTROY_CHANNEL_SLEEP>(100);
      }
      //LOG_WARN("DTL delete", K(chan), K(lbt()));
      if (nullptr != chan->get_msg_watcher()) {
        chan->get_msg_watcher()->remove_data_list(chan, true);
      }
      ob_delete(chan);
    }
  }
  return ret;
}
// Data-channel release is split in two: detach it from the registry, then wait
// for local readers to release their pins before DFC cleanup and destruction.
int ObDtl::remove_channel(uint64_t chid, ObDtlChannel *&ch)
{
  int ret = OB_SUCCESS;
  ch = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObDtlChannel *chan = nullptr;
    uint64_t hash_val = get_hash_value(chid);
    ObDtlChannelManager *ch_mgr = nullptr;
    if (OB_FAIL(get_dtl_channel_manager(hash_val, ch_mgr))) {
    } else if (nullptr == ch_mgr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ch_mgr->remove_channel(hash_val, chid, chan))) {
    } else if (nullptr != chan) {
      chan->unpin();
      // spin until there's no reference of this channel.
      while (chan->get_pins() != 0) {
      }
      // DFC cleanup starts only after all local channel users release their pins.
      ch = chan;
      if (nullptr != ch->get_msg_watcher()) {
        ch->get_msg_watcher()->remove_data_list(ch, true);
      }
    }
  }
  return ret;
}
// With channel pin, encapsulated inside, did not make a separate interface to handle
int ObDtl::get_channel(uint64_t chid, ObDtlChannel *&chan)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    uint64_t hash_val = get_hash_value(chid);
    ObDtlChannelManager *ch_mgr = nullptr;
    if (OB_FAIL(get_dtl_channel_manager(hash_val, ch_mgr))) {
    } else if (nullptr == ch_mgr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ch_mgr->get_channel(hash_val, chid, chan))) {
    }
  }
  return ret;
}
// Simply used for unpinning the channel, i.e., no longer referencing
int ObDtl::release_channel(ObDtlChannel *chan)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    chan->unpin();
  }
  return ret;
}

int ObDtl::create_local_channel(uint64_t chid, ObDtlChannel *&chan, ObDtlFlowControl *dfc)
{
  int ret = OB_SUCCESS;
  // if nullptr != chan, batch free chans until link_ch_sets
  const bool need_free_chan = (nullptr == chan);
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (nullptr == chan && OB_FAIL(new_channel(chid, chan))) {
  } else if (nullptr == chan) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(init_channel(chid, chan, dfc, need_free_chan))) {
  }
  return ret;
}

int ObDtl::new_channel(uint64_t chid, ObDtlChannel *&chan)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    chan = static_cast<ObDtlChannel *> (ob_malloc(sizeof(ObDtlLocalChannel), ObMemAttr("SqlDtlChan")));
    if (nullptr != chan) {
      new (chan) ObDtlLocalChannel(chid);
    }
    if (nullptr == chan) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  return ret;
}

int ObDtl::get_dtl_channel_manager(uint64_t hash_val, ObDtlChannelManager *&ch_mgr)
{
  int ret = OB_SUCCESS;
  int64_t nth_mgr = hash_val & (HASH_CNT - 1);
  if (nth_mgr < 0 || nth_mgr > HASH_CNT) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ch_mgr = &ch_mgrs_[nth_mgr];
  }
  return ret;
}

int ObDtl::init_channel(uint64_t chid, ObDtlChannel *&chan,
                        ObDtlFlowControl *dfc, const bool need_free_chan)
{
  int ret = OB_SUCCESS;
  if (nullptr == chan) {
    ret = OB_ERR_UNEXPECTED;
  } else if (nullptr != chan->dtl_owner_ && this != chan->dtl_owner_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    if (nullptr == chan->dtl_owner_) {
      ATOMIC_INC(&owned_channel_count_);
      chan->dtl_owner_ = this;
    }
    if (OB_FAIL(chan->init())) {
    }
  }
  if (OB_SUCC(ret)) {
    if (nullptr != dfc) {
      // Register DFC before publishing the channel so every local user observes
      // the channel and its flow-control state atomically.
      if (OB_FAIL(dfc_server_.register_dfc_channel(*dfc, chan))) {
      }
    }
    if (OB_SUCC(ret)) {
      IGNORE_RETURN chan->pin();
      uint64_t hash_val = get_hash_value(chid);
      ObDtlChannelManager *ch_mgr = nullptr;
      if (OB_FAIL(get_dtl_channel_manager(hash_val, ch_mgr))) {
      } else if (nullptr == ch_mgr) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ch_mgr->insert_channel(hash_val, chid, chan))) {
      }
    }
  }
  if (OB_FAIL(ret) && nullptr != chan) {
    if (nullptr != dfc) {
      // Note error codes are not overwritten
      int tmp_ret = OB_SUCCESS;
      // If registered to dfc before, must unregister, otherwise the channel in dfc will be an invalid address
      if (OB_SUCCESS != (tmp_ret = dfc_server_.unregister_dfc_channel(*dfc, chan))) {
        ret = tmp_ret;
      }
    }
    if (need_free_chan) {
      ob_delete(chan);
    }
    chan = nullptr;
  }
  return ret;
}

int ObDtl::foreach_refactored(std::function<int(ObDtlChannel *ch)> op)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  }
  for (int64_t i = 0; i < HASH_CNT && OB_SUCC(ret); ++i) {
    if (OB_FAIL(ch_mgrs_[i].foreach_refactored(HASH_CNT, op))) {
    }
  }
  return ret;
}

ObDtl *ObDtl::instance()
{
  static ObDtl *instance_ = nullptr;
  if (nullptr == instance_) {
    instance_ = static_cast<ObDtl *> (ob_malloc(sizeof(ObDtl), ObMemAttr("SqlDtlMgr")));
    if (nullptr != instance_) {
      new (instance_) ObDtl();
    }
  }
  return instance_;
}

}  // dtl
}  // sql
}  // oceanbase
