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

#include "config_bridge.h"
#include "ob_local_storage_meta_service.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/meta_store/storage_meta_service.h"
#include "storage/api/storage/runtime/ob_i_server_runtime.h"
#include "storage/ob_file_system_router.h"
#include "storage/tablet/ob_tablet_macro_info_iterator.h"
#include "storage/ls/ob_ls.h"

namespace oceanbase
{
using namespace compaction;
namespace storage
{

ObLocalStorageMetaService::ObLocalStorageMetaService()
  : is_inited_(false),
    is_started_(false),
    object_rwriter_(),
    object_raw_rwriter_()
{}

int ObLocalStorageMetaService::server_module_init(ObLocalStorageMetaService *&meta_service)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(meta_service->init())) {
  }
  return ret;
}

int ObLocalStorageMetaService::init()
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(object_rwriter_.init())) {
  } else if (OB_FAIL(object_raw_rwriter_.init())) {
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObLocalStorageMetaService::start()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    is_started_ = true;
  }
  return ret;
}

void ObLocalStorageMetaService::stop()
{
  if (IS_INIT) {
    is_started_ = false;
  }
}

void ObLocalStorageMetaService::wait() {}

void ObLocalStorageMetaService::destroy()
{
  object_rwriter_.reset();
  object_raw_rwriter_.reset();

  is_started_ = false;
  is_inited_ = false;
}

int ObLocalStorageMetaService::read_from_disk(
    const ObMetaDiskAddr &addr,
    common::ObArenaAllocator &allocator,
    char *&buf,
    int64_t &buf_len)
{
  int ret = OB_SUCCESS;
  if (addr.is_local()) {
    if (OB_FAIL(STORAGE_META_SERVICE.read_empty_shell(addr, allocator, buf, buf_len))) {
    }
  } else {
    if (OB_FAIL(read_from_block(addr, allocator, buf, buf_len))) {
    }
  }
  return ret;
}

int ObLocalStorageMetaService::read_from_block(
    const ObMetaDiskAddr &addr,
    common::ObArenaAllocator &allocator,
    char *&buf,
    int64_t &buf_len)
{
  int ret = OB_SUCCESS;
  ObObjectReadHandle read_handle(allocator);
  ObObjectReadInfo read_info;
  read_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);
  read_info.io_timeout_ms_ = config::_data_storage_io_timeout() / 1000;
  read_info.addr_ = addr;
  if (OB_FAIL(ObObjectReaderWriter::async_read(read_info, read_handle))) {
  } else if (OB_FAIL(read_handle.wait())) {
  } else if (OB_FAIL(read_handle.get_data(allocator, buf, buf_len))) {
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
