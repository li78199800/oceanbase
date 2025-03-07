/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX STORAGE

#include "storage/tablet/ob_tablet.h"

#include "common/sql_mode/ob_sql_mode_utils.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/lock/ob_thread_cond.h"
#include "lib/objectpool/ob_server_object_pool.h"
#include "lib/utility/ob_macro_utils.h"
#include "logservice/ob_log_base_header.h"
#include "logservice/ob_log_base_type.h"
#include "logservice/palf/palf_options.h"
#include "share/ob_ls_id.h"
#include "share/ob_rpc_struct.h"
#include "share/ob_tablet_autoincrement_param.h"
#include "share/rc/ob_tenant_base.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_tenant_schema_service.h"
#include "share/ob_tablet_autoincrement_service.h"
#include "storage/access/ob_dml_param.h"
#include "storage/ob_dml_running_ctx.h"
#include "storage/ob_i_store.h"
#include "storage/ob_i_table.h"
#include "storage/ob_row_reshape.h"
#include "storage/ob_sync_tablet_seq_clog.h"
#include "storage/ob_storage_schema.h"
#include "storage/blocksstable/ob_sstable.h"
#include "storage/blocksstable/ob_sstable_sec_meta_iterator.h"
#include "storage/compaction/ob_tenant_freeze_info_mgr.h"
#include "storage/memtable/ob_memtable.h"
#include "storage/meta_mem/ob_tenant_meta_mem_mgr.h"
#include "storage/meta_mem/ob_meta_obj_struct.h"
#include "storage/access/ob_table_scan_iterator.h"
#include "storage/access/ob_rows_info.h"
#include "storage/ddl/ob_ddl_clog.h"
#include "storage/ddl/ob_ddl_struct.h"
#include "storage/ddl/ob_tablet_ddl_kv_mgr.h"
#include "storage/ls/ob_ls_tablet_service.h"
#include "storage/tablet/ob_tablet_common.h"
#include "storage/tablet/ob_tablet_create_delete_helper.h"
#include "storage/tablet/ob_tablet_memtable_mgr.h"
#include "storage/tablet/ob_tablet_ddl_info.h"
#include "storage/tx/ob_trans_part_ctx.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/tx_storage/ob_ls_service.h"

namespace oceanbase
{
using namespace memtable;
using namespace share;
using namespace share::schema;
using namespace blocksstable;
using namespace logservice;

namespace storage
{
ObTablet::ObTablet()
  : version_(TABLET_VERSION),
    length_(0),
    wash_score_(INT64_MIN),
    ref_cnt_(0),
    pointer_hdl_(),
    tablet_meta_(),
    table_store_(),
    storage_schema_(),
    medium_info_list_(compaction::ObMediumCompactionInfoList::MEDIUM_LIST_IN_STORAGE),
    memtable_mgr_(nullptr),
    log_handler_(nullptr),
    table_store_lock_(),
    full_read_info_(),
    allocator_(nullptr),
    next_tablet_guard_(),
    is_inited_(false)
{
#if defined(__x86_64__)
  static_assert(sizeof(ObTablet) <= 2432, "The size of ObTablet will affect the meta memory manager, and the necessity of adding new fields needs to be considered.");
#endif
}

ObTablet::~ObTablet()
{
  reset();
}

void ObTablet::reset()
{
  FLOG_INFO("reset tablet", KP(this));
  wash_score_ = INT64_MIN;
  tablet_meta_.reset();
  table_store_.reset();
  storage_schema_.reset();
  medium_info_list_.reset();
  memtable_mgr_ = nullptr;
  log_handler_ = nullptr;
  pointer_hdl_.reset();
  full_read_info_.reset();
  next_tablet_guard_.reset();
  allocator_ = nullptr;
  is_inited_ = false;
}

int ObTablet::init(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const common::ObTabletID &data_tablet_id,
    const common::ObTabletID &lob_meta_tablet_id,
    const common::ObTabletID &lob_piece_tablet_id,
    const int64_t create_scn,
    const int64_t snapshot_version,
    const share::schema::ObTableSchema &table_schema,
    const lib::Worker::CompatMode compat_mode,
    const ObTabletTableStoreFlag &store_flag,
    ObTableHandleV2 &table_handle,
    ObFreezer *freezer)
{
  int ret = OB_SUCCESS;
  allocator_ = &(MTL(ObTenantMetaMemMgr*)->get_tenant_allocator());

  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!ls_id.is_valid())
      || OB_UNLIKELY(!tablet_id.is_valid())
      || OB_UNLIKELY(!data_tablet_id.is_valid())
      //|| OB_UNLIKELY(create_scn <= OB_INVALID_TIMESTAMP)
      || OB_UNLIKELY(OB_INVALID_VERSION == snapshot_version)
      || OB_UNLIKELY(!table_schema.is_valid())
      || OB_UNLIKELY(lib::Worker::CompatMode::INVALID == compat_mode)
      || OB_ISNULL(freezer)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(ls_id), K(tablet_id), K(data_tablet_id),
        K(create_scn), K(snapshot_version), K(table_schema), K(compat_mode), KP(freezer));
  } else if (OB_UNLIKELY(!pointer_hdl_.is_valid())
      || OB_ISNULL(memtable_mgr_)
      || OB_ISNULL(log_handler_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet pointer handle is invalid", K(ret), K_(pointer_hdl), K_(memtable_mgr), K_(log_handler));
  } else if (OB_FAIL(init_shared_params(ls_id, tablet_id, table_schema.get_schema_version(), freezer))) {
    LOG_WARN("failed to init shared params", K(ret), K(ls_id), K(tablet_id), KP(freezer));
  } else if (OB_FAIL(tablet_meta_.init(*allocator_, ls_id, tablet_id, data_tablet_id,
      lob_meta_tablet_id, lob_piece_tablet_id,
      create_scn, snapshot_version, compat_mode, store_flag, table_schema.get_schema_version()))) {
    LOG_WARN("failed to init tablet meta", K(ret), K(ls_id), K(tablet_id), K(data_tablet_id),
        K(lob_meta_tablet_id), K(lob_piece_tablet_id),
        K(create_scn), K(snapshot_version), K(compat_mode), K(store_flag));
  } else if (is_ls_inner_tablet() && OB_FAIL(inner_create_memtable())) {
    LOG_WARN("failed to create first memtable", K(ret), K(tablet_id));
  } else if (OB_FAIL(table_store_.init(*allocator_, this, &table_handle))) {
    LOG_WARN("failed to init table store", K(ret), K(table_handle));
  } else if (OB_FAIL(storage_schema_.init(*allocator_, table_schema, compat_mode))) {
    LOG_WARN("failed to init storage schema", K(ret), K(table_schema), K(compat_mode));
  } else if (OB_FAIL(medium_info_list_.init(*allocator_, nullptr))) {
    LOG_WARN("failed to init medium info list", K(ret));
  } else if (OB_FAIL(build_read_info(*allocator_))) {
    LOG_WARN("failed to build read info", K(ret));
  } else if (OB_FAIL(pre_transform_sstable_root_block(*full_read_info_.get_index_read_info()))) {
    LOG_WARN("failed to pre-transform sstable root block", K(ret), K(full_read_info_));
  } else {
    is_inited_ = true;
    LOG_INFO("succeeded to init tablet", K(ret), KP(this), K(ls_id), K(tablet_id), K(data_tablet_id),
        K(snapshot_version), K(table_schema), K(compat_mode), K(lob_meta_tablet_id), K(lob_piece_tablet_id),
        K(table_handle));
  }

  if (OB_UNLIKELY(!is_inited_)) {
    reset();
  }

  return ret;
}

int ObTablet::init(
    const ObUpdateTableStoreParam &param,
    const ObTablet &old_tablet,
    const ObTabletTxMultiSourceDataUnit &tx_data,
    const ObTabletBindingInfo &ddl_data,
    const ObTabletAutoincSeq &autoinc_seq)
{
  int ret = OB_SUCCESS;
  int64_t max_sync_schema_version = 0;
  int64_t input_max_sync_schema_version = 0;
  allocator_ = &(MTL(ObTenantMetaMemMgr*)->get_tenant_allocator());
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!param.is_valid())
      || OB_UNLIKELY(!old_tablet.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(param), K(old_tablet));
  } else if (OB_UNLIKELY(!pointer_hdl_.is_valid())
      || OB_ISNULL(memtable_mgr_)
      || OB_ISNULL(log_handler_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet pointer handle is invalid", K(ret), K_(pointer_hdl), K_(memtable_mgr), K_(log_handler));
  } else if (OB_FAIL(old_tablet.get_max_sync_storage_schema_version(max_sync_schema_version))) {
    LOG_WARN("failed to get max sync storage schema version", K(ret));
  } else if (FALSE_IT(input_max_sync_schema_version = MIN(MAX(param.storage_schema_->schema_version_,
      old_tablet.storage_schema_.schema_version_), max_sync_schema_version))) {
    // use min schema version to avoid lose storage_schema in replay/reboot
  } else if (OB_FAIL(tablet_meta_.init(*allocator_, old_tablet.tablet_meta_,
      param.snapshot_version_, param.multi_version_start_,
      tx_data, ddl_data, autoinc_seq, input_max_sync_schema_version,
      param.clog_checkpoint_ts_, param.ddl_checkpoint_ts_, param.ddl_start_log_ts_, param.ddl_snapshot_version_))) {
    LOG_WARN("failed to init tablet meta", K(ret), K(old_tablet), K(param),
        K(tx_data), K(ddl_data), K(autoinc_seq), K(input_max_sync_schema_version));
  } else if (OB_FAIL(table_store_.init(*allocator_, this, param, old_tablet.table_store_))) {
    LOG_WARN("failed to init table store", K(ret), K(old_tablet));
  } else if (OB_FAIL(choose_and_save_storage_schema(*allocator_, old_tablet.storage_schema_, *param.storage_schema_))) {
    LOG_WARN("failed to choose and save storage schema", K(ret), K(old_tablet), K(param));
  } else if (OB_FAIL(try_update_start_scn())) {
    LOG_WARN("failed to update start scn", K(ret), K(param), K(table_store_));
  } else if (OB_FAIL(try_update_ddl_checkpoint_ts())) {
    LOG_WARN("failed to update clog checkpoint ts", K(ret), K(param), K(table_store_));
  } else if (OB_FAIL(try_update_table_store_flag(param))) {
    LOG_WARN("failed to update table store flag", K(ret), K(param), K(table_store_));
  } else if (OB_FAIL(medium_info_list_.init(*allocator_, &(old_tablet.get_medium_compaction_info_list())))) {
    LOG_WARN("failed to init medium info list", K(ret));
  } else if (OB_FAIL(build_read_info(*allocator_))) {
    LOG_WARN("failed to build read info", K(ret));
  } else if (OB_FAIL(pre_transform_sstable_root_block(*full_read_info_.get_index_read_info()))) {
    LOG_WARN("failed to pre-transform sstable root block", K(ret), K(full_read_info_));
  } else if (OB_FAIL(check_max_sync_schema_version())) {
    LOG_WARN("unexpected max sync schema version", K(ret), K(param), K(old_tablet),
        K(max_sync_schema_version), K(storage_schema_));
  } else {
    if (old_tablet.get_tablet_meta().has_next_tablet_) {
      set_next_tablet_guard(old_tablet.next_tablet_guard_);
    }
    is_inited_ = true;
    LOG_INFO("succeeded to init tablet", K(ret), K(param), K(old_tablet), K(tx_data), K(ddl_data), K(autoinc_seq), KPC(this));
  }

  if (OB_SUCC(ret) && param.need_report_ && param.table_handle_.get_table()->is_major_sstable()) {
    const ObSSTable *major_table = static_cast<const ObSSTable *>(param.table_handle_.get_table());
    int tmp_ret = OB_SUCCESS;
    if (OB_ISNULL(major_table)) { // init tablet with no major table, skip to init report info
    } else if (OB_TMP_FAIL(ObTabletMeta::init_report_info(major_table,
        old_tablet.tablet_meta_.report_status_.cur_report_version_, tablet_meta_.report_status_))) {
      LOG_WARN("failed to init report info", K(tmp_ret));
    }
  }


  if (OB_UNLIKELY(!is_inited_)) {
    reset();
  }

  return ret;
}

int ObTablet::init(
    const ObMigrationTabletParam &param,
    const bool is_update,
    ObFreezer *freezer)
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = param.ls_id_;
  const common::ObTabletID &tablet_id = param.tablet_id_;
  allocator_ = &(MTL(ObTenantMetaMemMgr*)->get_tenant_allocator());

  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!param.is_valid())
      || OB_ISNULL(freezer)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(param), KP(freezer));
  } else if (OB_UNLIKELY(!pointer_hdl_.is_valid())
      || OB_ISNULL(memtable_mgr_)
      || OB_ISNULL(log_handler_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet pointer handle is invalid", K(ret), K_(pointer_hdl), K_(memtable_mgr), K_(log_handler));
  } else if (is_update
      && !tablet_id.is_ls_inner_tablet()
      && OB_FAIL(init_storage_related_member(ls_id, tablet_id, param.max_sync_storage_schema_version_))) {
    LOG_WARN("failed to init storage related member", K(ret), K(ls_id), K(tablet_id));
  } else if (!is_update && OB_FAIL(init_shared_params(ls_id, tablet_id, param.max_sync_storage_schema_version_, freezer))) {
    LOG_WARN("failed to init shared params", K(ret), K(ls_id), K(tablet_id), KP(freezer));
  } else if (OB_FAIL(tablet_meta_.init(*allocator_, param))) {
    LOG_WARN("failed to init tablet meta", K(ret), K(param));
  } else if (OB_FAIL(table_store_.init(*allocator_, this, nullptr/*ObTableHandleV2*/))) {
    LOG_WARN("failed to init table store", K(ret));
  } else if (OB_FAIL(storage_schema_.init(*allocator_, param.storage_schema_))) {
    LOG_WARN("failed to init storage schema", K(ret), K(param));
  } else if (OB_FAIL(medium_info_list_.init(*allocator_, nullptr))) {
    LOG_WARN("failed to init medium info list", K(ret));
  } else if (OB_FAIL(build_read_info(*allocator_))) {
    LOG_WARN("failed to build read info", K(ret), K(param));
  } else if (OB_FAIL(pre_transform_sstable_root_block(*full_read_info_.get_index_read_info()))) {
    LOG_WARN("failed to pre-transform sstable root block", K(ret), K(full_read_info_));
  } else if (OB_FAIL(check_max_sync_schema_version())) {
    LOG_WARN("unexpected max sync schema version", K(ret), K(param), K(is_update), K(storage_schema_));
  } else {
    is_inited_ = true;
    LOG_INFO("succeeded to init tablet", K(ret), K(param), KPC(this));
  }

  if (OB_UNLIKELY(!is_inited_)) {
    reset();
  }

  return ret;
}

