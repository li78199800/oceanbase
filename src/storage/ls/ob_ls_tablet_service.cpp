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

#include "storage/ls/ob_ls_tablet_service.h"

#include "common/row/ob_row_store.h"
#include "lib/objectpool/ob_server_object_pool.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/utility.h"
#include "logservice/ob_log_base_header.h"
#include "logservice/ob_log_base_type.h"
#include "logservice/ob_log_service.h"
#include "observer/report/ob_i_meta_report.h"
#include "share/ob_disk_usage_table_operator.h"
#include "share/ob_rpc_struct.h"
#include "share/rc/ob_tenant_base.h"
#include "share/schema/ob_table_param.h"
#include "share/schema/ob_tenant_schema_service.h"
#include "share/ob_ddl_common.h"
#include "storage/blocksstable/ob_index_block_builder.h"
#include "storage/blocksstable/ob_sstable_meta.h"
#include "storage/ob_dml_running_ctx.h"
#include "storage/ob_partition_range_spliter.h"
#include "storage/ob_query_iterator_factory.h"
#include "storage/ob_relative_table.h"
#include "storage/ob_row_reshape.h"
#include "storage/ob_storage_struct.h"
#include "storage/ob_storage_table_guard.h"
#include "storage/ob_value_row_iterator.h"
#include "storage/access/ob_table_scan_iterator.h"
#include "storage/access/ob_table_scan_range.h"
#include "storage/access/ob_rows_info.h"
#include "storage/access/ob_table_scan_range.h"
#include "storage/access/ob_rows_info.h"
#include "storage/access/ob_table_estimator.h"
#include "storage/access/ob_index_sstable_estimator.h"
#include "storage/blocksstable/ob_sstable.h"
#include "storage/ls/ob_ls.h"
#include "storage/tablet/ob_tablet.h"
#include "storage/tablet/ob_tablet_iterator.h"
#include "storage/tablet/ob_tablet_binding_helper.h"
#include "storage/tablet/ob_tablet_create_delete_helper.h"
#include "storage/tablet/ob_tablet_create_sstable_param.h"
#include "storage/tablet/ob_tablet_service_clog_replay_executor.h"
#include "storage/tablet/ob_tablet_status.h"
#include "storage/tablet/ob_tablet_slog_helper.h"
#include "storage/tx/ob_trans_define.h"
#include "storage/tx/ob_trans_part_ctx.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/meta_mem/ob_tablet_map_key.h"
#include "storage/meta_mem/ob_tenant_meta_mem_mgr.h"
#include "storage/slog/ob_storage_log_replayer.h"
#include "storage/slog/ob_storage_log_struct.h"
#include "storage/slog/ob_storage_logger.h"

using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::blocksstable;

namespace oceanbase
{
namespace storage
{
ObLSTabletService::ObLSTabletService()
  : ls_(nullptr),
    tx_data_memtable_mgr_(),
    tx_ctx_memtable_mgr_(),
    lock_memtable_mgr_(),
    tablet_id_set_(),
    bucket_lock_(),
    rs_reporter_(nullptr),
    is_inited_(false)
{
}

ObLSTabletService::~ObLSTabletService()
{
}

int ObLSTabletService::init(
    ObLS *ls,
    observer::ObIMetaReport *rs_reporter)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K_(is_inited));
  } else if (OB_ISNULL(ls) || OB_ISNULL(rs_reporter)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(ls), KP(rs_reporter));
  } else if (OB_FAIL(tablet_id_set_.init(ObTabletCommon::BUCKET_LOCK_BUCKET_CNT))) {
    LOG_WARN("fail to init tablet id set", K(ret));
  } else if (OB_FAIL(bucket_lock_.init(ObTabletCommon::BUCKET_LOCK_BUCKET_CNT,
      ObLatchIds::TABLET_BUCKET_LOCK))) {
    LOG_WARN("failed to init bucket lock", K(ret));
  } else {
    ls_ = ls;
    rs_reporter_ = rs_reporter;
    is_inited_ = true;
  }

  if (OB_UNLIKELY(!is_inited_)) {
    destroy();
  }

  return ret;
}

void ObLSTabletService::destroy()
{
  delete_all_tablets();
  tablet_id_set_.destroy();
  tx_data_memtable_mgr_.destroy();
  tx_ctx_memtable_mgr_.destroy();
  lock_memtable_mgr_.destroy();
  bucket_lock_.destroy();
  rs_reporter_ = nullptr;
  ls_= nullptr;
  is_inited_ = false;
}

int ObLSTabletService::offline()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    DestroyMemtableAndMemberOperator clean_mem_op(this);
    if (OB_FAIL(tablet_id_set_.foreach(clean_mem_op))) {
      LOG_WARN("fail to clean memtables", K(ret), K(clean_mem_op.cur_tablet_id_));
    }
  }
  return ret;
}

int ObLSTabletService::online()
{
  return OB_SUCCESS;
}

int ObLSTabletService::replay(
    const void *buffer,
    const int64_t nbytes,
    const palf::LSN &lsn,
    const int64_t ts_ns)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  const char *log_buf = static_cast<const char *>(buffer);

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(ObTabletServiceClogReplayExecutor::execute(ls_, log_buf, nbytes,
      pos, lsn, ts_ns))) {
    LOG_WARN("replay tablet log error", K(ret), K(lsn), K(ts_ns));
  }

  return ret;
}

void ObLSTabletService::switch_to_follower_forcedly()
{
  // TODO
}

int ObLSTabletService::switch_to_leader()
{
  int ret = OB_SUCCESS;
  //TODO
  return ret;
}

int ObLSTabletService::switch_to_follower_gracefully()
{
  int ret = OB_SUCCESS;
  //TODO
  return ret;
}

int ObLSTabletService::resume_leader()
{
  int ret = OB_SUCCESS;
  //TODO
  return ret;
}

int ObLSTabletService::flush(int64_t rec_log_ts)
{
  UNUSED(rec_log_ts);
  return OB_SUCCESS;
}

int64_t ObLSTabletService::get_rec_log_ts()
{
  return INT64_MAX;
}

int ObLSTabletService::prepare_for_safe_destroy()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(delete_all_tablets())) {
    LOG_WARN("fail to delete all tablets", K(ret));
  }
  return ret;
}

int ObLSTabletService::safe_to_destroy(bool &is_safe)
{
  int ret = OB_SUCCESS;
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
  bool all_table_released = false;
  is_safe = true;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    int64_t tx_data_memtable_mgr_ref = tx_data_memtable_mgr_.get_ref();
    int64_t tx_ctx_memtable_mgr_ref = tx_ctx_memtable_mgr_.get_ref();
    int64_t lock_memtable_mgr_ref = lock_memtable_mgr_.get_ref();
    if (0 != tx_data_memtable_mgr_ref || 0 != tx_ctx_memtable_mgr_ref
        || 0 != lock_memtable_mgr_ref) {
      if (REACH_TIME_INTERVAL(60L * 1000000)) {  // 60s
        LOG_INFO("inner tablet memtable mgr can't destroy", K(tx_data_memtable_mgr_ref),
          K(tx_ctx_memtable_mgr_ref), K(lock_memtable_mgr_ref));
      }
      is_safe = false;
    } else {
      tx_data_memtable_mgr_.destroy();
      tx_ctx_memtable_mgr_.destroy();
      lock_memtable_mgr_.destroy();
    }
    if (is_safe) {
      if (OB_FAIL(t3m->gc_tables_in_queue(all_table_released))) {
        LOG_WARN("failed to check all table released", K(ret));
        is_safe = false;
      } else {
        is_safe = all_table_released;
      }
    }
  }

  return ret;
}

int ObLSTabletService::batch_create_tablets(
    const obrpc::ObBatchCreateTabletArg &arg,
    const int64_t create_scn,
    const bool is_replay_clog)
{
  ALLOW_NEXT_LOG();
  LOG_INFO("batch create tablet", K(arg), K(is_replay_clog));
  int ret = OB_SUCCESS;
  ObTimeGuard time_guard("CreateTablets", 3000000/*3 seconds*/);

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!arg.is_valid())
      || OB_UNLIKELY(create_scn <= OB_INVALID_TIMESTAMP)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(arg), K(create_scn));
  } else if (OB_UNLIKELY(arg.id_ != ls_->get_ls_id())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls id does not equal", K(ret),
        "arg ls id", arg.id_,
        "ls id", ls_->get_ls_id());
  } else {
    NonLockedHashSet data_tablet_id_set;
    NonLockedHashSet existed_tablet_id_set;

    if (OB_FAIL(data_tablet_id_set.create(arg.tablets_.count()))) {
      LOG_WARN("failed to init data tablet id set", K(ret), K(arg));
    } else if (OB_FAIL(existed_tablet_id_set.create(arg.get_tablet_count()))) {
      LOG_WARN("failed to init existed tablet id set", K(ret), K(arg));
    } else if (OB_FAIL(verify_tablets(arg, existed_tablet_id_set))) {
      LOG_WARN("failed to verify tablets", K(ret), K(arg));
    } else if (!is_replay_clog && existed_tablet_id_set.size() > 0) {
      // NOT in clog replay procedure, existed tablet id is NOT allowed
      ret = OB_TABLET_EXIST;
      LOG_WARN("tablet already exists", K(ret), K(arg), K(existed_tablet_id_set));
    } else if (is_replay_clog && existed_tablet_id_set.size() > 0) {
      time_guard.click("Verify");
      // in clog replay procedure and tablet id exists
      // we should build new arg to do batch create tablets
      obrpc::ObBatchCreateTabletArg new_arg;
      if (OB_FAIL(build_batch_create_tablet_arg(arg, existed_tablet_id_set, new_arg))) {
        LOG_WARN("failed to build new batch create tablet arg", K(ret),
            K(arg), K(existed_tablet_id_set));
      } else if (new_arg.get_tablet_count() == 0) {
        // all tablet ids exist, no need to create, do nothing
      } else if (OB_UNLIKELY(!new_arg.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("new arg is invalid", K(ret), K(new_arg));
      } else if (FALSE_IT(time_guard.click("BuildArg"))) {
      } else if (OB_FAIL(do_batch_create_tablets(new_arg, create_scn, is_replay_clog, time_guard, data_tablet_id_set))) {
        LOG_WARN("failed to do batch create tablets", K(ret), K(create_scn), K(new_arg), K(is_replay_clog));
      }
    } else {
      if (OB_FAIL(do_batch_create_tablets(arg, create_scn, is_replay_clog, time_guard, data_tablet_id_set))) {
        LOG_WARN("failed to do batch create tablets", K(ret), K(arg), K(create_scn), K(is_replay_clog));
      }
    }
  }

  return ret;
}

int ObLSTabletService::do_batch_create_tablets(
    const obrpc::ObBatchCreateTabletArg &arg,
    const int64_t create_scn,
    const bool is_replay_clog,
    common::ObTimeGuard &time_guard,
    NonLockedHashSet &data_tablet_id_set)
{
  UNUSED(is_replay_clog);
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = arg.id_;
  ObSArray<ObTabletHandle> tablet_handles;

  if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(arg));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < arg.tablets_.count(); ++i) {
      const obrpc::ObCreateTabletInfo &info = arg.tablets_[i];
      if (OB_FAIL(create_tablet(ls_id, arg, create_scn, info, tablet_handles, data_tablet_id_set))) {
        LOG_WARN("failed to create tablet", K(ret), K(create_scn), K(info), K(i));
      }
    }
    time_guard.click("CreTablet");

    if (OB_FAIL(ret)) {
    } else {
      // bucket lock, protect write slog and concurrent tablet CAS operation
      ObSArray<uint64_t> all_tablet_id_hash_array;
      ObMultiBucketLockGuard lock_guard(bucket_lock_, true/*is_write_lock*/);
      if (OB_FAIL(get_all_tablet_id_hash_array(arg, all_tablet_id_hash_array))) {
        LOG_WARN("failed to get all tablet id array", K(ret), K(arg));
      } else if (OB_FAIL(lock_guard.lock_multi_buckets(all_tablet_id_hash_array))) {
        LOG_WARN("failed to lock multi buckets", K(ret));
      }
      time_guard.click("Lock");

      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(add_batch_tablets(ls_id, data_tablet_id_set, tablet_handles))) {
        LOG_ERROR("fatal error, failed to add batch tablets", K(ret), K(tablet_handles), K(lbt()));
        ob_usleep(1000 * 1000);
        ob_abort();
      }
      time_guard.click("WrSlog");
    }

    if (OB_FAIL(ret)) {
      // remove tablets just created
      int tmp_ret = OB_SUCCESS;
      if (OB_TMP_FAIL(post_handle_batch_create_tablets(arg))) {
        LOG_ERROR("fatal error, failed to remove redundant tablets", K(tmp_ret), K(arg), K(lbt()));
        ob_usleep(1000 * 1000);
        ob_abort(); // remove redundant tablets should never fail
      }
    }
  }

  return ret;
}

int ObLSTabletService::batch_remove_tablets(
    const obrpc::ObBatchRemoveTabletArg &arg,
    const bool is_replay_clog)
{
  LOG_INFO("batch remove tablet", K(arg), K(is_replay_clog));
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = MTL_ID();
  ObStorageLogger *slogger = MTL(ObStorageLogger*);
  ObSArray<uint64_t> all_tablet_id_hash_array;
  common::ObLinearHashMap<common::ObTabletID, DeleteTabletInfo> delete_tablet_infos;
  ObTimeGuard time_guard("RemoveTablets", 3000000/*3 seconds*/);

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(arg));
  } else if (OB_UNLIKELY(arg.id_ != ls_->get_ls_id())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("log stream id does not equal", K(ret),
             "arg ls id", arg.id_,
             "log stream id", ls_->get_ls_id());
  } else if (OB_FAIL(delete_tablet_infos.init("tablet info", tenant_id))) {
    LOG_WARN("failed to init hash map", K(ret), K(tenant_id));
  } else if (OB_FAIL(parse_and_verify_delete_tablet_info(arg, delete_tablet_infos))) {
    LOG_WARN("failed to parse and verify delete tablet info", K(ret), K(arg));
  } else if (OB_FAIL(get_all_tablet_id_hash_array(delete_tablet_infos, all_tablet_id_hash_array))) {
    LOG_WARN("failed to get all tablet id array", K(ret));
  } else {
    time_guard.click("Prepare");
    HashMapTabletDeleteFunctor functor(ls_);
    ObMultiBucketLockGuard lock_guard(bucket_lock_, true/*is_write_lock*/);
    if (OB_UNLIKELY(all_tablet_id_hash_array.empty())) {
      LOG_INFO("all tablets have been removed, do nothing", K(arg));
    } else if (OB_FAIL(lock_guard.lock_multi_buckets(all_tablet_id_hash_array))) {
      LOG_WARN("failed to lock multi buckets", K(ret));
    } else if (FALSE_IT(time_guard.click("Lock"))) {
    } else if (OB_FAIL(delete_tablet_infos.for_each(functor))) {
      LOG_WARN("failed to iterate hash map", K(ret));
    } else if (FALSE_IT(time_guard.click("Collect"))) {
    } else if (OB_FAIL(slogger->write_log(functor.get_slog_params()))) {
      LOG_WARN("failed to write slog for remove tablets", K(ret), K(functor));
    } else if (FALSE_IT(time_guard.click("WrSlog"))) {
    } else if (OB_FAIL(handle_remove_tablets(functor.get_slog_params(), delete_tablet_infos))) {
      LOG_ERROR("failed to handle remove tablets", K(ret), K(functor), K(lbt()));
      ob_usleep(1000 * 1000);
      ob_abort();
    } else if (FALSE_IT(time_guard.click("Execute"))) {
    }
  }
  return ret;
}

int ObLSTabletService::delete_all_tablets()
{
  int ret = OB_SUCCESS;

  if (OB_NOT_NULL(ls_)) {
    ObSArray<ObTabletID> tablet_id_array;
    ObSArray<uint64_t> tablet_id_hash_array;
    GetAllTabletIDOperator op(tablet_id_array);
    if (OB_FAIL(tablet_id_set_.foreach(op))) {
      LOG_WARN("failed to traverse tablet id set", K(ret));
    } else if (tablet_id_array.empty()) {
      // tablet id array is empty, do nothing
    } else if (OB_FAIL(tablet_id_hash_array.reserve(tablet_id_array.count()))) {
      LOG_WARN("failed to reserver memory for array", K(ret), "cnt", tablet_id_array.count());
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < tablet_id_array.count(); ++i) {
        const ObTabletID &tablet_id = tablet_id_array.at(i);
        if (OB_FAIL(tablet_id_hash_array.push_back(tablet_id.hash()))) {
          LOG_WARN("failed to push back tablet id hash value", K(ret), K(tablet_id));
        }
      }

      if (OB_SUCC(ret)) {
        ObMultiBucketLockGuard lock_guard(bucket_lock_, true/*is_write_lock*/);
        if (OB_FAIL(lock_guard.lock_multi_buckets(tablet_id_hash_array))) {
          LOG_WARN("failed to lock multi buckets", K(ret));
        } else {
          const ObLSID &ls_id = ls_->get_ls_id();
          for (int64_t i = 0; OB_SUCC(ret) && i < tablet_id_array.count(); ++i) {
            const ObTabletID &tablet_id = tablet_id_array.at(i);
            if (OB_FAIL(do_remove_tablet(ls_id, tablet_id))) {
              LOG_ERROR("failed to do remove tablet", K(ret), K(ls_id), K(tablet_id));
              ob_usleep(1000 * 1000);
              ob_abort();
            }
          }

          if (OB_SUCC(ret)) {
            report_tablet_to_rs(tablet_id_array);
          }
        }
      }
    }
  }
  return ret;
}

int ObLSTabletService::remove_tablets(const common::ObIArray<common::ObTabletID> &tablet_id_array)
{
  int ret = OB_SUCCESS;
  const int64_t tablet_cnt = tablet_id_array.count();
  ObSArray<uint64_t> all_tablet_id_hash_array;
  ObSArray<ObTabletID> tablet_ids;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(0 == tablet_cnt)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args, tablet id array is empty", K(ret), K(tablet_id_array));
  } else if (OB_FAIL(all_tablet_id_hash_array.reserve(tablet_cnt))) {
    LOG_WARN("failed to reserve memory for array", K(ret), K(tablet_cnt));
  } else if (OB_FAIL(tablet_ids.reserve(tablet_cnt))) {
    LOG_WARN("failed to reserve memory for array", K(ret), K(tablet_cnt));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < tablet_cnt; ++i) {
      const ObTabletID &tablet_id = tablet_id_array.at(i);
      if (OB_FAIL(all_tablet_id_hash_array.push_back(tablet_id.hash()))) {
        LOG_WARN("failed to push back tablet id hash value", K(ret), K(tablet_id));
      }
    }
  }

  if (OB_SUCC(ret)) {
    ObMultiBucketLockGuard lock_guard(bucket_lock_, true/*is_write_lock*/);
    if (OB_FAIL(lock_guard.lock_multi_buckets(all_tablet_id_hash_array))) {
      LOG_WARN("failed to lock multi buckets", K(ret));
    } else {
      const share::ObLSID &ls_id = ls_->get_ls_id();
      ObTabletHandle tablet_handle;
      ObTabletMapKey key;
      key.ls_id_ = ls_id;

      // check tablet existence
      for (int64_t i = 0; OB_SUCC(ret) && i < tablet_cnt; ++i) {
        const ObTabletID &tablet_id = tablet_id_array.at(i);
        key.tablet_id_ = tablet_id;
        if (OB_FAIL(ObTabletCreateDeleteHelper::get_tablet(key, tablet_handle))) {
          if (OB_TABLET_NOT_EXIST == ret) {
            ret = OB_SUCCESS;
            LOG_INFO("tablet does not exist, maybe already deleted", K(ret), K(key));
          } else {
            LOG_WARN("failed to get tablet", K(ret), K(key));
          }
        } else if (OB_FAIL(tablet_ids.push_back(tablet_id))) {
          LOG_WARN("failed to push back tablet id", K(ret), K(tablet_id));
        }
      }

      // write slog and do remove tablet
      if (OB_FAIL(ret)) {
      } else if (tablet_ids.empty()) {
        LOG_INFO("all tablets already deleted, do nothing", K(ret), K(ls_id), K(tablet_id_array));
      } else if (OB_FAIL(ObTabletSlogHelper::write_remove_tablet_slog(ls_id, tablet_ids))) {
        LOG_WARN("failed to write remove tablet slog", K(ret), K(ls_id), K(tablet_ids));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < tablet_ids.count(); ++i) {
          const ObTabletID &tablet_id = tablet_ids.at(i);
          if (OB_FAIL(do_remove_tablet(ls_id, tablet_id))) {
            LOG_ERROR("failed to do remove tablet", K(ret), K(ls_id), K(tablet_id));
            ob_usleep(1000 * 1000);
            ob_abort();
          }
        }

        if (OB_SUCC(ret)) {
          report_tablet_to_rs(tablet_ids);
        }
      }
    }
  }

  return ret;
}

// TODO(yunshan.tys) cope with failure of deleting tablet (tablet hasn't been loaded from disk)
int ObLSTabletService::do_remove_tablet(
    const share::ObLSID &ls_id,
    const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  const ObTabletMapKey key(ls_id, tablet_id);
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  if (OB_FAIL(tablet_id_set_.erase(tablet_id))) {
    if (OB_HASH_NOT_EXIST == ret) {
      // tablet id is already erased
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("fail to erase tablet id from set", K(ret), K(ls_id), K(tablet_id));
    }
  }

  if (OB_SUCC(ret)) {
    // loop retry to delete tablet from t3m
    while (OB_FAIL(t3m->del_tablet(key))) {
      if (REACH_TIME_INTERVAL(10 * 1000 * 1000)) {
        LOG_ERROR("failed to delete tablet from t3m", K(ret), K(ls_id), K(tablet_id));
      }
    }
  }

  if (OB_SUCC(ret)) {
    FLOG_INFO("succeeded to remove tablet", K(ret), K(ls_id), K(tablet_id));
  }

  return ret;
}

int ObLSTabletService::get_tablet(
    const ObTabletID &tablet_id,
    ObTabletHandle &handle,
    const int64_t timeout_us)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid()
      || timeout_us < ObTabletCommon::DIRECT_GET_COMMITTED_TABLET_TIMEOUT_US)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tablet_id), K(timeout_us));
  } else if (OB_FAIL(check_and_get_tablet(tablet_id, handle, timeout_us))) {
    if (OB_TABLET_NOT_EXIST == ret) {
      LOG_DEBUG("failed to check and get tablet", K(ret), K(tablet_id), K(timeout_us));
    } else {
      LOG_WARN("failed to check and get tablet", K(ret), K(tablet_id), K(timeout_us));
    }
  }

  return ret;
}

int ObLSTabletService::get_tablet_addr(const ObTabletMapKey &key, ObMetaDiskAddr &addr)
{
  int ret = OB_SUCCESS;
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    // 加读锁保证ckpt与对tablet的操作串行, 防止ckpt时tablet操作处在
    // slog已经写入而tablet addr未更新的状态，导致slog丢失
    ObBucketHashRLockGuard lock_guard(bucket_lock_, key.tablet_id_.hash());
    if (OB_FAIL(t3m->get_tablet_addr(key, addr))) {
      LOG_WARN("fail to get tablet addr", K(ret), K(key));
    }
  }

  return ret;
}

/**
int ObLSTabletService::report_update_tablet(
    const ObTabletHandle *old_tablet_handle,
    const ObTabletHandle *new_tablet_handle)
{
  int ret = OB_SUCCESS;

  int64_t data_size_before = 0;
  int64_t used_size_before = 0;
  int64_t data_size_after = 0;
  int64_t used_size_after = 0;
  ObDiskReportFileType file_type;

  if (OB_UNLIKELY(nullptr == old_tablet_handle && nullptr == new_tablet_handle)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(old_tablet_handle), KP(new_tablet_handle));
  } else {
    do {
      if (nullptr != old_tablet_handle && old_tablet_handle->is_valid() &&
          OB_FAIL(old_tablet_handle->get_obj()->get_sstables_size(data_size_before, used_size_before))) {
        if (OB_ALLOCATE_MEMORY_FAILED == ret) {
          ret = OB_EAGAIN;
          LOG_WARN("fail to allocate memory, retry", K(ret));
        } else {
          LOG_WARN("failed to get old tablet's disk usage", K(ret), K(data_size_before), K(used_size_before));
        }
      } else if (nullptr != new_tablet_handle && new_tablet_handle->is_valid() &&
          OB_FAIL(new_tablet_handle->get_obj()->get_sstables_size(data_size_after, used_size_after))) {
        if (OB_ALLOCATE_MEMORY_FAILED == ret) {
          ret = OB_EAGAIN;
          LOG_WARN("fail to allocate memory, retry", K(ret));
        } else {
          LOG_WARN("failed to get new tablet's disk usage", K(ret), K(data_size_after), K(used_size_after));
        }
      } else {
        if (nullptr != old_tablet_handle) {
          parse_file_type(*old_tablet_handle, file_type);
        } else {
          parse_file_type(*new_tablet_handle, file_type);
        }
        if (OB_FAIL(disk_reporter_->set_tenant_data_usage(data_size_before, used_size_before,
            data_size_after, used_size_after, file_type))) {
          LOG_WARN("failed to set tenant's data usage", K(ret), K(data_size_before), K(used_size_before),
              K(data_size_after), K(used_size_after), K(file_type));
        }
      }
    } while (OB_EAGAIN == ret);
  }

  return ret;
}
*/

void ObLSTabletService::report_tablet_to_rs(
    const common::ObIArray<common::ObTabletID> &tablet_id_array)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = MTL_ID();
  const share::ObLSID &ls_id = ls_->get_ls_id();

  // ignore ret on purpose
  for (int64_t i = 0; i < tablet_id_array.count(); ++i) {
    const common::ObTabletID &tablet_id = tablet_id_array.at(i);
    if (tablet_id.is_ls_inner_tablet()) {
      // no need to report for ls inner tablet
      continue;
    } else if (OB_FAIL(rs_reporter_->submit_tablet_update_task(tenant_id, ls_id, tablet_id))) {
      LOG_WARN("failed to report tablet info", KR(ret), K(tenant_id), K(ls_id), K(tablet_id));
    }
  }
}

int ObLSTabletService::table_scan(ObTableScanIterator &iter, ObTableScanParam &param, ObNewRowIterator *&result)
{
  int ret = OB_SUCCESS;
  ObTabletHandle data_tablet;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(prepare_scan_table_param(param, *(MTL(ObTenantSchemaService*)->get_schema_service())))) {
    LOG_WARN("failed to prepare scan table param", K(ret), K(param));
  } else if (OB_FAIL(get_tablet_with_timeout(param.tablet_id_, data_tablet, param.timeout_))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(param));
  } else if (OB_FAIL(inner_table_scan(data_tablet, iter, param))) {
    LOG_WARN("failed to do table scan", K(ret), K(param));
  } else {
    result = &iter;
  }
  return ret;
}

int ObLSTabletService::table_rescan(ObTableScanParam &param, ObNewRowIterator *&result)
{
  int ret = OB_SUCCESS;
  ObTabletHandle data_tablet;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K(result), K_(is_inited));
  } else if (OB_ISNULL(result)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else if (OB_FAIL(prepare_scan_table_param(param, *(MTL(ObTenantSchemaService*)->get_schema_service())))) {
    LOG_WARN("failed to prepare scan table param", K(ret), K(result), K(param));
  } else {
    ObTableScanIterator *iter = static_cast<ObTableScanIterator*>(result);
    if (OB_FAIL(get_tablet_with_timeout(param.tablet_id_, data_tablet, param.timeout_))) {
      LOG_WARN("failed to check and get tablet", K(ret), K(param));
    } else if (OB_FAIL(inner_table_scan(data_tablet, *iter, param))) {
      LOG_WARN("failed to do table scan", K(ret), K(result), K(param));
    }
  }
  return ret;
}

int ObLSTabletService::refresh_tablet_addr(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const ObMetaDiskAddr &new_addr,
    ObTabletHandle &tablet_handle)
{
  int ret = OB_SUCCESS;
  const ObTabletMapKey key(ls_id, tablet_id);
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  if (OB_UNLIKELY(!new_addr.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(new_addr));
  } else if (OB_FAIL(tablet_id_set_.set(tablet_id))) {
    LOG_WARN("fail to set tablet id set", K(ret), K(tablet_id));
  } else if (OB_FAIL(t3m->compare_and_swap_tablet(key, new_addr, tablet_handle, tablet_handle))) {
    LOG_WARN("failed to add tablet to meta mem mgr", K(ret), K(key), K(new_addr), K(tablet_handle));
  }

  return ret;
}