int ObTablet::init(
    const ObBatchUpdateTableStoreParam &param,
    const ObTablet &old_tablet,
    const ObTabletTxMultiSourceDataUnit &tx_data,
    const ObTabletBindingInfo &ddl_data,
    const ObTabletAutoincSeq &autoinc_seq)
{
  int ret = OB_SUCCESS;
  allocator_ = &(MTL(ObTenantMetaMemMgr*)->get_tenant_allocator());
  int64_t max_sync_schema_version = 0;
  const ObStorageSchema *storage_schema = nullptr;

  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!param.is_valid()) || OB_UNLIKELY(!old_tablet.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(param), K(old_tablet));
  } else if (OB_UNLIKELY(!pointer_hdl_.is_valid())
      || OB_ISNULL(memtable_mgr_)
      || OB_ISNULL(log_handler_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet pointer handle is invalid", K(ret), K_(pointer_hdl), K_(memtable_mgr), K_(log_handler));
  } else if (OB_FAIL(old_tablet.get_max_sync_storage_schema_version(max_sync_schema_version))) {
    LOG_WARN("failed to get max sync storage schema version", K(ret));
  } else if (FALSE_IT(storage_schema = OB_ISNULL(param.tablet_meta_) ? &old_tablet.storage_schema_ : &param.tablet_meta_->storage_schema_)) {
  } else if (OB_FAIL(tablet_meta_.init(*allocator_, old_tablet.tablet_meta_, tx_data, ddl_data, autoinc_seq, param.tablet_meta_
      // this interface for migration to batch update table store
      // use old tablet clog_checkpoint_ts to avoid lose tx data
      // use max schema to makesure sstable and schema match
     ))) {
    LOG_WARN("failed to init tablet meta", K(ret), K(old_tablet), K(param), K(tx_data), K(ddl_data), K(autoinc_seq));
  } else if (OB_FAIL(table_store_.build_ha_new_table_store(*allocator_, this, param, old_tablet.table_store_))) {
    LOG_WARN("failed to init table store", K(ret), K(old_tablet));
  } else if (OB_FAIL(choose_and_save_storage_schema(*allocator_, old_tablet.storage_schema_, *storage_schema))) {
    LOG_WARN("failed to choose and save storage schema", K(ret), K(old_tablet), K(param));
  } else if (OB_FAIL(try_update_start_scn())) {
    LOG_WARN("failed to update start scn", K(ret), K(param), K(table_store_));
    //This interface should not try_update_ddl_checkpoint_ts
    //Bug : 45542552
  } else if (OB_FAIL(medium_info_list_.init(*allocator_, &(old_tablet.get_medium_compaction_info_list())))) {
    LOG_WARN("failed to init medium info list", K(ret));
  } else if (OB_FAIL(build_read_info(*allocator_))) {
    LOG_WARN("failed to build read info", K(ret));
  } else if (OB_FAIL(pre_transform_sstable_root_block(*full_read_info_.get_index_read_info()))) {
    LOG_WARN("failed to pre-transform sstable root block", K(ret), K(full_read_info_));
  } else if (OB_FAIL(check_max_sync_schema_version())) {
    LOG_WARN("unexpected max sync schema version", K(ret), K(param), K(old_tablet), K(storage_schema_));
  } else {
    if (old_tablet.get_tablet_meta().has_next_tablet_) {
      set_next_tablet_guard(old_tablet.next_tablet_guard_);
    }
    is_inited_ = true;
    LOG_INFO("succeeded to init tablet", K(ret), K(param), K(old_tablet), KPC(this));
  }

  if (OB_SUCC(ret)) {
    const ObSSTable *last_major = static_cast<const ObSSTable *>(table_store_.get_major_sstables().get_boundary_table(true/*last*/));
    int tmp_ret = OB_SUCCESS;
    if (OB_ISNULL(last_major)) { // init tablet with no major table, skip to init report info
    } else if (OB_TMP_FAIL(ObTabletMeta::init_report_info(last_major,
      old_tablet.tablet_meta_.report_status_.cur_report_version_, tablet_meta_.report_status_))) {
      LOG_WARN("failed to init report info", K(tmp_ret));
    }
  }

  if (OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObTablet::serialize(char *buf, const int64_t len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t new_pos = pos;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_ISNULL(buf)
      || OB_UNLIKELY(len <= 0)
      || OB_UNLIKELY(pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(buf), K(len), K(pos));
  } else if (TABLET_VERSION != version_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid version", K(ret), K_(version));
  } else if (FALSE_IT(length_ = get_self_size())) {
    // do nothing
  } else if (OB_UNLIKELY(length_ > len - pos)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("buffer's length is not enough", K(ret), K(length_), K(len - new_pos));
  } else if (OB_FAIL(serialization::encode_i32(buf, len, new_pos, version_))) {
    LOG_WARN("failed to serialize tablet's version", K(ret), K(len), K(new_pos), K_(version));
  } else if (new_pos - pos < length_ && OB_FAIL(serialization::encode_i32(buf, len, new_pos, length_))) {
    LOG_WARN("failed to serialize tablet's length", K(ret), K(len), K(new_pos), K_(length));
  } else if (new_pos - pos < length_ && OB_FAIL(tablet_meta_.serialize(buf, len, new_pos))) {
    LOG_WARN("failed to serialize tablet meta", K(ret), K(len), K(new_pos), K_(length), K_(version));
  } else if (new_pos - pos < length_ && OB_FAIL(table_store_.serialize(buf, len, new_pos))) {
    LOG_WARN("failed to serialize table store", K(ret), K(len), K(new_pos));
  } else if (new_pos - pos < length_ && OB_FAIL(storage_schema_.serialize(buf, len, new_pos))) {
    LOG_WARN("failed to serialize storage schema", K(ret), K(len), K(new_pos));
  } else if (new_pos - pos < length_ && OB_FAIL(medium_info_list_.serialize(buf, len, new_pos))) {
    LOG_WARN("failed to serialize medium compaction list", K(ret), K(len), K(new_pos));
  } else if (OB_UNLIKELY(length_ != new_pos - pos)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet's length doesn't match standard length", K(ret), K(new_pos), K(pos), K_(length));
  } else if (tablet_meta_.has_next_tablet_ && OB_FAIL(next_tablet_guard_.get_obj()->serialize(buf, len, new_pos))) {
    LOG_WARN("failed to serialize next tablet", K(ret), K(len), K(new_pos));
  } else {
    pos = new_pos;
  }

  return ret;
}

int ObTablet::deserialize(
    common::ObIAllocator &allocator,
    const char *buf,
    const int64_t len,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(load_deserialize(allocator, buf, len, pos))) {
    LOG_WARN("fail to load deserialize", K(ret), KP(buf), K(len), K(pos));
  } else if (OB_FAIL(deserialize_post_work())) {
    LOG_WARN("fail to deserialize post work", K(ret), KP(buf), K(len), K(pos));
  }
  return ret;
}

int ObTablet::deserialize_post_work()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObITable *, MAX_SSTABLE_CNT_IN_STORAGE> sstables;
  if (OB_FAIL(get_all_sstables(sstables))) {
      LOG_WARN("fail to get all sstables", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < sstables.count(); i++) {
      ObSSTable *sstable = static_cast<ObSSTable *>(sstables.at(i));
      if (OB_ISNULL(sstable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error, sstable is nullptr", K(ret), KP(sstable));
      } else if (OB_FAIL(sstable->deserialize_post_work())) {
        LOG_WARN("fail to do deserialize post work for sstable", K(ret), KPC(sstable));
      }
    }
  }
  if (OB_SUCC(ret) && tablet_meta_.has_next_tablet_) {
    if (OB_FAIL(next_tablet_guard_.get_obj()->deserialize_post_work())) {
      LOG_WARN("fail to deserialize next post work", K(ret), K(next_tablet_guard_));
    }
  }
  return ret;
}

int ObTablet::dec_macro_disk_ref()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObITable *, MAX_SSTABLE_CNT_IN_STORAGE> sstables;
  if (OB_FAIL(get_all_sstables(sstables))) {
    LOG_WARN("fail to get all sstables", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < sstables.count(); i++) {
      ObSSTable *sstable = static_cast<ObSSTable *>(sstables.at(i));
      if (OB_ISNULL(sstable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error, sstable is nullptr", K(ret), KP(sstable));
      } else if (OB_FAIL(sstable->dec_disk_ref())) {
        LOG_ERROR("fail to dec disk ref cnt for sstable", K(ret), KPC(sstable));
      }
    }
  }
  if (OB_SUCC(ret) && tablet_meta_.has_next_tablet_) {
    if (OB_FAIL(next_tablet_guard_.get_obj()->dec_macro_disk_ref())) {
      LOG_WARN("fail to dec macro disk ref", K(ret), K(next_tablet_guard_));
    }
  }
  return ret;
}

int ObTablet::inc_macro_disk_ref()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObITable *, MAX_SSTABLE_CNT_IN_STORAGE> sstables;
  if (OB_FAIL(get_all_sstables(sstables))) {
    LOG_WARN("fail to get all sstables", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < sstables.count(); i++) {
      ObSSTable *sstable = static_cast<ObSSTable *>(sstables.at(i));
      if (OB_ISNULL(sstable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error, sstable is nullptr", K(ret), KP(sstable));
      } else if (OB_FAIL(sstable->add_disk_ref())) {
        LOG_ERROR("fail to add disk ref cnt for sstable", K(ret), KPC(sstable));
      }
    }
  }
  if (OB_SUCC(ret) && tablet_meta_.has_next_tablet_) {
    if (OB_FAIL(next_tablet_guard_.get_obj()->inc_macro_disk_ref())) {
      LOG_WARN("fail to inc macro disk ref", K(ret), K(next_tablet_guard_));
    }
  }
  return ret;
}

int ObTablet::load_deserialize(
    common::ObIAllocator &allocator,
    const char *buf,
    const int64_t len,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t new_pos = pos;
  allocator_ = &allocator;
  ObTabletHandle next_tablet_handle;
  ObLSHandle ls_handle;
  ObLSService *ls_service = MTL(ObLSService*);
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("cannot deserialize inited tablet", K(ret), K_(is_inited));
  } else if (OB_ISNULL(buf)
      || OB_UNLIKELY(len <= 0)
      || OB_UNLIKELY(pos < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(buf), K(len), K(pos));
  } else if (OB_ISNULL(memtable_mgr_) || OB_ISNULL(log_handler_)) {
    // tablet pointer handle is not ready here, so DO NOT validate it
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null pointer", K(ret), K_(memtable_mgr), K_(log_handler));
  } else if (OB_FAIL(serialization::decode_i32(buf, len, new_pos, &version_))) {
    LOG_WARN("failed to deserialize tablet's version", K(ret), K(len), K(new_pos));
  } else if (OB_FAIL(serialization::decode_i32(buf, len, new_pos, &length_))) {
    LOG_WARN("failed to deserialize tablet's length", K(ret), K(len), K(new_pos));
  } else if (TABLET_VERSION == version_) {
    if (OB_UNLIKELY(length_ > len - pos)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("buffer's length is not enough", K(ret), K(length_), K(len - new_pos));
    } else if (new_pos - pos < length_ && OB_FAIL(tablet_meta_.deserialize(allocator, buf, len, new_pos))) {
      LOG_WARN("failed to deserialize tablet meta", K(ret), K(len), K(new_pos));
    } else if (new_pos - pos < length_ && OB_FAIL(table_store_.deserialize(allocator, this, buf, len, new_pos))) {
      LOG_WARN("failed to deserialize table store", K(ret), K(len), K(new_pos));
    } else if (new_pos - pos < length_ && OB_FAIL(storage_schema_.deserialize(allocator, buf, len, new_pos))) {
      LOG_WARN("failed to deserialize storage schema", K(ret), K(len), K(new_pos));
    } else if (new_pos - pos < length_ && OB_FAIL(medium_info_list_.deserialize(allocator, buf, len, new_pos))) {
      LOG_WARN("failed to deserialize medium compaction list", K(ret), K(len), K(new_pos));
    } else if (OB_UNLIKELY(length_ != new_pos - pos)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("tablet's length doesn't match standard length", K(ret), K(new_pos), K(pos), K_(length));
    } else if (OB_FAIL(build_read_info(allocator))) {
      LOG_WARN("failed to build read info", K(ret));
    } else if (OB_FAIL(pre_transform_sstable_root_block(*full_read_info_.get_index_read_info()))) {
      LOG_WARN("failed to pre-transform sstable root block", K(ret), K(full_read_info_));
    } else if (tablet_meta_.has_next_tablet_) {
      LOG_WARN("The len and pos is", K(len), K(new_pos));
      const ObTabletMapKey key(tablet_meta_.ls_id_, tablet_meta_.tablet_id_);
      if (OB_FAIL(ls_service->get_ls(tablet_meta_.ls_id_, ls_handle, ObLSGetMod::TABLET_MOD))) {
        LOG_WARN("failed to get ls", K(ret), "ls_id", tablet_meta_.ls_id_);
      } else if (t3m->is_used_obj_pool(&allocator)) {
        if (OB_FAIL(t3m->acquire_tablet(WashTabletPriority::WTP_HIGH, key, ls_handle, next_tablet_handle, false/*only acquire*/))) {
          LOG_WARN("failed to acquire tablet", K(ret), K(key));
        }
      } else if (OB_FAIL(t3m->acquire_tablet(WashTabletPriority::WTP_HIGH, key, allocator, next_tablet_handle, false/*only acquire*/))) {
        LOG_WARN("failed to acquire tablet", K(ret), K(key));
      }
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(next_tablet_handle.get_obj())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("next tablet is null", K(ret));
      } else if (OB_FAIL(next_tablet_handle.get_obj()->load_deserialize(allocator, buf, len, new_pos))) {
        LOG_WARN("failed to deserialize next tablet", K(ret), K(len), K(new_pos));
      } else {
        set_next_tablet_guard(next_tablet_handle);
      }
    }
  }

  if (OB_SUCC(ret)) {
    pos = new_pos;
    if (tablet_meta_.max_sync_storage_schema_version_ > storage_schema_.schema_version_) {
      LOG_INFO("tablet meta status is not right, upgrade may happened. fix max_sync_schema_version on purpose",
          K(tablet_meta_.max_sync_storage_schema_version_),
          K(storage_schema_.schema_version_));
      tablet_meta_.max_sync_storage_schema_version_ = storage_schema_.schema_version_;
    }
    is_inited_ = true;
    LOG_INFO("succeeded to deserialize tablet", K(ret), K(*this));
  } else if (OB_UNLIKELY(!is_inited_)) {
    reset();
  }

  return ret;
}

int64_t ObTablet::get_serialize_size() const
{
  int64_t size = get_self_size();
  if (tablet_meta_.has_next_tablet_) {
    size += next_tablet_guard_.get_obj()->get_serialize_size();
  }
  return size;
}

int64_t ObTablet::get_self_size() const
{
  int64_t size = 0;
  size += serialization::encoded_length_i32(version_);
  size += serialization::encoded_length_i32(length_);
  size += tablet_meta_.get_serialize_size();
  size += table_store_.get_serialize_size();
  size += storage_schema_.get_serialize_size();
  size += medium_info_list_.get_serialize_size();
  return size;
}

void ObTablet::set_next_tablet_guard(const ObMetaObjGuard<ObTablet> &next_tablet_guard)
{
  tablet_meta_.has_next_tablet_ = true;
  next_tablet_guard_ = next_tablet_guard;
}

void ObTablet::trim_tablet_list()
{
  tablet_meta_.has_next_tablet_ = false;
  next_tablet_guard_.reset();
}

int ObTablet::deserialize_id(
      const char *buf,
      const int64_t len,
      share::ObLSID &ls_id,
      common::ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  int32_t version = 0;
  int32_t length = 0;
  int64_t pos = 0;
  if (OB_FAIL(serialization::decode_i32(buf, len, pos, (int32_t *)&version))) {
    LOG_WARN("fail to deserialize tablet meta's version", K(ret), K(len), K(pos));
  } else if (OB_FAIL(serialization::decode_i32(buf, len, pos, (int32_t *)&length))) {
    LOG_WARN("fail to deserialize tablet meta's length", K(ret), K(len), K(pos));
  } else if (TABLET_VERSION == version &&
      OB_FAIL(ObTabletMeta::deserialize_id(buf, len, pos, ls_id, tablet_id))) {
    LOG_WARN("fail to deserialize ls_id and tablet_id from tablet meta", K(ret), K(len));
  }

  return ret;
}

int ObTablet::get_max_sync_storage_schema_version(int64_t &max_schema_version) const
{
  int ret = OB_SUCCESS;
  max_schema_version = 0;
  ObIMemtableMgr *memtable_mgr = nullptr;
  ObTabletMemtableMgr *data_memtable_mgr = nullptr;
  if (is_ls_inner_tablet()) {
    // do nothing
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (FALSE_IT(data_memtable_mgr = static_cast<ObTabletMemtableMgr *>(memtable_mgr))) {
  } else {
    max_schema_version = data_memtable_mgr->get_storage_schema_recorder().get_max_sync_version();
  }
  return ret;
}

int ObTablet::try_update_storage_schema(
    const int64_t table_id,
    const int64_t schema_version,
    ObIAllocator &allocator,
    const int64_t timeout_ts)
{
  int ret = OB_SUCCESS;
  ObIMemtableMgr *memtable_mgr = nullptr;
  ObTabletMemtableMgr *data_memtable_mgr = nullptr;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (tablet_meta_.tablet_id_.is_special_merge_tablet()) {
    // do nothing
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (FALSE_IT(data_memtable_mgr = static_cast<ObTabletMemtableMgr *>(memtable_mgr))) {
  } else if (OB_FAIL(data_memtable_mgr->get_storage_schema_recorder().try_update_storage_schema(
      table_id, schema_version, allocator, timeout_ts))) {
    LOG_WARN("fail to record storage schema", K(ret), K(table_id), K(schema_version), K(timeout_ts));
  }
  return ret;
}

int ObTablet::get_max_schema_version(int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  schema_version = -1;
  common::ObSEArray<ObTableHandleV2, 8> table_handle_array;
  ObIMemtableMgr *memtable_mgr = nullptr;

  if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (OB_FAIL(memtable_mgr->get_all_memtables(table_handle_array))) {
    LOG_WARN("failed to get memtables", K(ret));
  } else {
    const memtable::ObMemtable *memtable = nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && i < table_handle_array.count(); ++i) {
      const ObTableHandleV2 &handle = table_handle_array[i];
      if (OB_UNLIKELY(!handle.is_valid())) {
        ret = OB_ERR_SYS;
        LOG_WARN("invalid memtable", K(ret), K(handle));
      } else if (OB_FAIL(handle.get_data_memtable(memtable))) {
        LOG_WARN("fail to get memtable", K(ret), K(handle));
      } else if (OB_ISNULL(memtable)) {
        ret = OB_ERR_SYS;
        LOG_WARN("memtable is null", K(ret), KP(memtable));
      } else {
        schema_version = common::max(schema_version, memtable->get_max_schema_version());
      }
    }
  }
  return ret;
}

int ObTablet::pre_transform_sstable_root_block(const ObTableReadInfo &index_read_info)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObITable *, MAX_SSTABLE_CNT_IN_STORAGE> sstables;
  if (OB_FAIL(inner_get_all_sstables(sstables))) {
    LOG_WARN("fail to get all sstables", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < sstables.count(); i++) {
      ObSSTable *sstable = static_cast<ObSSTable *>(sstables.at(i));
      if (OB_ISNULL(sstable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error, sstable is nullptr", K(ret), KP(sstable));
      } else if (OB_FAIL(sstable->pre_transform_root_block(index_read_info))) {
        LOG_WARN("fail to pre-transform sstable root block", K(ret), KPC(sstable));
      }
    }
  }
  return ret;
}

int ObTablet::check_schema_version_for_bounded_staleness_read(
    const int64_t table_version_for_read,
    const int64_t data_max_schema_version,
    const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  int64_t cur_table_version = OB_INVALID_VERSION;
  int64_t tenant_schema_version = OB_INVALID_VERSION;

  if (table_version_for_read >= data_max_schema_version) {
    // read schema version is biger than max schema version of data, pass
  } else {
    // read schema version is smaller than max schema version of data, two possible cases:
    // 1. max schema version of data is max schema version of table, return schema error, asking for schema refresh
    //
    //    standalone pg is in this case
    //
    // 2. max schema version of data is max schema version of multiple table partitions
    //
    //    It is the case when pg contains multiple partitions, it can only return max schema version of all partitions
    //
    // To differentiate the above two cases, check with the help of local schema version

    const uint64_t tenant_id = MTL_ID();
    ObMultiVersionSchemaService *schema_service = MTL(ObTenantSchemaService*)->get_schema_service();
    ObSchemaGetterGuard schema_guard;
    // get schema version of this table in schema service
    if (OB_ISNULL(schema_service)) {
      ret = OB_NOT_INIT;
      LOG_WARN("invalid schema service", K(ret), K(schema_service));
    } else if (OB_FAIL(schema_service->get_tenant_full_schema_guard(tenant_id, schema_guard))) {
      LOG_WARN("schema service get tenant schema guard fail", K(ret), K(tenant_id),
          K(table_id));
    } else if (OB_FAIL(schema_guard.get_schema_version(TABLE_SCHEMA, tenant_id, table_id, cur_table_version))) {
      LOG_WARN("get table schema version fail", K(ret), K(tenant_id), K(table_id));
    }

    // check whether input table version and schema version of this table in schema service same
    // if not same, refresh schema
    else if (OB_UNLIKELY(table_version_for_read != cur_table_version)) {
      ret = OB_SCHEMA_ERROR;
      LOG_WARN("schema version for read mismatch", K(ret), K(table_id),
          K(table_version_for_read), K(cur_table_version), K(data_max_schema_version));
    }
    // get max schema version of the tenant
    else if (OB_FAIL(schema_service->get_tenant_refreshed_schema_version(
        tenant_id, tenant_schema_version))) {
      LOG_WARN("get tenant refreshed schema version fail", K(ret), K(tenant_id));
    } else if (tenant_schema_version >= data_max_schema_version) {
      // if max schema version of the tenant is bigger than data's schema version,
      // then schema of read operation is newer than data's
    } else {
      ret = OB_SCHEMA_NOT_UPTODATE;
      LOG_WARN("schema is not up to date for read, need refresh", K(ret),
          K(table_version_for_read), K(cur_table_version), K(tenant_schema_version),
          K(data_max_schema_version), K(table_id), K(tenant_id));
    }
  }

  LOG_DEBUG("check schema version for bounded staleness read", K(ret),
      K(data_max_schema_version), K(table_version_for_read), K(cur_table_version),
      K(tenant_schema_version), K(table_id));
  return ret;
}

int ObTablet::lock_row(
    ObRelativeTable &relative_table,
    ObStoreCtx &store_ctx,
    const common::ObNewRow &row)
{
  int ret = OB_SUCCESS;
  ObStorageTableGuard guard(this, store_ctx, true);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (!relative_table.is_valid()
             || !store_ctx.is_valid()
             || !row.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument",
             K(ret), K(relative_table), K(store_ctx), K(row));
  } else if (OB_UNLIKELY(relative_table.get_tablet_id() != tablet_meta_.tablet_id_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet id doesn't match", K(ret), K(relative_table.get_tablet_id()), K(tablet_meta_.tablet_id_));
  } else if (OB_FAIL(try_update_storage_schema(relative_table.get_table_id(),
      relative_table.get_schema_version(),
      store_ctx.mvcc_acc_ctx_.get_mem_ctx()->get_query_allocator(),
      store_ctx.timeout_))) {
    LOG_WARN("fail to record table schema", K(ret));
  } else if (OB_FAIL(guard.refresh_and_protect_table(relative_table))) {
    LOG_WARN("fail to protect table", K(ret), "tablet_id", tablet_meta_.tablet_id_);
  }
  if (OB_SUCC(ret)) {
    ObMemtable *write_memtable = nullptr;
    if (OB_FAIL(prepare_memtable(relative_table, store_ctx, write_memtable))) {
      LOG_WARN("prepare write memtable fail", K(ret), K(relative_table));
    } else if (OB_FAIL(write_memtable->lock(store_ctx, relative_table.get_table_id(),
            full_read_info_, row))) {
      LOG_WARN("failed to lock write_memtable", K(ret), K_(full_read_info), K(row));
    }
  }
  return ret;
}

int ObTablet::lock_row(
    ObRelativeTable &relative_table,
    storage::ObStoreCtx &store_ctx,
    const ObDatumRowkey &rowkey)
{
  int ret = OB_SUCCESS;
  ObStorageTableGuard guard(this, store_ctx, true);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!relative_table.is_valid()
             || !store_ctx.is_valid()
             || !rowkey.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument",
                K(ret), K(relative_table), K(store_ctx), K(rowkey));
  } else if (OB_UNLIKELY(relative_table.get_tablet_id() != tablet_meta_.tablet_id_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet id doesn't match", K(ret), K(relative_table.get_tablet_id()), K(tablet_meta_.tablet_id_));
  } else if (OB_FAIL(try_update_storage_schema(relative_table.get_table_id(),
      relative_table.get_schema_version(),
      store_ctx.mvcc_acc_ctx_.get_mem_ctx()->get_query_allocator(),
      store_ctx.timeout_))) {
    LOG_WARN("fail to record table schema", K(ret));
  } else if (OB_FAIL(guard.refresh_and_protect_table(relative_table))) {
    LOG_WARN("fail to protect table", K(ret));
  } else {
    memtable::ObMemtable *write_memtable = nullptr;
    const uint64_t table_id = relative_table.get_table_id();
    if (OB_FAIL(prepare_memtable(relative_table, store_ctx, write_memtable))) {
      LOG_WARN("prepare write memtable fail", K(ret), K(relative_table));
    } else if (OB_FAIL(write_memtable->lock(store_ctx, table_id, full_read_info_, rowkey))) {
      LOG_WARN("failed to lock write memtable", K(ret), K(table_id), K_(full_read_info), K(rowkey));
    }
  }
  return ret;
}

int ObTablet::get_read_tables(
    const int64_t snapshot_version,
    ObTabletTableIterator &iter,
    const bool allow_no_ready_read)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(allow_to_read_())) {
    LOG_WARN("not allowed to read", K(ret), K(tablet_meta_));
  } else if (OB_UNLIKELY(!iter.tablet_handle_.is_valid() || iter.tablet_handle_.get_obj() != this)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(iter), K(this));
  } else if (OB_FAIL(get_read_tables(snapshot_version, iter.table_iter_, allow_no_ready_read))) {
    LOG_WARN("failed to get read tables", K(ret), K(snapshot_version), K(allow_no_ready_read));
  }
  return ret;
}

int ObTablet::get_read_tables(
    const int64_t snapshot_version,
    ObTableStoreIterator &iter,
    const bool allow_no_ready_read)
{
  TCRLockGuard guard(table_store_lock_);
  return table_store_.get_read_tables(snapshot_version, iter, allow_no_ready_read);
}

int ObTablet::get_read_major_sstable(
    const int64_t &major_snapshot_version,
      ObTabletTableIterator &iter)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(allow_to_read_())) {
    LOG_WARN("not allowed to read", K(ret), K(tablet_meta_));
  } else if (OB_UNLIKELY(!iter.tablet_handle_.is_valid() || iter.tablet_handle_.get_obj() != this)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(iter), K(this));
  } else if (OB_FAIL(get_read_major_sstable(major_snapshot_version, iter.table_iter_))) {
    LOG_WARN("failed to get read tables", K(ret), K(major_snapshot_version));
  }
  return ret;
}

int ObTablet::get_read_major_sstable(
    const int64_t &major_snapshot_version,
    ObTableStoreIterator &iter)
{
  TCRLockGuard guard(table_store_lock_);
  return table_store_.get_read_major_sstable(major_snapshot_version, iter);
}