int ObLSTabletService::update_tablet_object_and_addr(
    ObTabletHandle &new_tablet_handle,
    const ObMetaDiskAddr &new_addr)
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = MTL_ID();
  share::ObLSID ls_id;
  common::ObTabletID tablet_id;
  ObTabletHandle old_tablet_handle;
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  if (OB_UNLIKELY(!new_tablet_handle.is_valid())
      || OB_UNLIKELY(!new_addr.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(new_tablet_handle), K(new_addr));
  } else if (FALSE_IT(ls_id = new_tablet_handle.get_obj()->tablet_meta_.ls_id_)) {
    // do nothing
  } else if (FALSE_IT(tablet_id = new_tablet_handle.get_obj()->tablet_meta_.tablet_id_)) {
    // do nothing
  } else if (OB_FAIL(direct_get_tablet(tablet_id, old_tablet_handle))) {
    LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
  } else if (OB_FAIL(t3m->compare_and_swap_tablet(ObTabletMapKey(ls_id, tablet_id),
      new_addr, old_tablet_handle, new_tablet_handle))) {
    LOG_WARN("failed to compare and swap tablet", K(ret), K(ls_id), K(tablet_id), K(new_addr));
  } else {
    FLOG_INFO("succeeded to update tablet object and addr", K(ret), K(tenant_id), K(ls_id), K(tablet_id));
  }

  return ret;
}

int ObLSTabletService::trim_old_tablets(const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  ObTabletHandle tablet_handle_head;
  ObIAllocator &allocator = MTL(ObTenantMetaMemMgr*)->get_tenant_allocator();

  if (OB_FAIL(direct_get_tablet(tablet_id, tablet_handle_head))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(tablet_id));
  } else if (OB_UNLIKELY(!tablet_handle_head.get_obj()->get_tablet_meta().has_next_tablet_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("doesn't have old tablet", K(ret), "tablet_meta", tablet_handle_head.get_obj()->get_tablet_meta());
  } else {
    ObTablet *tablet_head = tablet_handle_head.get_obj();

    tablet_head->trim_tablet_list();
    ObMetaDiskAddr disk_addr;
    const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);

    if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(tablet_handle_head, disk_addr))) {
      LOG_WARN("failed to write update tablet slog", K(ret), K(tablet_handle_head), K(disk_addr));
    } else if (OB_FAIL(MTL(ObTenantMetaMemMgr*)->compare_and_swap_tablet(key,
        disk_addr, tablet_handle_head, tablet_handle_head))) {
      LOG_ERROR("failed to compare and swap tablet", K(key), K(disk_addr), K(lbt()));
      ob_usleep(1000 * 1000);
      ob_abort();
    } else {
      FLOG_INFO("succeeded to trim tablets list", K(key), K(disk_addr));
    }
  }

  return ret;
}

int ObLSTabletService::rollback_rebuild_tablet(const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  ObTabletHandle tablet_handle_head;

  if (OB_FAIL(direct_get_tablet(tablet_id, tablet_handle_head))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(tablet_id));
  } else {
    ObTablet *tablet_head = tablet_handle_head.get_obj();
    ObTabletHandle next_tablet_handle;
    ObMetaObj<ObTablet> meta_obj;
    tablet_head->get_next_tablet_guard().get_obj(meta_obj);
    next_tablet_handle.set_obj(meta_obj);
    ObMetaDiskAddr disk_addr;
    const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);

    if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(next_tablet_handle, disk_addr))) {
      LOG_WARN("failed to write update tablet slog", K(ret), K(next_tablet_handle), K(disk_addr));
    } else if (OB_FAIL(MTL(ObTenantMetaMemMgr*)->compare_and_swap_tablet(key,
        disk_addr, tablet_handle_head, next_tablet_handle))) {
      LOG_ERROR("failed to compare and swap tablet", K(ret), K(key), K(disk_addr), K(lbt()));
      ob_usleep(1000 * 1000);
      ob_abort();
    } else {
      FLOG_INFO("succeeded to rollback rebuild", K(key), K(disk_addr));
    }
  }

  return ret;
}

int ObLSTabletService::rebuild_tablet_with_old(
    const ObMigrationTabletParam &mig_tablet_param,
    ObMetaObjGuard<ObTablet> &tablet_guard)
{
  int ret = OB_SUCCESS;
  ObTabletHandle new_tablet_handle;
  ObTablet *new_tablet = nullptr;
  ObFreezer *freezer = ls_->get_freezer();
  ObMetaDiskAddr disk_addr;
  ObTabletHandle old_tablet_handle;
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  const common::ObTabletID &tablet_id = mig_tablet_param.tablet_id_;
  const share::ObLSID &ls_id = mig_tablet_param.ls_id_;
  const ObTabletMapKey key(ls_id, tablet_id);

  if (OB_FAIL(direct_get_tablet(tablet_id, old_tablet_handle))) {
    LOG_WARN("failed to get tablet", K(ret), K(key));
  } else if (OB_FAIL(ObTabletCreateDeleteHelper::acquire_tablet(key, new_tablet_handle, true/*only acquire*/))) {
    LOG_WARN("failed to acquire tablet", K(ret), K(key));
  } else if (FALSE_IT(new_tablet = new_tablet_handle.get_obj())) {
  } else if (OB_FAIL(new_tablet->init(mig_tablet_param, true/*is_update*/, freezer))) {
    LOG_WARN("failed to init tablet", K(ret), K(mig_tablet_param));
  } else if (FALSE_IT(new_tablet->set_next_tablet_guard(tablet_guard))) {
  } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(new_tablet_handle, disk_addr))) {
    LOG_WARN("failed to write update tablet slog", K(ret), K(new_tablet_handle), K(disk_addr));
  } else if (OB_FAIL(t3m->compare_and_swap_tablet(key, disk_addr,
      old_tablet_handle, new_tablet_handle))) {
    LOG_ERROR("failed to compare and swap tablet", K(ret), K(key), K(disk_addr));
    ob_usleep(1000 * 1000);
    ob_abort();
  } else if (OB_FAIL(new_tablet->start_ddl_if_need())) {
    LOG_WARN("start ddl if need failed", K(ret), K(key));
  }

  return ret;
}

int ObLSTabletService::migrate_update_tablet(
    const ObMigrationTabletParam &mig_tablet_param)
{
  int ret = OB_SUCCESS;
  const common::ObTabletID &tablet_id = mig_tablet_param.tablet_id_;
  const share::ObLSID &ls_id = mig_tablet_param.ls_id_;
  const ObTabletMapKey key(mig_tablet_param.ls_id_, mig_tablet_param.tablet_id_);
  ObTabletHandle new_tablet_handle;
  ObTablet *new_tablet = nullptr;
  ObMetaDiskAddr disk_addr;
  ObFreezer *freezer = ls_->get_freezer();
  ObTabletHandle old_tablet_handle;
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  if (OB_FAIL(direct_get_tablet(tablet_id, old_tablet_handle))) {
    LOG_WARN("failed to get tablet", K(ret), K(key));
  } else if (OB_FAIL(ObTabletCreateDeleteHelper::acquire_tablet(key, new_tablet_handle, true/*only acquire*/))) {
    LOG_WARN("failed to acquire tablet", K(ret), K(key));
  } else if (FALSE_IT(new_tablet = new_tablet_handle.get_obj())) {
  } else if (OB_FAIL(new_tablet->init(mig_tablet_param, true/*is_update*/, freezer))) {
    LOG_WARN("failed to init tablet", K(ret), K(mig_tablet_param));
  } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(new_tablet_handle, disk_addr))) {
    LOG_WARN("failed to write update tablet slog", K(ret), K(new_tablet_handle), K(disk_addr));
  } else if (OB_FAIL(t3m->compare_and_swap_tablet(key, disk_addr,
      old_tablet_handle, new_tablet_handle))) {
    LOG_ERROR("failed to compare and swap tablet", K(ret), K(key), K(disk_addr));
    ob_usleep(1000 * 1000);
    ob_abort();
  } else if (OB_FAIL(new_tablet->start_ddl_if_need())) {
    LOG_WARN("start ddl if need failed", K(ret));
  }

  return ret;
}

int ObLSTabletService::migrate_create_tablet(
    const ObMigrationTabletParam &mig_tablet_param,
    ObTabletHandle &handle)
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = mig_tablet_param.ls_id_;
  const common::ObTabletID &tablet_id = mig_tablet_param.tablet_id_;
  const ObTabletMapKey key(ls_id, tablet_id);
  ObFreezer *freezer = ls_->get_freezer();
  ObTabletHandle tablet_handle;
  ObTablet *tablet = nullptr;
  ObMetaDiskAddr disk_addr;

  if (OB_FAIL(ObTabletCreateDeleteHelper::acquire_tablet(key, tablet_handle))) {
    LOG_WARN("failed to acquire tablet", K(ret), K(key));
  } else if (FALSE_IT(tablet = tablet_handle.get_obj())) {
  } else if (OB_FAIL(tablet->init(mig_tablet_param, false/*is_update*/, freezer))) {
    LOG_WARN("failed to init tablet", K(ret), K(mig_tablet_param));
  } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(tablet_handle, disk_addr))) {
    LOG_WARN("failed to write create tablet slog", K(ret), K(tablet_handle), K(disk_addr));
  } else if (OB_FAIL(refresh_tablet_addr(ls_id, tablet_id, disk_addr, tablet_handle))) {
    LOG_WARN("failed to refresh tablet addr", K(ret), K(ls_id), K(tablet_id), K(disk_addr), K(lbt()));
    ob_usleep(1000 * 1000);
    ob_abort();
  } else if (OB_FAIL(tablet->start_ddl_if_need())) {
    LOG_WARN("start ddl if need failed", K(ret));
  } else if (OB_FAIL(try_pin_tablet_if_needed(tablet_handle))) {
    LOG_WARN("failed to try pin tablet", K(ret), K(ls_id), K(tablet_id));
  }

  if (OB_SUCC(ret)) {
    handle = tablet_handle;
  } else {
    int tmp_ret = OB_SUCCESS;
    ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
    if (OB_TMP_FAIL(tablet_id_set_.erase(tablet_id))) {
      if (OB_HASH_NOT_EXIST != ret) {
        LOG_ERROR("fail to erase tablet id from set", K(tmp_ret), K(tablet_id));
      }
    }
    if (OB_TMP_FAIL(t3m->del_tablet(key))) {
      LOG_WARN("fail to erase tablet from meta memory manager", K(tmp_ret), K(key));
    }
  }

  return ret;
}

int ObLSTabletService::update_tablet_table_store(
    const common::ObTabletID &tablet_id,
    const ObUpdateTableStoreParam &param,
    ObTabletHandle &handle)
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = ls_->get_ls_id();
  const ObTabletMapKey key(ls_id, tablet_id);
  ObTabletHandle old_tablet_handle;
  ObTabletHandle new_tablet_handle;
  ObTablet *new_tablet = nullptr;
  ObTimeGuard time_guard("UpdateTableStore", 3000000/*3 seconds*/);

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || !param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tablet_id), K(param));
  } else if (OB_FAIL(ObTabletCreateDeleteHelper::acquire_tablet(key, new_tablet_handle, true/*only acquire*/))) {
    LOG_WARN("failed to acquire tablet", K(ret), K(key));
  } else {
    new_tablet = new_tablet_handle.get_obj();
    time_guard.click("Acquire");
    ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
    time_guard.click("Lock");
    if (OB_FAIL(direct_get_tablet(tablet_id, old_tablet_handle))) {
      LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
    } else {
      time_guard.click("GetOld");
      ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
      ObTablet *old_tablet = old_tablet_handle.get_obj();
      const ObTabletTxMultiSourceDataUnit *tx_data = nullptr;
      const ObTabletBindingInfo *binding_info = nullptr;
      const ObTabletAutoincSeq *auto_inc_seq = nullptr;
      ObMetaDiskAddr disk_addr;

      if (OB_FAIL(choose_msd(param, *old_tablet, tx_data, binding_info, auto_inc_seq))) {
        LOG_WARN("failed to choose msd", K(ret), K(param), KPC(old_tablet));
      } else if (OB_FAIL(new_tablet->init(param, *old_tablet, *tx_data, *binding_info, *auto_inc_seq))) {
        LOG_WARN("failed to init tablet", K(ret), K(param), KPC(old_tablet), KPC(tx_data), KPC(binding_info), KPC(auto_inc_seq));
      } else if (FALSE_IT(time_guard.click("InitNew"))) {
      } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(new_tablet_handle, disk_addr))) {
        LOG_WARN("fail to write update tablet slog", K(ret), K(new_tablet_handle), K(disk_addr));
      } else if (FALSE_IT(time_guard.click("WrSlog"))) {
      } else if (OB_FAIL(t3m->compare_and_swap_tablet(key, disk_addr, old_tablet_handle, new_tablet_handle))) {
        LOG_ERROR("failed to compare and swap tablet", K(ret), K(key), K(disk_addr), K(old_tablet_handle), K(lbt()));
        ob_usleep(1000 * 1000);
        ob_abort();
      } else if (FALSE_IT(time_guard.click("CASwap"))) {
      } else {
        handle = new_tablet_handle;
        LOG_INFO("succeeded to build new tablet", K(ret), K(tablet_id), K(param), K(handle));
      }
    }
  }
  return ret;
}

int ObLSTabletService::choose_msd(
    const ObUpdateTableStoreParam &param,
    const ObTablet &old_tablet,
    const ObTabletTxMultiSourceDataUnit *&tx_data,
    const ObTabletBindingInfo *&binding_info,
    const share::ObTabletAutoincSeq *&auto_inc_seq)
{
  int ret = OB_SUCCESS;
  const ObTabletMeta &old_tablet_meta = old_tablet.get_tablet_meta();
  tx_data = &param.tx_data_;
  binding_info = &param.binding_info_;
  auto_inc_seq = &param.auto_inc_seq_;

  if (!tx_data->is_valid()) {
    tx_data = &old_tablet_meta.tx_data_;
  }
  if (!binding_info->is_valid()) {
    binding_info = &old_tablet_meta.ddl_data_;
  }
  if (!auto_inc_seq->is_valid()) {
    auto_inc_seq = &old_tablet_meta.autoinc_seq_;
  }

  return ret;
}

int ObLSTabletService::update_tablet_report_status(const common::ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  ObTabletHandle tablet_handle;
  ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tablet_id));
  } else if (OB_FAIL(direct_get_tablet(tablet_id, tablet_handle))) {
    LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
  } else {
    ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
    ObMetaDiskAddr disk_addr;
    const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);
    ObTablet *tablet = tablet_handle.get_obj();
    if (tablet->tablet_meta_.report_status_.need_report()) {
      tablet->tablet_meta_.report_status_.cur_report_version_ = tablet->tablet_meta_.report_status_.merge_snapshot_version_;
      if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(tablet_handle, disk_addr))) {
        LOG_WARN("failed to write update tablet slog", K(ret), K(tablet_handle), K(disk_addr));
      } else if (OB_FAIL(t3m->compare_and_swap_tablet(key,
          disk_addr, tablet_handle, tablet_handle))) {
        LOG_ERROR("failed to compare and swap tablet", K(ret), K(key), K(disk_addr), K(lbt()));
        ob_usleep(1000 * 1000);
        ob_abort();
      } else {
        LOG_INFO("succeeded to build new tablet", K(ret), K(tablet_id), K(tablet_handle));
      }
    } else {
      FLOG_INFO("tablet doesn't need to report", K(ret));
    }
  }
  return ret;
}

int ObLSTabletService::update_tablet_restore_status(
    const common::ObTabletID &tablet_id,
    const ObTabletRestoreStatus::STATUS &restore_status)
{
  int ret = OB_SUCCESS;
  ObTabletHandle tablet_handle;
  ObTabletRestoreStatus::STATUS current_status = ObTabletRestoreStatus::RESTORE_STATUS_MAX;
  bool can_change = false;

  ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid())
      || OB_UNLIKELY(!ObTabletRestoreStatus::is_valid(restore_status))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tablet_id), K(restore_status));
  } else if (OB_FAIL(direct_get_tablet(tablet_id, tablet_handle))) {
    LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
  } else {
    ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
    ObMetaDiskAddr disk_addr;
    const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);
    ObTablet *tablet = tablet_handle.get_obj();
    if (OB_FAIL(tablet->tablet_meta_.ha_status_.get_restore_status(current_status))) {
      LOG_WARN("failed to get restore status", K(ret), KPC(tablet));
    } else if (OB_FAIL(ObTabletRestoreStatus::check_can_change_status(current_status, restore_status, can_change))) {
      LOG_WARN("failed to check can change status", K(ret), K(current_status), K(restore_status), KPC(tablet));
    } else if (!can_change) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("can not change restore status", K(ret), K(current_status), K(restore_status), KPC(tablet));
    } else if (OB_FAIL(tablet->tablet_meta_.ha_status_.set_restore_status(restore_status))) {
      LOG_WARN("failed to set restore status", K(ret), K(restore_status), KPC(tablet));
    } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(tablet_handle, disk_addr))) {
      LOG_WARN("failed to write update tablet slog", K(ret), K(tablet_handle), K(disk_addr));
    } else if (OB_FAIL(t3m->compare_and_swap_tablet(key,
        disk_addr, tablet_handle, tablet_handle))) {
      LOG_ERROR("failed to compare and swap tablet", K(key), K(disk_addr));
      ob_abort();
    } else {
      LOG_INFO("succeeded to build new tablet", K(ret), K(tablet_id), K(restore_status), K(tablet_handle));
    }
  }

  return ret;
}

int ObLSTabletService::update_tablet_ha_data_status(
    const common::ObTabletID &tablet_id,
    const ObTabletDataStatus::STATUS &data_status)
{
  int ret = OB_SUCCESS;
  ObTabletHandle tablet_handle;
  ObTabletDataStatus::STATUS current_status = ObTabletDataStatus::DATA_STATUS_MAX;
  bool can_change = false;

  ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid())
      || OB_UNLIKELY(!ObTabletDataStatus::is_valid(data_status))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tablet_id), K(data_status));
  } else if (OB_FAIL(direct_get_tablet(tablet_id, tablet_handle))) {
    LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
  } else {
    ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
    ObMetaDiskAddr disk_addr;
    const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);
    ObTablet *tablet = tablet_handle.get_obj();

    if (OB_FAIL(tablet->tablet_meta_.ha_status_.get_data_status(current_status))) {
      LOG_WARN("failed to get data status", K(ret), KPC(tablet));
    } else if (OB_FAIL(ObTabletDataStatus::check_can_change_status(current_status, data_status, can_change))) {
      LOG_WARN("failed to check can change status", K(ret), K(current_status), K(data_status), KPC(tablet));
    } else if (!can_change) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("can not change data status", K(ret), K(current_status), K(data_status), KPC(tablet));
    } else if (current_status == data_status) {
      LOG_INFO("data status is same, skip update", K(tablet_id), K(current_status), K(data_status));
    } else if (OB_FAIL(tablet->tablet_meta_.ha_status_.set_data_status(data_status))) {
      LOG_WARN("failed to set data status", K(ret), KPC(tablet), K(data_status));
    } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(tablet_handle, disk_addr))) {
      LOG_WARN("failed to write update tablet slog", K(ret), K(tablet_handle), K(disk_addr));
    } else if (OB_FAIL(t3m->compare_and_swap_tablet(key,
        disk_addr, tablet_handle, tablet_handle))) {
      LOG_ERROR("failed to compare and swap tablet", K(ret), K(key), K(disk_addr), K(lbt()));
      ob_usleep(1000 * 1000);
      ob_abort();
    } else {
      LOG_INFO("succeeded to build new tablet", K(ret), K(tablet_id), K(data_status), K(tablet_handle));
    }
  }

  return ret;
}

int ObLSTabletService::update_tablet_ha_expected_status(
    const common::ObTabletID &tablet_id,
    const ObTabletExpectedStatus::STATUS &expected_status)
{
  int ret = OB_SUCCESS;
  ObTabletHandle tablet_handle;
  ObTabletExpectedStatus::STATUS current_status = ObTabletExpectedStatus::EXPECTED_STATUS_MAX;
  bool can_change = false;

  ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid())
      || OB_UNLIKELY(!ObTabletExpectedStatus::is_valid(expected_status))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tablet_id), K(expected_status));
  } else if (OB_FAIL(direct_get_tablet(tablet_id, tablet_handle))) {
    LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
  } else {
    ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
    ObMetaDiskAddr disk_addr;
    const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);
    ObTablet *tablet = tablet_handle.get_obj();

    if (OB_FAIL(tablet->tablet_meta_.ha_status_.get_expected_status(current_status))) {
      LOG_WARN("failed to get data status", K(ret), KPC(tablet));
    } else if (expected_status == current_status) {
      LOG_INFO("tablet ha expected status is same, no need update", K(tablet_id),
          K(current_status), K(expected_status));
    } else if (OB_FAIL(ObTabletExpectedStatus::check_can_change_status(current_status, expected_status, can_change))) {
      LOG_WARN("failed to check can change status", K(ret), K(current_status), K(expected_status), KPC(tablet));
    } else if (!can_change) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("can not change meta status", K(ret), K(current_status), K(expected_status), KPC(tablet));
    } else {
      if (OB_FAIL(tablet->tablet_meta_.ha_status_.set_expected_status(expected_status))) {
        LOG_WARN("failed to set ha meta status", K(ret), KPC(tablet), K(expected_status));
      } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(tablet_handle, disk_addr))) {
        LOG_WARN("failed to write update tablet slog", K(ret), K(tablet_handle), K(disk_addr));
      } else if (OB_FAIL(t3m->compare_and_swap_tablet(key,
          disk_addr, tablet_handle, tablet_handle))) {
        LOG_ERROR("failed to compare and swap tablet", K(ret), K(key), K(disk_addr), K(lbt()));
        usleep(1000 * 1000);
        ob_abort();
      } else {
        LOG_INFO("succeeded to update tablet meta status", K(ret), K(tablet_id), K(expected_status), KPC(tablet));
      }
    }
  }
  return ret;
}

int ObLSTabletService::handle_remove_tablets(
    const common::ObIArray<ObStorageLogParam> &slog_params,
    const common::ObLinearHashMap<common::ObTabletID, DeleteTabletInfo> &delete_tablet_infos)
{
  int ret = OB_SUCCESS;
  const int32_t put_tablet_cmd = ObIRedoModule::gen_cmd(ObRedoLogMainType::OB_REDO_LOG_TENANT_STORAGE,
      ObRedoLogSubType::OB_REDO_LOG_PUT_TABLET);
  const int32_t delete_tablet_cmd = ObIRedoModule::gen_cmd(ObRedoLogMainType::OB_REDO_LOG_TENANT_STORAGE,
      ObRedoLogSubType::OB_REDO_LOG_DELETE_TABLET);
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  for (int64_t i = 0; OB_SUCC(ret) && i < slog_params.count(); ++i) {
    const ObStorageLogParam &slog_param = slog_params.at(i);
    if (OB_UNLIKELY(!slog_param.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("slog entry is null", K(ret), K(slog_param));
    } else if (slog_param.cmd_ == put_tablet_cmd) {
      const ObMetaDiskAddr &disk_addr = slog_param.disk_addr_;
      const ObCreateTabletLog *slog = static_cast<ObCreateTabletLog*>(slog_param.data_);
      const common::ObTabletID &tablet_id = slog->tablet_->get_tablet_meta().tablet_id_;
      ObTabletMapKey key(ls_->get_ls_id(), tablet_id);
      DeleteTabletInfo info;
      if (OB_FAIL(delete_tablet_infos.get(tablet_id, info))) {
        LOG_WARN("failed to get info from hash map", K(ret));
      } else if (OB_FAIL(t3m->compare_and_swap_tablet(key, disk_addr,
          info.old_data_tablet_handle_, info.new_data_tablet_handle_))) {
        LOG_WARN("failed to compare and swap tablet", K(ret), K(key), K(disk_addr),
            "old_data_tablet_handle", info.old_data_tablet_handle_,
            "new_data_tablet_handle", info.new_data_tablet_handle_);
      }
    } else if (slog_param.cmd_ == delete_tablet_cmd) {
      const ObDeleteTabletLog *slog = static_cast<ObDeleteTabletLog*>(slog_param.data_);
      const share::ObLSID &ls_id = slog->ls_id_;
      const common::ObTabletID &tablet_id = slog->tablet_id_;
      if (OB_FAIL(do_remove_tablet(ls_id, tablet_id))) {
        LOG_ERROR("failed to remove tablet", K(ret), K(ls_id), K(tablet_id), K(i));
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected slog param cmd type", K(ret), K(slog_param));
    }
  }

  return ret;
}

int ObLSTabletService::replay_create_tablet(
    const ObMetaDiskAddr &disk_addr,
    const char *buf,
    const int64_t buf_len,
    const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  bool b_exist = false;
  ObFreezer *freezer = ls_->get_freezer();
  const ObLSID &ls_id = ls_->get_ls_id();
  common::ObIAllocator &allocator = MTL(ObTenantMetaMemMgr*)->get_tenant_allocator();

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_FAIL(has_tablet(ls_id, tablet_id, b_exist))) {
    LOG_WARN("fail to check tablet existence", K(ret), K(ls_id), K(tablet_id));
  } else if (b_exist) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("restart replay tablet should not exist", K(ret), K(ls_id), K(tablet_id));
  } else {
    const ObTabletMapKey key(ls_id, tablet_id);
    ObTabletHandle new_tablet_handle;
    ObTablet *new_tablet = nullptr;
    int64_t pos = 0;
    ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());

    if (OB_FAIL(ObTabletCreateDeleteHelper::acquire_tablet(key, new_tablet_handle))) {
      LOG_WARN("fail to acquire tablet", K(ret), K(key));
    } else if (FALSE_IT(new_tablet = new_tablet_handle.get_obj())) {
      // do nothing
    } else if (OB_FAIL(new_tablet->deserialize(allocator, buf, buf_len, pos))) {
      LOG_WARN("fail to deserialize tablet", K(ret), K(buf), K(buf_len), K(pos));
    } else if (OB_FAIL(new_tablet->init_shared_params(ls_id, tablet_id,
        new_tablet->get_tablet_meta().max_sync_storage_schema_version_, freezer))) {
      LOG_WARN("failed to init shared params", K(ret), K(ls_id), K(tablet_id));
    } else if (OB_FAIL(refresh_tablet_addr(ls_id, tablet_id, disk_addr, new_tablet_handle))) {
      LOG_WARN("failed to refresh tablet addr", K(key), K(ls_id), K(tablet_id), K(disk_addr));
    } else if (OB_FAIL(new_tablet->start_ddl_if_need())) {
      LOG_WARN("start ddl if need failed", K(ret));
    } else if (OB_FAIL(try_pin_tablet_if_needed(new_tablet_handle))) {
      LOG_WARN("failed to try pin tablet", K(ret), K(ls_id), K(tablet_id));
    } else {
      FLOG_INFO("succeeded to create tablet for replay slog", K(ret), K(ls_id), K(tablet_id), K(new_tablet_handle));
    }

    if (OB_FAIL(ret) && !b_exist) {
      int tmp_ret = OB_SUCCESS;
      ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
      if (OB_TMP_FAIL(tablet_id_set_.erase(tablet_id))) {
        if (OB_HASH_NOT_EXIST != ret) {
          LOG_ERROR("fail to erase tablet id from set", K(tmp_ret), K(tablet_id));
        }
      }
      if (OB_TMP_FAIL(t3m->del_tablet(key))) {
        LOG_WARN("fail to erase tablet from meta memory manager", K(tmp_ret), K(key));
      }
    }
  }

  return ret;
}