int ObTablet::get_all_sstables(common::ObIArray<ObITable *> &sstables) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(inner_get_all_sstables(sstables))) {
    LOG_WARN("fail to get all sstable", K(ret));
  }
  return ret;
}
int ObTablet::inner_get_all_sstables(common::ObIArray<ObITable *> &sstables) const
{
  int ret = OB_SUCCESS;
  const ObSSTableArray &major_sstables = table_store_.get_major_sstables();
  const ObSSTableArray &minor_sstables = table_store_.get_minor_sstables();
  const ObSSTableArray &ddl_sstables = table_store_.get_ddl_sstables();

  sstables.reset();
  if (!major_sstables.empty() && OB_FAIL(major_sstables.get_all_tables(sstables))) {
    LOG_WARN("fail to get all tables from major sstables", K(ret));
  } else if (!minor_sstables.empty() && OB_FAIL(minor_sstables.get_all_tables(sstables))) {
    LOG_WARN("fail to get all tables from minor sstables", K(ret));
  } else if (!ddl_sstables.empty() && OB_FAIL(ddl_sstables.get_all_tables(sstables))) {
    LOG_WARN("fail to get all tables from ddl sstables", K(ret));
  } else{
    ObITable *table = nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && i < ObTabletTableStore::ExtendTable::EXTEND_CNT; i++) {
      if (OB_NOT_NULL(table = table_store_.get_extend_sstable(i))) {
        if (OB_FAIL(sstables.push_back(table))) {
          LOG_WARN("fail to push back", K(ret), KPC(table));
        }
      }
    }
  }

  return ret;
}

int ObTablet::get_sstables_size(int64_t &used_size) const
{
  int ret = OB_SUCCESS;
  common::ObSArray<ObITable *> sstables;
  bool multi_version = false;
  int64_t mem_block_cnt = 0;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(get_all_sstables(sstables))) {
    LOG_WARN("fail to get all sstables", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < sstables.count(); i++) {
      ObITable *table = nullptr;
      ObSSTable *sstable = nullptr;
      if (OB_ISNULL(table = sstables[i])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("sstable is null", K(ret), K(*this), K(i));
      } else if (FALSE_IT(sstable = static_cast<ObSSTable *> (table))) {
      } else {
        const ObSSTableBasicMeta &basic_meta = sstable->get_meta().get_basic_meta();
        if (multi_version && sstable->is_major_sstable()) {
          mem_block_cnt -= basic_meta.get_total_use_old_macro_block_count();
        } else if (sstable->is_major_sstable()) {
          multi_version = true;
        }
        mem_block_cnt += basic_meta.get_total_macro_block_count();
      }
    }
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (tablet_meta_.has_next_tablet_ && OB_FAIL(
        next_tablet_guard_.get_obj()->get_sstables_size(used_size))) {
      LOG_WARN("failed to get size of tablets on the list", K(ret), K(used_size));
    } else {
      used_size += mem_block_cnt * common::OB_DEFAULT_MACRO_BLOCK_SIZE;
    }
  }
  return ret;
}

int ObTablet::get_memtables(common::ObIArray<storage::ObITable *> &memtables, const bool need_active) const
{
  TCRLockGuard guard(table_store_lock_);
  return table_store_.get_memtables(memtables, need_active);
}

int ObTablet::check_need_remove_old_table(
    const int64_t multi_version_start,
    bool &need_remove) const
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(table_store_.need_remove_old_table(multi_version_start, need_remove))) {
    LOG_WARN("failed to check need rebuild table store", K(ret), K(multi_version_start));
  }

  return ret;
}

int ObTablet::update_upper_trans_version(ObLS &ls, bool &is_updated)
{
  int ret = OB_SUCCESS;
  is_updated = false;
  bool is_paused = false;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (FALSE_IT(is_paused = false)) { // TODO(DanLing) get is_paused
  } else if (is_paused) {
    LOG_INFO("paused, cannot update trans version now", K(tablet_meta_.tablet_id_));
  } else {
    ObSSTableArray &minor_tables = table_store_.get_minor_sstables();
    for (int64_t i = 0; OB_SUCC(ret) && i < minor_tables.count(); ++i) {
      ObSSTable *sstable = static_cast<ObSSTable *>(minor_tables[i]);
      if (OB_ISNULL(sstable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("sstable must not be null", K(ret), K(i), K(minor_tables));
      } else if (INT64_MAX == sstable->get_upper_trans_version()) {
        int64_t max_trans_version = INT64_MAX;
        if (OB_FAIL(ls.get_upper_trans_version_before_given_log_ts(
            sstable->get_end_log_ts(), max_trans_version))) {
          LOG_WARN("failed to get upper trans version before given log ts", K(ret), KPC(sstable));
        } else if (0 == max_trans_version) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("max trans version should not be 0", KPC(sstable));
        } else if (INT64_MAX != max_trans_version) {
          if (OB_UNLIKELY(0 == max_trans_version)) {
            FLOG_INFO("get max_trans_version = 0, maybe all the trans have been rollbacked", K(max_trans_version), KPC(sstable));
          }
          if (OB_FAIL(sstable->set_upper_trans_version(max_trans_version))) {
            LOG_WARN("failed to set_upper_trans_version", K(ret), KPC(sstable));
          } else {
            is_updated = true;
            FLOG_INFO("success to update sstable's upper trans version", K(max_trans_version), KPC(sstable));
          }
        }
      }
    }
  }

  return ret;
}

int ObTablet::insert_row(
    ObRelativeTable &relative_table,
    ObStoreCtx &store_ctx,
    const ObColDescIArray &col_descs,
    const ObStoreRow &row)
{
  int ret = OB_SUCCESS;
  bool b_exist = false;
  if (OB_UNLIKELY(!store_ctx.is_valid() || col_descs.count() <= 0 || !row.is_valid()
      || !relative_table.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(store_ctx), K(col_descs), K(row), K(ret));
  } else {
    const bool check_exists = !relative_table.is_storage_index_table()
                              || relative_table.is_unique_index();
    if (OB_FAIL(ret)) {
      LOG_WARN("failed to get rowkey columns");
    } else if (check_exists
        && OB_FAIL(rowkey_exists(relative_table, store_ctx, row.row_val_, b_exist))) {
      LOG_WARN("failed to check whether row exists", K(row), K(ret));
    } else if (OB_UNLIKELY(b_exist)) {
      ret = OB_ERR_PRIMARY_KEY_DUPLICATE;
      LOG_WARN("rowkey already exists",  K(relative_table.get_table_id()), K(row), K(ret));
    } else if (OB_FAIL(insert_row_without_rowkey_check(relative_table, store_ctx, col_descs, row))) {
      if (OB_TRY_LOCK_ROW_CONFLICT != ret) {
        LOG_WARN("failed to set row", K(row), K(ret));
      }
    }
  }
  return ret;
}

int ObTablet::update_row(
    ObRelativeTable &relative_table,
    storage::ObStoreCtx &store_ctx,
    const common::ObIArray<share::schema::ObColDesc> &col_descs,
    const ObIArray<int64_t> &update_idx,
    const storage::ObStoreRow &old_row,
    const storage::ObStoreRow &new_row)
{
  int ret = OB_SUCCESS;

  {
    ObStorageTableGuard guard(this, store_ctx, true);
    ObMemtable *write_memtable = nullptr;

    if (OB_UNLIKELY(!is_inited_)) {
      ret = OB_NOT_INIT;
      LOG_WARN("not inited", K(ret), K_(is_inited));
    } else if (OB_UNLIKELY(!store_ctx.is_valid()
        || col_descs.count() <= 0
        || !full_read_info_.is_valid_full_read_info()
        || !old_row.is_valid()
        || !new_row.is_valid()
        || !relative_table.is_valid())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid args", K(ret), K(store_ctx),
          K(relative_table), K(col_descs), K(update_idx),
          K(old_row), K(new_row), K_(full_read_info));
    } else if (OB_UNLIKELY(relative_table.get_tablet_id() != tablet_meta_.tablet_id_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("tablet id doesn't match", K(ret), K(relative_table.get_tablet_id()), K(tablet_meta_.tablet_id_));
    } else if (OB_FAIL(try_update_storage_schema(relative_table.get_table_id(),
        relative_table.get_schema_version(),
        store_ctx.mvcc_acc_ctx_.get_mem_ctx()->get_query_allocator(),
        store_ctx.timeout_))) {
      LOG_WARN("fail to record table schema", K(ret));
    } else if (OB_FAIL(guard.refresh_and_protect_table(relative_table))) {
      LOG_WARN("fail to protect table", K(ret));
    } else if (OB_FAIL(prepare_memtable(relative_table, store_ctx, write_memtable))) {
      LOG_WARN("prepare write memtable fail", K(ret), K(relative_table));
    } else if (OB_FAIL(write_memtable->set(store_ctx, relative_table.get_table_id(),
        full_read_info_, col_descs, update_idx, old_row, new_row))) {
      LOG_WARN("failed to set write memtable", K(ret));
    }
  }

  if (OB_SUCC(ret)) {
    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(store_ctx.mvcc_acc_ctx_.tx_ctx_->submit_redo_log(false))) {
      TRANS_LOG(INFO, "submit log if neccesary failed", K(tmp_ret), K(store_ctx),
                K(relative_table));
    }
  }

  return ret;
}

int ObTablet::insert_row_without_rowkey_check(
    ObRelativeTable &relative_table,
    ObStoreCtx &store_ctx,
    const common::ObIArray<share::schema::ObColDesc> &col_descs,
    const storage::ObStoreRow &row)
{
  int ret = OB_SUCCESS;
  {
    ObStorageTableGuard guard(this, store_ctx, true);
    ObMemtable *write_memtable = nullptr;

    if (OB_UNLIKELY(!is_inited_)) {
      ret = OB_NOT_INIT;
      LOG_WARN("not inited", K(ret), K_(is_inited));
    } else if (OB_UNLIKELY(!store_ctx.is_valid()
        || col_descs.count() <= 0
        || !full_read_info_.is_valid_full_read_info()
        || !row.is_valid()
        || !relative_table.is_valid())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid args", K(ret), K(store_ctx), K(relative_table),
          K(col_descs), K(row), K_(full_read_info));
    } else if (OB_UNLIKELY(relative_table.get_tablet_id() != tablet_meta_.tablet_id_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("tablet id doesn't match", K(ret), K(relative_table.get_tablet_id()), K(tablet_meta_.tablet_id_));
    } else if (OB_FAIL(try_update_storage_schema(relative_table.get_table_id(),
        relative_table.get_schema_version(),
        store_ctx.mvcc_acc_ctx_.get_mem_ctx()->get_query_allocator(),
        store_ctx.timeout_))) {
      LOG_WARN("fail to record table schema", K(ret));
    } else if (OB_FAIL(guard.refresh_and_protect_table(relative_table))) {
      LOG_WARN("fail to protect table", K(ret));
    } else if (OB_FAIL(prepare_memtable(relative_table, store_ctx, write_memtable))) {
      LOG_WARN("prepare write memtable fail", K(ret), K(relative_table));
    } else if (OB_FAIL(write_memtable->set(store_ctx, relative_table.get_table_id(),
        full_read_info_, col_descs, row))) {
      LOG_WARN("failed to set memtable", K(ret));
    }
  }

  if (OB_SUCC(ret)) {
    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(store_ctx.mvcc_acc_ctx_.tx_ctx_->submit_redo_log(false))) {
      TRANS_LOG(INFO, "submit log if necessary failed", K(tmp_ret), K(store_ctx),
                K(relative_table));
    }
  }

  return ret;
}

int ObTablet::do_rowkey_exists(
    ObStoreCtx &store_ctx,
    const int64_t table_id,
    const blocksstable::ObDatumRowkey &rowkey,
    const ObQueryFlag &query_flag,
    bool &exists)
{
  int ret = OB_SUCCESS;
  ObTableStoreIterator table_iter(true/*reverse_iter*/);

  if (OB_UNLIKELY(!store_ctx.is_valid() || OB_INVALID_ID == table_id
      || !rowkey.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(store_ctx), K(rowkey), K(query_flag));
  } else if (OB_FAIL(allow_to_read_())) {
    LOG_WARN("not allowed to read", K(ret), K(tablet_meta_));
  } else if (OB_FAIL(get_read_tables(store_ctx.mvcc_acc_ctx_.get_snapshot_version(), table_iter, query_flag.index_invalid_))) {
    LOG_WARN("get read iterator fail", K(ret));
  } else {
    bool found = false;
    ObITable *table = nullptr;
    while (OB_SUCC(ret) && !found) {
      if (OB_FAIL(table_iter.get_next(table))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("failed to get next tables", K(ret));
        }
      } else if (OB_ISNULL(table)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table must not be null", K(ret), K(table_iter));
      } else if (OB_FAIL(table->exist(store_ctx, table_id, full_read_info_, rowkey, exists, found))) {
        LOG_WARN("Fail to check if exist in store", K(ret), KPC(table));
      } else {
        LOG_DEBUG("rowkey_exists check", KPC(table), K(rowkey), K(exists), K(found), K(table_iter));
      }
    }

    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }

    if (OB_SUCCESS == ret && false == found) {
      exists = false;
    }
  }
  return ret;
}

int ObTablet::do_rowkeys_exist(ObTableStoreIterator &tables_iter, ObRowsInfo &rows_info, bool &exists)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(tables_iter.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument-tables_iter", K(ret), K(tables_iter.count()));
  }
  bool all_rows_found = false;
  while (OB_SUCC(ret) && !exists && !all_rows_found) {
    ObITable *table = nullptr;
    if (OB_FAIL(tables_iter.get_next(table))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        break;
      }
      LOG_WARN("fail to get next table", K(ret), KP(table));
    } else if (OB_ISNULL(table)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the table is nullptr", K(ret));
    } else if (OB_FAIL(table->exist(rows_info, exists, all_rows_found))) {
      LOG_WARN("fail to check the existence of rows", K(ret), K(rows_info), K(exists));
    } else {
      LOG_DEBUG("rowkey exists check", K(rows_info), K(exists));
    }
  }

  return ret;
}

int ObTablet::rowkey_exists(
    ObRelativeTable &relative_table,
    ObStoreCtx &store_ctx,
    const common::ObNewRow &row,
    bool &exists)
{
  int ret = OB_SUCCESS;
  const bool read_latest = true;
  ObQueryFlag flag;
  flag.read_latest_ = read_latest & ObQueryFlag::OBSF_MASK_READ_LATEST;
  if (relative_table.is_storage_index_table()) {
    flag.index_invalid_ = !relative_table.can_read_index();
  }

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_UNLIKELY(!store_ctx.is_valid() || !row.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(store_ctx), K(row));
  } else if (OB_UNLIKELY(relative_table.get_tablet_id() != tablet_meta_.tablet_id_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet id doesn't match", K(ret), K(relative_table.get_tablet_id()), K(tablet_meta_.tablet_id_));
  } else if (OB_FAIL(allow_to_read_())) {
    LOG_WARN("not allowed to read", K(ret), K(tablet_meta_));
  } else {
    {
      ObStorageTableGuard guard(this, store_ctx, false);
      if (OB_FAIL(guard.refresh_and_protect_table(relative_table))) {
        LOG_WARN("fail to protect table", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      ObStoreRowkey rowkey;
      ObDatumRowkey datum_rowkey;
      ObDatumRowkeyHelper rowkey_helper;
      if (OB_FAIL(rowkey.assign(row.cells_, relative_table.get_rowkey_column_num()))) {
        LOG_WARN("Failed to assign rowkey", K(ret), K(row));
      } else if (OB_FAIL(rowkey_helper.convert_datum_rowkey(rowkey.get_rowkey(), datum_rowkey))) {
        LOG_WARN("Failed to transfer datum rowkey", K(ret), K(rowkey));
      } else if (OB_FAIL(do_rowkey_exists(store_ctx, relative_table.get_table_id(),
          datum_rowkey, flag, exists))) {
        LOG_WARN("do rowkey exist fail", K(ret), K(rowkey), K(flag));
      }
      LOG_DEBUG("chaser debug row", K(ret), K(row), K(rowkey));
    }
  }
  return ret;
}

int ObTablet::rowkeys_exists(
    ObStoreCtx &store_ctx,
    ObRelativeTable &relative_table,
    ObRowsInfo &rows_info,
    bool &exists)
{
  int ret = OB_SUCCESS;
  ObTableStoreIterator tables_iter(true/*reverse_iter*/);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_UNLIKELY(!rows_info.is_valid() || !relative_table.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(rows_info), K(relative_table));
  } else if (OB_UNLIKELY(relative_table.get_tablet_id() != tablet_meta_.tablet_id_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet id doesn't match", K(ret), K(relative_table.get_tablet_id()), K(tablet_meta_.tablet_id_));
  } else if (OB_FAIL(allow_to_read_())) {
    LOG_WARN("not allowed to read", K(ret), K(tablet_meta_));
  } else if (OB_FAIL(get_read_tables(store_ctx.mvcc_acc_ctx_.get_snapshot_version(), tables_iter, relative_table.allow_not_ready()))) {
    LOG_WARN("get read iterator fail", K(ret));
  } else {
    {
      ObStorageTableGuard guard(this, store_ctx, false);
      if (OB_FAIL(guard.refresh_and_protect_table(relative_table))) {
        LOG_WARN("fail to protect table", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(do_rowkeys_exist(tables_iter, rows_info, exists))) {
        LOG_WARN("fail to check the existence of rows", K(ret), K(rows_info), K(exists));
      }
    }
  }

  return ret;
}

int ObTablet::prepare_memtable(
    ObRelativeTable &relative_table,
    ObStoreCtx &store_ctx,
    memtable::ObMemtable *&write_memtable)
{
  int ret = OB_SUCCESS;
  write_memtable = nullptr;
  store_ctx.table_iter_ = &relative_table.tablet_iter_.table_iter_;
  ObITable* last_table = relative_table.tablet_iter_.table_iter_.get_boundary_table(true/*is_last*/);
  if (OB_ISNULL(last_table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("last table is null", K(relative_table));
  } else if (OB_UNLIKELY(!last_table->is_data_memtable())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("last table is not memtable", K(ret), K(*last_table));
  } else {
    write_memtable = reinterpret_cast<ObMemtable*>(last_table);
  }
  return ret;
}

int ObTablet::choose_and_save_storage_schema(
    common::ObIAllocator &allocator,
    const ObStorageSchema &tablet_schema,
    const ObStorageSchema &param_schema)
{
  int ret = OB_SUCCESS;
  const ObStorageSchema *schema = &tablet_schema;

  if (OB_UNLIKELY(!tablet_schema.is_valid()) || OB_UNLIKELY(!param_schema.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("input schema is invalid", K(ret), K(tablet_schema), K(param_schema));
  } else if (tablet_schema.schema_version_ >= param_schema.schema_version_) {
    schema = &tablet_schema;
    LOG_INFO("tablet storage schema version is no smaller than that in param",
        "tablet_schema_version", tablet_schema.get_schema_version(),
        "param_schema_version", param_schema.schema_version_);
  } else {
    schema = &param_schema;
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(storage_schema_.init(allocator, *schema))) {
    LOG_WARN("failed to init storage schema", K(ret), KPC(schema));
  }

  return ret;
}

int ObTablet::assign_pointer_handle(const ObTabletPointerHandle &ptr_hdl)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(pointer_hdl_.assign(ptr_hdl))) {
    LOG_WARN("assign tablet ptr fail", K(ret));
  }
  return ret;
}

int ObTablet::replay_update_storage_schema(
    const int64_t log_ts,
    const char *buf,
    const int64_t buf_size,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t new_pos = pos;
  ObIMemtableMgr *memtable_mgr = nullptr;
  ObTabletMemtableMgr *data_memtable_mgr = nullptr;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (tablet_meta_.tablet_id_.is_special_merge_tablet()) {
    // do nothing
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (FALSE_IT(data_memtable_mgr = static_cast<ObTabletMemtableMgr *>(memtable_mgr))) {
  } else if (OB_FAIL(data_memtable_mgr->get_storage_schema_recorder().replay_schema_log(log_ts, buf, buf_size, new_pos))) {
    LOG_WARN("storage schema recorder replay fail", K(ret), K(log_ts));
  } else {
    pos = new_pos;
  }
  if (OB_TIMEOUT == ret) {
    ret = OB_EAGAIN; // need retry.
  }
  return ret;
}

int ObTablet::get_schema_version_from_storage_schema(int64_t &schema_version)
{
  int ret = OB_SUCCESS;
  const common::ObTabletID &tablet_id = tablet_meta_.tablet_id_;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited), K(tablet_id));
  } else {
    schema_version = storage_schema_.schema_version_;
  }

  return ret;
}

int ObTablet::get_active_memtable(ObTableHandleV2 &handle) const
{
  int ret = OB_SUCCESS;
  ObIMemtableMgr *memtable_mgr = nullptr;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (OB_FAIL(memtable_mgr->get_active_memtable(handle))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("failed to get active memtable for tablet", K(ret), K(*this));
    }
  }
  return ret;
}

int ObTablet::create_memtable(const int64_t schema_version,
                              const bool for_replay)
{
  int ret = OB_SUCCESS;
  ObTimeGuard time_guard("ObTablet::create_memtable", 10 * 1000);
  TCWLockGuard guard(table_store_lock_);
  time_guard.click("lock");
  const int64_t clog_checkpoint_ts = tablet_meta_.clog_checkpoint_ts_;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(schema_version < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid schema version", K(ret), K(schema_version));
  } else if (OB_FAIL(table_store_.prepare_memtables())) {
    LOG_WARN("failed to pre-allocate memory for new memtable", K(ret), KPC(this));
  } else if (FALSE_IT(time_guard.click("prepare_memtables"))) {
  } else if (OB_FAIL(inner_create_memtable(clog_checkpoint_ts, schema_version, for_replay))) {
    if (OB_ENTRY_EXIST == ret) {
      ret = OB_SUCCESS;
    } else if (OB_MINOR_FREEZE_NOT_ALLOW != ret) {
      LOG_WARN("failed to create memtable", K(ret), K(clog_checkpoint_ts),
               K(schema_version), K(for_replay));
    }
  } else if (FALSE_IT(time_guard.click("inner_create_memtable"))) {
  } else if (OB_FAIL(table_store_.update_memtables())) {
    LOG_ERROR("failed to append new memtable to table store", K(ret), KPC(this));
    abort();
  } else {
    time_guard.click("update_memtables");
  }

  return ret;
}

int ObTablet::inner_create_memtable(
    const int64_t clog_checkpoint_ts,
    const int64_t schema_version,
    const bool for_replay)
{
  int ret = OB_SUCCESS;
  const common::ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  ObIMemtableMgr *memtable_mgr = nullptr;

  if (OB_UNLIKELY(clog_checkpoint_ts < 1) || OB_UNLIKELY(schema_version < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(clog_checkpoint_ts), K(schema_version));
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (OB_FAIL(memtable_mgr->create_memtable(clog_checkpoint_ts, schema_version, for_replay))) {
    if (OB_ENTRY_EXIST != ret && OB_MINOR_FREEZE_NOT_ALLOW != ret) {
      LOG_WARN("failed to create memtable for tablet", K(ret),
          K(clog_checkpoint_ts), K(schema_version), K(for_replay));
    }
  } else {
    LOG_INFO("succeeded to create memtable for tablet", K(ret), K(tablet_meta_),
        K(tablet_id), K(clog_checkpoint_ts), K(schema_version));
  }

  return ret;
}

int ObTablet::release_memtables(const int64_t log_ts)
{
  int ret = OB_SUCCESS;
  ObIMemtableMgr *memtable_mgr = nullptr;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (OB_FAIL(memtable_mgr->release_memtables(log_ts))) {
    LOG_WARN("failed to release memtables", K(ret), K(log_ts));
  }

  return ret;
}

int ObTablet::release_memtables()
{
  int ret = OB_SUCCESS;
  ObIMemtableMgr *memtable_mgr = nullptr;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (OB_FAIL(memtable_mgr->release_memtables())) {
    LOG_WARN("failed to release memtables", K(ret));
  }

  return ret;
}

int ObTablet::destroy_storage_related_member()
{
  int ret = OB_SUCCESS;
  ObIMemtableMgr *memtable_mgr = nullptr;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (is_ls_inner_tablet()) {
    // do nothing
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (OB_FAIL(memtable_mgr->destroy_storage_schema_recorder())) {
    LOG_WARN("failed to destroy storage schema recorder", K(ret), KPC(memtable_mgr));
  }
  return ret;
}

int ObTablet::get_memtable_mgr(ObIMemtableMgr *&memtable_mgr) const
{
  int ret = OB_SUCCESS;

  ObTabletPointer *tablet_ptr = static_cast<ObTabletPointer*>(pointer_hdl_.get_resource_ptr());
  ObMemtableMgrHandle &memtable_mgr_handle = tablet_ptr->memtable_mgr_handle_;
  if (OB_UNLIKELY(!memtable_mgr_handle.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("memtable mgr handle is invalid", K(ret), K(memtable_mgr_handle));
  } else {
    memtable_mgr = memtable_mgr_handle.get_memtable_mgr();
  }

  return ret;
}

int ObTablet::get_ddl_kv_mgr(ObDDLKvMgrHandle &ddl_kv_mgr_handle, bool try_create)
{
  int ret = OB_SUCCESS;
  ddl_kv_mgr_handle.reset();
  ObTabletPointer *tablet_ptr = static_cast<ObTabletPointer*>(pointer_hdl_.get_resource_ptr());
  if (try_create) {
    if (OB_FAIL(tablet_ptr->create_ddl_kv_mgr(tablet_meta_.ls_id_, tablet_meta_.tablet_id_, ddl_kv_mgr_handle))) {
      LOG_WARN("create ddl kv mgr failed", K(ret), K(tablet_meta_));
    }
  } else {
    tablet_ptr->get_ddl_kv_mgr(ddl_kv_mgr_handle);
    if (!ddl_kv_mgr_handle.is_valid()) {
      ret = OB_ENTRY_NOT_EXIST;
      LOG_DEBUG("ddl kv mgr not exist", K(ret), K(ddl_kv_mgr_handle));
    }
  }
  return ret;
}

void ObTablet::remove_ddl_kv_mgr()
{
  ObTabletPointer *tablet_pointer = static_cast<ObTabletPointer *>(pointer_hdl_.get_resource_ptr());
  tablet_pointer->remove_ddl_kv_mgr();
}


common::ObThreadCond &ObTablet::get_cond()
{
  ObTabletPointer *tablet_ptr = static_cast<ObTabletPointer*>(pointer_hdl_.get_resource_ptr());
  return tablet_ptr->cond_;
}

common::TCRWLock &ObTablet::get_rw_lock()
{
  ObTabletPointer *tablet_ptr = static_cast<ObTabletPointer*>(pointer_hdl_.get_resource_ptr());
  return tablet_ptr->msd_lock_;
}

int ObTablet::init_shared_params(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const int64_t max_saved_schema_version, // for init storage_schema_recorder on MemtableMgr
    ObFreezer *freezer)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!pointer_hdl_.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet pointer handle is invalid", K(ret), K_(pointer_hdl));
  } else {
    ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
    ObIMemtableMgr *memtable_mgr = nullptr;
    common::ObThreadCond &cond = get_cond();

    if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
      LOG_WARN("failed to get memtable mgr", K(ret));
    } else if (OB_FAIL(memtable_mgr->init(tablet_id, ls_id, max_saved_schema_version, log_handler_, freezer, t3m))) {
      LOG_WARN("failed to init memtable mgr", K(ret), K(tablet_id), K(ls_id), KP(freezer));
    } else if (OB_FAIL(cond.init(ObWaitEventIds::TABLET_LOCK_WAIT))) {
      LOG_WARN("failed to init thread cond", K(ret));
    }
  }

  return ret;
}

int ObTablet::init_storage_related_member(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const int64_t max_saved_schema_version) // for init storage_schema_recorder on MemtableMgr
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!pointer_hdl_.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet pointer handle is invalid", K(ret), K_(pointer_hdl));
  } else if (is_ls_inner_tablet()) {
    // do nothing
  } else {
    ObIMemtableMgr *memtable_mgr = nullptr;
    ObTabletMemtableMgr *data_memtable_mgr = nullptr;
    if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
      LOG_WARN("failed to get memtable mgr", K(ret));
    } else if (FALSE_IT(data_memtable_mgr = static_cast<ObTabletMemtableMgr *>(memtable_mgr))) {
    } else if (OB_FAIL(data_memtable_mgr->init_storage_schema_recorder(tablet_id, ls_id, max_saved_schema_version, log_handler_))) {
      LOG_WARN("failed to init storage schema recorder", K(ret), K(tablet_id), K(ls_id), KP(max_saved_schema_version));
    }
  }
  return ret;
}

int ObTablet::build_read_info(common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObSEArray<share::schema::ObColDesc, 16> cols_desc;
  if (OB_FAIL(storage_schema_.get_multi_version_column_descs(cols_desc))) {
    LOG_WARN("Fail to get rowkey column ids", K(ret));
  } else if (OB_FAIL(full_read_info_.init(allocator,
      storage_schema_.get_column_count(),
      storage_schema_.get_rowkey_column_num(),
      storage_schema_.is_oracle_mode(),
      cols_desc,
      true))) {
    LOG_WARN("Fail to init read info", K(ret));
  }

  return ret;
}