int ObLSTabletService::try_pin_tablet_if_needed(const ObTabletHandle &tablet_handle)
{
  int ret = OB_SUCCESS;
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
  ObTablet *tablet = tablet_handle.get_obj();
  ObTabletTxMultiSourceDataUnit tx_data;
  bool exist_on_memtable = false;

  if (OB_ISNULL(tablet)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, tablet is null", K(ret), K(tablet_handle));
  } else if (OB_FAIL(tablet->inner_get_tx_data(tx_data, exist_on_memtable))) {
    LOG_WARN("failed to get tx data", K(ret), KPC(tablet));
  } else if (!tx_data.is_valid()) {
    // tablet is not valid, do nothing
  } else if (!tx_data.is_in_tx()) {
    // tablet not in tx, do nothing
  } else {
    const ObLSID &ls_id = tablet->get_tablet_meta().ls_id_;
    const ObTabletID &tablet_id = tablet->get_tablet_meta().tablet_id_;
    const ObTabletMapKey key(ls_id, tablet_id);
    if (OB_FAIL(t3m->insert_pinned_tablet(key))) {
      LOG_WARN("failed to insert pinned tablet", K(ret), K(key));
    }
  }

  return ret;
}

int ObLSTabletService::check_and_get_tablet(
    const common::ObTabletID &tablet_id,
    ObTabletHandle &handle,
    const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);

  if (OB_FAIL(ObTabletCreateDeleteHelper::check_and_get_tablet(key, handle, timeout_us))) {
    if (OB_TABLET_NOT_EXIST == ret) {
      LOG_DEBUG("failed to check and get tablet", K(ret), K(key), K(timeout_us));
    } else {
      LOG_WARN("failed to check and get tablet", K(ret), K(key), K(timeout_us));
    }
  }

  return ret;
}

int ObLSTabletService::get_tablet_with_timeout(
    const common::ObTabletID &tablet_id,
    ObTabletHandle &handle,
    const int64_t retry_timeout_us,
    const int64_t get_timeout_us)
{
  int ret = OB_SUCCESS;
  const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);
  if (OB_FAIL(ObTabletCreateDeleteHelper::check_and_get_tablet(key, handle, get_timeout_us))) {
    while (OB_ALLOCATE_MEMORY_FAILED == ret && ObTimeUtility::current_time() < retry_timeout_us) {
      ret = ObTabletCreateDeleteHelper::check_and_get_tablet(key, handle, get_timeout_us);
    }
    if (OB_ALLOCATE_MEMORY_FAILED == ret) {
      ret = OB_TIMEOUT;
      LOG_WARN("retry until reaching the timeout", K(ret), K(retry_timeout_us));
    } else if (OB_FAIL(ret)) {
      LOG_WARN("fail to check and get tablet", K(ret), K(key));
    }
  }
  return ret;
}

int ObLSTabletService::direct_get_tablet(const common::ObTabletID &tablet_id, ObTabletHandle &handle)
{
  int ret = OB_SUCCESS;
  const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);

  if (OB_FAIL(ObTabletCreateDeleteHelper::get_tablet(key, handle))) {
    LOG_WARN("failed to get tablet from t3m", K(ret), K(key));
  }

  return ret;
}

int ObLSTabletService::inner_table_scan(
    ObTabletHandle &tablet_handle,
    ObTableScanIterator &iter,
    ObTableScanParam &param)
{
  // NOTICE: ObTableScanParam for_update_ param is ignored here,
   // upper layer will handle it, so here for_update_ is always false
   int ret = OB_SUCCESS;
   ObStoreCtx &store_ctx = iter.get_ctx_guard().get_store_ctx();
   int64_t data_max_schema_version = 0;
   bool is_bounded_staleness_read = (NULL == param.trans_desc_)
                                    ? false
                                    : param.snapshot_.is_weak_read();

   if (OB_UNLIKELY(!tablet_handle.is_valid()) || OB_UNLIKELY(!param.is_valid())) {
     ret = OB_INVALID_ARGUMENT;
     LOG_WARN("invalid args", K(ret), K(tablet_handle), K(param));
   } else if (is_bounded_staleness_read
       && OB_FAIL(tablet_handle.get_obj()->get_max_schema_version(data_max_schema_version))) {
     LOG_WARN("failed to get max schema version", K(ret), K(param));
   } else if (is_bounded_staleness_read
       && OB_FAIL(tablet_handle.get_obj()->check_schema_version_for_bounded_staleness_read(
           param.schema_version_, data_max_schema_version, param.index_id_))) {
     //check schema_version with ref_table_id, because schema_version of scan_param is from ref table
     LOG_WARN("check schema version for bounded staleness read fail", K(ret), K(param));
     //need to get store ctx of PG, cur_key_ saves the real partition
   } else if (0 == param.fb_snapshot_) {
     ret = OB_SNAPSHOT_DISCARDED;
  } else if (OB_FAIL(ObTabletBindingHelper::check_snapshot_readable(
      tablet_handle, store_ctx.mvcc_acc_ctx_.get_snapshot_version()))) {
    LOG_WARN("failed to check snapshot readable", K(ret));
   } else {
     if (param.need_switch_param_) {
       if (OB_FAIL(iter.switch_param(param, tablet_handle))) {
         LOG_WARN("failed to init table scan iterator, ", K(ret));
       }
     } else if (OB_FAIL(iter.init(param, tablet_handle))) {
       LOG_WARN("failed to init table scan iterator, ", K(ret));
     }
   }

   if (OB_FAIL(ret)) {
     LOG_WARN("failed to do table scan", K(ret), K(param), K(*this),
         K(data_max_schema_version));
   }

   return ret;
}

int ObLSTabletService::has_tablet(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    bool &b_exist)
{
  int ret = OB_SUCCESS;
  b_exist = false;
  const ObTabletMapKey key(ls_id, tablet_id);
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  if (OB_FAIL(t3m->has_tablet(key, b_exist))) {
    LOG_WARN("failed to check tablet exist", K(ret), K(key));
  }

  return ret;
}

int ObLSTabletService::create_tablet(
    const share::ObLSID &ls_id,
    const obrpc::ObBatchCreateTabletArg &arg,
    const int64_t create_scn,
    const obrpc::ObCreateTabletInfo &info,
    common::ObIArray<ObTabletHandle> &tablet_handle_array,
    NonLockedHashSet &data_tablet_id_set)
{
  int ret = OB_SUCCESS;
  const common::ObTabletID &data_tablet_id = info.data_tablet_id_;
  const common::ObSArray<common::ObTabletID> &tablet_ids = info.tablet_ids_;

  if (OB_UNLIKELY(!info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid create tablet info", K(ret), K(info));
  } else if (tablet_ids.count() == 1 && common::is_contain(tablet_ids, data_tablet_id)) {
    // only data tablet.
    if (OB_FAIL(build_single_data_tablet(ls_id, arg, create_scn, info, tablet_handle_array))) {
      LOG_WARN("failed to build single data tablet", K(ret), K(ls_id), K(arg), K(create_scn), K(info));
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid create tablet info", K(ret), K(info));
  }

  return ret;
}

int ObLSTabletService::do_create_tablet(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const common::ObTabletID &data_tablet_id,
    const common::ObIArray<common::ObTabletID> &index_tablet_array,
    const int64_t create_scn,
    const int64_t snapshot_version,
    const share::schema::ObTableSchema &table_schema,
    const lib::Worker::CompatMode &compat_mode,
    const common::ObTabletID &lob_meta_tablet_id,
    const common::ObTabletID &lob_piece_tablet_id,
    ObTabletHandle &tablet_handle)
{
  int ret = OB_SUCCESS;
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
  const ObTabletMapKey key(ls_id, tablet_id);
  ObTableHandleV2 table_handle;
  ObTabletCreateSSTableParam param;
  ObFreezer *freezer = ls_->get_freezer();
  bool need_create_empty_major_sstable = false;
  ObTabletTableStoreFlag table_store_flag;
  table_store_flag.set_with_major_sstable();
  ObTabletHandle handle;

  if (OB_FAIL(ObTabletCreateDeleteHelper::acquire_tablet(key, handle))) {
    LOG_WARN("failed to acquire tablet", K(ret), K(key));
  } else if (OB_FAIL(ObTabletCreateDeleteHelper::check_need_create_empty_major_sstable(
      table_schema, need_create_empty_major_sstable))) {
    LOG_WARN("failed to check need create sstable", K(ret));
  } else if (!need_create_empty_major_sstable) {
    table_store_flag.set_without_major_sstable();
    LOG_INFO("no need to create sstable", K(ls_id), K(tablet_id), K(table_schema));
  } else if (OB_FAIL(ObTabletCreateDeleteHelper::build_create_sstable_param(
      table_schema, tablet_id, snapshot_version, param))) {
    LOG_WARN("failed to build create sstable param", K(ret), K(tablet_id),
        K(table_schema), K(snapshot_version), K(param));
  } else if (OB_FAIL(ObTabletCreateDeleteHelper::create_sstable(param, table_handle))) {
    LOG_WARN("failed to create sstable", K(ret), K(param));
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(handle.get_obj()->init(ls_id, tablet_id, data_tablet_id, lob_meta_tablet_id, lob_piece_tablet_id,
      create_scn, snapshot_version, table_schema, compat_mode, table_store_flag, table_handle, freezer))) {
    LOG_WARN("failed to init tablet", K(ret), K(ls_id), K(tablet_id), K(data_tablet_id), K(index_tablet_array),
        K(create_scn), K(snapshot_version), K(table_schema), K(compat_mode), K(table_store_flag));

    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(t3m->del_tablet(key))) {
      LOG_ERROR("failed to delete tablet from t3m", K(ret), K(key), K(lbt()));
      ob_usleep(1000 * 1000);
      ob_abort();
    }
  } else {
    tablet_handle = handle;
  }

  return ret;
}

int ObLSTabletService::build_single_data_tablet(
    const share::ObLSID &ls_id,
    const obrpc::ObBatchCreateTabletArg &arg,
    const int64_t create_scn,
    const obrpc::ObCreateTabletInfo &info,
    common::ObIArray<ObTabletHandle> &tablet_handle_array)
{
  int ret = OB_SUCCESS;
  const int64_t snapshot_version = arg.frozen_timestamp_;
  const common::ObTabletID &data_tablet_id = info.data_tablet_id_;
  const common::ObSArray<common::ObTabletID> &tablet_ids = info.tablet_ids_;
  const common::ObSArray<share::schema::ObTableSchema> &table_schemas = arg.table_schemas_;
  const lib::Worker::CompatMode &compat_mode = info.compat_mode_;
  common::ObSArray<common::ObTabletID> empty_array;
  ObTabletHandle tablet_handle;
  common::ObTabletID empty_tablet_id;
  int64_t index = -1;

  if (OB_FAIL(get_tablet_schema_index(data_tablet_id, tablet_ids, index))) {
    LOG_WARN("failed to get tablet schema index in array", K(ret), K(data_tablet_id));
  } else if (OB_UNLIKELY(index < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, table schema index is invalid", K(ret), K(data_tablet_id), K(index));
  } else if (OB_FAIL(do_create_tablet(ls_id, data_tablet_id, data_tablet_id, empty_array,
      create_scn, snapshot_version, table_schemas[info.table_schema_index_[index]],
      compat_mode, empty_tablet_id, empty_tablet_id, tablet_handle))) {
    LOG_WARN("failed to do create tablet", K(ret), K(ls_id), K(data_tablet_id),
        K(create_scn), K(snapshot_version), K(compat_mode));
  } else if (OB_FAIL(tablet_handle_array.push_back(tablet_handle))) {
    LOG_WARN("failed to insert tablet handle into array", K(ret), K(tablet_handle));
  }

  return ret;
}

int ObLSTabletService::build_batch_create_tablet_arg(
    const obrpc::ObBatchCreateTabletArg &old_arg,
    const NonLockedHashSet &existed_tablet_id_set,
    obrpc::ObBatchCreateTabletArg &new_arg)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  const common::ObSArray<obrpc::ObCreateTabletInfo> &old_info_array = old_arg.tablets_;

  for (int64_t i = 0; OB_SUCC(ret) && i < old_info_array.count(); ++i) {
    const obrpc::ObCreateTabletInfo &old_info = old_info_array[i];
    const common::ObSArray<common::ObTabletID> &old_tablet_id_array = old_info.tablet_ids_;
    ObCreateTabletInfo new_info;
    new_info.data_tablet_id_ = old_info.data_tablet_id_;
    new_info.compat_mode_ = old_info.compat_mode_;

    for (int64_t j = 0; OB_SUCC(ret) && j < old_tablet_id_array.count(); ++j) {
      const common::ObTabletID &old_tablet_id = old_tablet_id_array[j];
      tmp_ret = existed_tablet_id_set.exist_refactored(old_tablet_id);
      if (OB_HASH_EXIST == tmp_ret) {
        LOG_INFO("tablet id exists, should skip", K(old_tablet_id));
      } else if (OB_HASH_NOT_EXIST == tmp_ret) {
        const int64_t old_table_schema_index = old_info.table_schema_index_[j];
        if (OB_FAIL(new_info.tablet_ids_.push_back(old_tablet_id))) {
          LOG_WARN("failed to push back old tablet id", K(ret), K(old_tablet_id));
        } else if (OB_FAIL(new_info.table_schema_index_.push_back(old_table_schema_index))) {
          LOG_WARN("failed to push back table schema index", K(ret), K(old_tablet_id));
        }
      } else {
        ret = tmp_ret;
        LOG_WARN("unexpected error when checking old tablet id", K(ret), K(old_tablet_id));
      }
    }

    if (OB_FAIL(ret)) { // do nothing
    } else if (new_info.get_tablet_count() > 0) {
      if (OB_FAIL(new_arg.tablets_.push_back(new_info))) {
        LOG_WARN("failed to push back tablet info", K(ret), K(new_info));
      }
    } else {
      LOG_INFO("skip create tablet info", K(old_info));
    }
  }

  // copy other members in arg
  if (OB_FAIL(ret)) { // do nothing
  } else {
    new_arg.id_ = old_arg.id_;
    new_arg.frozen_timestamp_ = old_arg.frozen_timestamp_;

    if (new_arg.get_tablet_count() > 0 && OB_FAIL(new_arg.table_schemas_.assign(old_arg.table_schemas_))) {
      LOG_WARN("failed to assign table schemas", K(ret), K(old_arg));
    }
  }

  return ret;
}

int ObLSTabletService::add_batch_tablets(
    const share::ObLSID &ls_id,
    const NonLockedHashSet &data_tablet_id_set,
    common::ObIArray<ObTabletHandle> &tablet_handle_array)
{
  int ret = OB_SUCCESS;

  const ObTablet* tablet = nullptr;
  ObSArray<ObMetaDiskAddr> disk_addr_arr;
  const int64_t tablet_count = tablet_handle_array.count();

  if (OB_FAIL(disk_addr_arr.reserve(tablet_count))) {
    LOG_WARN("fail to reserve memory for disk_addr_array", K(ret), K(tablet_count));
  } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(tablet_handle_array, disk_addr_arr))) {
    LOG_WARN("fail to write slog", K(ret), K(ls_id));
  }

  int tmp_ret = OB_SUCCESS;
  common::ObTabletID tablet_id;
  for (int64_t i = 0; OB_SUCC(ret) && i < tablet_count; ++i) {
    const ObMetaDiskAddr &new_addr = disk_addr_arr[i];
    ObTabletHandle &tablet_handle = tablet_handle_array.at(i);
    if (OB_ISNULL(tablet = tablet_handle.get_obj())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("tablet is null", K(ret), K(tablet_handle));
    } else if (FALSE_IT(tablet_id = tablet->tablet_meta_.tablet_id_)) {
    } else if (FALSE_IT(tmp_ret = data_tablet_id_set.exist_refactored(tablet_id))) {
    } else if (OB_UNLIKELY(OB_HASH_EXIST != tmp_ret && OB_HASH_NOT_EXIST != tmp_ret)) {
      ret = tmp_ret;
      LOG_WARN("failed to check tablet id existence", K(ret), K(tablet_id));
    } else if ((OB_HASH_EXIST == tmp_ret)
        && OB_FAIL(update_tablet_object_and_addr(tablet_handle, new_addr))) {
      LOG_WARN("failed to update tablet object and addr", K(ret), K(tablet_handle), K(new_addr));
    } else if ((OB_HASH_NOT_EXIST == tmp_ret)
        && OB_FAIL(refresh_tablet_addr(ls_id, tablet_id, new_addr, tablet_handle))) {
      LOG_WARN("failed to refresh tablet addr", K(ret), K(ls_id), K(tablet_id), K(new_addr));
    } else {
      FLOG_INFO("succeeded to add tablet", K(ret), K(tmp_ret), K(ls_id), K(tablet_id),
          K(tablet_handle), K(new_addr), K(tablet_count));
    }
    if (OB_FAIL(ret)) {
      LOG_ERROR("failed to modify memory after writing slog", K(ret), K(lbt()));
      ob_usleep(1000 * 1000);
      ob_abort();
    }
  }

  return ret;
}

int ObLSTabletService::post_handle_batch_create_tablets(
    const obrpc::ObBatchCreateTabletArg &arg)
{
  int ret = OB_SUCCESS;
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
  ObTabletMapKey key;
  key.ls_id_ = arg.id_;

  if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(arg));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < arg.tablets_.count(); ++i) {
      const obrpc::ObCreateTabletInfo &info = arg.tablets_[i];
      for (int64_t j = 0; OB_SUCC(ret) && j < info.tablet_ids_.count(); ++j) {
        const common::ObTabletID &tablet_id = info.tablet_ids_[j];
        key.tablet_id_ = tablet_id;
        if (OB_FAIL(tablet_id_set_.erase(tablet_id))) {
          if (OB_HASH_NOT_EXIST == ret) {
            // tablet id does not exist in hash set
            ret = OB_SUCCESS;
          } else {
            LOG_WARN("fail to erase tablet id from set", K(ret), K(tablet_id));
          }
        }

        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(t3m->del_tablet(key))) {
          if (OB_HASH_NOT_EXIST == ret) {
            // key does not exist in hash map
            ret = OB_SUCCESS;
          } else {
            LOG_WARN("failed to delete tablet", K(ret), K(key));
          }
        }
      }
    }
  }

  return ret;
}

int ObLSTabletService::create_memtable(
    const common::ObTabletID &tablet_id,
    const int64_t schema_version,
    const bool for_replay)
{
  int ret = OB_SUCCESS;
  ObTimeGuard time_guard("ObLSTabletService::create_memtable", 10 * 1000);
  ObTabletHandle handle;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || schema_version < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tablet_id), K(schema_version));
  } else {
    // we need bucket lock here to protect multi version tablet creation
    // during tablet creating new memtable and put it into table store.
    ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
    time_guard.click("Lock");
    if (OB_FAIL(direct_get_tablet(tablet_id, handle))) {
      LOG_WARN("fail to get tablet", K(ret), K(tablet_id));
    } else if (FALSE_IT(time_guard.click("get tablet"))) {
    } else if (OB_UNLIKELY(!handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected error, invalid tablet handle", K(ret), K(handle));
    } else if (OB_FAIL(handle.get_obj()->create_memtable(schema_version, for_replay))) {
      if (OB_MINOR_FREEZE_NOT_ALLOW != ret) {
        LOG_WARN("fail to create memtable", K(ret), K(handle), K(schema_version), K(tablet_id));
      }
    } else {
      time_guard.click("tablet create_memtable");
    }
  }
  return ret;
}

int ObLSTabletService::get_read_tables(
    const common::ObTabletID &tablet_id,
    const int64_t snapshot_version,
    ObTabletTableIterator &iter,
    const bool allow_no_ready_read)
{
  int ret = OB_SUCCESS;
  ObTabletHandle &handle = iter.tablet_handle_;
  iter.reset();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || snapshot_version < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(tablet_id), K(snapshot_version));
  } else if (OB_FAIL(check_and_get_tablet(tablet_id, handle))) {
    LOG_WARN("fail to check and get tablet", K(ret), K(tablet_id));
  } else if (OB_UNLIKELY(!handle.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, invalid tablet handle", K(ret), K(handle));
  } else if (OB_FAIL(handle.get_obj()->get_read_tables(snapshot_version, iter,
      allow_no_ready_read))) {
    LOG_WARN("fail to get read tables", K(ret), K(handle), K(tablet_id), K(snapshot_version),
        K(iter), K(allow_no_ready_read));
  }
  return ret;
}

int ObLSTabletService::build_create_sstable_param_for_migration(
    const blocksstable::ObMigrationSSTableParam &mig_param,
    ObTabletCreateSSTableParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!mig_param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(mig_param));
  } else {
    param.table_key_                     = mig_param.table_key_;
    param.schema_version_                = mig_param.basic_meta_.schema_version_;
    param.create_snapshot_version_       = mig_param.basic_meta_.create_snapshot_version_;
    param.progressive_merge_round_       = mig_param.basic_meta_.progressive_merge_round_;
    param.progressive_merge_step_        = mig_param.basic_meta_.progressive_merge_step_;
    param.is_ready_for_read_             = false;
    param.table_mode_                    = mig_param.basic_meta_.table_mode_;
    param.index_type_                    = static_cast<share::schema::ObIndexType>(mig_param.basic_meta_.index_type_);
    param.rowkey_column_cnt_             = mig_param.basic_meta_.rowkey_column_count_;
    param.root_row_store_type_           = mig_param.basic_meta_.row_store_type_;
    param.index_blocks_cnt_              = mig_param.basic_meta_.index_macro_block_count_;
    param.data_blocks_cnt_               = mig_param.basic_meta_.data_macro_block_count_;
    param.micro_block_cnt_               = mig_param.basic_meta_.data_micro_block_count_;
    param.use_old_macro_block_count_     = mig_param.basic_meta_.use_old_macro_block_count_;
    param.row_count_                     = mig_param.basic_meta_.row_count_;
    param.column_cnt_                    = mig_param.basic_meta_.column_cnt_;
    param.data_checksum_                 = mig_param.basic_meta_.data_checksum_;
    param.occupy_size_                   = mig_param.basic_meta_.occupy_size_;
    param.original_size_                 = mig_param.basic_meta_.original_size_;
    param.max_merged_trans_version_      = mig_param.basic_meta_.max_merged_trans_version_;
    param.ddl_log_ts_                    = mig_param.basic_meta_.ddl_log_ts_;
    param.filled_tx_log_ts_              = mig_param.basic_meta_.filled_tx_log_ts_;
    param.contain_uncommitted_row_       = mig_param.basic_meta_.contain_uncommitted_row_;
    param.compressor_type_               = mig_param.basic_meta_.compressor_type_;
    param.encrypt_id_                    = mig_param.basic_meta_.encrypt_id_;
    param.master_key_id_                 = mig_param.basic_meta_.master_key_id_;
    MEMCPY(param.encrypt_key_, mig_param.basic_meta_.encrypt_key_, share::OB_MAX_TABLESPACE_ENCRYPT_KEY_LENGTH);
    param.root_block_addr_.set_none_addr();
    param.data_block_macro_meta_addr_.set_none_addr();;
    if (OB_FAIL(param.column_checksums_.assign(mig_param.column_checksums_))) {
      LOG_WARN("fail to assign column checksums", K(ret), K(mig_param));
    }
  }
  return ret;
}

int ObLSTabletService::get_tablet_schema_index(
    const common::ObTabletID &tablet_id,
    const common::ObIArray<common::ObTabletID> &table_ids,
    int64_t &index)
{
  int ret = OB_SUCCESS;
  bool match = false;

  for (int64_t i = 0; !match && i < table_ids.count(); ++i) {
    if (table_ids.at(i) == tablet_id) {
      index = i;
      match = true;
    }
  }

  if (!match) {
    ret = OB_ENTRY_NOT_EXIST;
    LOG_WARN("cannot find target tablet id in array", K(ret), K(tablet_id));
  }

  return ret;
}

int ObLSTabletService::get_all_tablet_id_hash_array(
    const obrpc::ObBatchCreateTabletArg &arg,
    common::ObIArray<uint64_t> &all_tablet_id_hash_array)
{
  int ret = OB_SUCCESS;

  for (int i = 0; OB_SUCC(ret) && i < arg.tablets_.count(); ++i) {
    const obrpc::ObCreateTabletInfo &info = arg.tablets_[i];
    if (OB_FAIL(all_tablet_id_hash_array.push_back(info.data_tablet_id_.hash()))) {
      LOG_WARN("failed to push back data tablet id", K(ret),
          "data tablet id", info.data_tablet_id_);
    } else {
      // don't worry about duplicate data tablet id in arg,
      // ObMultiBucketLockGuard will try to push the array into set to remove duplicate elements,
      // and then sort the array to avoid possible dead lock
      const common::ObSArray<common::ObTabletID> &tablet_ids = info.tablet_ids_;
      for (int64_t j = 0; OB_SUCC(ret) && j < tablet_ids.count(); ++j) {
        const common::ObTabletID &tablet_id = tablet_ids[j];
        if (OB_FAIL(all_tablet_id_hash_array.push_back(tablet_id.hash()))) {
          LOG_WARN("failed to push back tablet id", K(ret), K(tablet_id));
        }
      }
    }
  }

  return ret;
}

int ObLSTabletService::parse_and_verify_delete_tablet_info(
    const obrpc::ObBatchRemoveTabletArg &arg,
    common::ObLinearHashMap<common::ObTabletID, DeleteTabletInfo> &delete_tablet_infos)
{
  int ret = OB_SUCCESS;
  const common::ObSArray<common::ObTabletID> &tablet_ids = arg.tablet_ids_;
  bool b_exist = true;
  ObTabletHandle tablet_handle;

  for (int64_t i = 0; i < tablet_ids.count(); ++i) {
    const common::ObTabletID &tablet_id = tablet_ids.at(i);
    if (OB_FAIL(direct_get_tablet(tablet_id, tablet_handle))) {
      if (OB_TABLET_NOT_EXIST == ret) {
        b_exist = false;
        ret = OB_SUCCESS;
        FLOG_INFO("tablet does not exist", K(ret), K(tablet_id));
      } else {
        LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
      }
    } else {
      b_exist = true;
    }

    if (OB_SUCC(ret) && b_exist) {
      if (tablet_handle.get_obj()->is_data_tablet()) {
        // delete data tablet and all local index tablets
        DeleteTabletInfo info;
        info.delete_data_tablet_ = true;
        info.old_data_tablet_handle_ = tablet_handle;
        if (OB_FAIL(delete_tablet_infos.insert(tablet_id, info))) {
          LOG_WARN("failed to insert into hash map", K(ret), K(tablet_id));
        }
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error, tablet handle is invalid", K(ret), K(tablet_handle));
      }
    }
  }

  return ret;
}

int ObLSTabletService::get_all_tablet_id_hash_array(
    common::ObLinearHashMap<common::ObTabletID, DeleteTabletInfo> &delete_tablet_infos,
    common::ObIArray<uint64_t> &all_tablet_id_hash_array)
{
  int ret = OB_SUCCESS;
  HashMapTabletGetFunctor functor(all_tablet_id_hash_array);

  // don't worry about duplicate data tablet id in arg,
  // ObMultiBucketLockGuard will try to push the array into set to remove duplicate elements,
  // and then sort the array to avoid possible dead lock
  if (OB_FAIL(delete_tablet_infos.for_each(functor))) {
    LOG_WARN("failed to iterate hash map to get all tablet id", K(ret));
  }

  return ret;
}