int ObTablet::try_update_start_scn()
{
  int ret = OB_SUCCESS;
  ObSSTable *first_minor = static_cast<ObSSTable *>(table_store_.get_minor_sstables().get_boundary_table(false /*first*/));
  const int64_t start_scn = OB_NOT_NULL(first_minor) ? first_minor->get_start_log_ts() : tablet_meta_.clog_checkpoint_ts_;
  if (OB_UNLIKELY(start_scn < tablet_meta_.start_scn_)) {
    // ignore ret on purpose
    LOG_WARN("tablet start scn can not fallback", K(start_scn), K(tablet_meta_));
  } else {
    tablet_meta_.start_scn_ = start_scn;
  }
  return ret;
}

int ObTablet::try_update_ddl_checkpoint_ts()
{
  int ret = OB_SUCCESS;
  ObSSTable *last_ddl_sstable = static_cast<ObSSTable *>(table_store_.get_ddl_sstables().get_boundary_table(true/*last*/));
  if (OB_NOT_NULL(last_ddl_sstable)) {
    const int64_t ddl_checkpoint_ts = last_ddl_sstable->get_end_log_ts();
    if (OB_UNLIKELY(ddl_checkpoint_ts < tablet_meta_.ddl_checkpoint_ts_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected clog checkpoint ts", K(ret), K(ddl_checkpoint_ts), K(tablet_meta_));
    } else {
      tablet_meta_.ddl_checkpoint_ts_ = ddl_checkpoint_ts;
    }
  }
  return ret;
}

int ObTablet::try_update_table_store_flag(const ObUpdateTableStoreParam &param)
{
  int ret = OB_SUCCESS;
  if (param.update_with_major_flag_) {
    tablet_meta_.table_store_flag_.set_with_major_sstable();
  }
  return ret;
}

int ObTablet::build_migration_tablet_param(ObMigrationTabletParam &mig_tablet_param) const
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    mig_tablet_param.ls_id_ = tablet_meta_.ls_id_;
    mig_tablet_param.tablet_id_ = tablet_meta_.tablet_id_;
    mig_tablet_param.data_tablet_id_ = tablet_meta_.data_tablet_id_;
    mig_tablet_param.ref_tablet_id_ = tablet_meta_.ref_tablet_id_;
    mig_tablet_param.start_scn_ = tablet_meta_.start_scn_;
    mig_tablet_param.clog_checkpoint_ts_ = tablet_meta_.clog_checkpoint_ts_;
    mig_tablet_param.snapshot_version_ = tablet_meta_.snapshot_version_;
    mig_tablet_param.multi_version_start_ = tablet_meta_.multi_version_start_;
    mig_tablet_param.autoinc_seq_ = tablet_meta_.autoinc_seq_;
    mig_tablet_param.compat_mode_ = tablet_meta_.compat_mode_;
    mig_tablet_param.ha_status_ = tablet_meta_.ha_status_;
    mig_tablet_param.tx_data_ = tablet_meta_.tx_data_;
    mig_tablet_param.table_store_flag_ = tablet_meta_.table_store_flag_;
    mig_tablet_param.ddl_checkpoint_ts_ = tablet_meta_.ddl_checkpoint_ts_;
    mig_tablet_param.ddl_start_log_ts_ = tablet_meta_.ddl_start_log_ts_;
    mig_tablet_param.ddl_snapshot_version_ = tablet_meta_.ddl_snapshot_version_;
    mig_tablet_param.max_sync_storage_schema_version_ = tablet_meta_.max_sync_storage_schema_version_;
    mig_tablet_param.report_status_.reset();

    if (OB_FAIL(mig_tablet_param.storage_schema_.init(mig_tablet_param.allocator_, storage_schema_))) {
      LOG_WARN("failed to copy storage schema", K(ret), K_(tablet_meta));
    } else if (OB_FAIL(mig_tablet_param.ddl_data_.assign(tablet_meta_.ddl_data_))) {
      LOG_WARN("failed to assign ddl data", K(ret), K_(tablet_meta));
    }
  }

  return ret;
}

int ObTablet::build_migration_sstable_param(
    const ObITable::TableKey &table_key,
    blocksstable::ObMigrationSSTableParam &mig_sstable_param) const
{
  int ret = OB_SUCCESS;
  ObTableHandleV2 handle;
  ObSSTable *sstable = nullptr;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!table_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_key));
  } else if (OB_FAIL(table_store_.get_table(table_key, handle))) {
    LOG_WARN("fail to get table from table store", K(ret), K(table_key));
  } else if (OB_FAIL(handle.get_sstable(sstable))) {
    LOG_WARN("fail to get sstable", K(ret), K(sstable));
  } else if (OB_ISNULL(sstable)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error", K(ret), KP(sstable));
  } else {
    const ObSSTableMeta &sstable_meta = sstable->get_meta();
    mig_sstable_param.basic_meta_ = sstable_meta.get_basic_meta();
    mig_sstable_param.table_key_ = table_key;
    if (OB_FAIL(mig_sstable_param.column_checksums_.assign(sstable_meta.get_col_checksum()))) {
      LOG_WARN("fail to assign column checksums", K(ret), K(sstable_meta));
    } else if (OB_FAIL(ObSSTableMergeRes::fill_column_default_checksum_from_schema(&storage_schema_,
        mig_sstable_param.column_default_checksums_))) {
      LOG_WARN("fail to assign column default checksums", K(ret), K(storage_schema_));
    } else {
      mig_sstable_param.table_key_ = sstable->get_key();
    }
  }

  if (OB_FAIL(ret)) {
    mig_sstable_param.reset();
  }
  return ret;
}

int ObTablet::get_ha_sstable_size(int64_t &data_size)
{
  int ret = OB_SUCCESS;
  data_size = 0;
  ObTableStoreIterator iter;
  bool is_ready_for_read = true;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(table_store_.get_ha_tables(iter, is_ready_for_read))) {
    LOG_WARN("failed to get read tables", K(ret));
  } else {
    while (OB_SUCC(ret)) {
      ObITable *table = nullptr;
      ObSSTable *sstable = nullptr;
      if (OB_FAIL(iter.get_next(table))) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        } else {
          LOG_WARN("failed to get read tables", K(ret), K(*this));
        }
      } else if (table->is_memtable()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get migration get memtable", K(ret), KPC(table));
      } else if (FALSE_IT(sstable = static_cast<ObSSTable *> (table))) {
      } else {
        data_size += sstable->get_meta().get_basic_meta().occupy_size_;
      }
    }
  }
  return ret;
}

int ObTablet::fetch_tablet_autoinc_seq_cache(
    const uint64_t cache_size,
    share::ObTabletAutoincInterval &result)
{
  int ret = OB_SUCCESS;
  ObTabletAutoincSeq autoinc_seq;
  uint64_t autoinc_seq_value = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(get_latest_autoinc_seq(autoinc_seq))) {
    LOG_WARN("fail to get latest autoinc seq", K(ret));
  } else if (OB_FAIL(autoinc_seq.get_autoinc_seq_value(autoinc_seq_value))) {
    LOG_WARN("failed to get autoinc seq value", K(ret), K(autoinc_seq));
  } else {
    const uint64_t interval_start = autoinc_seq_value;
    const uint64_t interval_end = autoinc_seq_value + cache_size - 1;
    const uint64_t result_autoinc_seq = autoinc_seq_value + cache_size;
    const ObTabletID &tablet_id = tablet_meta_.tablet_id_;
    int64_t clog_ts = 0;
    if (OB_FAIL(autoinc_seq.set_autoinc_seq_value(result_autoinc_seq))) {
      LOG_WARN("failed to set autoinc seq value", K(ret), K(result_autoinc_seq));
    } else if (OB_FAIL(write_sync_tablet_seq_log(autoinc_seq, result_autoinc_seq, clog_ts))) {
      LOG_WARN("fail to write sync tablet seq log", K(ret));
    } else {
      result.start_ = interval_start;
      result.end_ = interval_end;
      result.tablet_id_ = tablet_id;
    }
  }
  return ret;
}

int ObTablet::get_kept_multi_version_start(
    int64_t &multi_version_start,
    int64_t &min_reserved_snapshot)
{
  int ret = OB_SUCCESS;
  multi_version_start = 0;
  min_reserved_snapshot = 0;
  int64_t min_merged_snapshot = 0;
  ObTenantFreezeInfoMgr *freeze_info_mgr = MTL(ObTenantFreezeInfoMgr*);
  const ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (0 != table_store_.get_major_sstables().count()) {
    min_merged_snapshot = table_store_.get_major_sstables().get_boundary_table(true/*last*/)->get_snapshot_version();
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(freeze_info_mgr->get_min_reserved_snapshot(
      tablet_id, min_merged_snapshot, min_reserved_snapshot))) {
    LOG_WARN("failed to get multi version from freeze info mgr", K(ret), K(tablet_id));
  } else {
    multi_version_start = MIN(MAX(min_reserved_snapshot, get_multi_version_start()), get_snapshot_version());
  }

  return ret;
}

int ObTablet::get_latest_autoinc_seq(ObTabletAutoincSeq &autoinc_seq) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(memtable_mgr_->get_multi_source_data_unit(&autoinc_seq))) {
    if (OB_ENTRY_NOT_EXIST == ret || OB_NOT_SUPPORTED == ret) {
      ret = OB_SUCCESS;
      if (OB_FAIL(autoinc_seq.assign(tablet_meta_.autoinc_seq_))) {
        LOG_WARN("failed to assign autoinc seq", K(ret));
      }
    }
  }
  return ret;
}

int ObTablet::write_sync_tablet_seq_log(ObTabletAutoincSeq &autoinc_seq,
                                        const uint64_t new_autoinc_seq,
                                        int64_t &log_ts)
{
  int ret = OB_SUCCESS;
  const int64_t WAIT_TIME = 1000; // 1ms
  const int64_t SYNC_TABLET_SEQ_LOG_TIMEOUT = 1000L * 1000L * 30L; // 30s
  const ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  const enum ObReplayBarrierType replay_barrier_type = logservice::ObReplayBarrierType::NO_NEED_BARRIER;
  ObLogBaseHeader base_header(ObLogBaseType::TABLET_SEQ_SYNC_LOG_BASE_TYPE, replay_barrier_type);
  ObSyncTabletSeqLog log;
  // NOTICE: ObLogBaseHeader & ObSyncTabletSeqLog should have fixed serialize size!
  const int64_t buffer_size = base_header.get_serialize_size() + log.get_serialize_size();
  char buffer[buffer_size];
  int64_t retry_cnt = 0;
  int64_t pos = 0;
  ObSyncTabletSeqLogCb *cb = nullptr;
  ObLogHandler *log_handler = get_log_handler();
  palf::LSN lsn;
  const bool need_nonblock= false;
  const int64_t ref_ts_ns = 0;

  if (OB_FAIL(log.init(tablet_id, new_autoinc_seq))) {
    LOG_WARN("fail to init SyncTabletSeqLog", K(tablet_id), K(new_autoinc_seq));
  } else if (OB_ISNULL(cb = op_alloc(ObSyncTabletSeqLogCb))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc memory", K(ret));
  } else if (OB_FAIL(cb->init(tablet_meta_.ls_id_, tablet_id, new_autoinc_seq))) {
    LOG_WARN("failed to init ObSyncTabletSeqLogCb", K(ret), K(autoinc_seq), K(new_autoinc_seq));
  } else if (OB_FAIL(base_header.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("failed to serialize log base header", K(ret));
  } else if (OB_FAIL(log.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("fail to serialize sync tablet seq log", K(ret));
  } else if (autoinc_seq.get_intervals().count() == 0 && OB_FAIL(autoinc_seq.set_autoinc_seq_value(1))) {
    // need to do this to ensure the intervals list size is always 1, so the memory size is same before and after clog.
    LOG_WARN("failed to set autoinc seq value", K(ret));
  } else if (OB_FAIL(save_multi_source_data_unit(&autoinc_seq, ObLogTsRange::MAX_TS,
      false/*for_replay*/, memtable::MemtableRefOp::INC_REF))) {
    if (OB_BLOCK_FROZEN == ret) {
      ret = OB_EAGAIN;
    }
    LOG_WARN("failed to inc ref for auto inc seq", K(ret));
  } else if (OB_FAIL(log_handler->append(buffer,
                                         buffer_size,
                                         ref_ts_ns,
                                         need_nonblock,
                                         cb,
                                         lsn,
                                         log_ts))) {
    LOG_WARN("fail to submit sync tablet seq log", K(ret), K(buffer_size));
    // rollback, dec ref
    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(save_multi_source_data_unit(&autoinc_seq, ObLogTsRange::MAX_TS,
        false/*for_replay*/, memtable::MemtableRefOp::DEC_REF, true/*is_callback*/))) {
      LOG_ERROR("failed to dec ref for auto inc seq", K(tmp_ret));
      ob_usleep(1000 * 1000);
      ob_abort();
    }
  } else {
    // wait unti majority
    bool wait_timeout = false;
    int64_t start_time = ObTimeUtility::fast_current_time();
    while (!cb->is_finished() && !wait_timeout) {
      ob_usleep(WAIT_TIME);
      retry_cnt++;
      if (retry_cnt % 1000 == 0) {
        if (ObTimeUtility::fast_current_time() - start_time > SYNC_TABLET_SEQ_LOG_TIMEOUT) {
          wait_timeout = true;
        }
        LOG_WARN("submit sync tablet seq log wait too much time", K(retry_cnt), K(wait_timeout));
      }
    }
    if (wait_timeout) {
      ret = OB_TIMEOUT;
      LOG_WARN("submit sync tablet seq log timeout", K(ret));
    } else if (cb->is_failed()) {
      ret = cb->get_ret_code();
      LOG_WARN("submit sync tablet seq log failed", K(ret));
    } else {
      int64_t wait_time = ObTimeUtility::fast_current_time() - start_time;
      LOG_INFO("submit sync tablet seq log succeed", K(tablet_id), K(new_autoinc_seq), K(lsn), K(log_ts), K(wait_time));
    }
    if (nullptr != cb) {
      cb->try_release();
      cb = nullptr;
    }
  }
  if (OB_FAIL(ret) && nullptr != cb) {
    op_free(cb);
    cb = nullptr;
  }
  return ret;
}

int ObTablet::update_tablet_autoinc_seq(const uint64_t autoinc_seq, const int64_t replay_log_ts)
{
  int ret = OB_SUCCESS;
  ObTabletAutoincSeq curr_autoinc_seq;
  uint64_t curr_autoinc_seq_value;
  int64_t log_ts = replay_log_ts;
  bool is_replay = replay_log_ts != ObLogTsRange::MAX_TS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));

  } else if (OB_FAIL(get_latest_autoinc_seq(curr_autoinc_seq))) {
    LOG_WARN("fail to get latest autoinc seq", K(ret));
  } else if (OB_FAIL(curr_autoinc_seq.get_autoinc_seq_value(curr_autoinc_seq_value))) {
    LOG_WARN("failed to get autoinc seq value", K(ret));
  } else if (autoinc_seq > curr_autoinc_seq_value) {
    if (!is_replay) {
      // this is not replay, then we need to write clog by ourselves
      if (OB_FAIL(write_sync_tablet_seq_log(curr_autoinc_seq, autoinc_seq, log_ts))) {
        LOG_WARN("fail to write sync tablet seq log", K(ret));
      }
    } else if (OB_FAIL(curr_autoinc_seq.set_autoinc_seq_value(autoinc_seq))) {
      LOG_WARN("failed to set autoinc seq value", K(ret), K(autoinc_seq));
    } else if (OB_FAIL(save_multi_source_data_unit(&curr_autoinc_seq,
                                                   log_ts,
                                                   is_replay))) {
      LOG_WARN("failed to save autoinc seq", K(ret), K(curr_autoinc_seq));
    }
  }
  return ret;
}

int ObTablet::start_ddl_if_need()
{
  int ret = OB_SUCCESS;
  ObDDLKvMgrHandle ddl_kv_mgr_handle;
  if (tablet_meta_.ddl_start_log_ts_ <= 0) {
    LOG_DEBUG("no need to start ddl kv manager", K(ret), K(tablet_meta_));
  } else if (OB_FAIL(get_ddl_kv_mgr(ddl_kv_mgr_handle, true/*try_create*/))) {
    LOG_WARN("create ddl kv mgr failed", K(ret));
  } else {
    ObITable::TableKey table_key;
    table_key.table_type_ = ObITable::TableType::MAJOR_SSTABLE;
    table_key.tablet_id_ = tablet_meta_.tablet_id_;
    table_key.version_range_.base_version_ = 0;
    table_key.version_range_.snapshot_version_ = tablet_meta_.ddl_snapshot_version_;
    const int64_t start_log_ts = tablet_meta_.ddl_start_log_ts_;
    if (OB_FAIL(ddl_kv_mgr_handle.get_obj()->ddl_start(table_key,
                                                       start_log_ts,
                                                       GET_MIN_CLUSTER_VERSION(),
                                                       tablet_meta_.ddl_checkpoint_ts_))) {
      LOG_WARN("start ddl kv manager failed", K(ret), K(table_key), K(start_log_ts));
    }
  }
  return ret;
}

int ObTablet::check_schema_version_elapsed(
    const int64_t schema_version,
    const bool need_wait_trans_end,
    int64_t &max_commit_version,
    transaction::ObTransID &pending_tx_id)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = MTL_ID();
  const ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  ObMultiVersionSchemaService *schema_service = MTL(ObTenantSchemaService*)->get_schema_service();
  int64_t log_ts = 0;
  int64_t tenant_refreshed_schema_version = 0;
  int64_t refreshed_schema_ts = 0;
  int64_t refreshed_schema_version = 0;
  max_commit_version = 0L;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTablet has not been inited", K(ret));
  } else if (OB_UNLIKELY(schema_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(schema_version));
  } else if (!need_wait_trans_end) {
    // obtain_snapshot of offline ddl don't need to wait trans end.
    transaction::ObTransService *txs = MTL(transaction::ObTransService*);
    if (OB_FAIL(txs->get_max_commit_version(max_commit_version))) {
      LOG_WARN("fail to get max commit version", K(ret));
    }
  } else {
    if (OB_FAIL(get_ddl_info(refreshed_schema_version, refreshed_schema_ts))) {
      LOG_WARN("get ddl info failed", K(ret));
    } else if (refreshed_schema_version >= schema_version) {
      // schema version already refreshed
    } else if (OB_FAIL(schema_service->get_tenant_refreshed_schema_version(tenant_id, tenant_refreshed_schema_version))) {
      LOG_WARN("get tenant refreshed schema version failed", K(ret));
    } else if (tenant_refreshed_schema_version < schema_version) {
      ret = OB_EAGAIN;
      LOG_WARN("current schema version not latest, need retry", K(ret), K(schema_version), K(tenant_refreshed_schema_version));
    } else if (OB_FAIL(replay_schema_version_change_log(schema_version))) {
      LOG_WARN("set schema change version clog failed", K(ret), K(schema_version));
    } else if (OB_FAIL(write_tablet_schema_version_change_clog(schema_version, log_ts))) {
      LOG_WARN("write partition schema version change clog error", K(ret), K(schema_version));
      // override ret
      ret = OB_EAGAIN;
    } else if (OB_FAIL(update_ddl_info(schema_version, log_ts, refreshed_schema_ts))) {
      LOG_WARN("update ddl info failed", K(ret));
    }

    if (OB_SUCC(ret)) {
      transaction::ObTransService *txs = MTL(transaction::ObTransService*);
      ObLSService *ls_service = MTL(ObLSService*);
      ObLSHandle ls_handle;
      if (OB_FAIL(ls_service->get_ls(tablet_meta_.ls_id_, ls_handle, ObLSGetMod::TABLET_MOD))) {
        LOG_WARN("failed to get ls", K(ret), "ls_id", tablet_meta_.ls_id_);
      } else if (OB_FAIL(ls_handle.get_ls()->check_modify_schema_elapsed(tablet_id, schema_version, pending_tx_id))) {
        if (OB_EAGAIN != ret) {
          LOG_WARN("check schema version elapsed failed", K(ret), K(tablet_id), K(schema_version));
        } else {
          LOG_INFO("check schema version elapsed again", K(ret), K(tablet_id), K(schema_version), K(refreshed_schema_ts));
        }
      } else if (OB_FAIL(txs->get_max_commit_version(max_commit_version))) {
        LOG_WARN("fail to get max commit version", K(ret));
      } else {
        LOG_INFO("check wait trans end", K(max_commit_version), K(refreshed_schema_ts));
      }
    }
  }
  return ret;
}

int ObTablet::write_tablet_schema_version_change_clog(
    const int64_t schema_version,
    int64_t &log_ts)
{
  int ret = OB_SUCCESS;
  const ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  ObTabletSchemaVersionChangeLog log;
  if (OB_FAIL(log.init(tablet_id, schema_version))) {
    LOG_WARN("fail to init tablet schema version change log", K(ret), K(tablet_id), K(schema_version));
  } else {
    const int64_t CHECK_SCHEMA_VERSION_CHANGE_LOG_US = 1000;
    const int64_t CHECK_SCHEMA_VERSION_CHANGE_LOG_TIMEOUT = 1000L * 1000L * 30L; // 30s
    const enum ObReplayBarrierType replay_barrier_type = ObReplayBarrierType::STRICT_BARRIER;
    ObLogBaseHeader base_header(ObLogBaseType::DDL_LOG_BASE_TYPE, replay_barrier_type);
    ObDDLClogHeader ddl_header(ObDDLClogType::DDL_TABLET_SCHEMA_VERSION_CHANGE_LOG);
    const int64_t buffer_size = base_header.get_serialize_size() + ddl_header.get_serialize_size()
                              + log.get_serialize_size();
    char buffer[buffer_size];
    int64_t retry_cnt = 0;
    int64_t pos = 0;
    ObDDLClogCb *cb = nullptr;
    ObLogHandler *log_handler = get_log_handler();

    palf::LSN lsn;
    const bool need_nonblock= false;
    const int64_t ref_ts_ns = 0;
    log_ts = 0;

    if (OB_ISNULL(cb = op_alloc(ObDDLClogCb))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc memory", K(ret));
    } else if (OB_FAIL(base_header.serialize(buffer, buffer_size, pos))) {
      LOG_WARN("failed to serialize log base header", K(ret));
    } else if (OB_FAIL(ddl_header.serialize(buffer, buffer_size, pos))) {
      LOG_WARN("fail to seriaize sync tablet seq log", K(ret));
    } else if (OB_FAIL(log.serialize(buffer, buffer_size, pos))) {
      LOG_WARN("fail to seriaize schema version change log", K(ret));
    } else if (OB_FAIL(log_handler->append(buffer,
                                           buffer_size,
                                           ref_ts_ns,
                                           need_nonblock,
                                           cb,
                                           lsn,
                                           log_ts))) {
      LOG_WARN("fail to submit schema version change log", K(ret), K(buffer_size));
    } else {
      ObDDLClogCb *tmp_cb = cb;
      cb = nullptr;
      // wait unti majority
      bool wait_timeout = false;
      int64_t start_time = ObTimeUtility::fast_current_time();
      while (!tmp_cb->is_finished() && !wait_timeout) {
        ob_usleep(CHECK_SCHEMA_VERSION_CHANGE_LOG_US);
        retry_cnt++;
        if (retry_cnt % 1000 == 0) {
          if (ObTimeUtility::fast_current_time() - start_time > CHECK_SCHEMA_VERSION_CHANGE_LOG_TIMEOUT) {
            wait_timeout = true;
          }
          LOG_WARN("submit schema version change log wait too much time", K(retry_cnt), K(wait_timeout));
        }
      }
      if (wait_timeout) {
        ret = OB_TIMEOUT;
        LOG_WARN("submit schema version change log timeout", K(ret));
      } else if (tmp_cb->is_failed()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("submit schema version change log failed", K(ret));
      } else {
        LOG_INFO("submit schema version change log succeed", K(tablet_id), K(schema_version));
      }
      tmp_cb->try_release(); // release the memory no matter succ or not
    }
    if (nullptr != cb) {
      op_free(cb);
      cb = nullptr;
    }
  }
  return ret;
}

int ObTablet::replay_schema_version_change_log(const int64_t schema_version)
{
  int ret = OB_SUCCESS;
  common::ObSEArray<ObTableHandleV2, 8> table_handle_array;
  ObIMemtableMgr *memtable_mgr = nullptr;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (OB_FAIL(memtable_mgr->get_all_memtables(table_handle_array))) {
    LOG_WARN("failed to get memtables", K(ret));
  } else {
    memtable::ObMemtable *memtable = nullptr;
    const int64_t table_num = table_handle_array.count();
    if (0 == table_num) {
      // no memtable, no need to replay schema version change
    } else if (!table_handle_array[table_num - 1].is_valid()) {
      ret = OB_ERR_SYS;
      LOG_WARN("latest memtable is invalid", K(ret));
    } else if (OB_FAIL(table_handle_array[table_num - 1].get_data_memtable(memtable))) {
      LOG_WARN("fail to get memtable", K(ret));
    } else if (OB_ISNULL(memtable)) {
      ret = OB_ERR_SYS;
      LOG_WARN("memtable is null", K(ret), KP(memtable));
    } else if (OB_FAIL(memtable->replay_schema_version_change_log(schema_version))) {
      LOG_WARN("fail to replay schema version change log", K(ret), K(schema_version));
    }
  }

  return ret;
}

int ObTablet::get_tablet_report_info(
    common::ObIArray<int64_t> &column_checksums,
    int64_t &data_size,
    int64_t &required_size,
    const bool need_checksums)
{
  int ret = OB_SUCCESS;
  column_checksums.reset();
  data_size = 0;
  required_size = 0;
  const ObSSTable *main_major = nullptr;
  const ObSSTableArray &major_sstables = table_store_.get_major_sstables();

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (major_sstables.count_ == 0) {
    ret = OB_TABLET_NOT_EXIST;
    LOG_INFO("no major sstables in this tablet, cannot report", K(ret));
  } else if (OB_ISNULL(main_major = static_cast<ObSSTable *>(major_sstables.get_boundary_table(true)))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("faild to get unexpected null major", K(ret));
  } else if (need_checksums && OB_FAIL(column_checksums.assign(main_major->get_meta().get_col_checksum()))) {
    LOG_WARN("failed to assign column checksums", K(ret));
  }
  if (OB_SUCC(ret)) {
    data_size = main_major->get_meta().get_basic_meta().occupy_size_;
    const int64_t macro_block_size = OB_SERVER_BLOCK_MGR.get_macro_block_size();
    for (int64_t i = 0; OB_SUCC(ret) && i < major_sstables.count_; ++i) {
      const ObSSTable *table = static_cast<ObSSTable *>(major_sstables[i]);
      const ObSSTableBasicMeta &basic_meta = table->get_meta().get_basic_meta();
      if (0 == i) {
        required_size += (basic_meta.get_total_macro_block_count()) * macro_block_size;
      } else {
        required_size += (basic_meta.get_total_macro_block_count() - basic_meta.get_total_use_old_macro_block_count()) * macro_block_size;
      }
    }
  }
  return ret;
}

int ObTablet::get_ddl_sstable_handles(ObTablesHandleArray &ddl_sstable_handles)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_FAIL(table_store_.get_ddl_sstable_handles(ddl_sstable_handles))) {
    LOG_WARN("fail to get ddl sstable handles", K(ret));
  }
  return ret;
}

int ObTablet::get_ha_tables(
    ObTableStoreIterator &iter,
    bool &is_ready_for_read)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(table_store_.get_ha_tables(iter, is_ready_for_read))) {
    LOG_WARN("failed to get read tables", K(ret));
  }

  return ret;
}

int ObTablet::update_ddl_info(
    const int64_t schema_version,
    const int64_t log_ts,
    int64_t &schema_refreshed_ts)
{
  int ret = OB_SUCCESS;
  ObTabletPointer *tablet_ptr = static_cast<ObTabletPointer*>(pointer_hdl_.get_resource_ptr());
  if (OB_FAIL(tablet_ptr->ddl_info_.update(schema_version, log_ts, schema_refreshed_ts))) {
    LOG_WARN("fail to update ddl info", K(ret), K(schema_version), K(log_ts));
  }
  return ret;
}

int ObTablet::get_ddl_info(int64_t &schema_version, int64_t &schema_refreshed_ts) const
{
  int ret = OB_SUCCESS;
  ObTabletPointer *tablet_ptr = static_cast<ObTabletPointer*>(pointer_hdl_.get_resource_ptr());
  if (OB_FAIL(tablet_ptr->ddl_info_.get(schema_version, schema_refreshed_ts))) {
    LOG_WARN("fail to update ddl info", K(ret));
  }
  return ret;
}