int ObLSTabletService::insert_rows(
    ObStoreCtx &ctx,
    const ObDMLBaseParam &dml_param,
    const common::ObIArray<uint64_t> &column_ids,
    common::ObNewRowIterator *row_iter,
    int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  ObTabletHandle tablet_handle;
  int64_t afct_num = 0;
  int64_t dup_num = 0;
  ObTimeGuard timeguard(__func__, 3 * 1000 * 1000);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!ctx.is_valid())
      || !ctx.is_write()
      || OB_UNLIKELY(!dml_param.is_valid())
      || OB_UNLIKELY(column_ids.count() <= 0)
      || OB_ISNULL(row_iter)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(ctx), K(dml_param), K(column_ids), KP(row_iter));
  } else if (OB_FAIL(get_tablet_with_timeout(
      ctx.tablet_id_, tablet_handle, dml_param.timeout_))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(ctx.tablet_id_));
  } else {
    ObArenaAllocator lob_allocator(ObModIds::OB_LOB_ACCESS_BUFFER, OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    ObDMLRunningCtx run_ctx(ctx,
                            dml_param,
                            ctx.mvcc_acc_ctx_.mem_ctx_->get_query_allocator(),
                            lob_allocator,
                            ObDmlFlag::DF_INSERT);
    ObIAllocator &work_allocator = run_ctx.allocator_;
    void *ptr = nullptr;
    ObStoreRow *tbl_rows = nullptr;
    int64_t row_count = 0;
    //index of row that exists
    int64_t row_count_first_bulk = 0;
    bool first_bulk = true;
    ObNewRow *rows = nullptr;
    ObRowsInfo rows_info;
    const ObRelativeTable &data_table = run_ctx.relative_table_;

    if (OB_FAIL(prepare_dml_running_ctx(&column_ids, nullptr, tablet_handle, run_ctx))) {
      LOG_WARN("failed to prepare dml running ctx", K(ret));
    }

    while (OB_SUCC(ret) && OB_SUCC(get_next_rows(row_iter, rows, row_count))) {
      if (row_count <= 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("row_count should be greater than 0", K(ret));
      } else if (first_bulk) {
        first_bulk = false;
        row_count_first_bulk = row_count;
        const ObTableReadInfo &full_read_info = tablet_handle.get_obj()->get_full_read_info();
        if (OB_FAIL(rows_info.init(data_table, ctx, full_read_info))) {
          LOG_WARN("Failed to init rows info", K(ret), K(data_table));
        } else if (OB_ISNULL(ptr = work_allocator.alloc(row_count * sizeof(ObStoreRow)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_ERROR("fail to allocate memory", K(ret), K(row_count));
        } else {
          tbl_rows = new (ptr) ObStoreRow[row_count];
          for (int64_t i = 0; i < row_count; i++) {
            tbl_rows[i].flag_.set_flag(ObDmlFlag::DF_INSERT);
          }
        }
      }

      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(tbl_rows)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error, tbl_rows is NULL", K(ret), KP(tbl_rows));
      } else if (OB_FAIL(insert_rows_to_tablet(tablet_handle, run_ctx, rows,
          row_count, rows_info, tbl_rows, afct_num, dup_num))) {
        LOG_WARN("insert to each tablets fail", K(ret));
      }
    }

    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
    if (nullptr != ptr) {
      work_allocator.free(ptr);
    }
    lob_allocator.reset();
    if (OB_SUCC(ret)) {
      LOG_DEBUG("succeeded to insert rows", K(ret));
      affected_rows = afct_num;
      EVENT_ADD(STORAGE_INSERT_ROW_COUNT, afct_num);
    }
  }

  return ret;
}

int ObLSTabletService::insert_row(
    ObStoreCtx &ctx,
    const ObDMLBaseParam &dml_param,
    const common::ObIArray<uint64_t> &column_ids,
    const common::ObIArray<uint64_t> &duplicated_column_ids,
    const common::ObNewRow &row,
    const ObInsertFlag flag,
    int64_t &affected_rows,
    common::ObNewRowIterator *&duplicated_rows)
{
  int ret = OB_SUCCESS;
  const ObTabletID &data_tablet_id = ctx.tablet_id_;
  ObTabletHandle tablet_handle;
  ObTimeGuard timeguard(__func__, 3 * 1000 * 1000);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!ctx.is_valid()
             || !ctx.is_write()
             || !dml_param.is_valid()
             || column_ids.count() <= 0
             || duplicated_column_ids.count() <= 0
             || !row.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(ctx), K(dml_param),
        K(column_ids), K(duplicated_column_ids), K(row), K(flag));
  } else if (OB_FAIL(get_tablet_with_timeout(
      ctx.tablet_id_, tablet_handle, dml_param.timeout_))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(ctx.tablet_id_));
  } else {
    ObArenaAllocator lob_allocator(ObModIds::OB_LOB_ACCESS_BUFFER, OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    ObDMLRunningCtx run_ctx(ctx,
                            dml_param,
                            ctx.mvcc_acc_ctx_.mem_ctx_->get_query_allocator(),
                            lob_allocator,
                            ObDmlFlag::DF_INSERT);
    ObIAllocator &work_allocator = run_ctx.allocator_;
    duplicated_rows = nullptr;
    ObStoreRow &tbl_row = run_ctx.tbl_row_;
    const ObRelativeTable &data_table = run_ctx.relative_table_;
    if (OB_FAIL(prepare_dml_running_ctx(&column_ids, nullptr, tablet_handle, run_ctx))) {
      LOG_WARN("failed to prepare dml running ctx", K(ret));
    } else {
      tbl_row.flag_.set_flag(ObDmlFlag::DF_INSERT);
      tbl_row.row_val_ = row;
      if (OB_FAIL(get_conflict_rows(tablet_handle,
                                    run_ctx,
                                    flag,
                                    duplicated_column_ids,
                                    tbl_row.row_val_,
                                    duplicated_rows))) {
      LOG_WARN("failed to get conflict row(s)", K(ret), K(duplicated_column_ids), K(row));
      } else if (nullptr == duplicated_rows) {
        if (OB_FAIL(insert_row_to_tablet(tablet_handle, run_ctx, tbl_row))) {
          if (OB_TRY_LOCK_ROW_CONFLICT != ret) {
            LOG_WARN("failed to write row", K(ret));
          }
        } else {
          LOG_DEBUG("succeeded to insert row", K(ret), K(row));
          affected_rows = 1;
          EVENT_INC(STORAGE_INSERT_ROW_COUNT);
        }
      } else {
        ret = OB_ERR_PRIMARY_KEY_DUPLICATE;
      }
    }
    lob_allocator.reset();
  }

  return ret;
}

static inline
bool is_lob_update(ObDMLRunningCtx &run_ctx, const ObIArray<int64_t> &update_idx)
{
  bool bool_ret = false;
  for (int64_t i = 0; i < update_idx.count() && !bool_ret; ++i) {
    int64_t idx = update_idx.at(i);
    if (run_ctx.col_descs_->at(idx).col_type_.is_lob_v2()) {
      bool_ret = true;
    }
  }
  return bool_ret;
}

int ObLSTabletService::update_rows(
    ObStoreCtx &ctx,
    const ObDMLBaseParam &dml_param,
    const ObIArray<uint64_t> &column_ids,
    const ObIArray< uint64_t> &updated_column_ids,
    ObNewRowIterator *row_iter,
    int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  const ObTabletID &data_tablet_id = ctx.tablet_id_;
  ObTabletHandle tablet_handle;
  int64_t afct_num = 0;
  int64_t dup_num = 0;
  int64_t got_row_count = 0;
  ObTimeGuard timeguard(__func__, 3 * 1000 * 1000);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!ctx.is_valid()
             || !ctx.is_write()
             || !dml_param.is_valid()
             || column_ids.count() <= 0
             || updated_column_ids.count() <= 0
             || nullptr == row_iter)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(ctx), K(dml_param),
        K(column_ids), K(updated_column_ids), KP(row_iter));
  } else if (OB_FAIL(get_tablet_with_timeout(
      ctx.tablet_id_, tablet_handle, dml_param.timeout_))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(ctx.tablet_id_));
  } else {
    timeguard.click("Get");
    ObArenaAllocator lob_allocator(ObModIds::OB_LOB_ACCESS_BUFFER, OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    ObDMLRunningCtx run_ctx(ctx,
                            dml_param,
                            ctx.mvcc_acc_ctx_.mem_ctx_->get_query_allocator(),
                            lob_allocator,
                            ObDmlFlag::DF_UPDATE);
    ObIAllocator &work_allocator = run_ctx.allocator_;
    ObStoreRow old_tbl_row;
    void *old_row_cells = nullptr;
    ObStoreRow &new_tbl_row = run_ctx.tbl_row_;
    bool rowkey_change = false;
    UpdateIndexArray update_idx;
    ObRowStore row_store;
    bool delay_new = false;
    bool lob_update = false;
    const ObRelativeTable &relative_table = run_ctx.relative_table_;
    if (OB_FAIL(prepare_dml_running_ctx(&column_ids, &updated_column_ids, tablet_handle, run_ctx))) {
      LOG_WARN("failed to prepare dml running ctx", K(ret));
    } else if (FALSE_IT(timeguard.click("Prepare"))) {
    } else if (OB_UNLIKELY(!relative_table.is_valid())) {
      ret = OB_ERR_SYS;
      LOG_ERROR("data table is not prepared", K(ret));
    } else if (OB_FAIL(construct_update_idx(relative_table.get_rowkey_column_num(),
        run_ctx.col_map_,
        updated_column_ids, update_idx))) {
      LOG_WARN("failed to construct update_idx", K(ret), K(updated_column_ids));
    } else if (FALSE_IT(timeguard.click("Construct"))) {
    } else if (OB_FAIL(check_rowkey_change(updated_column_ids, relative_table, rowkey_change, delay_new))) {
      LOG_WARN("failed to check rowkey changes", K(ret));
    } else {
      timeguard.click("Check");
      const int64_t num = relative_table.get_column_count();
      if (OB_ISNULL(old_row_cells = work_allocator.alloc(
          static_cast<int64_t>(sizeof(common::ObObj) * num)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("failed to malloc temp row cells", K(ret));
      } else {
        timeguard.click("AllocOld");
        old_tbl_row.row_val_.cells_ = new (old_row_cells) ObObj[num]();
        old_tbl_row.flag_.set_flag(ObDmlFlag::DF_UPDATE);
        new_tbl_row.flag_.set_flag(ObDmlFlag::DF_UPDATE);
      }
      lob_update = is_lob_update(run_ctx, update_idx);
    }

    int64_t cur_time = 0;
    while (OB_SUCC(ret)
        && OB_SUCC(get_next_row_from_iter(row_iter, old_tbl_row, true))
        && OB_SUCC(get_next_row_from_iter(row_iter, new_tbl_row, false))) {
      LOG_DEBUG("get_dml_update_row", KP(row_iter), K(old_tbl_row), K(new_tbl_row));
      bool duplicate = false;
      ++got_row_count;
      cur_time = ObTimeUtility::current_time();
      if ((0 == (0x1FF & got_row_count)) && (cur_time > dml_param.timeout_)) {
        //checking timeout cost too much, so check every 512 rows
        ret = OB_TIMEOUT;
        LOG_WARN("query timeout", K(cur_time), K(dml_param), K(ret));
      } else if (OB_FAIL(update_row_to_tablet(tablet_handle,
                                              run_ctx,
                                              rowkey_change,
                                              update_idx,
                                              delay_new,
                                              lob_update,
                                              old_tbl_row,
                                              new_tbl_row,
                                              &row_store,
                                              duplicate))) {
        LOG_WARN("failed to update row to tablets", K(ret), K(old_tbl_row), K(new_tbl_row));
      } else if (duplicate) {
        dup_num++;
      } else {
        afct_num++;
      }
      timeguard.click("Update");
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }

    if (OB_SUCC(ret) && row_store.get_row_count() > 0) {
      void *ptr1 = nullptr;
      const int64_t num = relative_table.get_column_count();
      if (OB_ISNULL(ptr1 = work_allocator.alloc(sizeof(common::ObObj) * num))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("failed to malloc temp row cells", K(ret));
      } else {
        timeguard.click("AllocNew");
        new_tbl_row.row_val_.cells_ = new(ptr1) ObObj[num]();
        ObRowStore::Iterator row_iter2 = row_store.begin();
        // when total_quantity_log is true, we should iterate new_tbl_row and old_tbl_row, and
        // dispose these two rows together, otherwise, when total_quantity_log is false,
        // row_iter2 doesn't contain old rows, and old_tbl_row is a dummy param in process_new_row
        while (OB_SUCC(ret) && OB_SUCC(row_iter2.get_next_row(new_tbl_row.row_val_))) {
          int64_t data_tbl_rowkey_len = relative_table.get_rowkey_column_num();
          bool tbl_rowkey_change = false;
          if (OB_FAIL(row_iter2.get_next_row(old_tbl_row.row_val_))) {
            LOG_WARN("fail to get row from row stores", K(ret));
          } else if (FALSE_IT(timeguard.click("GetNext"))) {
          } else if (rowkey_change && OB_FAIL(check_rowkey_value_change(old_tbl_row.row_val_,
                                                                        new_tbl_row.row_val_,
                                                                        data_tbl_rowkey_len,
                                                                        tbl_rowkey_change))) {
            LOG_WARN("check data table rowkey change failed", K(ret), K(old_tbl_row),
                K(new_tbl_row), K(data_tbl_rowkey_len));
          } else if (OB_FAIL(process_new_row(tablet_handle,
                                             run_ctx,
                                             update_idx,
                                             old_tbl_row,
                                             new_tbl_row,
                                             tbl_rowkey_change))) {
            LOG_WARN("fail to process new row", K(ret), K(old_tbl_row), K(new_tbl_row));
          }
          timeguard.click("Process");
        }
        work_allocator.free(ptr1);
        ptr1 = nullptr;
        new_tbl_row.row_val_.cells_ = nullptr;
      }
    }

    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
    if (nullptr != old_row_cells) {
      work_allocator.free(old_row_cells);
    }
    lob_allocator.reset();
    old_tbl_row.row_val_.cells_ = nullptr;
    if (OB_SUCC(ret)) {
      affected_rows = afct_num;
      EVENT_ADD(STORAGE_UPDATE_ROW_COUNT, afct_num);
    }
    if (timeguard.get_diff() > 3 * 1000 * 1000) {
      LOG_WARN("update rows use too much time", K(afct_num), K(got_row_count));
    }
  }

  return ret;
}

int ObLSTabletService::put_rows(
    ObStoreCtx &ctx,
    const ObDMLBaseParam &dml_param,
    const ObIArray<uint64_t> &column_ids,
    ObNewRowIterator *row_iter,
    int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  const ObTabletID &data_tablet_id = ctx.tablet_id_;
  ObTabletHandle tablet_handle;
  int64_t afct_num = 0;
  ObTimeGuard timeguard(__func__, 3 * 1000 * 1000);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!ctx.is_valid())
      || OB_UNLIKELY(!ctx.is_write())
      || OB_UNLIKELY(!dml_param.is_valid())
      || OB_UNLIKELY(column_ids.count() <= 0)
      || OB_ISNULL(row_iter)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(ctx), K(dml_param), K(column_ids), KP(row_iter));
  } else if (OB_FAIL(get_tablet_with_timeout(
      ctx.tablet_id_, tablet_handle, dml_param.timeout_))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(ctx.tablet_id_));
  } else {
    ObArenaAllocator lob_allocator(ObModIds::OB_LOB_ACCESS_BUFFER, OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    ObDMLRunningCtx run_ctx(ctx,
                            dml_param,
                            ctx.mvcc_acc_ctx_.mem_ctx_->get_query_allocator(),
                            lob_allocator,
                            ObDmlFlag::DF_UPDATE);
    ObIAllocator &work_allocator = run_ctx.allocator_;
    ObNewRow *row = nullptr;
    ObStoreRow &tbl_row = run_ctx.tbl_row_;
    const ObRelativeTable &data_table = run_ctx.relative_table_;

    if (OB_FAIL(prepare_dml_running_ctx(&column_ids, nullptr, tablet_handle, run_ctx))) {
      LOG_WARN("failed to prepare dml running ctx", K(ret));
    } else {
      tbl_row.flag_.set_flag(ObDmlFlag::DF_UPDATE);
    }

    int64_t cur_time = 0;
    while (OB_SUCC(ret) && OB_SUCC(row_iter->get_next_row(row))) {
      cur_time = ObTimeUtility::current_time();
      tbl_row.row_val_ = *row;
      if (cur_time > dml_param.timeout_) {
        ret = OB_TIMEOUT;
        LOG_WARN("query timeout", K(ret), K(cur_time), K(dml_param));
      } else if (OB_FAIL(insert_row_to_tablet(tablet_handle, run_ctx, tbl_row))) {
        if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
          LOG_WARN("failed to write row", K(ret));
        }
      }
      ++afct_num;
    }

    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
    lob_allocator.reset();
    if (OB_SUCC(ret)) {
      affected_rows = afct_num;
      EVENT_ADD(STORAGE_INSERT_ROW_COUNT, afct_num);
    }
  }

  return ret;
}

int ObLSTabletService::delete_rows(
    ObStoreCtx &ctx,
    const ObDMLBaseParam &dml_param,
    const ObIArray<uint64_t> &column_ids,
    ObNewRowIterator *row_iter,
    int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  const ObTabletID &data_tablet_id = ctx.tablet_id_;
  ObTabletHandle tablet_handle;
  ObRowReshape *row_reshape = nullptr;
  int64_t afct_num = 0;
  ObTimeGuard timeguard(__func__, 3 * 1000 * 1000);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_ISNULL(row_iter) || !ctx.is_valid() || !ctx.is_write()
             || column_ids.count() <= 0 || OB_ISNULL(row_iter)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(dml_param), K(column_ids),
        KP(row_iter), K(ctx));
  } else if (OB_FAIL(get_tablet_with_timeout(
      ctx.tablet_id_, tablet_handle, dml_param.timeout_))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(ctx.tablet_id_));
  } else {
    ObArenaAllocator lob_allocator(ObModIds::OB_LOB_ACCESS_BUFFER, OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    ObDMLRunningCtx run_ctx(ctx,
                            dml_param,
                            ctx.mvcc_acc_ctx_.mem_ctx_->get_query_allocator(),
                            lob_allocator,
                            ObDmlFlag::DF_DELETE);
    ObNewRow *row = nullptr;
    if (OB_FAIL(prepare_dml_running_ctx(&column_ids, nullptr, tablet_handle, run_ctx))) {
      LOG_WARN("failed to prepare dml running ctx", K(ret));
    }
    // delete table rows
    int64_t cur_time = 0;
    while (OB_SUCC(ret) && OB_SUCC(row_iter->get_next_row(row))) {
      cur_time = ObTimeUtility::current_time();
      if (cur_time > run_ctx.dml_param_.timeout_) {
        ret = OB_TIMEOUT;
        LOG_WARN("query timeout", K(cur_time), K(run_ctx.dml_param_), K(ret));
      } else if (OB_ISNULL(row)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get next row from iterator failed", KP(row), K(ret));
      } else if (OB_FAIL(delete_row_in_tablet(tablet_handle, run_ctx, *row))) {
        LOG_WARN("fail to delete row", K(ret), K(row));
      } else {
        ++afct_num;
      }
    }
    lob_allocator.reset();
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
    if (OB_SUCC(ret)) {
      affected_rows = afct_num;
      EVENT_ADD(STORAGE_DELETE_ROW_COUNT, afct_num);
    }
  }

  return ret;
}

int ObLSTabletService::lock_rows(
    ObStoreCtx &ctx,
    const ObDMLBaseParam &dml_param,
    const int64_t abs_lock_timeout,
    const ObLockFlag lock_flag,
    const bool is_sfu,
    ObNewRowIterator *row_iter,
    int64_t &affected_rows)
{
  UNUSEDx(lock_flag, is_sfu);
  int ret = OB_SUCCESS;
  const ObTabletID &data_tablet_id = ctx.tablet_id_;
  ObTimeGuard timeguard(__func__, 3 * 1000 * 1000);
  ObTabletHandle tablet_handle;
  int64_t afct_num = 0;
  ObColDescArray col_desc;
  common::ObSEArray<uint64_t, 1> column_ids;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("tablet service is not initialized", K(ret));
  } else if (OB_UNLIKELY(!ctx.is_valid()
             || !ctx.is_write()
             || !dml_param.is_valid()
             || OB_ISNULL(row_iter))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ctx), K(dml_param), KPC(row_iter));
  } else if (OB_FAIL(get_tablet_with_timeout(
      ctx.tablet_id_, tablet_handle, dml_param.timeout_))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(ctx.tablet_id_));
  } else {
    timeguard.click("Get");
    ObDMLRunningCtx run_ctx(ctx,
                            dml_param,
                            ctx.mvcc_acc_ctx_.mem_ctx_->get_query_allocator(),
                            ctx.mvcc_acc_ctx_.mem_ctx_->get_query_allocator(),
                            ObDmlFlag::DF_LOCK);
    ObNewRow *row = nullptr;
    if (OB_FAIL(prepare_dml_running_ctx(nullptr, nullptr, tablet_handle, run_ctx))) {
      LOG_WARN("failed to prepare dml running ctx", K(ret));
    } else if (FALSE_IT(timeguard.click("Prepare"))) {
    } else if (OB_FAIL(run_ctx.relative_table_.get_rowkey_column_ids(col_desc))) {
      LOG_WARN("Fail to get column desc", K(ret));
    } else if (OB_FAIL(run_ctx.relative_table_.get_rowkey_column_ids(column_ids))) {
      LOG_WARN("Fail to get column ids", K(ret));
    } else {
      timeguard.click("GetIds");
      run_ctx.column_ids_ = &column_ids;
      ctx.mvcc_acc_ctx_.abs_lock_timeout_ =
        ObTablet::get_lock_wait_timeout(abs_lock_timeout, dml_param.timeout_);
      while (OB_SUCCESS == ret && OB_SUCC(row_iter->get_next_row(row))) {
        ObRelativeTable &relative_table = run_ctx.relative_table_;
        bool is_exists = true;
        if (ObTimeUtility::current_time() > dml_param.timeout_) {
          ret = OB_TIMEOUT;
          int64_t cur_time = ObTimeUtility::current_time();
          LOG_WARN("query timeout", K(cur_time), K(dml_param), K(ret));
        } else if (GCONF.enable_defensive_check()
            && OB_FAIL(check_old_row_legitimacy(tablet_handle, run_ctx, *row))) {
          LOG_WARN("check row legitimacy failed", K(ret), KPC(row));
        } else if (GCONF.enable_defensive_check()
            && OB_FAIL(check_new_row_nullable_value(col_desc, relative_table, *row))) {
          LOG_WARN("check lock row nullable failed", K(ret));
        } else if (FALSE_IT(timeguard.click("Check"))) {
        } else if (OB_FAIL(tablet_handle.get_obj()->lock_row(run_ctx.relative_table_, ctx, *row))) {
          if (OB_TRY_LOCK_ROW_CONFLICT != ret) {
            LOG_WARN("failed to lock row", K(*row), K(ret));
          }
        } else {
          ++afct_num;
        }
        timeguard.click("Lock");
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        affected_rows = afct_num;
      }
    }
  }
  return ret;
}

int ObLSTabletService::lock_row(
    ObStoreCtx &ctx,
    const ObDMLBaseParam &dml_param,
    const int64_t abs_lock_timeout,
    const ObNewRow &row,
    const ObLockFlag lock_flag,
    const bool is_sfu)
{
  UNUSEDx(lock_flag, is_sfu);
  int ret = OB_SUCCESS;
  const ObTabletID &data_tablet_id = ctx.tablet_id_;
  ObTimeGuard timeguard(__func__, 3 * 1000 * 1000);
  ObTabletHandle tablet_handle;
  int64_t afct_num = 0;
  ObColDescArray col_desc;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("tablet service is not initialized", K(ret));
  } else if (OB_UNLIKELY(!ctx.is_valid() || !dml_param.is_valid() || !row.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ctx), K(dml_param), K(row));
  } else if (OB_FAIL(get_tablet_with_timeout(
      ctx.tablet_id_, tablet_handle, dml_param.timeout_))) {
    LOG_WARN("failed to check and get tablet", K(ret), K(ctx.tablet_id_));
  } else {
    ObDMLRunningCtx run_ctx(ctx,
                            dml_param,
                            ctx.mvcc_acc_ctx_.mem_ctx_->get_query_allocator(),
                            ctx.mvcc_acc_ctx_.mem_ctx_->get_query_allocator(),
                            ObDmlFlag::DF_LOCK);
    if (OB_FAIL(prepare_dml_running_ctx(nullptr, nullptr, tablet_handle, run_ctx))) {
      LOG_WARN("failed to prepare dml running ctx", K(ret));
    } else if (OB_FAIL(run_ctx.relative_table_.get_rowkey_column_ids(col_desc))) {
      LOG_WARN("Fail to get column desc", K(ret));
    } else {
      ctx.mvcc_acc_ctx_.abs_lock_timeout_ =
        ObTablet::get_lock_wait_timeout(abs_lock_timeout, dml_param.timeout_);
      if (ObTimeUtility::current_time() > dml_param.timeout_) {
        ret = OB_TIMEOUT;
        int64_t cur_time = ObTimeUtility::current_time();
        LOG_WARN("query timeout", K(cur_time), K(dml_param), K(ret));
      } else if (OB_FAIL(tablet_handle.get_obj()->lock_row(run_ctx.relative_table_, ctx, row))) {
        if (OB_TRY_LOCK_ROW_CONFLICT != ret) {
          LOG_WARN("failed to lock row", K(row), K(ret));
        }
      } else {
        ++afct_num;
      }
    }
  }

  return ret;
}

int ObLSTabletService::trim_rebuild_tablet(
    const ObTabletID &tablet_id,
    const bool is_rollback)
{
  int ret = OB_SUCCESS;
  ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
  if (OB_UNLIKELY(!tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tablet_id));
  } else if (is_rollback && OB_FAIL(rollback_rebuild_tablet(tablet_id))) {
    LOG_WARN("failed to rollback tablet rebuilt", K(ret), K(is_rollback), K(tablet_id));
  } else if (!is_rollback && OB_FAIL(trim_old_tablets(tablet_id))) {
    LOG_WARN("failed to trim old tablets", K(ret), K(is_rollback), K(tablet_id));
  }
  return ret;
}

int ObLSTabletService::create_or_update_migration_tablet(
    const ObMigrationTabletParam &mig_tablet_param,
    const bool is_transfer)
{
  // TODO: is_transfer may be redundant, temporarily unused
  UNUSEDx(is_transfer);
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = mig_tablet_param.ls_id_;
  const common::ObTabletID &tablet_id = mig_tablet_param.tablet_id_;
  ObTabletHandle tablet_handle;
  bool b_exist = false;

  ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!mig_tablet_param.is_valid())
      || OB_UNLIKELY(ls_id != ls_->get_ls_id())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(mig_tablet_param), K_(ls));
  } else if (OB_FAIL(has_tablet(ls_id, tablet_id, b_exist))) {
    LOG_WARN("failed to check tablet existence", K(ls_id), K(tablet_id));
  } else if (b_exist
      && OB_FAIL(migrate_update_tablet(mig_tablet_param))) {
    LOG_WARN("failed to update tablet meta", K(ret), K(tablet_id), K(mig_tablet_param));
  } else if (!b_exist
      && OB_FAIL(migrate_create_tablet(mig_tablet_param, tablet_handle))) {
    LOG_WARN("failed to migrate create tablet", K(ret), K(mig_tablet_param));
  }

  return ret;
}

int ObLSTabletService::rebuild_create_tablet(
    const ObMigrationTabletParam &mig_tablet_param,
    const bool keep_old)
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = mig_tablet_param.ls_id_;
  const common::ObTabletID &tablet_id = mig_tablet_param.tablet_id_;
  bool b_exist = false;
  ObTabletHandle new_tablet_handle;
  ObTabletHandle old_tablet_handle;

  ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!mig_tablet_param.is_valid())
      || OB_UNLIKELY(ls_id != ls_->get_ls_id())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(mig_tablet_param), K_(ls));
  } else if (OB_FAIL(has_tablet(ls_id, tablet_id, b_exist))) {
    LOG_WARN("fail to check tablet existence", K(ls_id), K(tablet_id));
  } else if (!b_exist &&
      OB_FAIL(migrate_create_tablet(mig_tablet_param, new_tablet_handle))) {
    LOG_WARN("failed to rebuild create tablet", K(ret), K(tablet_id), K(mig_tablet_param));
  } else if (b_exist && !keep_old &&
      OB_FAIL(migrate_update_tablet(mig_tablet_param))) {
    LOG_WARN("failed to rebuild create tablet", K(ret), K(tablet_id), K(mig_tablet_param));
  } else if (b_exist && keep_old) {
    if (OB_FAIL(check_and_get_tablet(tablet_id, old_tablet_handle))) {
      LOG_WARN("failed to check and get tablet", K(ret), K(tablet_id));
    } else if (OB_UNLIKELY(old_tablet_handle.get_obj()->get_tablet_meta().has_next_tablet_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("number of nodes on list exceeds 2", K(ret));
    } else if (OB_FAIL(rebuild_tablet_with_old(mig_tablet_param, old_tablet_handle))) {
      LOG_WARN("failed to rebuild tablet and maintain linked list", K(ret), K(tablet_id));
    }
  }

  return ret;
}

int ObLSTabletService::create_migration_sstable(
    const blocksstable::ObMigrationSSTableParam &mig_sstable_param,
    const ObSSTableStatus status,
    ObTableHandleV2 &table_handle)
{
  int ret = OB_SUCCESS;
  ObTabletCreateSSTableParam create_param;
  ObTabletHandle tablet_handle;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!mig_sstable_param.is_valid()
      || (ObSSTableStatus::SSTABLE_READY_FOR_REMOTE_LOGICAL_READ != status
         && ObSSTableStatus::SSTABLE_READY_FOR_REMOTE_PHYTSICAL_READ != status
         && ObSSTableStatus::SSTABLE_WRITE_BUILDING != status))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(mig_sstable_param), K(status));
  } else if (OB_FAIL(build_create_sstable_param_for_migration(mig_sstable_param, create_param))) {
    LOG_WARN("fail to build create sstable param", K(ret), K(mig_sstable_param));
  } else if (OB_FAIL(ObTabletCreateDeleteHelper::create_sstable(create_param, table_handle))) {
    LOG_WARN("fail to create sstable", K(ret), K(create_param));
  } else if (ObSSTableStatus::SSTABLE_WRITE_BUILDING != status) {
    ObSSTable *sstable = nullptr;
    if (OB_FAIL(table_handle.get_sstable(sstable))) {
      LOG_WARN("fail to get sstable", K(ret), KP(sstable));
    } else if (OB_ISNULL(sstable)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected error, sstable is nullptr", K(ret), KP(sstable));
    } else if (OB_FAIL(sstable->set_status_for_read(status))) {
      LOG_WARN("fail to set status for sstable", K(ret), K(status), KPC(sstable));
    }
  }
  return ret;
}

int ObLSTabletService::finish_copy_migration_sstable(
    const ObTabletID &tablet_id,
    const ObITable::TableKey &sstable_key)
{
  int ret = OB_SUCCESS;
  const ObTabletMapKey key(ls_->get_ls_id(), tablet_id);
  ObMetaDiskAddr disk_addr;
  ObTabletHandle tablet_handle;
  ObTableHandleV2 sstable_handle;
  ObSSTable *sstable = nullptr;
  ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || !sstable_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(tablet_id), K(sstable_key));
  } else if (OB_FAIL(check_and_get_tablet(tablet_id, tablet_handle))) {
    LOG_WARN("fail to check and get tablet", K(ret), K(tablet_id));
  } else if (OB_FAIL(tablet_handle.get_obj()->get_table_store().get_table(
      sstable_key, sstable_handle))) {
    LOG_WARN("fail to get sstable", K(ret), K(tablet_id), K(sstable_key));
  } else if (OB_FAIL(sstable_handle.get_sstable(sstable))) {
    LOG_WARN("fail to get sstable", K(ret), KP(sstable));
  } else if (OB_ISNULL(sstable)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, sstable is nullptr", K(ret), KP(sstable));
  } else if (OB_FAIL(sstable->set_status_for_read(ObSSTableStatus::SSTABLE_READY_FOR_READ))) {
    LOG_WARN("fail to set status for sstable", K(ret), KPC(sstable));
  } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(tablet_handle, disk_addr))) {
    LOG_WARN("fail to write update tablet slog", K(ret), K(tablet_handle), K(disk_addr));
  } else if (OB_FAIL(t3m->compare_and_swap_tablet(key, disk_addr,
      tablet_handle, tablet_handle))) {
    LOG_ERROR("failed to create tablet", K(ret), K(key), K(disk_addr), K(tablet_handle), K(lbt()));
    ob_usleep(1000 * 1000);
    ob_abort();
  }
  return ret;
}

int ObLSTabletService::build_ha_tablet_new_table_store(
    const ObTabletID &tablet_id,
    const ObBatchUpdateTableStoreParam &param)
{
  int ret = OB_SUCCESS;
  ObTabletHandle old_tablet_handle;
  ObTablet *old_tablet = nullptr;
  ObTabletHandle new_tablet_handle;
  ObTablet *new_tablet = nullptr;
  ObTenantMetaMemMgr *t3m = nullptr;
  ObMetaDiskAddr disk_addr;
  ObFreezer *freezer = nullptr;

  ObBucketHashWLockGuard lock_guard(bucket_lock_, tablet_id.hash());
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || !param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tablet_id), K(param));
  } else if (OB_ISNULL(t3m = MTL(ObTenantMetaMemMgr*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("t3m is null", K(ret));
  } else if (OB_FAIL(direct_get_tablet(tablet_id, old_tablet_handle))) {
    LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
  } else if (OB_ISNULL(freezer = ls_->get_freezer())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("freezer should not be NULL", K(ret), KP(freezer), KPC(ls_));
  } else {
    old_tablet = old_tablet_handle.get_obj();
    const share::ObLSID &ls_id = ls_->get_ls_id();
    const ObTabletMapKey key(ls_id, tablet_id);
    ObTabletTxMultiSourceDataUnit tx_data;
    ObTabletBindingInfo ddl_data;
    ObTabletAutoincSeq autoinc_seq;
    //In order to merge tablet meta
    //it is necessary to make the left side of the newly created memtable start from clog_checkpinoit_ts
    //the new memtable can be stuck during the creation of the tablet, it is safe here

    if (OB_FAIL(old_tablet->get_tx_data(tx_data))) {
      LOG_WARN("failed to get tx data from old tablet", K(ret), K(tablet_id));
    } else if (OB_FAIL(old_tablet->get_ddl_data(ddl_data))) {
      LOG_WARN("failed to get tx data from old tablet", K(ret), K(tablet_id));
    } else if (OB_FAIL(old_tablet->get_latest_autoinc_seq(autoinc_seq))) {
      LOG_WARN("failed to get autoinc seq from old tablet", K(ret));
    } else if (OB_FAIL(ObTabletCreateDeleteHelper::acquire_tablet(key, new_tablet_handle, true/*only acquire*/))) {
      LOG_WARN("failed to acquire tablet", K(ret), K(key));
    } else if (FALSE_IT(new_tablet = new_tablet_handle.get_obj())) {
    } else if (OB_FAIL(new_tablet->init(param, *old_tablet, tx_data, ddl_data, autoinc_seq))) {
      LOG_WARN("failed to init tablet", K(ret), KPC(old_tablet));
    } else if (old_tablet->get_tablet_meta().clog_checkpoint_ts_ < new_tablet->get_tablet_meta().clog_checkpoint_ts_
        && OB_FAIL(freezer->tablet_freeze(tablet_id))) {
      if (OB_ENTRY_EXIST == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("failed to freeze tablet", K(ret), K(tablet_id), KPC(old_tablet), KPC(new_tablet));
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(new_tablet->set_memtable_clog_checkpoint_ts(param.tablet_meta_))) {
      LOG_WARN("failed to set memtable clog checkpoint ts", K(ret), KPC(old_tablet), KPC(new_tablet), K(param));
    } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(new_tablet_handle, disk_addr))) {
      LOG_WARN("fail to write update tablet slog", K(ret), K(new_tablet_handle), K(disk_addr));
    } else if (OB_FAIL(t3m->compare_and_swap_tablet(key, disk_addr, old_tablet_handle, new_tablet_handle))) {
      LOG_ERROR("failed to compare and swap tablet", K(ret), K(key), K(disk_addr));
      ob_usleep(1000 * 1000);
      ob_abort();
    } else {
      LOG_INFO("succeed to build ha tablet new table store", K(ret), K(tablet_id), K(param));
    }
  }
  return ret;
}

int ObLSTabletService::verify_tablets(
    const obrpc::ObBatchCreateTabletArg &arg,
    NonLockedHashSet &existed_tablet_id_set)
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = arg.id_;
  bool b_exist = true;
  ObTabletHandle tablet_handle;

  for (int64_t i = 0; OB_SUCC(ret) && i < arg.tablets_.count(); ++i) {
    const ObCreateTabletInfo &tablet_arg = arg.tablets_[i];
    const common::ObSArray<common::ObTabletID> &tablet_ids = tablet_arg.tablet_ids_;
    const common::ObTabletID &data_tablet_id = tablet_arg.data_tablet_id_;
    bool is_pure_index = true;
    bool is_data_tablet_exist = true;
    for (int64_t k = 0; is_pure_index && k < tablet_ids.count(); ++k) {
      if (tablet_ids[k] == data_tablet_id) {
        is_pure_index = false;
      }
    }

    if (is_pure_index) {
      ObTabletHandle data_tablet_handle;
      if (OB_FAIL(direct_get_tablet(data_tablet_id, data_tablet_handle))) {
        if (OB_TABLET_NOT_EXIST == ret) {
          is_data_tablet_exist = false;
          ret = OB_SUCCESS;
          // TODO (bowen.gbw): data tablet might be removed when replaying index tablet creation
          // so that we should skip such clog to avoid replaying failure.
          // Temp fix: consider those index tablets as existed ones and skip in replaying
          // Final solution:  Multi Data Source Transaction
          FLOG_INFO("data tablet does not exist, need skip create pure index tablets", K(ret), K(data_tablet_id));
        } else {
          LOG_WARN("failed to get tablet", K(ret), K(data_tablet_id));
        }
      }
    }

    for (int64_t j = 0; OB_SUCC(ret) && j < tablet_ids.count(); ++j) {
      const common::ObTabletID &tablet_id = tablet_ids[j];
      if (is_pure_index && !is_data_tablet_exist) {
        b_exist = true;
      } else if (OB_FAIL(direct_get_tablet(tablet_id, tablet_handle))) {
        if (OB_TABLET_NOT_EXIST == ret) {
          b_exist = false;
          ret = OB_SUCCESS;
          LOG_INFO("tablet does not exist", K(ret), K(tablet_id));
        } else {
          LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
        }
      } else {
        b_exist = true;
      }

      if (OB_SUCC(ret) && b_exist) {
        FLOG_INFO("tablet already exists", K(ls_id), K(tablet_id));

        if (OB_FAIL(existed_tablet_id_set.set_refactored(tablet_id))) {
          LOG_WARN("failed to insert into hash set", K(ret), K(tablet_id));
        }
      }
    }
  }

  return ret;
}

int ObLSTabletService::need_check_old_row_legitimacy(ObDMLRunningCtx &run_ctx,
                                                     bool &need_check,
                                                     bool &is_udf)
{
  int ret = OB_SUCCESS;
  ObRelativeTable &data_table = run_ctx.relative_table_;
  need_check = false;
  const ObDMLBaseParam &dml_param = run_ctx.dml_param_;
  is_udf = false;
  // TODO(jingxing): setting this to true
  if (OB_FAIL(data_table.has_udf_column(need_check))) {
    LOG_WARN("check has udf column failed", K(ret));
  } else if (need_check) {
    is_udf = true;
    ObTableStoreIterator &table_iter =  data_table.tablet_iter_.table_iter_;
    while (OB_SUCC(ret) && !need_check) {
      ObITable *table_ptr = nullptr;
      if (OB_FAIL(table_iter.get_next(table_ptr))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("get next table failed", K(ret));
        }
      } else if (OB_ISNULL(table_ptr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("error unexpected, table ptr must not be nullptr", K(ret));
      } else {
        need_check = table_ptr->is_major_sstable();
      }
    }
  } else if (dml_param.is_batch_stmt_ && !data_table.is_index_table()) {
    //batch stmt execution dependency defensive check to check
    //if the same row was modified multiple times
    need_check = true;
  } else if (GCONF.enable_defensive_check()) {
    need_check = true;
    if (data_table.is_index_table() && !data_table.can_read_index()) {
      //index can not be read during building index, so does not check old index row
      need_check = false;
    }
  }
  return ret;
}

int ObLSTabletService::construct_table_rows(
    const ObNewRow *rows,
    ObStoreRow *tbl_rows,
    int64_t row_count)
{
  int ret = OB_SUCCESS;
  if (row_count <= 0) {
    ret = OB_ERR_WRONG_VALUE_COUNT_ON_ROW;
    LOG_WARN("row count should be bigger than 0", K(row_count), K(ret));
  } else {
    for (int64_t i = 0; i < row_count; i++) {
      tbl_rows[i].row_val_ = rows[i];
    }
  }
  return ret;
}

int ObLSTabletService::check_old_row_legitimacy(
    ObTabletHandle &data_tablet_handle,
    ObDMLRunningCtx &run_ctx,
    const common::ObNewRow &old_row)
{
  int ret = OB_SUCCESS;

  ObRelativeTable &data_table = run_ctx.relative_table_;
  ObStoreRowkey rowkey;
  bool need_check = false;
  bool is_udf = false;
  rowkey.assign(old_row.cells_, data_table.get_rowkey_column_num());
  if (OB_UNLIKELY(rowkey.get_obj_cnt() > old_row.count_) || OB_ISNULL(run_ctx.column_ids_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("old row is invalid", K(ret), K(old_row), K(rowkey.get_obj_cnt()), KP(run_ctx.column_ids_));
  } else if (OB_FAIL(need_check_old_row_legitimacy(run_ctx, need_check, is_udf))) {
    LOG_WARN("identify need check old row legitimacy", K(ret));
  } else if (need_check) {
    //the vertical partition is no longer maintained,
    //and the defense check skips the vertical partition function
    const ObDMLBaseParam &dml_param = run_ctx.dml_param_;
    ObArenaAllocator scan_allocator(ObModIds::OB_TABLE_SCAN_ITER);
    ObIAllocator *allocator = &scan_allocator;
    ObSingleRowGetter old_row_getter(*allocator, *data_tablet_handle.get_obj());
    ObNewRow *storage_old_row = nullptr;
    //check old row whether different with SQL.old_row,
    ObDatumRowkey datum_rowkey;
    ObDatumRowkeyHelper rowkey_helper;
    const ObIArray<uint64_t> &column_ids = *run_ctx.column_ids_;
    if (OB_FAIL(rowkey_helper.convert_datum_rowkey(rowkey.get_rowkey(), datum_rowkey))) {
      STORAGE_LOG(WARN, "Failed to transfer datum rowkey", K(ret), K(rowkey));
    } else if (OB_FAIL(old_row_getter.init_dml_access_ctx(run_ctx.store_ctx_, dml_param))) {
      LOG_WARN("init dml access ctx failed", K(ret));
    } else if (OB_FAIL(old_row_getter.init_dml_access_param(data_table, dml_param, column_ids))) {
      LOG_WARN("init dml access param failed", K(ret));
    } else if (OB_FAIL(old_row_getter.open(datum_rowkey, true))) {
      LOG_WARN("open old row getter failed", K(ret), K(rowkey));
    } else if (OB_FAIL(old_row_getter.get_next_row(storage_old_row))) {
      if (OB_ITER_END == ret) {
        ret = OB_ERR_DEFENSIVE_CHECK;
        LOG_WARN("old row in storage is not exists", K(ret));
      } else {
        LOG_WARN("get next row from old_row_iter failed", K(ret), KPC(run_ctx.column_ids_), K(old_row));
      }
    } else if (OB_ISNULL(storage_old_row)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected error, storage old row is NULL", K(ret));
    } else if (storage_old_row->get_count() != old_row.get_count()) {
      ret = OB_ERR_DEFENSIVE_CHECK;
      LOG_WARN("storage old row is not matched with sql old row", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < old_row.get_count(); ++i) {
        const ObObj &storage_val = storage_old_row->get_cell(i);
        const ObObj &sql_val = old_row.get_cell(i);
        int cmp = 0;
        if (OB_UNLIKELY(ObLongTextType == storage_val.get_type() && sql_val.is_lob_locator())) {
          //skip check lob column type when do the normal sql write check
        } else if (OB_UNLIKELY(storage_val.is_nop_value())) {
          bool is_nop = false;
          if (OB_FAIL(data_table.is_nop_default_value(column_ids.at(i), is_nop))) {
            LOG_WARN("check column whether has nop default value failed", K(ret), K(column_ids.at(i)));
          } else if (!is_nop) {
            ret = OB_ERR_DEFENSIVE_CHECK;
            LOG_WARN("storage old row is not matched with sql old row", K(ret),
                    K(i), K(column_ids.at(i)), K(storage_val), K(sql_val));
          }
        } else if (sql_val.is_nop_value()) {
          //this column is nop val, means that this column does not be touched by DML
          //just ignore it
        } else if (OB_FAIL(storage_val.compare(sql_val, cmp)) || 0 != cmp) {
          LOG_WARN("storage_val is not equal with sql_val, maybe catch a bug", K(ret),
                  K(storage_val), K(sql_val), K(cmp), K(column_ids.at(i)));
          ret = OB_ERR_DEFENSIVE_CHECK;
        }
      }
    }

    if (OB_ERR_DEFENSIVE_CHECK == ret && dml_param.is_batch_stmt_) {
      // 批量删除的时候可能索引表的删除在主表前边，所以所有的表在batch删除的时候出现4377，都可能是重复删导致的
      ret = OB_BATCHED_MULTI_STMT_ROLLBACK;
      LOG_TRACE("batch stmt execution has a correctness error, needs rollback", K(ret),
                "column_id", column_ids,
                KPC(storage_old_row),
                "sql_old_row", old_row,
                "dml_param", run_ctx.dml_param_,
                "dml_type", run_ctx.dml_flag_);
    }
    if (OB_ERR_DEFENSIVE_CHECK == ret) {
      if (is_udf) {
        ret = OB_ERR_INDEX_KEY_NOT_FOUND;
        LOG_WARN("index key not found on udf column", K(ret), K(old_row));
      } else {
        ObString func_name = ObString::make_string("check_old_row_legitimacy");
        LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
        LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
            "column_id", column_ids,
            KPC(storage_old_row),
            "sql_old_row", old_row,
            "dml_param", run_ctx.dml_param_,
            "dml_flag", run_ctx.dml_flag_,
            "store_ctx", run_ctx.store_ctx_,
            "relative_table", run_ctx.relative_table_);
        LOG_ERROR("Dump data table info", K(ret), K(data_table));
        run_ctx.store_ctx_.force_print_trace_log();
      }
    }
  }
  return ret;
}

int ObLSTabletService::check_new_row_legitimacy(
    ObDMLRunningCtx &run_ctx,
    const common::ObNewRow &new_row)
{
  int ret = OB_SUCCESS;
  ObRelativeTable &data_table = run_ctx.relative_table_;
  int64_t data_table_cnt = data_table.get_column_count();
  if (OB_ISNULL(run_ctx.column_ids_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("column ids is nullptr", K(ret));
  } else if (OB_FAIL(check_new_row_nullable_value(*run_ctx.column_ids_, data_table, new_row))) {
    LOG_WARN("check new row nullable value failed", K(ret),
             "dml_param", run_ctx.dml_param_,
             "dml_type", run_ctx.dml_flag_);
  } else if (OB_FAIL(check_new_row_shadow_pk(*run_ctx.column_ids_, data_table, new_row))) {
    LOG_WARN("check new row nullable value failed", K(ret),
             "dml_param", run_ctx.dml_param_,
             "dml_type", run_ctx.dml_flag_);
  }
  return ret;
}

int ObLSTabletService::insert_rows_to_tablet(
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx,
    const ObNewRow * const rows,
    const int64_t row_count,
    ObRowsInfo &rows_info,
    ObStoreRow *tbl_rows,
    int64_t &afct_num,
    int64_t &dup_num)
{
  int ret = OB_SUCCESS;
  ObStoreCtx &ctx = run_ctx.store_ctx_;
  const ObDMLBaseParam &dml_param = run_ctx.dml_param_;
  ObRelativeTable &data_table = run_ctx.relative_table_;
  if (OB_FAIL(ret)) {
  } else if (ObTimeUtility::current_time() > dml_param.timeout_) {
    ret = OB_TIMEOUT;
    int64_t cur_time = ObTimeUtility::current_time();
    LOG_WARN("query timeout", K(cur_time), K(dml_param), K(ret));
  } else if (OB_FAIL(construct_table_rows(rows, tbl_rows, row_count))) {
    LOG_WARN("fail to construct table rows", K(ret));
  } else if (OB_FAIL(rows_info.check_duplicate(tbl_rows, row_count, data_table))) {
    if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
      char rowkey_buffer[OB_TMP_BUF_SIZE_256];
      if (OB_SUCCESS != extract_rowkey(data_table,
                                       rows_info.get_duplicate_rowkey(),
                                       rowkey_buffer,
                                       OB_TMP_BUF_SIZE_256,
                                       run_ctx.dml_param_.tz_info_)) {
        LOG_WARN("extract rowkey failed");
      } else {
        ObString index_name = "PRIMARY";
        if (data_table.is_index_table()) {
          data_table.get_index_name(index_name);
        }
        LOG_USER_ERROR(OB_ERR_PRIMARY_KEY_DUPLICATE,
                       rowkey_buffer, index_name.length(), index_name.ptr());
      }
    } else {
      LOG_WARN("fail to check duplicate", K(ret));
    }
  } else if (OB_FAIL(insert_lob_tablet_rows(tablet_handle, run_ctx, tbl_rows, row_count))) {
    LOG_WARN("failed to insert rows to lob tablet", K(ret));
  } else if (OB_FAIL(insert_tablet_rows(tablet_handle, run_ctx, tbl_rows, row_count, rows_info))) {
    LOG_WARN("failed to insert rows to data tablet", K(ret));
  } else {
    afct_num = afct_num + row_count;
  }
  return ret;
}

int ObLSTabletService::insert_tablet_rows(
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx,
    ObStoreRow *rows,
    const int64_t row_count,
    ObRowsInfo &rows_info)
{
  int ret = OB_SUCCESS;
  ObRelativeTable &table = run_ctx.relative_table_;
  bool exists = false;
  const bool check_exists = !table.is_storage_index_table()
                            || table.is_unique_index();
  if (check_exists && OB_FAIL(tablet_handle.get_obj()->rowkeys_exists(
      run_ctx.store_ctx_, table, rows_info, exists))) {
    LOG_WARN("fail to check the existence of rows", K(ret), K(rows_info), K(exists));
  } else if (exists) {
    ret = OB_ERR_PRIMARY_KEY_DUPLICATE;
    LOG_WARN("rowkeys already exist", K(ret), K(table), K(rows_info));
  }

  for (int64_t k = 0; OB_SUCC(ret) && k < row_count; k++) {
    ObStoreRow &tbl_row = rows[k];
    if (GCONF.enable_defensive_check()
          && OB_FAIL(check_new_row_legitimacy(run_ctx, tbl_row.row_val_))) {
        LOG_WARN("check new row legitimacy failed", K(ret), K(tbl_row.row_val_));
    } else if (OB_FAIL(tablet_handle.get_obj()->insert_row_without_rowkey_check(table, run_ctx.store_ctx_,
        *run_ctx.col_descs_, tbl_row))) {
      if (OB_TRY_LOCK_ROW_CONFLICT != ret) {
        LOG_WARN("fail to insert row to data tablet", K(ret), K(tbl_row));
      }
    }
  }

  if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret && !run_ctx.dml_param_.is_ignore_) {
    int tmp_ret = OB_SUCCESS;
    char rowkey_buffer[OB_TMP_BUF_SIZE_256];
    ObString index_name = "PRIMARY";
    if (OB_TMP_FAIL(extract_rowkey(table, rows_info.get_duplicate_rowkey(),
        rowkey_buffer, OB_TMP_BUF_SIZE_256, run_ctx.dml_param_.tz_info_))) {
      LOG_WARN("failed to extract rowkey", K(ret), K(tmp_ret));
    }

    if (table.is_index_table()) {
      // maybe here data table is a global index table
      if (OB_TMP_FAIL(table.get_index_name(index_name))) {
        LOG_WARN("get_index_name failed", K(tmp_ret));
      }
    } else if (lib::is_oracle_mode() && OB_TMP_FAIL(table.get_primary_key_name(index_name))) {
      LOG_WARN("failed to get pk name", K(ret), K(tmp_ret));
    }
    LOG_USER_ERROR(OB_ERR_PRIMARY_KEY_DUPLICATE, rowkey_buffer, index_name.length(), index_name.ptr());
  }
  return ret;
}

int ObLSTabletService::insert_lob_col(
    ObDMLRunningCtx &run_ctx,
    const ObColDesc &column,
    ObObj &obj,
    ObLobAccessParam *del_param,
    ObLobCommon *lob_common)
{
  int ret = OB_SUCCESS;
  uint64_t lob_id;
  ObLobManager *lob_mngr = MTL(ObLobManager*);
  if (OB_ISNULL(lob_mngr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[STORAGE_LOB]failed to get lob manager handle.", K(ret));
  } else if (!column.col_type_.is_lob_v2() || obj.is_nop_value() || obj.is_null()) {
    // do nothing
  } else {
    ObString data = obj.get_string();
    // init lob access param
    ObLobAccessParam lob_param;
    lob_param.tx_desc_ = run_ctx.store_ctx_.mvcc_acc_ctx_.tx_desc_;
    lob_param.snapshot_ = run_ctx.dml_param_.snapshot_;
    lob_param.is_total_quantity_log_ = run_ctx.dml_param_.is_total_quantity_log_;
    if (lob_param.snapshot_.is_none_read()) {
      // NOTE:
      // lob_insert need table_scan, the snapshot already generated in
      // run_ctx.store_ctx, use it as an LS ReadSnapshot
      lob_param.snapshot_.init_ls_read(run_ctx.store_ctx_.ls_id_,
                                       run_ctx.store_ctx_.mvcc_acc_ctx_.snapshot_);
    }
    lob_param.tx_id_ = lob_param.tx_desc_->get_tx_id();
    lob_param.sql_mode_ = run_ctx.dml_param_.sql_mode_;
    lob_param.ls_id_ = run_ctx.store_ctx_.ls_id_;
    lob_param.tablet_id_ = run_ctx.relative_table_.get_tablet_id();
    lob_param.coll_type_ = column.col_type_.get_collation_type();
    lob_param.allocator_ = &run_ctx.lob_allocator_;
    lob_param.lob_common_ = lob_common;
    if (OB_NOT_NULL(del_param)) {
      lob_param.handle_size_ = del_param->handle_size_;
      lob_param.checksum_ = del_param->checksum_;
      lob_param.total_seq_cnt_ = del_param->total_seq_cnt_;
      lob_param.used_seq_cnt_ = del_param->used_seq_cnt_;
      lob_param.seq_no_st_ = del_param->seq_no_st_;
    }
    lob_param.timeout_ = run_ctx.dml_param_.timeout_;
    lob_param.scan_backward_ = false;
    lob_param.offset_ = 0;
    lob_param.len_ = ObCharset::strlen_char(lob_param.coll_type_, data.ptr(), data.length());

    if (OB_FAIL(lob_mngr->append(lob_param, data))) {
      LOG_WARN("[STORAGE_LOB]lob append failed.", K(ret));
    } else {
      ObLobCommon *res_lob_common = lob_param.lob_common_;
      obj.set_lob_value(obj.get_type(), res_lob_common, res_lob_common->get_handle_size(lob_param.byte_size_));
      LOG_DEBUG("[STORAGE_LOB]write ob lob data.", K(lob_param), KPC(res_lob_common),
                K(res_lob_common->get_handle_size(lob_param.byte_size_)),
                K(column.col_type_.get_collation_type()));
    }
  }
  return ret;
}

int ObLSTabletService::check_lob_tablet_valid(ObTabletHandle &data_tablet)
{
  int ret = OB_SUCCESS;
  bool is_valid_aux_lob_table = false;
  ObTabletBindingInfo ddl_data;
  if (OB_FAIL(data_tablet.get_obj()->get_ddl_data(ddl_data))) {
    LOG_WARN("failed to get ddl data from tablet", K(ret), K(data_tablet));
  } else {
    is_valid_aux_lob_table = ddl_data.lob_meta_tablet_id_.is_valid() && ddl_data.lob_piece_tablet_id_.is_valid();
    if (!is_valid_aux_lob_table) {
      ObTabletTxMultiSourceDataUnit tx_data;
      if (OB_FAIL(data_tablet.get_obj()->get_tx_data(tx_data))) {
        LOG_WARN("fail to get tx data", K(ret), K(data_tablet));
      } else if (tx_data.is_in_tx()) {
        ret = OB_SCHEMA_EAGAIN;
        LOG_WARN("do retry for data tablet is in tx, maybe wait for lob tablet finish", K(ret), K(ddl_data));
      } else {
        // maybe has committed, refresh binding info and check once
        if (OB_FAIL(data_tablet.get_obj()->get_ddl_data(ddl_data))) {
          LOG_WARN("failed to get ddl data from tablet", K(ret), K(data_tablet));
        } else {
          is_valid_aux_lob_table = ddl_data.lob_meta_tablet_id_.is_valid() && ddl_data.lob_piece_tablet_id_.is_valid();
          if (!is_valid_aux_lob_table) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("aux lob table must valid when lob column exist", K(ret), K(ddl_data));
          }
        }
      }
    }
  }
  return ret;
}