// only for redo
int ObTablet::set_tx_log_ts(
    const transaction::ObTransID &tx_id,
    const int64_t log_ts,
    const bool for_replay)
{
  int ret = OB_SUCCESS;
  ObTabletTxMultiSourceDataUnit tx_data;

  if (OB_UNLIKELY(log_ts <= OB_INVALID_TIMESTAMP || INT64_MAX == log_ts)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(log_ts));
  } else if (OB_FAIL(get_tx_data(tx_data))) {
    LOG_WARN("failed to get tx data", K(ret));
  } else if (OB_UNLIKELY(tx_data.tx_id_ != tx_id)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("set log ts for non-locked tablet", K(ret), K(tx_data), K(tx_id), K(get_tablet_meta()));
  } else {
    tx_data.tx_log_ts_ = log_ts;
    if (OB_FAIL(save_multi_source_data_unit(&tx_data, log_ts, for_replay, memtable::MemtableRefOp::DEC_REF, true/*is_callback*/))) {
      LOG_WARN("failed to save tx data", K(ret), K(tx_data), K(log_ts));
    }
  }

  return ret;
}

// only for commit, abort
int ObTablet::set_tablet_final_status(
    ObTabletTxMultiSourceDataUnit &tx_data,
    const int64_t memtable_log_ts,
    const bool for_replay,
    const memtable::MemtableRefOp ref_op)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(ObTabletStatus::Status::MAX == tx_data.tablet_status_)
      || OB_UNLIKELY(for_replay && tx_data.tx_log_ts_ <= OB_INVALID_TIMESTAMP)
      || OB_UNLIKELY(INT64_MAX == tx_data.tx_log_ts_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tx_data), K(memtable_log_ts), K(for_replay));
  } else if (OB_FAIL(set_multi_data_for_commit(tx_data, memtable_log_ts, for_replay, ref_op))) {
    LOG_WARN("failed to save tx data", K(ret), K(tx_data), K(memtable_log_ts), K(for_replay), K(ref_op));
  }

  return ret;
}

int ObTablet::set_tx_data(
    const ObTabletTxMultiSourceDataUnit &tx_data,
    const bool for_replay,
    const MemtableRefOp ref_op,
    const bool is_callback)
{
  int ret = OB_SUCCESS;
  const int64_t log_ts = tx_data.tx_log_ts_;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tx_data.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tx_data));
  } else if (OB_FAIL(save_multi_source_data_unit(&tx_data, log_ts, for_replay, ref_op, is_callback))) {
    LOG_WARN("failed to save tx data", K(ret), K(tx_data), K(log_ts), K(for_replay), K(ref_op), K(is_callback));
  }

  return ret;
}

int ObTablet::set_tx_data(
    const ObTabletTxMultiSourceDataUnit &tx_data,
    const int64_t memtable_log_ts,
    const bool for_replay,
    const memtable::MemtableRefOp ref_op,
    const bool is_callback)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tx_data.is_valid())
      || OB_UNLIKELY(memtable_log_ts < ObLogTsRange::MIN_TS)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tx_data), K(memtable_log_ts));
  } else if (OB_FAIL(save_multi_source_data_unit(&tx_data, memtable_log_ts, for_replay, ref_op, is_callback))) {
    LOG_WARN("failed to save tx data", K(ret), K(tx_data), K(memtable_log_ts), K(for_replay), K(ref_op), K(is_callback));
  }

  return ret;
}

int ObTablet::inner_get_tx_data(ObTabletTxMultiSourceDataUnit &tx_data, bool &exist_on_memtable) const
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = tablet_meta_.ls_id_;
  const common::ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  exist_on_memtable = false;

  if (OB_FAIL(get_msd_from_memtable(tx_data))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      exist_on_memtable = false;
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to get msd from memtable", K(ret), K(ls_id), K(tablet_id));
    }
  } else {
    exist_on_memtable = true;
  }

  if (OB_FAIL(ret)) {
  } else if (exist_on_memtable) {
  } else if (OB_FAIL(tx_data.deep_copy(&tablet_meta_.tx_data_))) {
    LOG_WARN("failed to get tx data from tablet", K(ret), K(ls_id), K(tablet_id), "tx_data", tablet_meta_.tx_data_);
  }

  return ret;
}

int ObTablet::get_tx_data(ObTabletTxMultiSourceDataUnit &tx_data) const
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = MTL_ID();
  const share::ObLSID &ls_id = tablet_meta_.ls_id_;
  const common::ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  bool exist_on_memtable = true;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited), K(tenant_id), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(inner_get_tx_data(tx_data, exist_on_memtable))) {
    LOG_WARN("fail to inner get tx data", K(ret));
  } else if (OB_UNLIKELY(!tx_data.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, tx data is invalid", K(ret), K(tenant_id), K(ls_id), K(tablet_id),
        K(exist_on_memtable), K(tx_data));
  }
  return ret;
}

int ObTablet::check_tx_data(bool &is_valid) const
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = tablet_meta_.ls_id_;
  const common::ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  ObTabletTxMultiSourceDataUnit tx_data;
  bool exist_on_memtable = true;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(inner_get_tx_data(tx_data, exist_on_memtable))) {
    LOG_WARN("fail to inner get tx data", K(ret), K(ls_id), K(tablet_id));
  } else {
    is_valid = tx_data.is_valid();
  }
  return ret;
}

int ObTablet::get_tablet_status(ObTabletStatus::Status &tablet_status)
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = tablet_meta_.ls_id_;
  const common::ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  ObTabletTxMultiSourceDataUnit tx_data;
  bool exist_on_memtable = true;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(inner_get_tx_data(tx_data, exist_on_memtable))) {
    LOG_WARN("failed to get tx data", K(ret), K(ls_id), K(tablet_id));
  }

  if (OB_FAIL(ret)) {
  } else if (OB_UNLIKELY(ObTabletStatus::MAX == tx_data.tablet_status_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, tablet status is invalid", K(ret), K(ls_id), K(tablet_id), K(tx_data));
  } else {
    tablet_status = tx_data.tablet_status_;
  }

  return ret;
}

int ObTablet::get_rec_log_ts(int64_t &rec_log_ts)
{
  int ret = OB_SUCCESS;
  rec_log_ts = INT64_MAX;
  ObTableHandleV2 handle;
  memtable::ObMemtable *mt;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", KR(ret), K_(is_inited));
  } else if (OB_ISNULL(memtable_mgr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("memtable_mgr is NULL", KR(ret), KPC(this));
  } else if (OB_FAIL(memtable_mgr_->get_first_memtable(handle))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("fail to get first memtable", KR(ret), K(handle));
    }
  } else if (OB_FAIL(handle.get_data_memtable(mt))) {
    LOG_WARN("fail to get data memtables", KR(ret), K(handle));
  } else if (OB_ISNULL(mt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("mt is NULL", KR(ret), K(handle));
  } else {
    rec_log_ts = mt->get_rec_log_ts();
  }
  return ret;
}

int ObTablet::get_ddl_data(ObTabletBindingInfo &info) const
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = tablet_meta_.ls_id_;
  const common::ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  bool exist_on_memtable = false;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(get_msd_from_memtable(info))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      exist_on_memtable = false;
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to get msd from memtable", K(ret), K(ls_id), K(tablet_id));
    }
  } else {
    exist_on_memtable = true;
  }

  if (OB_FAIL(ret)) {
  } else if (exist_on_memtable) {
  } else if (OB_FAIL(info.assign(tablet_meta_.ddl_data_))) {
    LOG_WARN("failed to get tx data from tablet", K(ret), K(ls_id), K(tablet_id), "ddl_data", tablet_meta_.ddl_data_);
  }

  return ret;
}

int ObTablet::get_msd_from_memtable(memtable::ObIMultiSourceDataUnit &msd) const
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = tablet_meta_.ls_id_;
  const common::ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  bool exist_on_memtable = false;

  if (is_ls_inner_tablet()) {
    // won't do anything for ls inner tablet
  } else {
    ObSEArray<ObITable*, MAX_MEMSTORE_CNT> memtable_array;
    if (OB_FAIL(get_memtables(memtable_array, true/*need_active*/))) {
      LOG_WARN("failed to get memtables", K(ret));
    } else {
      for (int64_t i = memtable_array.count() - 1; OB_SUCC(ret) && !exist_on_memtable && i >= 0; --i) {
        ObMemtable *memtable = static_cast<ObMemtable*>(memtable_array[i]);
        if (OB_ISNULL(memtable)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected error, memtable is null", K(ret), K(i), KP(memtable));
        } else if (OB_FAIL(memtable->get_multi_source_data_unit(&msd, nullptr/*allocator*/))) {
          if (OB_ENTRY_NOT_EXIST == ret) {
            LOG_DEBUG("multi source data does not exist on memtable", K(ret), K(ls_id), K(tablet_id), K(tablet_meta_));
            ret = OB_SUCCESS;
            exist_on_memtable = false;
          } else {
            LOG_WARN("failed to get multi source data", K(ret), K(ls_id), K(tablet_id));
          }
        } else {
          exist_on_memtable = true;
          LOG_DEBUG("succeeded to get msd from memtable", K(ret), K(ls_id), K(tablet_id), KPC(memtable));
        }
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (!exist_on_memtable) {
    ret = OB_ENTRY_NOT_EXIST;
    LOG_DEBUG("msd does not exist on memtable", K(ret), K(ls_id), K(tablet_id));
  }

  return ret;
}

int ObTablet::set_tx_data_in_tablet_pointer(const ObTabletTxMultiSourceDataUnit &tx_data)
{
  int ret = OB_SUCCESS;
  const ObTabletMapKey key(tablet_meta_.ls_id_, tablet_meta_.tablet_id_);
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  if (OB_FAIL(t3m->set_tablet_pointer_tx_data(key, tx_data))) {
    LOG_WARN("failed to set tx data in tablet pointer", K(ret), K(key), K(tx_data));
  }

  return ret;
}

int ObTablet::set_tx_data_in_tablet_pointer()
{
  int ret = OB_SUCCESS;
  const ObLSID &ls_id = tablet_meta_.ls_id_;
  const ObTabletID &tablet_id = tablet_meta_.tablet_id_;
  ObTabletTxMultiSourceDataUnit &tx_data = tablet_meta_.tx_data_;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(set_tx_data_in_tablet_pointer(tx_data))) {
    LOG_WARN("failed to set tx data in tablet pointer", K(ret), K(ls_id), K(tablet_id), K(tx_data));
  }

  return ret;
}

int ObTablet::allow_to_read_()
{
  int ret = OB_SUCCESS;
  const bool is_ready_for_read = tablet_meta_.ha_status_.is_none();

  if (!is_ready_for_read) {
    ret = OB_REPLICA_NOT_READABLE;
    LOG_WARN("tablet not allowed to read", K(ret), K(tablet_meta_));
  }
  return ret;
}

int ObTablet::check_max_sync_schema_version() const
{
  int ret = OB_SUCCESS;
  int64_t max_sync_schema_version = 0;
  if (is_ls_inner_tablet()) {
    // do nothing
  } else if (OB_FAIL(get_max_sync_storage_schema_version(max_sync_schema_version))) {
    LOG_WARN("failed to get max sync storage schema version", K(ret));
  } else if (max_sync_schema_version > storage_schema_.schema_version_) { // need check memtable
    ObArenaAllocator tmp_allocator;
    ObStorageSchema storage_schema;
    ObIMemtableMgr *memtable_mgr = nullptr;
    ObTabletMemtableMgr *data_memtable_mgr = nullptr;
    if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
      LOG_WARN("failed to get memtable mgr", K(ret));
    } else if (FALSE_IT(data_memtable_mgr = static_cast<ObTabletMemtableMgr *>(memtable_mgr))) {
    } else if (OB_UNLIKELY(!data_memtable_mgr->get_storage_schema_recorder().is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("schema recorder is invalid", K(ret), K_(tablet_meta), KPC(data_memtable_mgr));
    } else if (OB_FAIL(data_memtable_mgr->get_multi_source_data_unit(&storage_schema, &tmp_allocator))) {
      LOG_ERROR("failed to storage schema from memtable, max_sync_schema_version is invalid", K(ret),
          K(max_sync_schema_version), KPC(data_memtable_mgr));
    } else if (OB_UNLIKELY(storage_schema.schema_version_ < max_sync_schema_version)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("unexpected max sync schema version", K(ret), K(max_sync_schema_version),
          "storage_schema_on_memtable", storage_schema,
          "storage_schema_on_tablet", storage_schema_, K_(tablet_meta), KPC(data_memtable_mgr));
    }
  }
  return ret;
}

int ObTablet::set_memtable_clog_checkpoint_ts(
    const ObMigrationTabletParam *tablet_meta)
{
  int ret = OB_SUCCESS;
  ObIMemtableMgr *memtable_mgr = nullptr;
  ObTableHandleV2 handle;
  memtable::ObMemtable *memtable = nullptr;

  if (OB_ISNULL(tablet_meta)) {
    //no need to set memtable clog checkpoint ts
  } else if (tablet_meta->clog_checkpoint_ts_ <= tablet_meta_.clog_checkpoint_ts_) {
    //do nothing
  } else if (OB_FAIL(get_memtable_mgr(memtable_mgr))) {
    LOG_WARN("failed to get memtable mgr", K(ret));
  } else if (tablet_meta_.tablet_id_.is_ls_inner_tablet()) {
    if (memtable_mgr->has_memtable()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ls inner tablet should not has memtable", K(ret), KPC(tablet_meta));
    }
  } else if (OB_FAIL(memtable_mgr->get_active_memtable(handle))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("failed to get active memtable for tablet", K(ret), KPC(this), KPC(tablet_meta));
    } else {
      ret = OB_SUCCESS;
    }
  } else if (OB_FAIL(handle.get_data_memtable(memtable))) {
    LOG_WARN("failed to get memtalbe", K(ret), K(handle));
  } else if (OB_ISNULL(memtable)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null memtable", K(ret), KPC(memtable));
  } else if (OB_FAIL(memtable->set_migration_clog_checkpoint_ts(tablet_meta->clog_checkpoint_ts_))) {
    LOG_WARN("failed to set migration clog checkpoint ts", K(ret), K(handle), KPC(this));
  }
  return ret;
}


} // namespace storage
} // namespace oceanbase