int ObLSTabletService::insert_lob_tablet_row(
    ObTabletHandle &data_tablet,
    ObDMLRunningCtx &run_ctx,
    ObStoreRow &row)
{
  int ret = OB_SUCCESS;
  int64_t col_cnt = run_ctx.col_descs_->count();
  ObLobManager *lob_mngr = MTL(ObLobManager*);
  bool check_lob = false;
  if (OB_ISNULL(lob_mngr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[STORAGE_LOB]failed to get lob manager handle.", K(ret));
  } else if (row.row_val_.count_ != col_cnt) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[STORAGE_LOB]column count invalid", K(ret), K(col_cnt), K(row.row_val_.count_));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
      const ObColDesc &column = run_ctx.col_descs_->at(i);
      ObObj &obj = row.row_val_.get_cell(i);
      if (!column.col_type_.is_lob_v2() || obj.is_null() || obj.is_nop_value()) {
        // do nothing  
      } else {
        if (!check_lob) {
          if (OB_FAIL(check_lob_tablet_valid(data_tablet))) {
            LOG_WARN("failed to check_lob_tablet_valid", K(ret), K(data_tablet));
          } else {
            check_lob = true;
          }
        }
        if (OB_FAIL(ret)) {
        } else if(OB_FAIL(insert_lob_col(run_ctx, column, obj, nullptr, nullptr))) {
          LOG_WARN("[STORAGE_LOB]failed to insert lob col.", K(ret), K(row), K(i));
        }
      }
    }
  }
  return ret;
}

int ObLSTabletService::insert_lob_tablet_rows(
    ObTabletHandle &data_tablet,
    ObDMLRunningCtx &run_ctx,
    ObStoreRow *rows,
    int64_t row_count)
{
  int ret = OB_SUCCESS;
  // DEBUG_SYNC(DELAY_INDEX_WRITE);
  ObLobManager *lob_mngr = MTL(ObLobManager*);
  if (OB_ISNULL(lob_mngr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[STORAGE_LOB]failed to get lob manager handle.", K(ret));
  } else {
    int64_t col_cnt = run_ctx.col_descs_->count();
    for (int64_t k = 0; OB_SUCC(ret) && k < row_count; k++) {
      if (OB_FAIL(insert_lob_tablet_row(data_tablet, run_ctx, rows[k]))) {
        LOG_WARN("[STORAGE_LOB]failed to insert lob row.", K(ret));
      }
    }
  }
  return ret;
}

int ObLSTabletService::extract_rowkey(
    const ObRelativeTable &table,
    const blocksstable::ObDatumRowkey &rowkey,
    char *buffer,
    const int64_t buffer_len,
    const ObTimeZoneInfo *tz_info)
{
  int ret = OB_SUCCESS;
  common::ObSEArray<share::schema::ObColDesc, common::OB_MAX_ROWKEY_COLUMN_NUMBER> rowkey_cols;
  ObStoreRowkey store_rowkey;
  ObDatumRowkeyHelper rowkey_helper;
  if (OB_FAIL(table.get_rowkey_column_ids(rowkey_cols))) {
    STORAGE_LOG(WARN, "Failed to get rowkey cols", K(ret), K(table));
  } else if (OB_FAIL(rowkey_helper.convert_store_rowkey(rowkey, rowkey_cols, store_rowkey))) {
    STORAGE_LOG(WARN, "Failed to convert store rowkey", K(ret), K(rowkey));
  } else {
    ret = extract_rowkey(table, store_rowkey, buffer, buffer_len, tz_info);
  }
  return ret;
}

int ObLSTabletService::extract_rowkey(
    const ObRelativeTable &table,
    const common::ObStoreRowkey &rowkey,
    char *buffer,
    const int64_t buffer_len,
    const ObTimeZoneInfo *tz_info)
{
  int ret = OB_SUCCESS;

  if (!table.is_valid() || !rowkey.is_valid() || OB_ISNULL(buffer) || buffer_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table), K(rowkey), K(buffer), K(buffer_len), K(tz_info));
  } else {
    const int64_t rowkey_size = table.get_rowkey_column_num();
    int64_t pos = 0;
    int64_t valid_rowkey_size = 0;
    uint64_t column_id = OB_INVALID_ID;

    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_size; i++) {
      if (OB_FAIL(table.get_rowkey_col_id_by_idx(i, column_id))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Failed to get rowkey column description", K(i), K(ret));
      } else if (column_id <= OB_MIN_SHADOW_COLUMN_ID) {
        valid_rowkey_size ++;
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < valid_rowkey_size; ++i) {
      const ObObj &obj  = rowkey.get_obj_ptr()[i];
      if (OB_FAIL(obj.print_plain_str_literal(buffer, buffer_len - 1, pos, tz_info))) {
        LOG_WARN("fail to print_plain_str_literal", K(ret));
      } else if (i < valid_rowkey_size - 1) {
        if (OB_FAIL(databuff_printf(buffer,  buffer_len - 1, pos, "-"))) {
          LOG_WARN("databuff print failed", K(ret));
        }
      }
    }
    if (buffer != nullptr) {
      buffer[pos++] = '\0';
    }
  }

  return ret;
}

int ObLSTabletService::get_next_rows(
    ObNewRowIterator *row_iter,
    ObNewRow *&rows,
    int64_t &row_count)
{
  row_count = 1;
  return row_iter->get_next_rows(rows, row_count);
}

int ObLSTabletService::construct_update_idx(
    const int64_t schema_rowkey_cnt,
    const share::schema::ColumnMap *col_map,
    const common::ObIArray<uint64_t> &upd_col_ids,
    UpdateIndexArray &update_idx)
{
  int ret = OB_SUCCESS;
  int err = OB_SUCCESS;

  if (OB_ISNULL(col_map) || upd_col_ids.count() <= 0 || update_idx.count() > 0 || schema_rowkey_cnt <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(col_map), K(upd_col_ids), K(upd_col_ids.count()), K(schema_rowkey_cnt));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < upd_col_ids.count(); ++i) {
      int32_t idx = -1;
      const uint64_t &col_id = upd_col_ids.at(i);
      if (OB_SUCCESS != (err = col_map->get(col_id, idx)) || idx < 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("column id doesn't exist", K(ret), K(col_id), K(err));
      } else if (idx < schema_rowkey_cnt) {
        // update_idx should not contain rowkey
      } else if (OB_FAIL(update_idx.push_back(idx))) {
        LOG_WARN("fail to push idx into update_idx", K(ret), K(idx));
      }
    }
    if (OB_SUCC(ret) && update_idx.count() > 1) {
      std::sort(update_idx.begin(), update_idx.end());
    }
  }

  return ret;
}

int ObLSTabletService::check_rowkey_change(
    const ObIArray<uint64_t> &update_ids,
    const ObRelativeTable &relative_table,
    bool &rowkey_change,
    bool &delay_new)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(update_ids.count() <= 0 || !relative_table.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(update_ids), K(ret));
  } else {
    const int64_t count = update_ids.count();
    bool is_rowkey = false;
    rowkey_change = false;
    delay_new = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < count && !rowkey_change; ++i) {
      if (OB_FAIL(relative_table.is_rowkey_column_id(update_ids.at(i), is_rowkey))) {
        LOG_WARN("check is_rowkey fail", K(ret), K(update_ids.at(i)));
      } else {
        rowkey_change = is_rowkey;
      }
    }

    if (OB_FAIL(ret)) {
      // do nothing
    } else if (relative_table.is_unique_index() && !rowkey_change) {
      uint64_t cid = OB_INVALID_ID;
      bool innullable = true;
      for (int64_t j = 0; OB_SUCC(ret) && j < relative_table.get_rowkey_column_num() && !rowkey_change; ++j) {
        if (OB_FAIL(relative_table.get_rowkey_col_id_by_idx(j, cid))) {
          LOG_WARN("get rowkey column id fail", K(ret), K(j));
        } else if (cid > OB_MIN_SHADOW_COLUMN_ID) {
          if (innullable) {
            break; // other_change
          } else {
            cid -= OB_MIN_SHADOW_COLUMN_ID;
            for (int64_t k = 0; OB_SUCC(ret) && k < count; ++k) {
              if (cid == update_ids.at(k)) {
                rowkey_change = true;
                break;
              }
            }
          }
        } else {
          bool is_nullable = false;
          if (OB_FAIL(relative_table.is_column_nullable_for_write(cid, is_nullable))) {
            LOG_WARN("check nullable fail", K(ret), K(cid), K(relative_table));
          } else if (is_nullable) {
            innullable = false;
          }
        }
      }
    }

    if (OB_SUCC(ret) && rowkey_change) {
      delay_new = true;
    }
    LOG_DEBUG("check rowkey change for update", K(rowkey_change), K(delay_new), K(update_ids), K(relative_table));
  }

  return ret;
}

int ObLSTabletService::check_rowkey_value_change(
    const common::ObNewRow &old_row,
    const common::ObNewRow &new_row,
    const int64_t rowkey_len,
    bool &rowkey_change)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(rowkey_len <= 0)
      || OB_UNLIKELY(old_row.count_ < rowkey_len)
      || OB_UNLIKELY(new_row.count_ < rowkey_len)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(old_row), K(new_row),
        K(rowkey_len), K(ret));
  } else {
    rowkey_change = false;
    for (int64_t i = 0; !rowkey_change && i < rowkey_len; ++i) {
      if (old_row.cells_[i] != new_row.cells_[i]) {
        rowkey_change = true;
      }
    }
  }
  LOG_DEBUG("check rowkey value for update", K(rowkey_change), K(old_row), K(new_row));
  return ret;
}

int ObLSTabletService::process_lob_row(
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx,
    const ObIArray<int64_t> &update_idx,
    bool data_tbl_rowkey_change,
    ObStoreRow &old_sql_row,
    ObStoreRow &old_row,
    ObStoreRow &new_row)
{
  int ret = OB_SUCCESS;
  bool check_lob = false;
  if (OB_UNLIKELY(old_row.row_val_.get_count() != new_row.row_val_.get_count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("[STORAGE_LOB]invalid args", K(old_row), K(new_row), K(ret));
  } else if (OB_UNLIKELY(old_row.row_val_.get_count() != run_ctx.col_descs_->count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("[STORAGE_LOB]invalid args", K(old_row), K(new_row), KPC(run_ctx.col_descs_));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < old_row.row_val_.get_count(); ++i) {
      if (run_ctx.col_descs_->at(i).col_type_.is_lob_v2()) {
        ObObj &old_obj = old_row.row_val_.get_cell(i);
        ObObj &old_sql_obj = old_sql_row.row_val_.get_cell(i);
        ObObj &new_obj = new_row.row_val_.get_cell(i);
        bool is_update = false;
        for (int64_t j = 0; !is_update && j < update_idx.count(); ++j) {
          if (update_idx.at(j) == i) {
            is_update = true;
          }
        }
        if (is_update) {
          ObLobCommon *lob_common = nullptr;
          ObLobAccessParam lob_param;
          lob_param.update_len_ = new_obj.get_string_len();
          if (!check_lob) {
            ObTabletBindingInfo ddl_data;
            if (OB_FAIL(check_lob_tablet_valid(tablet_handle))) {
              LOG_WARN("failed to check_lob_tablet_valid", K(ret), K(tablet_handle));
            } else {
              check_lob = true;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(delete_lob_col(run_ctx, run_ctx.col_descs_->at(i), old_obj, old_sql_obj, lob_common, lob_param))) {
            LOG_WARN("[STORAGE_LOB]failed to erase old lob col", K(ret), K(old_sql_row), K(old_row));
          } else if (OB_FAIL(insert_lob_col(run_ctx, run_ctx.col_descs_->at(i), new_obj, &lob_param, lob_common))) {
            LOG_WARN("[STORAGE_LOB]failed to insert new lob col.", K(ret), K(new_row));
          }
        } else {
          if (old_obj.is_null()) {
            new_obj.set_null();
          } else if (old_obj.is_nop_value()) {
            new_obj.set_nop_value();
          } else if (new_obj.is_nop_value() || new_obj.is_null()) {
            // do nothing
          } else {
            ObString val_str = old_obj.get_string();
            ObLobCommon *lob_common = reinterpret_cast<ObLobCommon*>(val_str.ptr());
            if (!lob_common->in_row_ && data_tbl_rowkey_change) {
              if (val_str.length() < ObLobManager::LOB_OUTROW_HEADER_SIZE) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("not enough space for lob header", K(ret), K(val_str));
              } else {
                char *buf = reinterpret_cast<char*>(run_ctx.lob_allocator_.alloc(val_str.length()));
                if (OB_ISNULL(buf)) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                  LOG_WARN("alloc memory failed.", K(ret), K(val_str));
                } else {
                  MEMCPY(buf, val_str.ptr(), val_str.length());
                  lob_common = reinterpret_cast<ObLobCommon*>(buf);
                  ObLobData *lob_data = reinterpret_cast<ObLobData*>(lob_common->buffer_);
                  ObLobDataOutRowCtx *ctx = reinterpret_cast<ObLobDataOutRowCtx*>(lob_data->buffer_);
                  ctx->op_ = ObLobDataOutRowCtx::OpType::EMPTY_SQL;
                  new_obj.set_string(new_obj.get_type(), buf, val_str.length());
                }
              }
            } else {
              new_obj.set_string(new_obj.get_type(), val_str);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObLSTabletService::update_row_to_tablet(
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx,
    const bool rowkey_change,
    const ObIArray<int64_t> &update_idx,
    const bool delay_new,
    const bool lob_update,
    ObStoreRow &old_tbl_row,
    ObStoreRow &new_tbl_row,
    ObRowStore *row_store,
    bool &duplicate)
{
  int ret = OB_SUCCESS;
  const ObDMLBaseParam &dml_param = run_ctx.dml_param_;
  const ObColDescIArray &col_descs = *run_ctx.col_descs_;
  bool data_tbl_rowkey_change = false;
  int64_t data_tbl_rowkey_len = run_ctx.relative_table_.get_rowkey_column_num();
  ObSQLMode sql_mode = dml_param.sql_mode_;
  duplicate = false;
  ObStoreRow old_sql_tbl_row;
  old_sql_tbl_row.row_val_ = old_tbl_row.row_val_;

  if (OB_UNLIKELY(col_descs.count() != old_tbl_row.row_val_.get_count()
      || col_descs.count() != new_tbl_row.row_val_.get_count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(col_descs.count()),
        K(old_tbl_row.row_val_), K(new_tbl_row.row_val_));
  } else if (rowkey_change && OB_FAIL(check_rowkey_value_change(old_tbl_row.row_val_,
                                                                new_tbl_row.row_val_,
                                                                data_tbl_rowkey_len,
                                                                data_tbl_rowkey_change))) {
    LOG_WARN("failed to check data table rowkey change", K(ret),
        K(old_tbl_row), K(new_tbl_row), K(data_tbl_rowkey_len));
  } else if (OB_FAIL(process_old_row(tablet_handle,
                                     run_ctx,
                                     data_tbl_rowkey_change,
                                     lob_update,
                                     old_tbl_row))) {
    if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
      LOG_WARN("fail to process old row", K(ret), K(*run_ctx.col_descs_),
          K(old_tbl_row), K(data_tbl_rowkey_change));
    }
  } else if (OB_FAIL(process_lob_row(tablet_handle,
                                     run_ctx,
                                     update_idx,
                                     data_tbl_rowkey_change,
                                     old_sql_tbl_row,
                                     old_tbl_row,
                                     new_tbl_row))) {
    LOG_WARN("failed to process lob col change", K(ret), K(old_tbl_row), K(new_tbl_row));
  } else if (delay_new && lib::is_oracle_mode()) {
    // if total quantity log is needed, we should cache both new row and old row,
    // and the sequence is new_row1, old_row1, new_row2, old_row2....,
    // if total quantity log isn't needed, just cache new row
    if (OB_ISNULL(row_store)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("row_store is NULL", K(ret));
    } else if (OB_FAIL(row_store->add_row(new_tbl_row.row_val_))) {
      LOG_WARN("failed to store new row", K(new_tbl_row), K(ret));
    } else if (OB_FAIL(row_store->add_row(old_tbl_row.row_val_))) {
      LOG_WARN("failed to store old row", K(old_tbl_row), K(ret));
    } else {
      LOG_DEBUG("add row store for delay new", K(old_tbl_row), K(new_tbl_row));
    }
  } else if (OB_FAIL(process_new_row(tablet_handle,
                                     run_ctx,
                                     update_idx,
                                     old_tbl_row,
                                     new_tbl_row,
                                     data_tbl_rowkey_change))) {
    if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
      LOG_WARN("fail to process new row", K(new_tbl_row), K(ret));
    }
  }

  return ret;
}

int ObLSTabletService::process_old_row(
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx,
    const bool data_tbl_rowkey_change,
    const bool lob_update,
    ObStoreRow &tbl_row)
{
  int ret = OB_SUCCESS;
  ObStoreCtx &store_ctx = run_ctx.store_ctx_;
  ObRelativeTable &relative_table = run_ctx.relative_table_;
  bool is_delete_total_quantity_log = run_ctx.dml_param_.is_total_quantity_log_;

  if (OB_UNLIKELY(!relative_table.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid relative tables", K(ret), K(relative_table));
  } else if (OB_UNLIKELY(!store_ctx.is_valid()
      || nullptr == run_ctx.col_descs_
      || run_ctx.col_descs_->count() <= 0
      || !tbl_row.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(store_ctx), KP(run_ctx.col_descs_), K(tbl_row), K(is_delete_total_quantity_log));
  } else if (OB_FAIL(check_old_row_legitimacy(tablet_handle, run_ctx, tbl_row.row_val_))) {
    if (OB_ERR_DEFENSIVE_CHECK == ret) {
      dump_diag_info_for_old_row_loss(relative_table, store_ctx, tbl_row);
    }
    LOG_WARN("check old row legitimacy failed", K(tbl_row.row_val_));
  } else if (OB_FAIL(process_old_row_lob_col(tablet_handle, run_ctx, tbl_row))){
    LOG_WARN("failed to process old row lob col", K(ret), K(tbl_row));
  } else {
    ObColDescIArray &col_descs = const_cast<ObColDescIArray&>(*run_ctx.col_descs_);
    const uint64_t &table_id = relative_table.get_table_id();
    int64_t rowkey_size = relative_table.get_rowkey_column_num();
    ObStoreRowkey store_rowkey;
    ObDatumRowkey datum_rowkey;
    ObDatumRowkeyHelper rowkey_helper(run_ctx.allocator_);

    if (OB_UNLIKELY(run_ctx.dml_param_.prelock_)) {
      bool locked = false;
      if (OB_FAIL(store_rowkey.assign(tbl_row.row_val_.cells_, rowkey_size))) {
        LOG_WARN("Failed to assign rowkey", K(ret), K(tbl_row), K(rowkey_size));
      } else if (OB_FAIL(rowkey_helper.convert_datum_rowkey(store_rowkey.get_rowkey(), datum_rowkey))) {
        STORAGE_LOG(WARN, "Failed to transfer datum rowkey", K(ret), K(store_rowkey), K(rowkey_size), K(tbl_row));
      } else if (OB_FAIL(check_row_locked_by_myself(tablet_handle, relative_table, store_ctx, datum_rowkey, locked))) {
        LOG_WARN("fail to check row locked", K(ret), K(tbl_row));
      } else if (!locked) {
        ret = OB_ERR_ROW_NOT_LOCKED;
        LOG_DEBUG("row has not been locked", K(ret), K(tbl_row));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (data_tbl_rowkey_change) {
      ObStoreRow del_row(tbl_row);
      del_row.flag_.set_flag(ObDmlFlag::DF_DELETE);
      if (!is_delete_total_quantity_log) {
        if (OB_FAIL(tablet_handle.get_obj()->insert_row_without_rowkey_check(relative_table,
            run_ctx.store_ctx_, col_descs, del_row))) {
          if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
            LOG_WARN("failed to write data tablet row", K(ret), K(del_row));
          }
        }
      } else {
        ObStoreRow new_tbl_row;
        new_tbl_row.flag_.set_flag(ObDmlFlag::DF_DELETE);
        new_tbl_row.row_val_ = tbl_row.row_val_;
        del_row.flag_.set_flag(ObDmlFlag::DF_UPDATE);
        ObSEArray<int64_t, 8> update_idx;
        if (OB_FAIL(tablet_handle.get_obj()->update_row(relative_table,
            run_ctx.store_ctx_, col_descs, update_idx, del_row, new_tbl_row))) {
          if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
            LOG_WARN("failed to write data tablet row", K(ret), K(del_row), K(new_tbl_row));
          }
        }
      }
    } else if (lob_update) {
      // need to lock main table rows that don't need to be deleted
      if (OB_FAIL(store_rowkey.assign(tbl_row.row_val_.cells_, rowkey_size))) {
        LOG_WARN("Failed to assign rowkey", K(ret), K(tbl_row), K(rowkey_size));
      } else if (OB_FAIL(rowkey_helper.convert_datum_rowkey(store_rowkey.get_rowkey(), datum_rowkey))) {
        STORAGE_LOG(WARN, "Failed to transfer datum rowkey", K(ret), K(store_rowkey));
      } else if (OB_FAIL(tablet_handle.get_obj()->lock_row(relative_table, store_ctx, datum_rowkey))) {
        if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
          LOG_WARN("lock row failed", K(ret), K(table_id), K(tbl_row), K(rowkey_size));
        }
      }
      LOG_DEBUG("generate lock node before update lob columns", K(ret), K(table_id), K(tbl_row));
    }
  }
  return ret;
}

int ObLSTabletService::process_new_row(
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx,
    const common::ObIArray<int64_t> &update_idx,
    const ObStoreRow &old_tbl_row,
    const ObStoreRow &new_tbl_row,
    const bool rowkey_change)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(update_idx.count() < 0 || !new_tbl_row.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(update_idx), K(new_tbl_row), K(rowkey_change));
  } else if (GCONF.enable_defensive_check()
      && OB_FAIL(check_new_row_legitimacy(run_ctx, new_tbl_row.row_val_))) {
    LOG_WARN("check new row legitimacy failed", K(ret), K(new_tbl_row.row_val_));
  } else {
    // write full column clog needs to construct update_idx and pass to memtable
    if (OB_FAIL(process_data_table_row(tablet_handle,
                                       run_ctx,
                                       update_idx,
                                       old_tbl_row,
                                       new_tbl_row,
                                       rowkey_change))) {
      if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
        LOG_WARN("fail to process data table row", K(ret),
            K(update_idx), K(old_tbl_row), K(new_tbl_row), K(rowkey_change));
      }
    }
  }
  return ret;
}

int ObLSTabletService::process_data_table_row(
    ObTabletHandle &data_tablet,
    ObDMLRunningCtx &run_ctx,
    const ObIArray<int64_t> &update_idx,
    const ObStoreRow &old_tbl_row,
    const ObStoreRow &new_tbl_row,
    const bool rowkey_change)
{
  int ret = OB_SUCCESS;
  ObStoreCtx &ctx = run_ctx.store_ctx_;
  ObRelativeTable &relative_table = run_ctx.relative_table_;
  bool is_update_total_quantity_log = run_ctx.dml_param_.is_total_quantity_log_;
  const common::ObTimeZoneInfo *tz_info = run_ctx.dml_param_.tz_info_;
  if (OB_UNLIKELY(!ctx.is_valid()
      || !relative_table.is_valid()
      || nullptr == run_ctx.col_descs_
      || run_ctx.col_descs_->count() <= 0
      || update_idx.count() < 0
      || (is_update_total_quantity_log && !old_tbl_row.is_valid())
      || !new_tbl_row.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(ctx),
        KP(run_ctx.col_descs_), K(update_idx), K(old_tbl_row), K(new_tbl_row),
        K(is_update_total_quantity_log), K(rowkey_change));
  } else {
    const ObColDescIArray &col_descs = *run_ctx.col_descs_;
    bool exists = false;
    if (rowkey_change && OB_FAIL(data_tablet.get_obj()->rowkey_exists(
                                  relative_table,
                                  ctx,
                                  new_tbl_row.row_val_,
                                  exists))) {
      LOG_WARN("failed to check rowkey exists", K(ret), K(new_tbl_row));
    } else if (exists) {
      char buffer[OB_TMP_BUF_SIZE_256];
      ObStoreRowkey rowkey;
      ret = OB_ERR_PRIMARY_KEY_DUPLICATE;
      if (OB_SUCCESS != rowkey.assign(new_tbl_row.row_val_.cells_, relative_table.get_rowkey_column_num())) {
        LOG_WARN("Failed to assign rowkey", K(new_tbl_row));
      } else if (OB_SUCCESS != extract_rowkey(relative_table, rowkey, buffer, OB_TMP_BUF_SIZE_256, tz_info)) {
        LOG_WARN("extract rowkey failed", K(rowkey));
      } else {
        ObString index_name = "PRIMARY";
        if (relative_table.is_index_table()) {
          relative_table.get_index_name(index_name);
        }
        LOG_USER_ERROR(OB_ERR_PRIMARY_KEY_DUPLICATE, buffer, index_name.length(), index_name.ptr());
      }
      LOG_WARN("rowkey already exists", K(ret), K(new_tbl_row));
    } else {
      ObStoreRow new_row;
      new_row.flag_.set_flag(rowkey_change ? ObDmlFlag::DF_INSERT : ObDmlFlag::DF_UPDATE);
      new_row.row_val_ = new_tbl_row.row_val_;
      if (is_update_total_quantity_log && !rowkey_change) {
        ObStoreRow old_row;
        old_row.flag_.set_flag(ObDmlFlag::DF_UPDATE);
        old_row.row_val_ = old_tbl_row.row_val_;
        if (OB_FAIL(data_tablet.get_obj()->update_row(relative_table,
            ctx, col_descs, update_idx, old_row, new_row))) {
          if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
            LOG_WARN("failed to update to row", K(ret), K(old_row), K(new_row));
          }
        }
      } else {
        if (OB_FAIL(data_tablet.get_obj()->insert_row_without_rowkey_check(relative_table,
            ctx, col_descs, new_row))) {
          if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
            LOG_WARN("failed to update to row", K(ret), K(new_row));
          }
        }
      }
    }
  }
  return ret;
}

int ObLSTabletService::check_new_row_nullable_value(
    const ObIArray<uint64_t> &column_ids,
    ObRelativeTable &data_table,
    const ObNewRow &new_row)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(column_ids.count() != new_row.get_count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("new row is invalid", K(ret), K(new_row.get_count()), K(column_ids.count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
    uint64_t column_id = column_ids.at(i);
    bool is_nullable = false;
    if (OB_UNLIKELY(is_shadow_column(column_id))) {
      //the shadow pk is generated internally,
      //and the nullable attribute check for it is skipped
    } else if (OB_FAIL(data_table.is_column_nullable_for_write(column_id, is_nullable))) {
      LOG_WARN("check is_column_nullable_for_write failed", K(ret), K(column_id));
    } else if (new_row.get_cell(i).is_null() && !is_nullable) {
      bool is_hidden = false;
      bool is_gen_col = false;
      bool is_nullable_for_read = false;
      if (OB_FAIL(data_table.is_column_nullable_for_read(column_id, is_nullable_for_read))) {
        LOG_WARN("check is nullable for read failed", K(ret));
      } else if (is_nullable_for_read) {
        LOG_TRACE("Catch a defensive nullable error, but this column is not null novalidate",
                  K(column_id), K(column_ids), K(new_row), K(data_table));
      } else if (OB_FAIL(data_table.is_hidden_column(column_id, is_hidden))) {
        LOG_WARN("get is hidden column failed", K(ret), K(column_id));
      } else if (OB_FAIL(data_table.is_gen_column(column_id, is_gen_col))) {
        LOG_WARN("get is gen column failed", K(ret), K(column_id));
      } else if (is_hidden && !is_gen_col) {
        ret = OB_BAD_NULL_ERROR;
        LOG_WARN("Catch a defensive nullable error, "
                 "maybe cause by add column not null default null ONLINE", K(ret),
                 K(column_id), K(column_ids), K(new_row), K(data_table));
      } else {
        ret = OB_ERR_DEFENSIVE_CHECK;
        ObString func_name = ObString::make_string("check_new_row_nullable_value");
        LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
        LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
                  K(column_id), K(column_ids), K(new_row), K(data_table));
      }
    } else if (new_row.get_cell(i).is_number()) {
      number::ObNumber num;
      if (OB_FAIL(new_row.get_cell(i).get_number(num))) {
        LOG_WARN("get number value from object fail", K(ret), K(new_row.get_cell(i)));
      } else if (OB_FAIL(num.sanity_check())) {
        LOG_WARN("sanity check number failed", K(ret), K(new_row.get_cell(i)));
      }
      if (OB_SUCCESS != ret) {
        ret = OB_ERR_DEFENSIVE_CHECK;
        ObString func_name = ObString::make_string("check_new_row_nullable_value");
        LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
        LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
                  K(column_id), K(column_ids), K(new_row), K(data_table));
      }
    }
  }
  return ret;
}

int ObLSTabletService::check_new_row_nullable_value(const ObIArray<ObColDesc> &col_descs,
                                                    ObRelativeTable &relative_table,
                                                    const ObNewRow &new_row)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(col_descs.count() > new_row.get_count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("new row is invalid", K(ret), K(new_row.get_count()), K(col_descs.count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < col_descs.count(); ++i) {
    uint64_t column_id = col_descs.at(i).col_id_;
    bool is_nullable = false;
    if (OB_UNLIKELY(is_shadow_column(column_id))) {
      //the shadow pk is generated internally,
      //and the nullable attribute check for it is skipped
    } else if (OB_FAIL(relative_table.is_column_nullable_for_write(column_id, is_nullable))) {
      LOG_WARN("check is_column_nullable_for_write failed", K(ret), K(column_id));
    } else if (new_row.get_cell(i).is_null() && !is_nullable) {
      bool is_hidden = false;
      bool is_gen_col = false;
      bool is_nullable_for_read = false;
      if (OB_FAIL(relative_table.is_column_nullable_for_read(column_id, is_nullable_for_read))) {
        LOG_WARN("check is nullable for read failed", K(ret));
      } else if (is_nullable_for_read) {
        //this column is not null novalidate, maybe the null column come from the old data
        //so output trace log and ignore it
        LOG_TRACE("Catch a defensive nullable error, but this column is not null novalidate",
                  K(column_id), K(col_descs), K(new_row), K(relative_table));
      } else if (OB_FAIL(relative_table.is_hidden_column(column_id, is_hidden))) {
        LOG_WARN("get is hidden column failed", K(ret), K(column_id));
      } else if (OB_FAIL(relative_table.is_gen_column(column_id, is_gen_col))) {
        LOG_WARN("get is gen column failed", K(ret), K(column_id));
      } else if (is_hidden && !is_gen_col) {
        ret = OB_BAD_NULL_ERROR;
        LOG_WARN("Catch a defensive nullable error, "
                 "maybe cause by add column not null default null ONLINE", K(ret),
                 K(column_id), K(col_descs), K(new_row), K(relative_table));
      } else {
        ret = OB_ERR_DEFENSIVE_CHECK;
        ObString func_name = ObString::make_string("check_new_row_nullable_value");
        LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
        LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
                  K(column_id), K(col_descs), K(new_row), K(relative_table));
      }
    } else if (new_row.get_cell(i).is_number()) {
      number::ObNumber num;
      if (OB_FAIL(new_row.get_cell(i).get_number(num))) {
        LOG_WARN("get number value from object fail", K(ret), K(new_row.get_cell(i)));
      } else if (OB_FAIL(num.sanity_check())) {
        LOG_WARN("sanity check number failed", K(ret), K(new_row.get_cell(i)));
      }
      if (OB_SUCCESS != ret) {
        ret = OB_ERR_DEFENSIVE_CHECK;
        ObString func_name = ObString::make_string("check_new_row_nullable_value");
        LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
        LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
                  K(column_id), K(col_descs), K(new_row), K(relative_table));
      }
    }
  }
  return ret;
}

int ObLSTabletService::check_new_row_shadow_pk(
    const ObIArray<uint64_t> &column_ids,
    ObRelativeTable &data_table,
    const ObNewRow &new_row)
{
  int ret = OB_SUCCESS;
  if (data_table.get_shadow_rowkey_column_num() > 0) {
    //check shadow pk
    int64_t rowkey_cnt = data_table.get_rowkey_column_num();
    int64_t spk_cnt = data_table.get_shadow_rowkey_column_num();
    int64_t index_col_cnt = rowkey_cnt - spk_cnt;
    bool need_spk = false;
    if (OB_UNLIKELY(index_col_cnt <= 0) || OB_UNLIKELY(column_ids.count() < rowkey_cnt)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("index column count is invalid", K(ret),
               K(index_col_cnt), K(rowkey_cnt), K(spk_cnt), K(column_ids.count()));
    } else if (lib::is_mysql_mode()) {
      // mysql兼容：只要unique index key中有null列，则需要填充shadow列
      bool rowkey_has_null = false;
      for (int64_t i = 0; !rowkey_has_null && i < index_col_cnt; i++) {
        rowkey_has_null = new_row.get_cell(i).is_null();
      }
      need_spk = rowkey_has_null;
    } else {
      // oracle兼容：只有unique index key全为null列时，才需要填充shadow列
      bool is_rowkey_all_null = true;
      for (int64_t i = 0; is_rowkey_all_null && i < index_col_cnt; i++) {
        is_rowkey_all_null = new_row.get_cell(i).is_null();
      }
      need_spk = is_rowkey_all_null;
    }
    for (int64_t i = index_col_cnt; OB_SUCC(ret) && i < rowkey_cnt; ++i) {
      uint64_t spk_column_id = column_ids.at(i);
      uint64_t real_pk_id = spk_column_id - OB_MIN_SHADOW_COLUMN_ID;
      const ObObj &spk_value = new_row.get_cell(i);
      int64_t pk_idx = OB_INVALID_INDEX;
      int cmp = 0;
      if (OB_LIKELY(!need_spk)) {
        if (!spk_value.is_null()) {
          ret = OB_ERR_DEFENSIVE_CHECK;
          ObString func_name = ObString::make_string("check_new_row_shadow_pk");
          LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
          LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
                    "column_id", column_ids, K(new_row), K(data_table),
                    K(spk_value), K(i), K(spk_column_id), K(real_pk_id));
        }
      } else if (OB_UNLIKELY(!has_exist_in_array(column_ids, real_pk_id, &pk_idx))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("real pk column not exists in column_ids", K(ret), K(column_ids), K(real_pk_id));
      } else if (OB_FAIL(new_row.get_cell(pk_idx).compare(spk_value, cmp)) || 0 != cmp) {
        ret = OB_ERR_DEFENSIVE_CHECK;
        ObString func_name = ObString::make_string("check_new_row_shadow_pk");
        LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
        LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
                  "column_id", column_ids, K(new_row), K(data_table),
                  K(spk_value), "pk_value", new_row.get_cell(pk_idx),
                  K(pk_idx), K(i), K(spk_column_id), K(real_pk_id));
      }
    }
  }
  return ret;
}

int ObLSTabletService::check_row_locked_by_myself(
    ObTabletHandle &tablet_handle,
    ObRelativeTable &relative_table,
    ObStoreCtx &store_ctx,
    const ObDatumRowkey &rowkey,
    bool &locked)
{
  int ret = OB_SUCCESS;
  ObTablet *tablet = tablet_handle.get_obj();

  if (OB_UNLIKELY(nullptr == tablet
      || !relative_table.is_valid()
      || !store_ctx.is_valid()
      || !rowkey.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(tablet_handle),
        K(relative_table), K(store_ctx), K(rowkey));
  } else {
    ObStorageTableGuard guard(tablet, store_ctx, true);
    if (OB_FAIL(guard.refresh_and_protect_table(relative_table))) {
      LOG_WARN("fail to protect table", K(ret), K(tablet_handle));
    } else {
      memtable::ObMemtable *write_memtable = nullptr;
      const uint64_t table_id = relative_table.get_table_id();
      if (OB_FAIL(tablet->prepare_memtable(relative_table, store_ctx, write_memtable))) {
        LOG_WARN("prepare write memtable fail", K(ret), K(relative_table));
      } else if (OB_FAIL(write_memtable->check_row_locked_by_myself(
          store_ctx, table_id, tablet->get_full_read_info(), rowkey, locked))) {
        LOG_WARN("failed to lock write memtable", K(ret), K(table_id), K(rowkey));
      }
    }
  }

  return ret;
}

int ObLSTabletService::get_conflict_rows(
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx,
    const ObInsertFlag flag,
    const common::ObIArray<uint64_t> &out_col_ids,
    const common::ObNewRow &row,
    common::ObNewRowIterator *&duplicated_rows)
{
  TRANS_LOG(DEBUG, "get conflict rows", K(flag), K(row), K(lbt()));
  int ret = OB_SUCCESS;
  ObRelativeTable &data_table = run_ctx.relative_table_;
  ObArenaAllocator scan_allocator(ObModIds::OB_TABLE_SCAN_ITER);
  ObIAllocator *allocator = &scan_allocator;
  ObTablet *data_tablet = tablet_handle.get_obj();
  ObDatumRowkeyHelper rowkey_helper(scan_allocator);
  ObDatumRowkey datum_rowkey;
  ObStoreRowkey rowkey;
  rowkey.assign(row.cells_, data_table.get_rowkey_column_num());

  if (OB_ISNULL(data_tablet)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet is null", K(ret), K(tablet_handle));
  } else {
    ObSingleRowGetter single_row_getter(*allocator, *data_tablet);
    if (OB_FAIL(init_single_row_getter(single_row_getter, run_ctx, out_col_ids, data_table))) {
      LOG_WARN("failed to init single row getter", K(ret));
    } else if (OB_FAIL(rowkey_helper.convert_datum_rowkey(rowkey.get_rowkey(), datum_rowkey))) {
      STORAGE_LOG(WARN, "Failed to transfer datum rowkey", K(ret), K(rowkey));
    } else if (OB_FAIL(single_get_row(single_row_getter, datum_rowkey, duplicated_rows))) {
      LOG_WARN("failed to single get row", K(ret));
    }
  }

  if (OB_FAIL(ret)) {
    if (nullptr != duplicated_rows) {
      ObQueryIteratorFactory::free_insert_dup_iter(duplicated_rows);
      duplicated_rows = nullptr;
    }
  }

  return ret;
}

int ObLSTabletService::init_single_row_getter(
    ObSingleRowGetter &row_getter,
    ObDMLRunningCtx &run_ctx,
    const ObIArray<uint64_t> &out_col_ids,
    ObRelativeTable &relative_table,
    bool skip_read_lob)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(row_getter.init_dml_access_ctx(run_ctx.store_ctx_, run_ctx.dml_param_, skip_read_lob))) {
    LOG_WARN("init dml access ctx failed", K(ret));
  } else if (OB_FAIL(row_getter.init_dml_access_param(relative_table,
      run_ctx.dml_param_, out_col_ids))) {
    LOG_WARN("init dml access param failed", K(ret));
  }

  return ret;
}

int ObLSTabletService::single_get_row(
    ObSingleRowGetter &row_getter,
    const ObDatumRowkey &rowkey,
    ObNewRowIterator *&duplicated_rows)
{
  int ret = OB_SUCCESS;
  ObNewRow *row = nullptr;

  if (OB_FAIL(row_getter.open(rowkey))) {
    LOG_WARN("init single row getter failed", K(ret));
  } else if (OB_FAIL(row_getter.get_next_row(row))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("get next row from single row getter failed", K(ret));
    } else {
      ret = OB_SUCCESS;
    }
  } else if (NULL == duplicated_rows) {
    ObValueRowIterator *dup_iter = NULL;
    if (OB_ISNULL(dup_iter = ObQueryIteratorFactory::get_insert_dup_iter())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("no memory to alloc ObValueRowIterator", K(ret));
    } else {
      duplicated_rows = dup_iter;
      if (OB_FAIL(dup_iter->init(true))) {
        LOG_WARN("failed to initialize ObValueRowIterator", K(ret));
      }
    }
  }
  if (OB_SUCC(ret) && row != nullptr) {
    ObValueRowIterator *dup_iter = static_cast<ObValueRowIterator*>(duplicated_rows);
    if (OB_FAIL(dup_iter->add_row(*row))) {
      LOG_WARN("failed to store conflict row", K(ret), K(*row));
    } else {
      LOG_DEBUG("get conflict row", KPC(row));
    }
  }

  return ret;
}

int ObLSTabletService::convert_row_to_rowkey(
    ObSingleRowGetter &index_row_getter,
    ObStoreRowkey &rowkey)
{
  int ret = OB_SUCCESS;
  ObNewRow *row = nullptr;
  if (OB_FAIL(index_row_getter.get_next_row(row))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("get next row from index row getter failed", K(ret));
    }
  } else {
    rowkey.assign(row->cells_, row->count_);
  }

  return ret;
}

/* this func is an encapsulation of ObNewRowIterator->get_next_row.
 * 1. need_copy_cells is true, perform a cells copy, but not a deep copy.
 *    memory for store_row.row_val.cells_ has already allocated before
 *    this func is invoked, no need to alloc memory in this func.
 * 2. need_copy_cells is false, just perform an assignment, no any copy behavior,
 */
int ObLSTabletService::get_next_row_from_iter(
    ObNewRowIterator *row_iter,
    ObStoreRow &store_row,
    const bool need_copy_cells)
{
  int ret = OB_SUCCESS;
  ObNewRow *row = nullptr;

  if (OB_ISNULL(row_iter)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(row_iter));
  } else if (OB_FAIL(row_iter->get_next_row(row))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("fail to iterate a row", K(ret), K(row));
    }
  } else {
    if (need_copy_cells) {
      // in this situation, store_row.row_val has already hold mem for cells_,
      // no need to alloc mem here, we copy cells only.
      store_row.row_val_.count_ = row->count_;
      for (int64_t i = 0; i < row->count_; ++i) {
        store_row.row_val_.cells_[i] = row->cells_[i];
      }
    } else {
      store_row.row_val_ = *row;
    }
  }

  return ret;
}

int ObLSTabletService::insert_row_to_tablet(
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx,
    ObStoreRow &tbl_row)
{
  int ret = OB_SUCCESS;
  ObStoreCtx &store_ctx = run_ctx.store_ctx_;
  ObRelativeTable &relative_table = run_ctx.relative_table_;
  const ObColDescIArray &idx_col_descs = run_ctx.idx_col_descs_;

  if (OB_UNLIKELY(!store_ctx.is_valid()
      || !relative_table.is_valid()
      || nullptr == run_ctx.col_descs_
      || run_ctx.col_descs_->count() <= 0
      || !tbl_row.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(store_ctx), KP(run_ctx.col_descs_), K(tbl_row), K(ret));
  } else if (GCONF.enable_defensive_check()
      && OB_FAIL(check_new_row_legitimacy(run_ctx, tbl_row.row_val_))) {
    LOG_WARN("check new row legitimacy failed", K(ret), K(tbl_row.row_val_));
  } else if (OB_FAIL(insert_lob_tablet_row(tablet_handle, run_ctx, tbl_row))) {
    LOG_WARN("failed to write lob tablets rows", K(ret));
  } else {
    const ObColDescIArray &col_descs = *run_ctx.col_descs_;
    if (OB_FAIL(tablet_handle.get_obj()->insert_row_without_rowkey_check(
                                                relative_table,
                                                store_ctx,
                                                col_descs,
                                                tbl_row))) {
      if (OB_TRY_LOCK_ROW_CONFLICT != ret) {
        LOG_WARN("failed to write table row", K(ret),
            "table id", relative_table.get_table_id(),
            K(col_descs), K(tbl_row));
      }
    }
  }

  return ret;
}

int ObLSTabletService::process_old_row_lob_col(
    ObTabletHandle &data_tablet_handle,
    ObDMLRunningCtx &run_ctx,
    ObStoreRow &tbl_row)
{
  int ret = OB_SUCCESS;
  run_ctx.is_old_row_valid_for_lob_ = false;
  bool has_lob_col = false;
  int64_t col_cnt = run_ctx.col_descs_->count();
  if (tbl_row.row_val_.count_ != col_cnt) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[STORAGE_LOB]Invliad row col cnt", K(ret), K(col_cnt), K(tbl_row));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
      const ObColDesc &column = run_ctx.col_descs_->at(i);
      if (column.col_type_.is_lob_v2()) {
        has_lob_col = true;
        break;
      }
    }
  }
  if (OB_SUCC(ret) && has_lob_col) {
    if (OB_FAIL(table_refresh_row(data_tablet_handle, run_ctx, tbl_row.row_val_))) {
      LOG_WARN("[STORAGE_LOB]re-read lob col failed", K(ret));
    }

  }
  return ret;
}

int ObLSTabletService::table_refresh_row(
    ObTabletHandle &data_tablet_handle,
    ObDMLRunningCtx &run_ctx,
    ObNewRow &row)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator scan_allocator(ObModIds::OB_LOB_ACCESS_BUFFER);
  ObTablet *data_tablet = data_tablet_handle.get_obj();
  ObRelativeTable &data_table = run_ctx.relative_table_;
  ObStoreRowkey rowkey;
  if (OB_FAIL(rowkey.assign(row.cells_, data_table.get_rowkey_column_num()))) {
    LOG_WARN("get rowkey col num failed", K(ret));
  }

  int64_t col_cnt = run_ctx.col_descs_->count();
  ObSEArray<uint64_t, 8> out_col_ids;
  for (int i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
    if (OB_FAIL(out_col_ids.push_back(run_ctx.col_descs_->at(i).col_id_))) {
      LOG_WARN("push col id failed.", K(ret), K(i));
    }
  }
  ObDatumRowkey datum_rowkey;
  ObDatumRowkeyHelper rowkey_helper(scan_allocator);
  ObSingleRowGetter single_row_getter(scan_allocator, *data_tablet);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(init_single_row_getter(single_row_getter,
      run_ctx, out_col_ids, data_table, true))) {
    LOG_WARN("failed to init single row getter", K(ret));
  } else if (OB_FAIL(rowkey_helper.convert_datum_rowkey(rowkey.get_rowkey(), datum_rowkey))) {
    LOG_WARN("Failed to transfer datum rowkey", K(ret), K(rowkey));
  } else {
    ObNewRow *new_row = nullptr;
    if (OB_FAIL(single_row_getter.open(datum_rowkey))) {
      LOG_WARN("init single row getter failed", K(ret));
    } else if (OB_FAIL(single_row_getter.get_next_row(new_row))) {
      if (ret == OB_ITER_END) {
        LOG_DEBUG("re-read old row not exist", K(ret), K(row));
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("get next row from single row getter failed", K(ret));
      }
    } else if (OB_ISNULL(new_row)) {
      ret = OB_ERR_NULL_VALUE;
      LOG_WARN("get next row from single row null", K(ret));
    } else if (new_row->get_count() != row.get_count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get row from single row col count not equal.", K(ret), K(row.get_count()), K(new_row->get_count()));
    } else {
      LOG_DEBUG("get new row success.", K(row), KPC(new_row));
      if (OB_FAIL(ob_write_row(run_ctx.lob_allocator_, *new_row, row))) {
        LOG_WARN("failed to deep copy new row", K(ret));
      } else {
        run_ctx.is_old_row_valid_for_lob_ = true;
      }
    }
  }
  return ret;
}

int ObLSTabletService::delete_row_in_tablet(
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx,
    const ObNewRow &row)
{
  int ret = OB_SUCCESS;
  const ObDMLBaseParam &dml_param = run_ctx.dml_param_;
  ObStoreCtx &ctx = run_ctx.store_ctx_;
  ObRelativeTable &relative_table = run_ctx.relative_table_;
  ObStoreRow &tbl_row = run_ctx.tbl_row_;
  ObStoreRow new_tbl_row;
  ObSEArray<int64_t, 8> update_idx; // update_idx is a dummy param here
  tbl_row.flag_.set_flag(ObDmlFlag::DF_DELETE);
  tbl_row.row_val_ = row;

  if (OB_FAIL(check_old_row_legitimacy(tablet_handle, run_ctx, row))) {
    if (OB_ERR_DEFENSIVE_CHECK == ret) {
      dump_diag_info_for_old_row_loss(relative_table, ctx, tbl_row);
    }
    LOG_WARN("check old row legitimacy failed", K(row));
  } else if (OB_FAIL(process_old_row_lob_col(tablet_handle, run_ctx, tbl_row))) {
    LOG_WARN("failed to process old row lob col", K(ret), K(tbl_row));
  } else if (!dml_param.is_total_quantity_log_) {
    if (OB_FAIL(tablet_handle.get_obj()->insert_row_without_rowkey_check(relative_table,
        ctx, *run_ctx.col_descs_, tbl_row))) {
      if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
        LOG_WARN("failed to set row", K(ret), K(*run_ctx.col_descs_), K(tbl_row));
      }
    }
  } else if (OB_FAIL(delete_lob_tablet_rows(run_ctx, tablet_handle, tbl_row, row))) {
    LOG_WARN("failed to delete lob rows.", K(ret), K(tbl_row), K(row));
  } else {
    update_idx.reset(); // update_idx is a dummy param here
    new_tbl_row.reset();
    new_tbl_row.flag_.set_flag(ObDmlFlag::DF_DELETE);
    new_tbl_row.row_val_ = tbl_row.row_val_;
    tbl_row.flag_.set_flag(ObDmlFlag::DF_UPDATE);
    if (OB_FAIL(tablet_handle.get_obj()->update_row(relative_table, ctx,
        *run_ctx.col_descs_, update_idx, tbl_row, new_tbl_row))) {
      if (OB_TRY_LOCK_ROW_CONFLICT != ret && OB_TRANSACTION_SET_VIOLATION != ret) {
        LOG_WARN("failed to set row", K(ret), K(*run_ctx.col_descs_), K(tbl_row), K(new_tbl_row));
      }
    } else {
      LOG_DEBUG("succeeded to del main table row", K(tbl_row), K(new_tbl_row));
    }
  }

  return ret;
}

int ObLSTabletService::delete_lob_col(
    ObDMLRunningCtx &run_ctx,
    const ObColDesc &column,
    ObObj &obj,
    const ObObj &sql_obj,
    ObLobCommon *&lob_common,
    ObLobAccessParam &lob_param)
{
  int ret = OB_SUCCESS;
  ObLobManager *lob_mngr = MTL(ObLobManager*);
  if (OB_ISNULL(lob_mngr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[STORAGE_LOB]get lob manager instance failed.", K(ret));;
  } else if (!column.col_type_.is_lob_v2() || obj.is_nop_value() || obj.is_null() ||
             !run_ctx.is_old_row_valid_for_lob_) {
    // do nothing
  } else {
    ObString data = obj.get_string();
    ObString sql_data = sql_obj.get_string();
    if (data.length() < sizeof(ObLobCommon)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("[STORAGE_LOB]Invalid Lob data.", K(ret), K(obj), K(data));
    } else {
      void *buf = run_ctx.lob_allocator_.alloc(data.length());
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to deep copy lob data.", K(ret), K(data));
      } else {
        MEMCPY(buf, data.ptr(), data.length());
        lob_common = reinterpret_cast<ObLobCommon*>(buf);
        lob_param.tx_desc_ = run_ctx.store_ctx_.mvcc_acc_ctx_.tx_desc_;
        lob_param.snapshot_ = run_ctx.dml_param_.snapshot_;
        lob_param.tx_id_ = lob_param.tx_desc_->get_tx_id();
        lob_param.sql_mode_ = run_ctx.dml_param_.sql_mode_;
        lob_param.is_total_quantity_log_ = run_ctx.dml_param_.is_total_quantity_log_;
        lob_param.ls_id_ = run_ctx.store_ctx_.ls_id_;
        lob_param.tablet_id_ = run_ctx.relative_table_.get_tablet_id();
        lob_param.coll_type_ = column.col_type_.get_collation_type();
        lob_param.allocator_ = &run_ctx.lob_allocator_;
        lob_param.lob_common_ = lob_common;
        lob_param.handle_size_ = data.length();
        lob_param.byte_size_ = lob_param.lob_common_->get_byte_size(data.length());
        lob_param.timeout_ = run_ctx.dml_param_.timeout_;
        lob_param.scan_backward_ = false;
        lob_param.offset_ = 0;
        lob_param.len_ = ObCharset::strlen_char(lob_param.coll_type_, sql_data.ptr(), sql_data.length());
        if (lob_param.byte_size_ < 0) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("calc byte size is negative.", K(ret), K(data), K(lob_param));
        } else if (OB_FAIL(lob_mngr->erase(lob_param))) {
          LOG_WARN("[STORAGE_LOB]lob erase failed.", K(ret), K(lob_param));
        }
      }
    }
  }
  return ret;
}

int ObLSTabletService::delete_lob_tablet_rows(
    ObDMLRunningCtx &run_ctx,
    ObTabletHandle &data_tablet,
    ObStoreRow &tbl_row,
    const ObNewRow &row)
{
  int ret = OB_SUCCESS;
  int64_t col_cnt = run_ctx.col_descs_->count();
  if (tbl_row.row_val_.count_ != col_cnt) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[STORAGE_LOB]Invliad row col cnt", K(col_cnt), K(tbl_row));
  } else {
    ObLobCommon *lob_common = nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
      const ObColDesc &column = run_ctx.col_descs_->at(i);
      ObObj &obj = tbl_row.row_val_.get_cell(i);
      const ObObj &sql_obj = row.get_cell(i);
      ObLobAccessParam lob_param;
      if (OB_FAIL(delete_lob_col(run_ctx, column, obj, sql_obj, lob_common, lob_param))) {
        LOG_WARN("[STORAGE_LOB]failed to erase lob col.", K(ret), K(i), K(tbl_row));
      }
    }
  }
  return ret;
}

int ObLSTabletService::prepare_scan_table_param(
    ObTableScanParam &param,
    share::schema::ObMultiVersionSchemaService &schema_service)
{
  int ret =  OB_SUCCESS;
  if (NULL == param.table_param_ || OB_INVALID_ID == param.table_param_->get_table_id()) {
    void *buf = NULL;
    ObTableParam *table_param = NULL;
    ObSchemaGetterGuard schema_guard;
    const ObTableSchema *table_schema = NULL;
    const uint64_t tenant_id = MTL_ID();
    const bool check_formal = param.index_id_ > OB_MAX_CORE_TABLE_ID;
    if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id, schema_guard))) {
      LOG_WARN("failed to get schema manager", K(ret), K(tenant_id));
    } else if (check_formal && OB_FAIL(schema_guard.check_formal_guard())) {
      LOG_WARN("Fail to check formal schema, ", K(param.index_id_), K(ret));
    } else  if (OB_FAIL(schema_guard.get_table_schema(tenant_id,
                param.index_id_, table_schema))) {
      LOG_WARN("Fail to get table schema", K(param.index_id_), K(ret));
    } else if (NULL == table_schema) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("table not exist", K(param.index_id_), K(ret));
    } else {
       if (NULL == (buf = param.allocator_->alloc(sizeof(ObTableParam)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("Fail to allocate memory, ", K(ret));
       } else {
         //TODO table param should not generate twice!!!!
         table_param = new (buf) ObTableParam(*param.allocator_);
         if (OB_FAIL(table_param->convert(*table_schema, param.column_ids_))) {
           LOG_WARN("Fail to convert table param, ", K(ret));
         } else {
           param.table_param_ = table_param;
         }
       }
    }
  }
  return ret;
}

void ObLSTabletService::dump_diag_info_for_old_row_loss(
    ObRelativeTable &data_table,
    ObStoreCtx &store_ctx,
    const ObStoreRow &tbl_row)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;
  ObTableAccessParam access_param;
  ObTableAccessContext access_ctx;
  ObSEArray<int32_t, 16> out_col_pros;
  ObStoreRowkey rowkey;
  ObDatumRowkey datum_rowkey;
  ObDatumRowkeyHelper rowkey_helper(allocator);
  const int64_t schema_rowkey_cnt = data_table.get_rowkey_column_num();
  ObTableStoreIterator &table_iter = data_table.tablet_iter_.table_iter_;
  const ObTableReadInfo &full_read_info = data_table.tablet_iter_.tablet_handle_.get_obj()->get_full_read_info();
  ObQueryFlag query_flag(ObQueryFlag::Forward,
      false, /*is daily merge scan*/
      false, /*is read multiple macro block*/
      false, /*sys task scan, read one macro block in single io*/
      false /*is full row scan?*/,
      false,
      false);
  common::ObVersionRange trans_version_rang;
  trans_version_rang.base_version_ = 0;
  trans_version_rang.multi_version_start_ = 0;
  trans_version_rang.snapshot_version_ = store_ctx.mvcc_acc_ctx_.get_snapshot_version();

  for (int64_t i = 0; OB_SUCC(ret) && i < full_read_info.get_request_count(); i++) {
    if (OB_FAIL(out_col_pros.push_back(i))) {
      STORAGE_LOG(WARN, "Failed to push back col project", K(ret), K(i));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(rowkey.assign(tbl_row.row_val_.cells_, schema_rowkey_cnt))) {
    LOG_WARN("Failed to assign rowkey", K(ret));
  } else if (OB_FAIL(rowkey_helper.convert_datum_rowkey(rowkey.get_rowkey(), datum_rowkey))) {
    STORAGE_LOG(WARN, "Failed to transfer datum rowkey", K(ret), K(rowkey));
  } else if (OB_FAIL(access_ctx.init(query_flag, store_ctx, allocator, trans_version_rang))) {
    LOG_WARN("Fail to init access ctx", K(ret));
  } else {
    access_param.is_inited_ = true;
    access_param.iter_param_.table_id_ = data_table.get_table_id();
    access_param.iter_param_.tablet_id_ = data_table.tablet_iter_.tablet_handle_.get_obj()->get_tablet_meta().tablet_id_;
    access_param.iter_param_.read_info_ = &full_read_info;
    access_param.iter_param_.full_read_info_ = &full_read_info;
    access_param.iter_param_.out_cols_project_ = &out_col_pros;;
    access_param.iter_param_.tablet_handle_ = data_table.tablet_iter_.tablet_handle_;

    ObStoreRowIterator *getter = nullptr;
    ObITable *table = nullptr;
    const ObDatumRow *row = nullptr;

    FLOG_INFO("Try to find the specified rowkey within all the sstable", K(tbl_row), K(table_iter));
    FLOG_INFO("Prepare the diag env to dump the rows", K(store_ctx), K(rowkey), K(datum_rowkey),
        K(access_ctx.trans_version_range_));

    table_iter.resume();
    while (OB_SUCC(ret)) {
      if (OB_FAIL(table_iter.get_next(table))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("failed to get next tables", K(ret));
        }
      } else if (OB_ISNULL(table)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table must not be null", K(ret), K(table_iter));
      } else if (OB_FAIL(table->get(access_param.iter_param_, access_ctx, datum_rowkey, getter))) {
        LOG_WARN("Failed to get param", K(ret), KPC(table));
      } else if (OB_FAIL(getter->get_next_row(row))) {
        LOG_WARN("Failed to get next row", K(ret), KPC(table));
      } else if (row->row_flag_.is_not_exist() || row->row_flag_.is_delete()){
        FLOG_INFO("Cannot found rowkey in the table", KPC(row), KPC(table));
      } else if (table->is_sstable()) {
        FLOG_INFO("Found rowkey in the sstable",
            KPC(row), KPC(reinterpret_cast<ObSSTable*>(table)));
      } else {
        FLOG_INFO("Found rowkey in the memtable",
            KPC(row), KPC(reinterpret_cast<memtable::ObMemtable*>(table)));
      }

      // ignore error in the loop
      if (OB_FAIL(ret) && OB_ITER_END != ret) {
        ret = OB_SUCCESS;
      }
      if (OB_NOT_NULL(getter)) {
        getter->~ObStoreRowIterator();
        getter = nullptr;
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }

    if (OB_SUCC(ret)) {
      FLOG_INFO("prepare to use single merge to find row", K(rowkey), K(datum_rowkey), K(access_param));
      ObSingleMerge *get_merge = nullptr;
      ObGetTableParam get_table_param;
      ObDatumRow *row = nullptr;
      get_table_param.tablet_iter_ = data_table.tablet_iter_;
      void *buf = nullptr;
      if (OB_ISNULL(buf = allocator.alloc(sizeof(ObSingleMerge)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("Failed to alloc memory for single merge", K(ret));
      } else if (FALSE_IT(get_merge = new(buf)ObSingleMerge())) {
      } else if (OB_FAIL(get_merge->init(access_param, access_ctx, get_table_param))) {
        LOG_WARN("Failed to init single get merge", K(ret));
      } else if (OB_FAIL(get_merge->open(datum_rowkey))) {
        LOG_WARN("Failed to open single merge", K(ret));
      } else if (FALSE_IT(get_merge->disable_fill_default())) {
      } else {
        while (OB_SUCC(get_merge->get_next_row(row))) {
          FLOG_INFO("Found one row for the rowkey", KPC(row));
        }
        FLOG_INFO("Finish to find rowkey with single merge", K(ret), K(rowkey), K(datum_rowkey));
      }
      if (OB_NOT_NULL(get_merge)) {
        get_merge->~ObSingleMerge();
        get_merge = nullptr;
      }
    }
#ifdef ENABLE_DEBUG_LOG
    // print single row check info
    if (store_ctx.mvcc_acc_ctx_.tx_id_.is_valid()) {
      transaction::ObTransService *trx = MTL(transaction::ObTransService *);
      if (OB_NOT_NULL(trx)
          && NULL != trx->get_defensive_check_mgr()) {
        (void)trx->get_defensive_check_mgr()->dump(store_ctx.mvcc_acc_ctx_.tx_id_);
      }
    }
#endif
  }
}

int ObLSTabletService::prepare_dml_running_ctx(
    const common::ObIArray<uint64_t> *column_ids,
    const common::ObIArray<uint64_t> *upd_col_ids,
    ObTabletHandle &tablet_handle,
    ObDMLRunningCtx &run_ctx)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(run_ctx.init(
      column_ids,
      upd_col_ids,
      MTL(ObTenantSchemaService*)->get_schema_service(),
      tablet_handle))) {
    LOG_WARN("failed to init run ctx", K(ret));
  }

  return ret;
}

int ObLSTabletService::get_ls_min_end_log_ts_in_old_tablets(int64_t &end_log_ts)
{
  int ret = OB_SUCCESS;
  const ObLSID &ls_id = ls_->get_ls_id();
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(t3m->get_min_end_log_ts_for_ls(ls_id, end_log_ts))) {
    LOG_WARN("fail to get ls min end log ts in all of old tablets", K(ret), K(ls_id));
  }
  return ret;
}

int ObLSTabletService::get_multi_ranges_cost(
    const ObTabletID &tablet_id,
    const common::ObIArray<common::ObStoreRange> &ranges,
    int64_t &total_size)
{
  int ret = OB_SUCCESS;
  ObTabletTableIterator iter;
  const int64_t max_snapshot_version = INT64_MAX;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_FAIL(get_read_tables(tablet_id, max_snapshot_version, iter))) {
    LOG_WARN("fail to get all read tables", K(ret), K(tablet_id), K(max_snapshot_version));
  } else {
    ObPartitionMultiRangeSpliter spliter;
    if (OB_FAIL(spliter.get_multi_range_size(
        ranges,
        iter.tablet_handle_.get_obj()->get_index_read_info(),
        iter.table_iter_,
        total_size))) {
      LOG_WARN("fail to get multi ranges cost", K(ret), K(ranges));
    }
  }
  return ret;
}

int ObLSTabletService::split_multi_ranges(
    const ObTabletID &tablet_id,
    const ObIArray<ObStoreRange> &ranges,
    const int64_t expected_task_count,
    common::ObIAllocator &allocator,
    ObArrayArray<ObStoreRange> &multi_range_split_array)
{
  int ret = OB_SUCCESS;
  ObTabletTableIterator iter;
  const int64_t max_snapshot_version = INT64_MAX;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_FAIL(get_read_tables(tablet_id, max_snapshot_version, iter))) {
    LOG_WARN("fail to get all read tables", K(ret), K(tablet_id), K(max_snapshot_version));
  } else {
    ObPartitionMultiRangeSpliter spliter;
    if (OB_FAIL(spliter.get_split_multi_ranges(
        ranges,
        expected_task_count,
        iter.tablet_handle_.get_obj()->get_index_read_info(),
        iter.table_iter_,
        allocator,
        multi_range_split_array))) {
      LOG_WARN("fail to get splitted ranges", K(ret), K(ranges), K(expected_task_count));
    }
  }
  return ret;
}

int ObLSTabletService::estimate_row_count(
    const ObTableScanParam &param,
    const ObTableScanRange &scan_range,
    ObIArray<ObEstRowCountRecord> &est_records,
    int64_t &logical_row_count,
    int64_t &physical_row_count)
{
  int ret = OB_SUCCESS;
  ObPartitionEst batch_est;
  ObTabletTableIterator tablet_iter;
  common::ObSEArray<ObITable*, 4> tables;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!param.is_estimate_valid() || !scan_range.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(param), K(scan_range));
  } else if (scan_range.is_empty()) {
  } else {
    const int64_t snapshot_version = -1 == param.frozen_version_ ?
        GET_BATCH_ROWS_READ_SNAPSHOT_VERSION : param.frozen_version_;
    if (OB_FAIL(get_read_tables(param.tablet_id_, snapshot_version, tablet_iter, false))) {
      LOG_WARN("failed to get tablet_iter", K(ret), K(snapshot_version), K(param));
    } else {
      while(OB_SUCC(ret)) {
        ObITable *table = nullptr;
        if (OB_FAIL(tablet_iter.table_iter_.get_next(table))) {
          if (OB_ITER_END != ret) {
            LOG_WARN("failed to get next table", K(ret), K(tablet_iter.table_iter_));
          } else {
            ret = OB_SUCCESS;
            break;
          }
        } else if (OB_ISNULL(table)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("table shoud not be null", K(ret), K(tablet_iter.table_iter_));
        } else if (table->is_sstable() && static_cast<ObSSTable*>(table)->get_meta().is_empty()) {
          LOG_DEBUG("cur sstable is empty", K(ret), K(*table));
          continue;
        } else if (OB_FAIL(tables.push_back(table))) {
          LOG_WARN("failed to push back table", K(ret), K(tables));
        }
      }
    }
    if (OB_SUCC(ret) && tables.count() > 0) {
      ObTableEstimateBaseInput base_input(param.scan_flag_, param.index_id_, tables, tablet_iter.tablet_handle_);
      if (scan_range.is_get()) {
        if (OB_FAIL(ObTableEstimator::estimate_row_count_for_get(base_input, scan_range.get_rowkeys(), batch_est))) {
          LOG_WARN("failed to estimate row count", K(ret), K(param), K(scan_range));
        }
      } else if (OB_FAIL(ObTableEstimator::estimate_row_count_for_scan(base_input, scan_range.get_ranges(), batch_est, est_records))) {
        LOG_WARN("failed to estimate row count", K(ret), K(param), K(scan_range));
      }
    }
  }
  if (OB_SUCC(ret)) {
    logical_row_count = batch_est.logical_row_count_;
    physical_row_count = batch_est.physical_row_count_;
  }
  LOG_DEBUG("estimate result", K(ret), K(batch_est), K(est_records));
  return ret;
}

int ObLSTabletService::estimate_block_count(
    const common::ObTabletID &tablet_id,
    int64_t &macro_block_count,
    int64_t &micro_block_count)
{
  int ret = OB_SUCCESS;
  macro_block_count = 0;
  micro_block_count = 0;
  ObTabletTableIterator tablet_iter;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(get_read_tables(tablet_id, INT64_MAX, tablet_iter, false/*allow_no_ready_read*/))) {
    LOG_WARN("failed to get read tables", K(ret));
  }

  ObITable *table = nullptr;
  ObSSTable *sstable = nullptr;
  int64_t total_sample_table_cnt = 2;
  int64_t sample_table_cnt = 0;
  int64_t sampled_table_row_cnt = 0;
  int64_t total_row_count = 0;

  while (OB_SUCC(ret)) {
    if (OB_FAIL(tablet_iter.table_iter_.get_next(table))) {
      if (OB_ITER_END != ret) {
        LOG_WARN("failed to get next tables", K(ret));
      } else {
        ret = OB_SUCCESS;
        break;
      }
    } else if (OB_ISNULL(table)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null table", K(ret), K(tablet_iter.table_iter_));
    } else if (!table->is_sstable()) {
      break;
    } else {
      sstable = static_cast<ObSSTable *>(table);
      macro_block_count += sstable->get_meta().get_basic_meta().get_data_macro_block_count();
      micro_block_count += sstable->get_meta().get_basic_meta().get_data_micro_block_count();
      total_row_count += sstable->get_meta().get_row_count();
      if (sample_table_cnt++ < total_sample_table_cnt) {
        sampled_table_row_cnt += sstable->get_meta().get_row_count();
      }
    }
  }
  return ret;
}

int ObLSTabletService::get_tx_data_memtable_mgr(ObMemtableMgrHandle &mgr_handle)
{
  mgr_handle.reset();
  return mgr_handle.set_memtable_mgr(&tx_data_memtable_mgr_);
}

int ObLSTabletService::get_tx_ctx_memtable_mgr(ObMemtableMgrHandle &mgr_handle)
{
  mgr_handle.reset();
  return mgr_handle.set_memtable_mgr(&tx_ctx_memtable_mgr_);
}

int ObLSTabletService::get_lock_memtable_mgr(ObMemtableMgrHandle &mgr_handle)
{
  mgr_handle.reset();
  return mgr_handle.set_memtable_mgr(&lock_memtable_mgr_);
}

int ObLSTabletService::get_bf_optimal_prefix(int64_t &prefix)
{
  int ret = OB_SUCCESS;
  prefix = 0;
  return ret;
}

int ObLSTabletService::on_prepare_create_tablets(
    const obrpc::ObBatchCreateTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.prepare_create_tablets(arg, trans_flags))) {
      LOG_WARN("failed to prepare create tablets", K(ret), K(arg), K(trans_flags));
    }
  }
  return ret;
}

int ObLSTabletService::on_redo_create_tablets(
    const obrpc::ObBatchCreateTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.redo_create_tablets(arg, trans_flags))) {
      LOG_WARN("failed to redo create tablets", K(ret), K(arg), K(trans_flags));
    }
  }
  return ret;
}

int ObLSTabletService::on_commit_create_tablets(
    const obrpc::ObBatchCreateTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    common::ObSArray<common::ObTabletID> tablet_id_array;
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.commit_create_tablets(arg, trans_flags, tablet_id_array))) {
      LOG_WARN("failed to commit create tablets", K(ret), K(trans_flags));
    } else {
      report_tablet_to_rs(tablet_id_array);
    }
  }
  return ret;
}

int ObLSTabletService::on_tx_end_create_tablets(
    const obrpc::ObBatchCreateTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.tx_end_create_tablets(arg, trans_flags))) {
      LOG_WARN("failed to tx end create tablets", K(ret), K(trans_flags));
    }
  }
  return ret;
}

int ObLSTabletService::on_abort_create_tablets(
    const obrpc::ObBatchCreateTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.abort_create_tablets(arg, trans_flags))) {
      LOG_WARN("failed to abort create tablets", K(ret), K(trans_flags));
    }
  }
  return ret;
}

int ObLSTabletService::on_prepare_remove_tablets(
    const obrpc::ObBatchRemoveTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.prepare_remove_tablets(arg, trans_flags))) {
      LOG_WARN("failed to prepare remove tablets", K(ret), K(arg), K(trans_flags));
    }
  }
  return ret;
}

int ObLSTabletService::on_redo_remove_tablets(
    const obrpc::ObBatchRemoveTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.redo_remove_tablets(arg, trans_flags))) {
      LOG_WARN("failed to redo remove tablets", K(ret), K(arg), K(trans_flags));
    }
  }
  return ret;
}

int ObLSTabletService::on_commit_remove_tablets(
    const obrpc::ObBatchRemoveTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.commit_remove_tablets(arg, trans_flags))) {
      LOG_WARN("failed to commit remove tablets", K(ret), K(trans_flags));
    }
  }
  return ret;
}

int ObLSTabletService::on_tx_end_remove_tablets(
    const obrpc::ObBatchRemoveTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.tx_end_remove_tablets(arg, trans_flags))) {
      LOG_WARN("failed to tx end remove tablets", K(ret), K(trans_flags));
    }
  }
  return ret;
}

int ObLSTabletService::on_abort_remove_tablets(
    const obrpc::ObBatchRemoveTabletArg &arg,
    const transaction::ObMulSourceDataNotifyArg &trans_flags)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    ObTabletCreateDeleteHelper helper(*ls_, tablet_id_set_);
    if (OB_FAIL(helper.abort_remove_tablets(arg, trans_flags))) {
      LOG_WARN("failed to abort remove tablets", K(ret), K(trans_flags));
    }
  }
  return ret;
}

int ObLSTabletService::create_ls_inner_tablet(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const int64_t memstore_version,
    const int64_t frozen_timestamp,
    const share::schema::ObTableSchema &table_schema,
    const lib::Worker::CompatMode &compat_mode,
    const int64_t create_scn)
{
  int ret = OB_SUCCESS;
  const int64_t data_version = ObVersion(memstore_version - 1, 0);
  const int64_t snapshot_version = frozen_timestamp;
  const common::ObSArray<common::ObTabletID> empty_index_tablet_array;
  bool b_exist = false;
  ObTabletHandle tablet_handle;
  common::ObTabletID empty_tablet_id;
  ObMetaDiskAddr disk_addr; // no use

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!ls_id.is_valid())
      || OB_UNLIKELY(!tablet_id.is_valid())
      || OB_UNLIKELY(OB_INVALID_VERSION == memstore_version)
      || OB_UNLIKELY(OB_INVALID_TIMESTAMP == frozen_timestamp)
      || OB_UNLIKELY(!table_schema.is_valid())
      || OB_UNLIKELY(lib::Worker::CompatMode::INVALID == compat_mode)
      /*|| OB_UNLIKELY(create_scn <= OB_INVALID_TIMESTAMP)*/) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(ls_id), K(tablet_id), K(frozen_timestamp),
        K(table_schema), K(compat_mode), K(create_scn));
  } else if (OB_UNLIKELY(ls_id != ls_->get_ls_id())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls id is unexpected", K(ret), "arg_ls_id", ls_id, "ls_id", ls_->get_ls_id());
  } else if (OB_UNLIKELY(!tablet_id.is_ls_inner_tablet())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet id is not ls inner tablet", K(ret), K(tablet_id));
  } else if (OB_FAIL(has_tablet(ls_id, tablet_id, b_exist))) {
    LOG_WARN("failed to check tablet existence", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_UNLIKELY(b_exist)) {
    ret = OB_TABLET_EXIST;
    LOG_WARN("tablet already exists", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(do_create_tablet(ls_id, tablet_id, tablet_id/*data_tablet_id*/, empty_index_tablet_array,
      create_scn, snapshot_version, table_schema, compat_mode, empty_tablet_id, empty_tablet_id, tablet_handle))) {
    LOG_WARN("failed to do create tablet", K(ret), K(ls_id), K(tablet_id),
        K(create_scn), K(snapshot_version), K(table_schema), K(compat_mode));
  } else if (OB_FAIL(ObTabletSlogHelper::write_create_tablet_slog(tablet_handle, disk_addr))) {
    LOG_WARN("failed to write create tablet slog", K(ret), K(tablet_handle));
  } else if (OB_FAIL(tablet_id_set_.set(tablet_id))) {
    LOG_ERROR("fail to set tablet id set", K(ret), K(tablet_id), K(lbt()));
    ob_usleep(1000 * 1000);
    ob_abort();
  }

  return ret;
}

int ObLSTabletService::remove_ls_inner_tablet(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(!ls_id.is_valid())
      || OB_UNLIKELY(!tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_UNLIKELY(ls_id != ls_->get_ls_id())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls id is unexpected", K(ret), "arg_ls_id", ls_id, "ls_id", ls_->get_ls_id());
  } else if (OB_FAIL(do_remove_tablet(ls_id, tablet_id))) {
    LOG_WARN("failed to remove tablet", K(ret), K(ls_id), K(tablet_id));
  }

  return ret;
}

int ObLSTabletService::build_tablet_iter(ObLSTabletIterator &iter)
{
  int ret = common::OB_SUCCESS;
  GetAllTabletIDOperator op(iter.tablet_ids_);
  iter.ls_tablet_service_ = this;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(tablet_id_set_.foreach(op))) {
    STORAGE_LOG(WARN, "fail to get all tablet ids from set", K(ret));
  } else if (OB_UNLIKELY(!iter.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("iter is invalid", K(ret), K(iter));
  }

  if (OB_FAIL(ret)) {
    iter.reset();
  }
  return ret;
}

int ObLSTabletService::build_tablet_iter(ObLSTabletIDIterator &iter)
{
  int ret = common::OB_SUCCESS;
  GetAllTabletIDOperator op(iter.tablet_ids_);
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "not inited", K(ret), K_(is_inited));
  } else if (OB_FAIL(tablet_id_set_.foreach(op))) {
    STORAGE_LOG(WARN, "fail to get all tablet ids from set", K(ret));
  } else if (OB_UNLIKELY(!iter.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("iter is invalid", K(ret), K(iter));
  }

  if (OB_FAIL(ret)) {
    iter.reset();
  }
  return ret;
}


ObLSTabletService::DeleteTabletInfo::DeleteTabletInfo()
  : delete_data_tablet_(false),
    old_data_tablet_handle_(),
    new_data_tablet_handle_(),
    delete_index_tablet_ids_()
{
}

ObLSTabletService::DeleteTabletInfo::DeleteTabletInfo(const ObLSTabletService::DeleteTabletInfo &other)
  : delete_data_tablet_(other.delete_data_tablet_),
    old_data_tablet_handle_(other.old_data_tablet_handle_),
    new_data_tablet_handle_(other.new_data_tablet_handle_),
    delete_index_tablet_ids_(other.delete_index_tablet_ids_)
{
}

ObLSTabletService::DeleteTabletInfo &ObLSTabletService::DeleteTabletInfo::operator=(
    const ObLSTabletService::DeleteTabletInfo &other)
{
  if (this != &other) {
    delete_data_tablet_ = other.delete_data_tablet_;
    old_data_tablet_handle_ = other.old_data_tablet_handle_;
    new_data_tablet_handle_ = other.new_data_tablet_handle_;
    delete_index_tablet_ids_ = other.delete_index_tablet_ids_;
  }
  return *this;
}

ObLSTabletService::HashMapTabletDeleteFunctor::HashMapTabletDeleteFunctor(ObLS *ls)
  : ls_(ls),
    slog_params_()
{
}

ObLSTabletService::HashMapTabletDeleteFunctor::~HashMapTabletDeleteFunctor()
{
  destroy();
}

void ObLSTabletService::HashMapTabletDeleteFunctor::destroy()
{
  for (int64_t i = 0; i < slog_params_.count(); i++) {
    ObIBaseStorageLogEntry *data = slog_params_.at(i).data_;
    if (nullptr != data) {
      OB_DELETE(ObIBaseStorageLogEntry, "", data);
      data = nullptr;
    }
  }
}

bool ObLSTabletService::HashMapTabletDeleteFunctor::operator()(
    const common::ObTabletID &tablet_id,
    ObLSTabletService::DeleteTabletInfo &info)
{
  bool b_ret = true;
  int ret = OB_SUCCESS;

  if (info.delete_data_tablet_
      && OB_FAIL(handle_remove_data_tablet(info.old_data_tablet_handle_, info.delete_index_tablet_ids_))) {
    // delete data tablet and all index tablets
    LOG_WARN("failed to handle remove data tablet", K(ret));
  } else if (!info.delete_data_tablet_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("failed to handle alter index tablet", K(ret));
  }

  if (OB_FAIL(ret)) {
    b_ret = false;
  }

  return b_ret;
}

int ObLSTabletService::HashMapTabletDeleteFunctor::handle_remove_data_tablet(
    ObTabletHandle &data_tablet_handle,
    const common::ObIArray<common::ObTabletID> &delete_index_tablet_ids)
{
  int ret = OB_SUCCESS;
  const int32_t delete_tablet_cmd = ObIRedoModule::gen_cmd(ObRedoLogMainType::OB_REDO_LOG_TENANT_STORAGE,
      ObRedoLogSubType::OB_REDO_LOG_DELETE_TABLET);
  const share::ObLSID &ls_id = data_tablet_handle.get_obj()->get_tablet_meta().ls_id_;
  const common::ObTabletID &data_tablet_id = data_tablet_handle.get_obj()->get_tablet_meta().tablet_id_;

  // delete local index tablet
  for (int64_t i = 0; OB_SUCC(ret) && i < delete_index_tablet_ids.count(); ++i) {
    const common::ObTabletID &index_tablet_id = delete_index_tablet_ids.at(i);
    ObDeleteTabletLog *slog_entry = OB_NEW(ObDeleteTabletLog, "delete tablet", ls_id, index_tablet_id);
    ObStorageLogParam slog_param;
    slog_param.cmd_ = delete_tablet_cmd;
    slog_param.data_ = slog_entry;
    if (OB_FAIL(slog_params_.push_back(slog_param))) {
      LOG_WARN("failed to push back index tablet slog param", K(ret), K(slog_param));
    }
  }

  // delete data tablet
  if (OB_SUCC(ret)) {
    ObStorageLogParam slog_param;
    slog_param.cmd_ = delete_tablet_cmd;
    slog_param.data_ = OB_NEW(ObDeleteTabletLog, "delete tablet", ls_id, data_tablet_id);
    if (OB_FAIL(slog_params_.push_back(slog_param))) {
      LOG_WARN("failed to push back data tablet slog param", K(ret), K(slog_param));
    }
  }

  return ret;
}

bool ObLSTabletService::HashMapTabletGetFunctor::operator()(
    const common::ObTabletID &tablet_id,
    ObLSTabletService::DeleteTabletInfo &info)
{
  bool b_ret = true;
  int ret = OB_SUCCESS;
  common::ObIArray<common::ObTabletID> &index_tablet_ids = info.delete_index_tablet_ids_;

  // always push back data tablet id
  if (OB_FAIL(tablet_id_hash_array_.push_back(tablet_id.hash()))) {
    LOG_WARN("failed to push back data tablet id", K(ret), K(tablet_id));
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < index_tablet_ids.count(); ++i) {
    const common::ObTabletID &index_tablet_id = index_tablet_ids.at(i);
    if (OB_FAIL(tablet_id_hash_array_.push_back(tablet_id.hash()))) {
      LOG_WARN("failed to push back index tablet id", K(ret), K(tablet_id));
    }
  }

  if (OB_FAIL(ret)) {
    b_ret = false;
  }

  return b_ret;
}

int ObLSTabletService::GetAllTabletIDOperator::operator()(const common::ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(tablet_id));
  } else if (OB_FAIL(tablet_ids_.push_back(tablet_id))) {
    LOG_WARN("failed to push back tablet id", K(ret), K(tablet_id));
  }
  return ret;
}

int ObLSTabletService::DestroyMemtableAndMemberOperator::operator()(const common::ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObTabletHandle handle;
  const uint64_t tenant_id = MTL_ID();
  cur_tablet_id_ = tablet_id;
  if (OB_UNLIKELY(!tablet_id.is_valid()) ||
      OB_ISNULL(tablet_svr_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(tablet_id), K(tablet_svr_));
  } else if (OB_FAIL(tablet_svr_->get_tablet(tablet_id,
                                             handle,
                                             ObTabletCommon::NO_CHECK_GET_TABLET_TIMEOUT_US))) {
    if (OB_TABLET_NOT_EXIST == ret) {
      LOG_WARN("failed to get tablet, skip clean memtable", K(ret), K(tablet_id));
      ret = OB_SUCCESS;
    } else {
      LOG_ERROR("failed to get tablet", K(ret), K(tablet_id));
    }
  } else if (OB_FAIL(handle.get_obj()->release_memtables())) {
    LOG_WARN("failed to release memtables", K(tenant_id), K(tablet_id));
  } else if (!tablet_id.is_ls_inner_tablet() && OB_FAIL(handle.get_obj()->destroy_storage_related_member())) {
    LOG_WARN("failed to destroy storage related member", K(ret), K(tenant_id), K(tablet_id));
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
