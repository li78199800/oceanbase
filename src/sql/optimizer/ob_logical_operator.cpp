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

#define USING_LOG_PREFIX SQL_OPT
#include <algorithm>
#include "sql/engine/ob_operator_factory.h"
#include "sql/optimizer/ob_logical_operator.h"
#include "lib/hash_func/murmur_hash.h"
#include "sql/resolver/expr/ob_raw_expr_replacer.h"
#include "sql/rewrite/ob_transform_utils.h"
#include "sql/code_generator/ob_static_engine_cg.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"
#include "ob_log_exchange.h"
#include "ob_log_group_by.h"
#include "ob_log_distinct.h"
#include "ob_log_insert.h"
#include "ob_log_join.h"
#include "ob_log_set.h"
#include "ob_log_sort.h"
#include "ob_log_subplan_scan.h"
#include "ob_log_table_scan.h"
#include "ob_log_limit.h"
#include "ob_log_window_function.h"
#include "ob_log_granule_iterator.h"
#include "ob_log_table_lookup.h"
#include "ob_log_update.h"
#include "ob_log_merge.h"
#include "ob_opt_est_cost.h"
#include "ob_optimizer_util.h"
#include "ob_raw_expr_add_to_context.h"
#include "ob_raw_expr_check_dep.h"
#include "ob_log_count.h"
#include "ob_log_monitoring_dump.h"
#include "ob_log_subplan_filter.h"
#include "ob_log_topk.h"
#include "ob_log_material.h"
#include "ob_log_join_filter.h"
#include "ob_log_temp_table_access.h"
#include "ob_log_temp_table_insert.h"
#include "ob_log_function_table.h"
#include "sql/rewrite/ob_transform_utils.h"
#include "common/ob_smart_call.h"
#include "sql/resolver/expr/ob_raw_expr_printer.h"
#include "ob_log_err_log.h"
#include "ob_log_temp_table_transformation.h"
#include "ob_log_expr_values.h"
#include "sql/optimizer/ob_join_order.h"
#include "sql/optimizer/ob_opt_selectivity.h"
#include "sql/optimizer/ob_log_merge.h"

using namespace oceanbase::sql;
using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::json;
using namespace oceanbase::sql::log_op_def;
using oceanbase::share::schema::ObSchemaGetterGuard;
using oceanbase::share::schema::ObTableSchema;
using oceanbase::share::schema::ObColumnSchemaV2;

int ObExchangeInfo::init_calc_part_id_expr(ObOptimizerContext &opt_ctx)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  ObRawExprFactory &expr_factory = opt_ctx.get_expr_factory();
  int64_t part_expr_cnt = repartition_func_exprs_.count();
  share::schema::ObPartitionLevel part_level = share::schema::PARTITION_LEVEL_ONE;
  ObRawExpr *part_expr = NULL;
  ObRawExpr *subpart_expr = NULL;
  if (part_expr_cnt <= 0 || part_expr_cnt > 2) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid part expr count", K(ret));
  } else {
    if (OB_ISNULL(repartition_func_exprs_.at(0))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("part func expr is null", K(ret));
    } else if (T_OP_ROW == repartition_func_exprs_.at(0)->get_expr_type()) {
      ObOpRawExpr *op_row_expr = NULL;
      if (OB_FAIL(expr_factory.create_raw_expr(T_OP_ROW, op_row_expr))) {
        LOG_WARN("fail to create raw expr", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < repartition_keys_.count(); i ++) {
          if (OB_FAIL(op_row_expr->add_param_expr(repartition_keys_.at(i)))) {
            LOG_WARN("fail to add param expr", K(ret));
          }
        } // for end
        part_expr = op_row_expr;
      }
    } else {
      part_expr = repartition_func_exprs_.at(0);
    }
  }
  if (OB_SUCC(ret) && part_expr_cnt == 2) {
    part_level = share::schema::PARTITION_LEVEL_TWO;
    if (OB_ISNULL(repartition_func_exprs_.at(1))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("part func expr is null", K(ret));
    } else if (T_OP_ROW == repartition_func_exprs_.at(1)->get_expr_type()) {
      ObOpRawExpr *op_row_expr = NULL;
      if (OB_FAIL(expr_factory.create_raw_expr(T_OP_ROW, op_row_expr))) {
        LOG_WARN("fail to create raw expr", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < repartition_sub_keys_.count(); i ++) {
          if (OB_FAIL(op_row_expr->add_param_expr(repartition_sub_keys_.at(i)))) {
            LOG_WARN("fail to add param expr", K(ret));
          }
        } // for end
        subpart_expr = op_row_expr;
      }
    } else {
      subpart_expr = repartition_func_exprs_.at(1);
    }
  }
  if (OB_FAIL(ret)) {
    // do nothing
  } else if (OB_ISNULL(session = opt_ctx.get_session_info())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("session is null", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_calc_tablet_id_expr(expr_factory,
                                                              *session,
                                                              repartition_ref_table_id_,
                                                              part_level,
                                                              part_expr,
                                                              subpart_expr,
                                                              calc_part_id_expr_))) {
    LOG_WARN("fail to init calc part id expr", K(ret));
  } else if (OB_ISNULL(calc_part_id_expr_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("fail to init calc part id expr", K(ret));
  } else if (MayAddIntervalPart::YES == may_add_interval_part_) {
    calc_part_id_expr_->set_may_add_interval_part(may_add_interval_part_);
  }
  return ret;
}

int ObExchangeInfo::append_hash_dist_expr(const common::ObIArray<ObRawExpr *> &exprs)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < exprs.count(); i++) {
    ObRawExpr *raw_expr = NULL;
    if (OB_ISNULL(raw_expr = exprs.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(hash_dist_exprs_.push_back(HashExpr(raw_expr, raw_expr->get_result_type())))) {
      LOG_WARN("failed to push back expr", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObExchangeInfo::assign(ObExchangeInfo &other)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sort_keys_.assign(other.sort_keys_))) {
    LOG_WARN("failed to assign sort keys", K(ret));
  } else if (OB_FAIL(repartition_keys_.assign(other.repartition_keys_))) {
    LOG_WARN("failed to assign exprs", K(ret));
  } else if (OB_FAIL(repartition_sub_keys_.assign(other.repartition_sub_keys_))) {
    LOG_WARN("failed to assign exprs", K(ret));
  } else if (OB_FAIL(repartition_func_exprs_.assign(other.repartition_func_exprs_))) {
    LOG_WARN("failed to assign exprs", K(ret));
  } else if (OB_FAIL(hash_dist_exprs_.assign(other.hash_dist_exprs_))) {
    LOG_WARN("failed to assign exprs", K(ret));
  } else if (OB_FAIL(weak_sharding_.assign(other.weak_sharding_))) {
    LOG_WARN("failed to assign weak sharding", K(ret));
  } else if (OB_FAIL(repart_all_tablet_ids_.assign(other.repart_all_tablet_ids_))) {
    LOG_WARN("failed to assign partition ids", K(ret));
  } else {
    is_remote_ = other.is_remote_;
    is_task_order_ = other.is_task_order_;
    is_merge_sort_ = other.is_merge_sort_;
    is_sort_local_order_ = other.is_sort_local_order_;
    slice_count_ = other.slice_count_;
    repartition_type_ = other.repartition_type_;
    repartition_ref_table_id_ = other.repartition_ref_table_id_;
    repartition_table_id_ = other.repartition_table_id_;
    repartition_table_name_ = other.repartition_table_name_;
    calc_part_id_expr_ = other.calc_part_id_expr_;
    dist_method_ = other.dist_method_;
    unmatch_row_dist_method_ = other.unmatch_row_dist_method_;
    null_row_dist_method_ = other.null_row_dist_method_;
    slave_mapping_type_ = other.slave_mapping_type_;
    strong_sharding_ = other.strong_sharding_;
  }
  return ret;
}


ObPxPipeBlockingCtx::ObPxPipeBlockingCtx(ObIAllocator &alloc) : alloc_(alloc)
{
}

ObPxPipeBlockingCtx::~ObPxPipeBlockingCtx()
{
  FOREACH(it, op_ctxs_) {
    if (NULL != *it) {
      alloc_.free(*it);
      *it = NULL;
    }
  }
}

ObPxPipeBlockingCtx::OpCtx *ObPxPipeBlockingCtx::alloc()
{
  OpCtx *ctx = NULL;
  void *mem = alloc_.alloc(sizeof(OpCtx));
  if (OB_ISNULL(mem)) {
    LOG_WARN("allocate memory failed");
  } else {
    ctx = new(mem)OpCtx();
  }
  return ctx;
}

int ObAllocExprContext::find(const ObRawExpr *expr, ExprProducer *&producer)
{
  int ret = OB_SUCCESS;
  int64_t idx = -1;
  producer = NULL;
  if (expr_producers_.empty()) {
    // do nothing
  } else if (OB_UNLIKELY(expr_producers_.count() != expr_map_.size())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("hash map has invalid size", K(ret));
  } else if (OB_FAIL(expr_map_.get_refactored(reinterpret_cast<uint64_t>(expr),
                                              idx))) {
    if (OB_HASH_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to get expr entry from the map", K(ret));
    }
  } else if (idx < 0 || idx >= expr_producers_.count() ||
             OB_UNLIKELY(expr != expr_producers_.at(idx).expr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("index is invalid", K(ret), K(idx));
  } else {
    producer = &expr_producers_.at(idx);
  }
  return ret;
}

int ObAllocExprContext::add(const ExprProducer &producer)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(expr_producers_.push_back(producer))) {
    LOG_WARN("failed to push back producer", K(ret));
  } else if (expr_producers_.count() == 1 &&
             expr_map_.create(128, "ExprAlloc")) {
    LOG_WARN("failed to init hash map", K(ret));
  } else if (OB_FAIL(expr_map_.set_refactored(reinterpret_cast<uint64_t>(producer.expr_),
                                              expr_producers_.count() - 1))) {
    LOG_WARN("failed to add entry into hash map", K(ret));
  } else if (OB_FAIL(add_flattern_expr(producer.expr_))) {
    LOG_WARN("failed to add flattern expr", K(ret));
  }
  return ret;
}

int ObAllocExprContext::add_flattern_expr(const ObRawExpr* expr)
{
  int ret = OB_SUCCESS;
  int64_t ref_cnt = 0;
  if (OB_FAIL(flattern_expr_map_.get_refactored(reinterpret_cast<uint64_t>(expr),
                                                        ref_cnt)))
  {
    if (OB_HASH_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
      if (OB_FAIL(flattern_expr_map_.set_refactored(reinterpret_cast<uint64_t>(expr),
                                                    1))) {
        LOG_WARN("failed to add entry into hash map", K(ret));
      }
    } else {
      LOG_WARN("failed to get expr entry from the map", K(ret));
    }
  } else if (OB_UNLIKELY(ref_cnt < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ref_cnt is invalid", K(ret), K(ref_cnt));
  } else if (OB_FAIL(flattern_expr_map_.set_refactored(reinterpret_cast<uint64_t>(expr),
                                                       ref_cnt + 1, 1))) {
    LOG_WARN("failed to add entry into hash map", K(ret));
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
    ret = SMART_CALL(add_flattern_expr(expr->get_param_expr(i)));
  }
  return ret;
}

int ObAllocExprContext::get_expr_ref_cnt(const ObRawExpr* expr, int64_t &ref_cnt)
{
  int ret = OB_SUCCESS;
  ref_cnt = 0;
  if (OB_FAIL(flattern_expr_map_.get_refactored(reinterpret_cast<uint64_t>(expr),
                                                ref_cnt)))
  {
    if (OB_HASH_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to get expr entry from the map", K(ret));
    }
  }
  return ret;
}

ObAllocExprContext::~ObAllocExprContext()
{
  expr_map_.destroy();
  flattern_expr_map_.destroy();
}

ObLogicalOperator::ObLogicalOperator(ObLogPlan &plan)
  : child_(),
    type_(LOG_OP_INVALID),
    my_plan_(&plan),
    startup_exprs_(),
    output_const_exprs_(),
    output_equal_sets_(NULL),
    fd_item_set_(NULL),
    table_set_(NULL),
    id_(OB_INVALID_ID),
    branch_id_(OB_INVALID_ID),
    op_id_(OB_INVALID_ID),
    parent_(NULL),
    is_plan_root_(false),
    cost_(0.0),
    op_cost_(0.0),
    card_(0.0),
    width_(0.0),
    traverse_ctx_(NULL),
    exchange_allocated_(false),
    phy_plan_type_(ObPhyPlanType::OB_PHY_PLAN_UNINITIALIZED),
    location_type_(ObPhyPlanType::OB_PHY_PLAN_UNINITIALIZED),
    is_partition_wise_(false),
    px_est_size_factor_(),
    dblink_id_(0),   // 0 represent local cluster.
    plan_depth_(0),
    contain_fake_cte_(false),
    contain_pw_merge_op_(false),
    contain_das_op_(false),
    contain_match_all_fake_cte_(false),
    dup_table_pos_(),
    strong_sharding_(NULL),
    weak_sharding_(),
    is_pipelined_plan_(false),
    is_nl_style_pipelined_plan_(false),
    is_at_most_one_row_(false),
    is_local_order_(false),
    is_range_order_(false),
    op_ordering_(),
    empty_expr_sets_(plan.get_empty_expr_sets()),
    empty_fd_item_set_(plan.get_empty_fd_item_set()),
    empty_table_set_(plan.get_empty_table_set()),
    interesting_order_info_(OrderingFlag::NOT_MATCH),
    parallel_(1),
    server_cnt_(1),
    need_late_materialization_(false),
    op_exprs_()
{
}

ObLogicalOperator::~ObLogicalOperator()
{}

const ObDMLStmt* ObLogicalOperator::get_stmt() const
{
  const ObDMLStmt *ret = (get_plan() != NULL ? get_plan()->get_stmt() : NULL);
  return ret;
}

void ObLogicalOperator::set_child(int64_t child_num,
                                  ObLogicalOperator *child_op)
{
  int ret = OB_SUCCESS;
  for (; OB_SUCC(ret) && child_.count() <= child_num;) {
    if (OB_FAIL(child_.push_back(NULL))) {
      LOG_WARN("failed to enlarge child array", K(ret));
    }
  }
  if (OB_SUCC(ret) && child_.count() > child_num) {
    child_.at(child_num) = child_op;
  }
}

int ObLogicalOperator::get_parent(ObLogicalOperator *root, ObLogicalOperator *&parent)
{
  int ret = OB_SUCCESS;
  parent = NULL;
  if (NULL != root) {
    for (int64_t i = 0;
        OB_SUCC(ret) && parent == NULL && i < root->get_num_of_child(); i++) {
      if (OB_ISNULL(root->get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (root->get_child(i) == this) {
        parent = root;
      } else if (OB_FAIL(get_parent(root->get_child(i), parent))) {
        LOG_WARN("failed to get parent", K(ret));
      } else { /*do nothing*/ }
    }
  }
  return ret;
}

double FilterCompare::get_selectivity(ObRawExpr *expr)
{
  bool found = false;
  double selectivity = 1;
  for (int64_t i = 0; !found && i < predicate_selectivities_.count(); i++) {
    if (predicate_selectivities_.at(i).expr_ == expr) {
      found = true;
      selectivity = predicate_selectivities_.at(i).sel_;
    }
  }
  if (!found) {
    LOG_PRINT_EXPR(WARN, "Failed to get selectivity", expr);
  }
  return selectivity;
}

// Add a child to the end of the array
int ObLogicalOperator::add_child(ObLogicalOperator *child_op)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(child_.push_back(child_op))) {
    LOG_WARN("failed to push back child op", K(ret));
  }
  return ret;
}

int ObLogicalOperator::add_child(const ObIArray<ObLogicalOperator*> &child_ops)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < child_ops.count(); i++) {
    if (OB_ISNULL(child_ops.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(child_.push_back(child_ops.at(i)))) {
      LOG_WARN("failed to push back child ops", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObLogicalOperator::set_op_ordering(const common::ObIArray<OrderItem> &op_ordering)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(op_ordering_.assign(op_ordering))) {
    LOG_WARN("failed to assign ordering", K(ret));
  } else { /*do nothing*/ }
  return ret;
}

int ObLogicalOperator::get_input_equal_sets(EqualSets &input_esets) const
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); ++i) {
    if (OB_ISNULL(child = get_child(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("child is null", K(ret), K(i));
    } else if (OB_FAIL(append(input_esets,
                              child->get_output_equal_sets()))) {
      LOG_WARN("failed to append ordering equal sets", K(ret));
    }
  }
  return ret;
}

int ObLogicalOperator::get_input_const_exprs(ObIArray<ObRawExpr *> &const_exprs) const
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); ++i) {
    if (OB_ISNULL(child = get_child(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("child is null", K(ret), K(i));
    } else if (OB_FAIL(append(const_exprs, child->get_output_const_exprs()))) {
      LOG_WARN("failed to append output const exprs", K(ret));
    }
  }
  return ret;
}

int ObLogicalOperator::compute_equal_set()
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  EqualSets *ordering_esets = NULL;
  if (OB_ISNULL(my_plan_) || OB_UNLIKELY(get_num_of_child() < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("operator is invalid", K(ret), K(get_num_of_child()), K(my_plan_));
  } else if (OB_UNLIKELY(get_num_of_child() == 0)) {
    // do nothing
  } else if (OB_ISNULL(child = get_child(0))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("child is null", K(ret), K(child));
  } else if (filter_exprs_.empty()) {
    // inherit equal sets from the first child directly
    set_output_equal_sets(&child->get_output_equal_sets());
  } else if (OB_ISNULL(ordering_esets = get_plan()->create_equal_sets())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to create equal sets", K(ret));
  } else if (OB_FAIL(ObEqualAnalysis::compute_equal_set(
                       &my_plan_->get_allocator(),
                       filter_exprs_,
                       child->get_output_equal_sets(),
                       *ordering_esets))) {
    LOG_WARN("failed to compute ordering output equal set", K(ret));
  } else {
    set_output_equal_sets(ordering_esets);
  }
  return ret;
}

int ObLogicalOperator::compute_const_exprs()
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  if (OB_ISNULL(my_plan_) || OB_UNLIKELY(get_num_of_child() < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("operator is invalid", K(ret), K(get_num_of_child()), K(my_plan_));
  } else if (OB_UNLIKELY(get_num_of_child() == 0)) {
    /*do nothing*/
  } else if (OB_ISNULL(child = get_child(0))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("child is null", K(ret), K(child));
  } else if (OB_FAIL(append(output_const_exprs_, child->output_const_exprs_))) {
    LOG_WARN("failed to append exprs", K(ret));
  } else if (filter_exprs_.empty()) {
    /*do nothing*/
  } else if (OB_FAIL(ObOptimizerUtil::compute_const_exprs(filter_exprs_, output_const_exprs_))) {
    LOG_WARN("failed to compute const conditionexprs", K(ret));
  } else {/*do nothing*/}
  return ret;
}

int ObLogicalOperator::compute_fd_item_set()
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  if (OB_UNLIKELY(get_num_of_child() == 0)) {
    set_fd_item_set(&empty_fd_item_set_);
  } else if (OB_ISNULL(child = get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("operator does not have child", K(ret));
  } else {
    set_fd_item_set(&child->get_fd_item_set());
  }
  return ret;
}

int ObLogicalOperator::deduce_const_exprs_and_ft_item_set(ObFdItemSet &fd_item_set)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr *, 8> column_exprs;
  if (OB_ISNULL(get_stmt()) || OB_ISNULL(my_plan_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret));
  } else if (OB_FAIL(get_stmt()->get_column_exprs(column_exprs))) {
    LOG_WARN("failed to get column exprs", K(ret));
  } else if (OB_FAIL(my_plan_->get_fd_item_factory().deduce_fd_item_set(
                                                          get_output_equal_sets(),
                                                          column_exprs,
                                                          get_output_const_exprs(),
                                                          fd_item_set))) {
    LOG_WARN("falied to remove const in fd item", K(ret));
  }
  return ret;
}

int ObLogicalOperator::compute_op_ordering()
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  if (OB_UNLIKELY(get_num_of_child() == 0)) {
  } else if (OB_ISNULL(child= get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FAIL(set_op_ordering(child->get_op_ordering()))) {
    LOG_WARN("failed to set op ordering", K(ret));
  } else {
    is_local_order_ = child->get_is_local_order();
    is_range_order_ = child->get_is_range_order();
  }
  return ret;
}

int ObLogicalOperator::compute_op_interesting_order_info()
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  if (get_num_of_child() == 0 || op_ordering_.empty()) {
    set_interesting_order_info(OrderingFlag::NOT_MATCH);
  } else if (OB_ISNULL(child= get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("operator does not have child", K(ret));
  } else {
    set_interesting_order_info(child->get_interesting_order_info());
  }
  return ret;
}

int ObLogicalOperator::compute_op_parallel_and_server_info()
{
  int ret = OB_SUCCESS;
  ObLogPlan *plan = NULL;
  if (OB_ISNULL(plan = get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpect null plan", K(ret));
  } else if (get_num_of_child() == 0) {
    //do nothing
  } else {
    ObOptimizerContext &opt_ctx = plan->get_optimizer_context();
    ObLogicalOperator* child = NULL;
    int64_t parallel = is_single() ? 1 : opt_ctx.get_parallel();
    int64_t server_cnt = 1;
    if (OB_NOT_NULL(child = get_child(first_child))) {
      server_cnt = child->get_server_cnt();
    }
    //选择非EXCHANGE算子继承并行度
    //如果所有的孩子节点都需要shuff数据，那么选择第一个孩子的并行度
    for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); ++i) {
      child = get_child(i);
      if (OB_ISNULL(child)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpect null child", K(ret));
      } else if (log_op_def::LOG_EXCHANGE != child->get_type()) {
        parallel = child->get_parallel();
        server_cnt = child->get_server_cnt();
      }
    }
    //选择合适的并行度后需要refine所有的EXCHANGE-IN算子的并行度
    for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); ++i) {
      child = get_child(i);
      if (OB_ISNULL(child)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpect null child", K(ret));
      } else {
        child->set_parallel(parallel);
        child->set_server_cnt(server_cnt);
      }
    }
    set_parallel(parallel);
    set_server_cnt(server_cnt);
  }
  return ret;
}

int ObLogicalOperator::est_width()
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  if (get_num_of_child() == 0) {
    /*do nothing*/
  } else if (OB_ISNULL(child = get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else {
    width_ = child->get_width();
  }
  return ret;
}

int ObLogicalOperator::compute_table_set()
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  if (LOG_SUBPLAN_FILTER == get_type() || get_num_of_child() == 1) {
    // subplan filter 只能看到左表，外层不应该看到右表
    if (OB_ISNULL(child = get_child(0))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else {
      set_table_set(&child->get_table_set());
    }
  } else {
    ObRelIds *table_set = NULL;
    if (OB_ISNULL(get_plan())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_ISNULL(table_set = (ObRelIds*) get_plan()->get_allocator().alloc(sizeof(ObRelIds)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate memory", K(ret));
    } else {
      table_set = new(table_set) ObRelIds();
      for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); i++) {
        if (OB_ISNULL(child = get_child(i))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret));
        } else {
          table_set->add_members(child->get_table_set());
        }
      }
      if (OB_SUCC(ret)) {
        set_table_set(table_set);
      }
    }
  }
  return ret;
}

int ObLogicalOperator::compute_one_row_info()
{
  int ret = OB_SUCCESS;
  is_at_most_one_row_ = false;
  if (get_num_of_child() > 0) {
    is_at_most_one_row_ = true;
    for (int64_t i = 0; OB_SUCC(ret) && is_at_most_one_row_ && i < get_num_of_child(); i++) {
      if (OB_ISNULL(get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (!get_child(i)->get_is_at_most_one_row()) {
        is_at_most_one_row_ = false;
      } else { /*do nothing*/ }
    }
  }
  return ret;
}

int ObLogicalOperator::compute_sharding_info()
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  if (get_num_of_child() == 0) {
    /*do nothing*/
  } else if (OB_ISNULL(child = get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FAIL(weak_sharding_.assign(child->get_weak_sharding()))) {
    LOG_WARN("failed to assign sharding info", K(ret));
  } else {
    strong_sharding_ = child->get_strong_sharding();
  }
  return ret;
}

int ObLogicalOperator::compute_plan_type()
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  if (is_local()) {
    phy_plan_type_ = ObPhyPlanType::OB_PHY_PLAN_LOCAL;
  } else if (is_remote()) {
    phy_plan_type_ = ObPhyPlanType::OB_PHY_PLAN_REMOTE;
  } else if (is_distributed()) {
    phy_plan_type_ = ObPhyPlanType::OB_PHY_PLAN_DISTRIBUTED;
  } else {
    phy_plan_type_ = ObPhyPlanType::OB_PHY_PLAN_UNINITIALIZED;
  }
  bool child_has_exchange = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); i++) {
    if (OB_ISNULL(child = get_child(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(child), K(ret));
    } else {
      if (child->is_exchange_allocated()) {
        child_has_exchange = true;
      }
      if (ObPhyPlanType::OB_PHY_PLAN_UNCERTAIN == child->get_location_type()) {
        location_type_ = ObPhyPlanType::OB_PHY_PLAN_UNCERTAIN;
      }
    }
  }
  if (OB_SUCC(ret) && child_has_exchange) {
    exchange_allocated_ = true;
    phy_plan_type_ = ObPhyPlanType::OB_PHY_PLAN_DISTRIBUTED;
  }
  return ret;
}

int ObLogicalOperator::compute_op_other_info()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else {
    // compute contains fake cte
    if (OB_SUCC(ret)) {
      if (get_type() == log_op_def::ObLogOpType::LOG_SET &&
          static_cast<ObLogSet*>(this)->is_recursive_union()) {
        /*do nothing*/
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && !contain_fake_cte_ && i < get_num_of_child(); i++) {
          if (OB_ISNULL(get_child(i))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get unexpected null", K(ret));
          } else {
            contain_fake_cte_ |= get_child(i)->get_contains_fake_cte();
          }
        }
      }
    }
    
    // compute contains fake cte match all sharding
    if (OB_SUCC(ret)) {
      if (get_type() == log_op_def::ObLogOpType::LOG_SET &&
          static_cast<ObLogSet*>(this)->is_recursive_union()) {
        /*do nothing*/
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && !contain_match_all_fake_cte_ && i < get_num_of_child(); i++) {
          if (OB_ISNULL(get_child(i))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get unexpected null", K(ret));
          } else {
            contain_match_all_fake_cte_ |= get_child(i)->get_contains_match_all_fake_cte();
          }
        }
      }
    }

    // compute contains merge style op
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && !contain_pw_merge_op_ && i < get_num_of_child(); i++) {
        if (OB_ISNULL(get_child(i))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret));
        } else {
          contain_pw_merge_op_ |= get_child(i)->get_contains_pw_merge_op() && 
                                  log_op_def::LOG_EXCHANGE != get_child(i)->get_type();
        }
      }
      if (OB_SUCC(ret) && !contain_pw_merge_op_) {
        if (log_op_def::LOG_GROUP_BY == get_type()) {
          ObLogGroupBy *group_by = static_cast<ObLogGroupBy*>(this);
          contain_pw_merge_op_ = !group_by->get_group_by_exprs().empty() &&
                               (AggregateAlgo::MERGE_AGGREGATE == group_by->get_algo()) &&
                               is_partition_wise();
        } else if (log_op_def::LOG_DISTINCT == get_type()) {
          ObLogDistinct *distinct = static_cast<ObLogDistinct*>(this);
          contain_pw_merge_op_ = AggregateAlgo::MERGE_AGGREGATE == distinct->get_algo() &&
                               is_partition_wise();
        } else if (log_op_def::LOG_SET == get_type()) {
          ObLogSet *set = static_cast<ObLogSet*>(this);
          contain_pw_merge_op_ = set->is_set_distinct() && SetAlgo::MERGE_SET == set->get_algo() &&
                               is_partition_wise();
        } else if (log_op_def::LOG_WINDOW_FUNCTION == get_type()) {
          contain_pw_merge_op_ = is_block_op() &&
                               is_partition_wise();
        } else { /*do nothing*/ }
      }
    }

    // compute contains das op
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && !contain_das_op_ && i < get_num_of_child(); i++) {
        if (OB_ISNULL(get_child(i))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret));
        } else {
          contain_das_op_ |= get_child(i)->get_contains_das_op();
        }
      }
    }
  }
  return ret;
}

int ObLogicalOperator::compute_pipeline_info()
{
  int ret = OB_SUCCESS;
  is_nl_style_pipelined_plan_ = false;
  if (is_block_op()) {
    is_pipelined_plan_ = false;
  } else {
    is_pipelined_plan_ = true;
    for (int64_t i = 0; OB_SUCC(ret) && is_pipelined_plan_ && i < get_num_of_child(); ++i) {
      ObLogicalOperator *child = get_child(i);
      if (OB_ISNULL(child)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpect null child", K(ret));
      } else {
        is_pipelined_plan_ &= child->is_pipelined_plan();
      }
    }
  }
  return ret;
}

int ObLogicalOperator::compute_property(Path *path)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(path) || OB_ISNULL(path->parent_) || OB_ISNULL(path->parent_->get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("path is null", K(ret), K(path));
  } else if (OB_FAIL(append(get_output_const_exprs(), path->parent_->get_output_const_exprs()))) {
    LOG_WARN("failed to append const exprs", K(ret));
  } else if (OB_FAIL(set_op_ordering(path->get_ordering()))) {
    LOG_WARN("failed to set ordering", K(ret));
  } else if (OB_FAIL(set_weak_sharding(path->weak_sharding_))) {
    LOG_WARN("failed to set weak sharding", K(ret));
  } else {
    set_output_equal_sets(&path->parent_->get_output_equal_sets());
    set_fd_item_set(&path->parent_->get_fd_item_set());
    set_table_set(&path->parent_->get_tables());
    set_is_at_most_one_row(path->parent_->get_is_at_most_one_row());
    set_strong_sharding(path->strong_sharding_);
    set_exchange_allocated(path->exchange_allocated_);
    set_phy_plan_type(path->phy_plan_type_);
    set_location_type(path->location_type_);
    set_contains_fake_cte(path->contain_fake_cte_);
    set_contains_pw_merge_op(path->contain_pw_merge_op_);
    set_contains_match_all_fake_cte(path->contain_match_all_fake_cte_);
    set_contains_das_op(path->contain_das_op_);
    is_pipelined_plan_ = path->is_pipelined_path();
    is_nl_style_pipelined_plan_ = path->is_nl_style_pipelined_path();

    //set cost, card, width
    set_op_cost(path->op_cost_);
    set_cost(path->cost_);
    set_card(path->get_path_output_rows());
    set_width(path->parent_->get_output_row_size());
    set_interesting_order_info(path->get_interesting_order_info());
    set_is_local_order(path->is_local_order_);
    set_is_range_order(path->is_range_order_);
    set_parallel(path->parallel_);
    set_server_cnt(path->server_cnt_);
    
    LOG_TRACE("compute property finished",
              K(get_op_name(type_)),
              K(get_cost()),
              K(is_local_order_),
              K(is_range_order_),
              K(op_ordering_),
              K(is_at_most_one_row_),
              K(output_const_exprs_),
              K(output_equal_sets_),
              K(strong_sharding_),
              K(weak_sharding_),
              K(fd_item_set_),
              K(phy_plan_type_),
              K(location_type_));
  }
  return ret;
}

int ObLogicalOperator::re_est_cost(EstimateCostInfo &param, double &card, double &cost)
{
  int ret = OB_SUCCESS;
  //default by pass operator
  if (1 == get_num_of_child()) {
    ObLogicalOperator *child = NULL;
    int parallel = 1.0;
    if (OB_ISNULL(child = get_child(ObLogicalOperator::first_child)) ||
        OB_ISNULL(get_plan())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(child), K(ret));
    } else if (OB_UNLIKELY((parallel = get_parallel()) < 1)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected parallel degree", K(parallel), K(ret));
    } else {
      double selectivity = 1.0;
      double child_card = child->get_card();
      double child_cost = child->get_cost();
      ObOptimizerContext &opt_ctx = get_plan()->get_optimizer_context();
      double op_cost = ObOptEstCost::cost_get_rows(child_card / parallel,
                                                   opt_ctx.get_cost_model_type());
      if (OB_FAIL(SMART_CALL(child->re_est_cost(param, child_card, child_cost)))) {
        LOG_WARN("failed to re est exchange cost", K(ret));
      } else if (OB_FAIL(ObOptSelectivity::calculate_selectivity(get_plan()->get_basic_table_metas(),
                                                                get_plan()->get_selectivity_ctx(),
                                                                get_filter_exprs(),
                                                                selectivity,
                                                                get_plan()->get_predicate_selectivities()))) {
        LOG_WARN("failed to calculate selectivity", K(ret));
      } else {
        cost = child_cost + op_cost;
        card = child_card * selectivity;
        if (param.override_) {
          set_op_cost(op_cost);
          set_cost(cost);
          set_card(card);
        }
      }
    }
  } else {
    card = get_card();
    cost = get_cost();
  }
  return ret;
}

int ObLogicalOperator::compute_property()
{
  int ret = OB_SUCCESS;
  // compute new property
  if (OB_FAIL(compute_const_exprs())) {
    LOG_WARN("failed to compute compute const exprs", K(ret));
  } else if (OB_FAIL(compute_equal_set())) {
    LOG_WARN("failed to compute equal sets", K(ret));
  } else if (OB_FAIL(compute_fd_item_set())) {
    LOG_WARN("failed to compute fd item sets", K(ret));
  } else if (OB_FAIL(compute_table_set())) {
    LOG_WARN("failed to compute table sets", K(ret));
  } else if (OB_FAIL(compute_one_row_info())) {
    LOG_WARN("failed to compute one row info", K(ret));
  } else if (OB_FAIL(compute_pipeline_info())) {
    LOG_WARN("failed to compute pipeline info", K(ret));
  } else if (OB_FAIL(compute_sharding_info())) {
    LOG_WARN("failed to compute sharding info", K(ret));
  } else if (OB_FAIL(compute_plan_type())) {
    LOG_WARN("failed to compute plan type", K(ret));
  } else if (OB_FAIL(compute_op_other_info())) {
    LOG_WARN("failed to check and replace aggr exprs", K(ret));
  } else if (OB_FAIL(compute_op_ordering())) {
    LOG_WARN("failed to compute op ordering", K(ret));
  } else if (OB_FAIL(compute_op_interesting_order_info())) {
    LOG_WARN("failed to compute op ordering match info", K(ret));
  } else if (OB_FAIL(compute_op_parallel_and_server_info())) {
    LOG_WARN("failed to compute op server info", K(ret));
  } else if (OB_FAIL(est_width())) {
    LOG_WARN("failed to compute width", K(ret));
  } else if (OB_FAIL(est_cost())) {
    LOG_WARN("failed to estimate cost", K(ret));
  } else {
    LOG_TRACE("compute property finished",
              K(get_op_name(type_)),
              K(get_cost()),
              K(is_local_order_),
              K(is_range_order_),
              K(op_ordering_),
              K(is_at_most_one_row_),
              K(output_const_exprs_),
              K(output_equal_sets_),
              K(strong_sharding_),
              K(weak_sharding_),
              K(fd_item_set_),
              K(phy_plan_type_),
              K(location_type_),
              K(contain_fake_cte_),
              K(contain_pw_merge_op_),
              K(contain_das_op_),
              K(width_));
  }

  return ret;
}

int ObLogicalOperator::explain_collect_width_pre(void *ctx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ctx is unexpected null", K(ret));
  } else {
    planText *explain_ctx = reinterpret_cast<planText *>(ctx);
    planText &plan = *explain_ctx;
    plan_formatter &formatter = plan.formatter;
    char buffer[50];
    for (int i = 0 ; OB_SUCC(ret) && i < formatter.num_of_columns; i++) {
      int32_t length = 0;
      switch (i) {
      case Id: {
        snprintf(buffer, sizeof(buffer), "%ld", op_id_);
        length = (int32_t) strlen(buffer);
      }
        break;
      case Operator: {
        length = get_explain_name_length() + plan.level;
      }
        break;
      case Name: {
        if (log_op_def::LOG_TABLE_SCAN == type_) {
          ObLogTableScan *scan = reinterpret_cast<ObLogTableScan *>(this);
          length = scan->get_table_name().length();
          if (!scan->get_index_name().empty()) {
            length += 2 + scan->get_index_name().length(); //"(index_name)"
            if (is_descending_direction(scan->get_scan_direction())) {
              length += 8; //",REVERSE"
            } else { /* Do nothing */ }
          } else {
            if (is_descending_direction(scan->get_scan_direction())) {
              length += 9; //"(REVERSE)"
            } else { /* Do nothing */ }
          }
        } else if (log_op_def::LOG_SUBPLAN_SCAN == type_ || log_op_def::LOG_UNPIVOT == type_) {
          ObLogSubPlanScan *scan = static_cast<ObLogSubPlanScan *>(this);
          length = scan->get_subquery_name().length();
        } else if (log_op_def::LOG_TABLE_LOOKUP == type_) {
          ObLogTableLookup *table_lookup = reinterpret_cast<ObLogTableLookup *>(this);
          length += table_lookup->get_table_name().length();
        } else if (log_op_def::LOG_TEMP_TABLE_ACCESS == type_) {
          // ignore LOG_TEMP_TABLE_INSERT
          ObLogTempTableAccess *temp_table_access = static_cast<ObLogTempTableAccess *>(this);
          length = 2 + temp_table_access->get_table_name().length()
                   + temp_table_access->get_access_name().length();
        } else if (log_op_def::LOG_EXCHANGE == type_) {
          ObLogExchange *exchange = static_cast<ObLogExchange*>(this);
          if (OB_INVALID_ID != exchange->get_dfo_id()) {
            snprintf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, ":EX%ld%04ld",
                     exchange->get_px_id(), exchange->get_dfo_id());
            length = (int32_t) strlen(buffer);
          } else {
            length = 0;
          }
        } else if (log_op_def::LOG_GRANULE_ITERATOR == type_) {
          ObLogGranuleIterator *gi_op = static_cast<ObLogGranuleIterator*>(this);
          if (gi_op->get_join_filter_info().is_inited_ &&
              OB_INVALID != gi_op->get_join_filter_info().filter_id_) {
            length = 8; // :BF0000
          }
        } else if (log_op_def::LOG_JOIN_FILTER == type_) {
          ObLogJoinFilter *join_filter = static_cast<ObLogJoinFilter*>(this);
          if (OB_INVALID_ID != join_filter->get_filter_id()) {
            length = 8; // :BF0000
          }
        } else if (log_op_def::LOG_UPDATE == type_||
                   log_op_def::LOG_DELETE == type_||
                   log_op_def::LOG_INSERT == type_||
                   log_op_def::LOG_INSERT_ALL == type_) {
          ObLogDelUpd *dml_op = static_cast<ObLogDelUpd*>(this);
          ObString table_name;
          ObString index_name;
          if (dml_op->is_pdml()
              && dml_op->is_index_maintenance()
              && NULL != dml_op->get_index_dml_infos().at(0)
              && OB_SUCC(dml_op->get_table_index_name(*dml_op->get_index_dml_infos().at(0),
                                                      table_name,
                                                      index_name))) {
            int64_t base_table_len = table_name.length();
            int64_t index_table_len = index_name.length();
            int64_t bracket_len = 2; // base_table(index_table)
            length = base_table_len + index_table_len + bracket_len;
          }
        } else if (log_op_def::LOG_ERR_LOG == type_) {
          ObLogErrLog *err_log = static_cast<ObLogErrLog*>(this);
          length += err_log->get_err_log_define().err_log_table_name_.length();
        } else if (log_op_def::LOG_FUNCTION_TABLE == type_) {
          ObLogFunctionTable *func_table = static_cast<ObLogFunctionTable *>(this);
          length = func_table->get_table_name().length();
        } else {
          length = 0;
        }
      }
        break;
      case Est_Rows: {
  //      length = (int) strlen("TBD");
        snprintf(buffer, sizeof(buffer), "%ld", static_cast<int64_t>(ceil(get_card())));
        length = (int32_t) strlen(buffer);
      }
        break;
      case Cost: {
        snprintf(buffer, sizeof(buffer), "%ld", static_cast<int64_t>(ceil(cost_)));
        length = (int32_t) strlen(buffer);
      }
        break;

      default: {
        LOG_WARN("Unexpected access to default branch", K(i));
      }
        break;
      }

      if (i < 0 || i >= formatter.max_plan_column_width) {
        ret = OB_INDEX_OUT_OF_RANGE;
        LOG_WARN("i is out of range of [0,max_plan_column_width)", K(ret));
      } else if (formatter.column_width[i] < length) {
          formatter.column_width[i] = length;
      } else { /* Do nothing */ }
    }
    if (OB_SUCC(ret)) {
      plan_formatter::NameLeftPadding pad;
      pad.op_ = this;
      pad.level_ = plan.level;
      int64_t size = sizeof(*pad.tree_line_) * (plan.level + 1); // + 1 to avoid alloc empty memory
      pad.tree_line_ = static_cast<plan_formatter::TreeLine *>(formatter.alloc_.alloc(size));
      if (NULL == pad.tree_line_) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("allocate memory failed", K(ret));
      } else {
        MEMSET(pad.tree_line_, 0, size);
        if (OB_FAIL(formatter.names_left_padding_.push_back(pad))) {
          LOG_WARN("array push back failed", K(ret));
        }
      }
    }

    if (OB_SUCC(ret)) {
      plan.level += 1;
    }
  }

  return ret;
}

int ObLogicalOperator::explain_collect_width_post(void *ctx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ctx is unexpected null", K(ret));
  } else {
    planText *explain_ctx = reinterpret_cast<planText *>(ctx);
    explain_ctx->level--;
  }
  return ret;
}

#define NEW_LINE       "\n"
#define SEPARATOR       "|"
#define SPACE           " "
#define PLAN_WRAPPER    "="
#define LINE_SEPARATOR  "-"

int ObLogicalOperator::explain_index_selection_info_pre(void *ctx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ctx is null", K(ret));
  } else {
    planText *explain_ctx = reinterpret_cast<planText *>(ctx);
    planText &plan = *explain_ctx;
    char *buf = plan.buf;
    int64_t &buf_len = plan.buf_len;
    int64_t &pos = plan.pos;
    int64_t saved_pos = plan.pos;
    if (OB_FAIL(explain_index_selection_info(buf, buf_len, pos))) {
      LOG_WARN("print index selection info fails", K(ret));
    } else if (pos != saved_pos && OB_FAIL(BUF_PRINTF(NEW_LINE))) {
    }
  }
  return ret;
}

int ObLogicalOperator::explain_write_buffer_post(void *ctx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ctx is unexpected null", K(ret));
  } else {
    planText *explain_ctx = reinterpret_cast<planText *>(ctx);
    explain_ctx->level--;
  }
  return ret;
}

int ObLogicalOperator::explain_write_buffer_pre(void *ctx)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ctx is unexpected null", K(ret));
  } else {
    planText *explain_ctx = reinterpret_cast<planText *>(ctx);
    planText &plan = *explain_ctx;
    char buffer[OB_MAX_PLAN_EXPLAIN_NAME_LENGTH];
    char *buf = plan.buf;
    int64_t &buf_len = plan.buf_len;
    int64_t &pos = plan.pos;

    // skip the ID
    if (OB_FAIL(BUF_PRINTF(SEPARATOR))) { /* Do nothing */
    } else if (plan.formatter.max_plan_column_width <= 0) {
      ret = OB_INDEX_OUT_OF_RANGE;
      LOG_WARN("max_plan_column_width is not greater than 0", K(ret));
    } else if (OB_FAIL(BUF_PRINTF("%-*ld", plan.formatter.column_width[0], op_id_))) { /* Do nothing */
    } else if (OB_FAIL(BUF_PRINTF(SEPARATOR))) {
      /* Do nothing */
    }

    for (int i = 1; OB_SUCC(ret) && i < plan.formatter.num_of_columns; i++) {
      int64_t tmp_pos = 0;
      switch (i) {
      case Operator: {
        break;
      }
      case Name: {
        if (log_op_def::LOG_TABLE_SCAN == type_) {
          ObLogTableScan *scan = static_cast<ObLogTableScan *>(this);
          const ObString &name = scan->get_table_name();
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                          "%.*s", name.length(), name.ptr());
          if (scan->is_index_scan()) {
            const ObString &index_name = scan->get_index_name();
            databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                            "%s", LEFT_BRACKET);
            databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                            "%.*s", index_name.length(), index_name.ptr());
            if (is_descending_direction(scan->get_scan_direction())) {
              databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                              "%s", COMMA_REVERSE);
            }
            databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                            "%s", RIGHT_BRACKET);
          } else {
            if (is_descending_direction(scan->get_scan_direction())) {
              databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                              "%s", BRACKET_REVERSE);
            }
          }
        } else if (log_op_def::LOG_SUBPLAN_SCAN == type_ || log_op_def::LOG_UNPIVOT == type_) {
          ObLogSubPlanScan *scan = reinterpret_cast<ObLogSubPlanScan *>(this);
          ObString &name = scan->get_subquery_name();
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                          "%.*s", name.length(), name.ptr());
        } else if (log_op_def::LOG_ERR_LOG == type_) {
          ObLogErrLog *err_log = reinterpret_cast<ObLogErrLog *>(this);
          ObString &name = err_log->get_err_log_define().err_log_table_name_;
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                          "%.*s", name.length(), name.ptr());
        } else if (log_op_def::LOG_TEMP_TABLE_INSERT == type_) {
          ObLogTempTableInsert *insert = reinterpret_cast<ObLogTempTableInsert *>(this);
          const ObString &name = insert->get_table_name();
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                          "%.*s", name.length(), name.ptr());
        } else if (log_op_def::LOG_TEMP_TABLE_ACCESS == type_) {
          ObLogTempTableAccess *access = reinterpret_cast<ObLogTempTableAccess *>(this);
          const ObString &temp_table_name = access->get_table_name();
          const ObString &access_name = access->get_access_name();
          if (access_name.empty()) {
            databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                          "%.*s", temp_table_name.length(), temp_table_name.ptr());
          } else {
            databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                          "%.*s(%.*s)", access_name.length(), access_name.ptr(),
                                        temp_table_name.length(), temp_table_name.ptr());
          }
        } else if (log_op_def::LOG_TABLE_LOOKUP == type_) {
          ObLogTableLookup *table_lookup = reinterpret_cast<ObLogTableLookup *>(this);
          const ObString &name = table_lookup->get_table_name();
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                          "%.*s", name.length(), name.ptr());
        } else if (log_op_def::LOG_EXCHANGE == type_) {
          ObLogExchange *exchange = static_cast<ObLogExchange*>(this);
          if (OB_INVALID_ID != exchange->get_dfo_id()) {
            databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                            ":EX%ld%04ld", exchange->get_px_id(), exchange->get_dfo_id());
          }
        } else if (log_op_def::LOG_GRANULE_ITERATOR == type_) {
          ObLogGranuleIterator *gi_op = static_cast<ObLogGranuleIterator*>(this);
          if (gi_op->get_join_filter_info().is_inited_ &&
              OB_INVALID_ID != gi_op->get_join_filter_info().filter_id_) {
            databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                ":BF%04ld", gi_op->get_join_filter_info().filter_id_);
          }
        } else if (log_op_def::LOG_JOIN_FILTER == type_) {
          ObLogJoinFilter *join_filter = static_cast<ObLogJoinFilter*>(this);
          if (OB_INVALID_ID != join_filter->get_filter_id()) {
            databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                ":BF%04ld", join_filter->get_filter_id());
          }
        } else if (log_op_def::LOG_UPDATE == type_
                   || log_op_def::LOG_DELETE == type_
                   || log_op_def::LOG_INSERT == type_
                   || log_op_def::LOG_INSERT_ALL == type_) {
          ObLogUpdate *dml_op = static_cast<ObLogUpdate*>(this);
          ObString base_table;
          ObString index_table;
          if (dml_op->is_pdml()
              && dml_op->is_index_maintenance()
              && NULL != dml_op->get_index_dml_infos().at(0)
              && OB_SUCC(dml_op->get_table_index_name(*dml_op->get_index_dml_infos().at(0),
                                                      base_table,
                                                      index_table))) {
            databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                            "%.*s(%.*s)", base_table.length(), base_table.ptr(),
                            index_table.length(), index_table.ptr());
          }
        } else if (log_op_def::LOG_FUNCTION_TABLE == type_) {
          ObLogFunctionTable *func_table = static_cast<ObLogFunctionTable *>(this);
          const ObString &name = func_table->get_table_name();
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                          "%.*s", name.length(), name.ptr());
        }
        // left padding with space
        int64_t N = plan.formatter.column_width[i] - tmp_pos;
        for (int64_t n = 0; n < N; ++n) {
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos, " ");
        }
        break;
      }
      case Est_Rows: {
        if (1000000000000 > get_card()) {
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                        "%ld", static_cast<int64_t>(ceil(get_card())));
        } else {
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                        "%e", get_card());
        }
        break;
      }
      case Cost: {
        if (1000000000000 > cost_) {
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                 "%ld", static_cast<int64_t>(ceil(cost_)));
        } else {
          databuff_printf(buffer, OB_MAX_PLAN_EXPLAIN_NAME_LENGTH, tmp_pos,
                        "%e", cost_);
        }
        break;
      }
      default: {
        LOG_WARN("Unexpected access to default branch", K(i));
        break;
      }
      } // end of switch

      if (OB_FAIL(ret)) {
        LOG_WARN("Get unexpected error", K(ret));
      } else if ( Operator == i) {
        if (OB_FAIL(plan.formatter.padding_name_left(get_op_id(), buf, buf_len, pos))) {
          LOG_WARN("padding name left failed", K(ret));
        }
        //BUF_PRINTF("%-*s", plan.formatter.column_width[i] - plan.level, get_name());
        int64_t pos_internal = 0;
        if (OB_FAIL(ret)) {
          LOG_WARN("Previous BUF_PRINTF fails", K(ret));
        } else if (OB_FAIL(get_explain_name_internal(buf + pos, buf_len - pos, pos_internal))) {
          LOG_WARN("Getting explain name internal fails");
        } else {
          pos += pos_internal;
        }
        for (int64_t m = 0;
             OB_SUCC(ret)
             && m < plan.formatter.column_width[i]
                   - get_explain_name_length()
                   - plan.level;
             ++m) {
          ret = BUF_PRINTF(SPACE);
        }
      } else {
        ret = BUF_PRINTF("%-*s", plan.formatter.column_width[i], buffer);
      }
      if (OB_SUCC(ret)) {
        ret = BUF_PRINTF(SEPARATOR);
      } else { /* Do nothing */ }
    } // for
    if (OB_FAIL(ret)) {
      LOG_WARN("Get unexpected error", K(ret));
    } else if (OB_FAIL(BUF_PRINTF(NEW_LINE))) { /* Do nothing */
    } else {
      plan.level++;
    }
  }

  return ret;
}

int ObLogicalOperator::print_plan_annotation(char *buf,
                                             int64_t &buf_len,
                                             int64_t &pos,
                                             ExplainType type)
{
  int ret = OB_SUCCESS;
  ExplainType old_explain_type = EXPLAIN_UNINITIALIZED;
  ExplainType head_explain_type = (EXPLAIN_EXTENDED_NOADDR == old_explain_type ? old_explain_type : type);
  if (EXPLAIN_EXTENDED_NOADDR == type
      || EXPLAIN_PLANREGRESS == type ) {
    old_explain_type = type;
    type = EXPLAIN_BASIC;
  } else { /* Do nothing */ }
  // print some msg at the head of this print line
  if (OB_FAIL(print_plan_head_annotation(buf, buf_len, pos, head_explain_type))) {
    LOG_WARN("Print my plan annotation fails", K(ret));
  } else { /* Do nothing */ }

  // print output
  const ObIArray<ObRawExpr *> &output = output_exprs_;
  EXPLAIN_PRINT_EXPRS(output, type);
  if (OB_FAIL(ret)) {
    LOG_WARN("Previous step fails", K(ret));
  } else {
    ret = BUF_PRINTF(", ");
  }

  // print filter
  if (OB_FAIL(ret)) {
    LOG_WARN("Previous step fails", K(ret));
  } else if (LOG_UNPIVOT != get_type()) {
    const ObIArray<ObRawExpr *> &filter = filter_exprs_;
    EXPLAIN_PRINT_EXPRS(filter, type);
  }

  // print startup filter
  const ObIArray<ObRawExpr *> &startup_filter = startup_exprs_;
  if (OB_FAIL(ret)) {
    LOG_WARN("Previous step fails", K(ret));
  } else if (!startup_filter.empty()) {
    ret = BUF_PRINTF(", ");
    EXPLAIN_PRINT_EXPRS(startup_filter, type);
  } else { /* Do nothing */ }

  if (OB_SUCC(ret) && EXPLAIN_UNINITIALIZED != old_explain_type) {
    type = old_explain_type;
  } else { /* Do nothing */ }

  // print vectorized execution batch row count
  ObPhyOperatorType phy_type = PHY_INVALID;
  if (OB_FAIL(ObStaticEngineCG::get_phy_op_type(
          *this, phy_type, is_plan_root() && (get_parent() == nullptr)))) {
    /* Do nothing */
  } else if (OB_SUCC(ret) && NULL != get_plan() &&
             get_plan()->get_optimizer_context().get_batch_size() > 0 &&
             ObOperatorFactory::is_vectorized(phy_type)) {
    BUF_PRINTF(", rowset=%ld", get_plan()->get_optimizer_context().get_batch_size());
  } else { /* Do nothing */ }

  if (OB_SUCC(ret) && OB_FAIL(print_my_plan_annotation(buf, buf_len, pos, type))) {
    LOG_WARN("Print my plan annotation fails", K(ret));
  } else { /* Do nothing */ }

  return ret;
}

int ObLogicalOperator::explain_write_buffer_output_pre(void *ctx)
{
  int ret = OB_SUCCESS;
  CK( OB_NOT_NULL(ctx) );
  if (OB_SUCC(ret)) {
    planText *explain_ctx = reinterpret_cast<planText *>(ctx);
    planText &plan = *explain_ctx;
    char *buf = plan.buf;
    int64_t &buf_len = plan.buf_len;
    int64_t &pos = plan.pos;
    OC( (BUF_PRINTF)(NEW_LINE));
    OC( (BUF_PRINTF)("  "));
    OC( (BUF_PRINTF)("%ld", op_id_));
    OC( (BUF_PRINTF)(" - "));
    OC( (print_plan_annotation)(buf, buf_len, pos, plan.format));
  }
  return ret;
}

int ObLogicalOperator::explain_write_buffer_outline_pre(void *ctx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx) || OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(get_plan()));
  } else if (is_plan_root() && OB_FALSE_IT(get_plan()->reset_outline_print_flags())) {
  } else if (OB_FAIL(print_outline(*reinterpret_cast<planText *>(ctx)))) {
    LOG_WARN("fail to print outline", K(ret));
  }
  return ret;
}

int ObLogicalOperator::do_pre_traverse_operation(const TraverseOp &op, void *ctx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Get unexpected null", K(ret), K(get_plan()));
  } else {
    switch (op) {
    case PX_PIPE_BLOCKING: {
      if (get_plan()->get_optimizer_context().get_parallel() > 1) {
        ObPxPipeBlockingCtx *pipe_blocking_ctx = static_cast<ObPxPipeBlockingCtx *>(ctx);
        if (OB_ISNULL(pipe_blocking_ctx)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("NULL ctx", K(ret));
        } else if (OB_FAIL(px_pipe_blocking_pre(*pipe_blocking_ctx))) {
          LOG_WARN("blocking px pipe failed", K(ret));
        }
      }
      break;
    }
    case PX_RESCAN: {
      if (OB_FAIL(px_rescan_pre())) {
        LOG_WARN("blocking px pipe failed", K(ret));
      }
      break;
    }
    case ALLOC_GI: {
      //do nothing
      AllocGIContext *alloc_gi_ctx = static_cast<AllocGIContext *>(ctx);
      if (OB_ISNULL(alloc_gi_ctx)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else {
        if (get_num_of_child() >= 2) {
          alloc_gi_ctx->add_multi_child_op_count();
        }
        if (OB_FAIL(allocate_granule_pre(*alloc_gi_ctx))) {
          LOG_WARN("allocate granule pre failed", K(ret));
        } else if (alloc_gi_ctx->alloc_gi_ && OB_FAIL(allocate_granule_nodes_above(*alloc_gi_ctx))) {
          LOG_WARN("allocate granule iterator failed", K(ret));
        }
      }
      break;
    }
    case ALLOC_MONITORING_DUMP: {
      AllocMDContext *md_ctx = static_cast<AllocMDContext *>(ctx);
      op_id_ = md_ctx->org_op_id_++;
      break;
    }
    case ALLOC_EXPR: {
      ObSQLSessionInfo *session = NULL;
      ObAllocExprContext *alloc_expr_context = static_cast<ObAllocExprContext *>(ctx);
      if (OB_ISNULL(alloc_expr_context) ||
          OB_ISNULL(session = get_plan()->get_optimizer_context().get_session_info())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(session), K(ret));
      } else if (is_plan_root() && OB_FAIL(adjust_plan_root_output_exprs())) {
        LOG_WARN("failed to set plan root output", K(ret));
      } else if (OB_FAIL(allocate_expr_pre(*alloc_expr_context))) {
        LOG_WARN("failed to do allocate expr pre", K(ret));
      } else {
        LOG_TRACE("succeed to do allcoate expr pre", K(get_type()), K(get_name()), K(get_op_id()), K(ret));
      }
      break;
    }
    case BLOOM_FILTER: {
      break;
    }
    case PROJECT_PRUNING: {
      if (OB_FAIL(project_pruning_pre())) {
        LOG_WARN("Project pruning pre error", K(ret));
      } else { /* Do nothing */ }
      break;
    }
    case OPERATOR_NUMBERING: {
      if (OB_ISNULL(ctx)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ctx is null", K(ret));
      } else if (OB_FAIL(numbering_operator_pre(*static_cast<NumberingCtx *>(ctx)))) {
        LOG_WARN("numbering operator pre failed", K(ret));
      }
      break;
    }
    case EXCHANGE_NUMBERING: {
      if (OB_ISNULL(ctx)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ctx is null", K(ret));
      } else if (OB_FAIL(numbering_exchange_pre(*static_cast<NumberingExchangeCtx *>(ctx)))) {
        LOG_WARN("fail numbering exchange pre", K(ret));
      }
      break;
    }
    case GEN_SIGNATURE: {
      break;
    }
    case GEN_LOCATION_CONSTRAINT: {
      break;
    }
    case EXPLAIN_COLLECT_WIDTH: {
      if (OB_FAIL(explain_collect_width_pre(ctx))) {
        LOG_WARN("explain_collect_width_pre fails", K(ret));
      } else { /* Do nothing */ }
      break;
    }
    case EXPLAIN_WRITE_BUFFER: {
      if (OB_FAIL(explain_write_buffer_pre(ctx))) {
        LOG_WARN("explain_write_buffer_pre fails", K(ret));
      } else { /* Do nothing */ }
      break;
    }
    case EXPLAIN_WRITE_BUFFER_OUTPUT: {
      if (OB_FAIL(explain_write_buffer_output_pre(ctx))) {
        LOG_WARN("explain_write_buffer_output_pre fails", K(ret));
      }
      break;
    }
    case EXPLAIN_WRITE_BUFFER_OUTLINE: {
      if (OB_FAIL(explain_write_buffer_outline_pre(ctx))) {
        LOG_WARN("explain_write_buffer_outline_pre fails", K(ret));
      }
      break;
    }
    case EXPLAIN_INDEX_SELECTION_INFO: {
       if (OB_FAIL(explain_index_selection_info_pre(ctx))) {
         LOG_WARN("explain index_selection_info_pre", K(ret));
       }
       break;
     }
    case PX_ESTIMATE_SIZE: {
      ret = px_estimate_size_factor_pre();
      break;
    }
    case ALLOC_LINK: {
      break;
    }
    case GEN_LINK_STMT: {
      GenLinkStmtPostContext *link_ctx = static_cast<GenLinkStmtPostContext *>(ctx);
      if (OB_ISNULL(link_ctx)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("link stmt is NULL", K(ret));
      } else if (OB_FAIL(collect_link_sql_context_pre(*link_ctx))) {
        LOG_WARN("failed to collect link sql context", K(ret));
      }
      break;
    }
    case ALLOC_STARTUP_EXPR:
      break;
    case COPY_PART_EXPR: {
      CopyPartExprCtx *copy_part_expr_ctx = static_cast<CopyPartExprCtx*>(ctx);
      if (OB_ISNULL(copy_part_expr_ctx)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("ctx is NULL", K(ret));
      } else if (OB_FAIL(copy_part_expr_pre(*copy_part_expr_ctx))) {
        LOG_WARN("failed to copy part expr", K(ret));
      }
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Unexpected access of default branch", K(op), K(ret));
      break;
    }
    }
  }
  return ret;
}

int ObLogicalOperator::do_post_traverse_operation(const TraverseOp &op, void *ctx)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  CK(
    !OB_ISNULL(get_plan()),
    !OB_ISNULL(get_stmt()));

  if (OB_SUCC(ret)) {
    switch (op) {
      case PX_PIPE_BLOCKING: {
        if (get_plan()->get_optimizer_context().get_parallel() > 1) {
          ObPxPipeBlockingCtx *pipe_blocking_ctx = static_cast<ObPxPipeBlockingCtx *>(ctx);
          CK(OB_NOT_NULL(pipe_blocking_ctx));
          OC( (px_pipe_blocking_post)(*pipe_blocking_ctx) );
        } else if (get_type() == LOG_EXCHANGE) {
          // parallel = 1,it must use single-dfo
          ObLogExchange *exchange = static_cast<ObLogExchange *>(this);
          if (OB_ISNULL(exchange)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected status: exchange is null", K(ret));
          } else {
            exchange->set_old_unblock_mode(false);
            LOG_TRACE("pipe blocking ctx", K(get_name()));
          }
        }
        break;
      }
    	case ALLOC_GI: {
    	  AllocGIContext *alloc_gi_ctx = static_cast<AllocGIContext *>(ctx);
    	  if (OB_ISNULL(alloc_gi_ctx)) {
    	    ret = OB_ERR_UNEXPECTED;
    	    LOG_WARN("get unexpected null", K(ret));
    	  } else {
    	    if (get_num_of_child() >= 2) {
    	      alloc_gi_ctx->delete_multi_child_op_count();
    	    }
    	    if (OB_FAIL(allocate_granule_post(*alloc_gi_ctx))) {
    	      LOG_WARN("failed to allocate granule post", K(ret));
    	    } else if (alloc_gi_ctx->alloc_gi_ &&
    	               OB_FAIL(allocate_granule_nodes_above(*alloc_gi_ctx))) {
    	      LOG_WARN("failed to allcoate granule nodes", K(ret));
    	    }
    	  }
    		break;
    	}
      case ALLOC_EXPR: {
        ObAllocExprContext *alloc_expr_ctx = static_cast<ObAllocExprContext *>(ctx);
        if (OB_ISNULL(alloc_expr_ctx)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret));
        } else if (OB_FAIL(allocate_expr_post(*alloc_expr_ctx))) {
          LOG_WARN("failed to do allocate expr post", K(ret));
        } else if (is_plan_root() && NULL == get_parent()) {
          // 如果是root节点，检查是否所有的表达式都被分配完毕
          if (is_all_expr_produced(alloc_expr_ctx->expr_producers_)) {
            LOG_TRACE("succeed to produce all exprs", K(ret));
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("failed to produce all exprs", K(ret));
          }
        }
        break;
      }
      case BLOOM_FILTER: {
        AllocBloomFilterContext *alloc_bf_ctx = static_cast<AllocBloomFilterContext *>(ctx);
        CK( OB_NOT_NULL(alloc_bf_ctx));
        OC( (allocate_bf_node_for_hash_join)(*alloc_bf_ctx));
        break;
      }
      case ALLOC_MONITORING_DUMP: {
        if (OB_ISNULL(ctx)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Ctx is null", K(ret));
        } else if (OB_FAIL(alloc_md_post(*static_cast<AllocMDContext *>(ctx)))) {
          LOG_WARN("Failed to alloc monitroing dump operator", K(ret));
        }
        break;
      }
      case OPERATOR_NUMBERING: {
        if (OB_ISNULL(ctx)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ctx is null", K(ret));
        } else {
          numbering_operator_post(*static_cast<NumberingCtx *>(ctx));
        }
        break;
      }
      case EXCHANGE_NUMBERING: {
        if (OB_ISNULL(ctx)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ctx is null", K(ret));
        } else if (LOG_EXCHANGE == type_) {
          if (OB_FAIL(numbering_exchange_post(*static_cast<NumberingExchangeCtx *>(ctx)))) {
            LOG_WARN("fail numbering exchange post", K(ret));
          } else if (static_cast<ObLogExchange*>(this)->is_rescanable() &&
             OB_FAIL(static_cast<ObLogExchange*>(this)->gen_px_pruning_table_locations())) {
            LOG_WARN("fail to gen px pruning table locations", K(ret));
          }
        }
        break;
      }
      case GEN_SIGNATURE: {
        if (OB_ISNULL(ctx)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ctx is null", K(ret));
        } else {
          uint64_t *seed = reinterpret_cast<uint64_t *>(ctx);
          *seed = hash(*seed);
          LOG_TRACE("", "operator", get_name(), "hash_value", *seed);
        }
        break;
      }
      case GEN_LOCATION_CONSTRAINT: {
        ret = gen_location_constraint(ctx);
        break;
      }
      case EXPLAIN_COLLECT_WIDTH: {
        explain_collect_width_post(ctx);
        break;
      }
      case EXPLAIN_WRITE_BUFFER: {
        explain_write_buffer_post(ctx);
        break;
      }
      case EXPLAIN_WRITE_BUFFER_OUTPUT: {
        break;
      }
      case EXPLAIN_WRITE_BUFFER_OUTLINE: {
        break;
      }
      case EXPLAIN_INDEX_SELECTION_INFO: {
        break;
      }
      case PX_ESTIMATE_SIZE: {
        ret = px_estimate_size_factor_post();
        break;
      }
      case ALLOC_LINK: {
        if (OB_FAIL(allocate_link_post())) {
          LOG_WARN("failed to allocate link post", K(ret));
        }
        break;
      }
      case GEN_LINK_STMT: {
        GenLinkStmtPostContext *link_ctx = static_cast<GenLinkStmtPostContext *>(ctx);
        if (OB_ISNULL(link_ctx)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("link stmt is NULL", K(ret));
        } else if (OB_FAIL(generate_link_sql_post(*link_ctx))) {
          LOG_WARN("failed to gen link sql", K(ret));
        }
      break;
      }
      case ALLOC_STARTUP_EXPR: {
        if (OB_FAIL(allocate_startup_expr_post())) {
          LOG_WARN("failed to alloc startup expr post", K(ret));
        }
        break;
      }
      default:
        break;
    }
  }
  return ret;
}

int ObLogicalOperator::do_plan_tree_traverse(const TraverseOp &operation, void *ctx)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(do_pre_traverse_operation(operation, ctx))) {
    LOG_WARN("failed to perform traverse operation", K(ret), "operator", get_name(), K(operation));
  } else {
    LOG_TRACE("succ to perform pre traverse operation", "operator", get_name(), K(operation));
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); i++) {
    if (OB_ISNULL(get_child(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get_child(i) is null", K(ret), K(i));
    } else if (OB_FAIL(SMART_CALL(get_child(i)->do_plan_tree_traverse(operation, ctx)))) {
      LOG_WARN("failed to bottom-up traverse operator", "operator", get_name(), K(operation), K(ret));
    } else { /*do nothing*/ }
  }

  // post_traverse_operation
  if (OB_SUCC(ret)) {
    if (OB_FAIL(do_post_traverse_operation(operation, ctx))) {
      LOG_WARN("failed to perform post traverse action", K(ret), K(operation));
    } else {
      LOG_TRACE("succ to perform post traverse action", K(operation), "operator", get_name());
    }
  } else { /*do nothing*/ }

  return ret;
}

int ObLogicalOperator::should_allocate_gi_for_dml(bool &is_valid)
{
  int ret = OB_SUCCESS;
  if (LOG_JOIN == get_type() || LOG_SET == get_type()) {
    is_valid = false;
  } else if (LOG_INSERT == get_type()) {
    const ObLogInsert *log_insert = static_cast<const ObLogInsert *>(this);
    if (!log_insert->is_multi_part_dml()) {
      is_valid = false;
    }
  } else if (LOG_GROUP_BY == get_type()) {
    ObLogGroupBy *log_group_by = static_cast<ObLogGroupBy*>(this);
    is_valid = (AggregateAlgo::MERGE_AGGREGATE != log_group_by->get_algo());
  } else if (LOG_DISTINCT == get_type()) {
    ObLogDistinct *log_distinct = static_cast<ObLogDistinct*>(this);
    is_valid = (AggregateAlgo::MERGE_AGGREGATE != log_distinct->get_algo());
  } else if (LOG_WINDOW_FUNCTION == get_type()) {
    is_valid = false;
  } else { /*do nothing*/ }

  for (int64_t i = 0; OB_SUCC(ret) && is_valid && i < get_num_of_child(); i++) {
    if (OB_ISNULL(get_child(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(get_child(i)->should_allocate_gi_for_dml(is_valid))) {
      LOG_WARN("failed to check should allocate gi for dml", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObLogicalOperator::to_json(char *buf, const int64_t buf_len, int64_t &pos, json::Value *&ret_val)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Get unexpected null", K(ret), K(get_plan()));
  } else {
    ObIAllocator *allocator = &get_plan()->get_allocator();

    // this operator
    ret_val = NULL;
    json::Pair *id = NULL;
    json::Pair *op = NULL;
    json::Pair *name = NULL;
    json::Pair *rows = NULL;
    json::Pair *cost = NULL;
//    json::Pair *op_cost = NULL;
    json::Pair *output = NULL;

    Value *id_value = NULL;
    Value *op_value = NULL;
    Value *name_value = NULL;
    Value *rows_value = NULL;
    Value *cost_value = NULL;
//    Value *op_cost_value = NULL;
    Value *output_value = NULL;
    if (OB_ISNULL(ret_val = (Value *)allocator->alloc(sizeof(Value)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(id = (Pair *)allocator->alloc(sizeof(Pair)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(op = (Pair *)allocator->alloc(sizeof(Pair)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(name = (Pair *)allocator->alloc(sizeof(Pair)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(rows = (Pair *)allocator->alloc(sizeof(Pair)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(cost = (Pair *)allocator->alloc(sizeof(Pair)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } /*else if (OB_ISNULL(op_cost = (Pair *)allocator->alloc(sizeof(Pair)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    }*/else if (OB_ISNULL(output = (Pair *)allocator->alloc(sizeof(Pair)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(id_value = (Value *)allocator->alloc(sizeof(Value)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(op_value = (Value *)allocator->alloc(sizeof(Value)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(name_value = (Value *)allocator->alloc(sizeof(Value)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(rows_value = (Value *)allocator->alloc(sizeof(Value)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(cost_value = (Value *)allocator->alloc(sizeof(Value)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } /*else if (OB_ISNULL(op_cost_value = (Value *)allocator->alloc(sizeof(Value)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } */else if (OB_ISNULL(output_value = (Value *)allocator->alloc(sizeof(Value)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("no memory");
    } else if (OB_ISNULL(ret_val = new(ret_val) Value())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Value");
    } else if (OB_ISNULL(id = new(id) Pair())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Pair");
    } else if (OB_ISNULL(op = new(op) Pair())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Pair");
    } else if (OB_ISNULL(name = new(name) Pair())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Pair");
    } else if (OB_ISNULL(rows = new(rows) Pair())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Pair");
    } else if (OB_ISNULL(cost = new(cost) Pair())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Pair");
    } /*else if (OB_ISNULL(op_cost = new(op_cost) Pair())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Pair");
    }*/ else if (OB_ISNULL(output = new(output) Pair())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Pair");
    } else if (OB_ISNULL(id_value = new(id_value) Value())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Value");
    } else if (OB_ISNULL(op_value = new(op_value) Value())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Value");
    } else if (OB_ISNULL(name_value = new(name_value) Value())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Value");
    } else if (OB_ISNULL(rows_value = new(rows_value) Value())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Value");
    } else if (OB_ISNULL(cost_value = new(cost_value) Value())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Value");
    } /*else if (OB_ISNULL(op_cost_value = new (op_cost_value) Value())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json value");
    }*/ else if (OB_ISNULL(output_value = new(output_value) Value())) {
      ret = OB_ERROR;
      LOG_WARN("failed to new json Value");
    } else {
      ret_val->set_type(JT_OBJECT);
      id_value->set_type(JT_NUMBER);
      // TBD
      id_value->set_int(id_);
      id->name_ = ID;
      id->value_ = id_value;

      op_value->set_type(JT_STRING);
      // TBD
      op_value->set_string(const_cast<char *>(this->get_name()),
                           static_cast<int32_t>(strlen(get_name())));
      op->name_ = OPERATOR;
      op->value_ = op_value;

      name_value->set_type(JT_STRING);
      // TBD
      name_value->set_string(const_cast<char *>(this->get_name()),
                             static_cast<int32_t>(strlen(get_name())));
      name->name_ = NAME;
      name->value_ = name_value;

      rows_value->set_type(JT_NUMBER);
      // TBD
      rows_value->set_int(static_cast<int>(get_card()));
      rows->name_ = ROWS;
      rows->value_ = rows_value;

      cost_value->set_type(JT_NUMBER);
      // TBD
      cost_value->set_int(static_cast<int>(cost_));
      cost->name_ = COST;
      cost->value_ = cost_value;

//      op_cost_value->set_type(JT_NUMBER);
//      op_cost_value->set_int(static_cast<int>(op_cost_));
//      op_cost->name_ = OPCOST;
//      op_cost->value_ = op_cost_value;
      // output expressions
      int64_t N = output_exprs_.count();
      if (0 < N) {
        output_value->set_type(JT_ARRAY);
        Value *expr_value = NULL;
        for (int64_t i = 0; OB_SUCC(ret) && i < N; i++) {
          if (OB_ISNULL(expr_value = (Value *)allocator->alloc(sizeof(Value)))) {
            // best effort
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_ERROR("failed to allocate json value");
          } else if (OB_ISNULL(expr_value = new(expr_value) Value())){
            ret = OB_ERROR;
            LOG_WARN("failed to new json Value");
          } else if (OB_ISNULL(output_exprs_.at(i))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("output_exprs_.at(i) is unexpected null", K(ret), K(i));
          } else {
            // generate string for expr
            int64_t pos_prev = pos;
            int64_t tmp_pos = 0;
            if (OB_FAIL(output_exprs_.at(i)->get_name(buf + pos_prev, buf_len - pos_prev, tmp_pos))) {
              LOG_WARN("failed to get_name", K(ret));
            } else {
              pos = pos_prev + tmp_pos;
              // set value
              expr_value->set_type(JT_STRING);
              expr_value->set_string(buf + pos_prev, static_cast<int32_t>(pos - pos_prev));
              output_value->array_add(expr_value);
            }
          }
        }
      } else {
        output_value->set_type(JT_STRING);
        LOG_TRACE("", K(pos));
        if (OB_FAIL(BUF_PRINTF("nil"))) {
          /* Do Nothing */
        } else {
          output_value->set_string(buf + pos, (int)strlen("nil"));
          LOG_TRACE("", K(pos));
          LOG_TRACE("", K(pos));
        }
      }

      if (OB_SUCC(ret)) {
        output->name_ = "output";
        output->value_ = output_value;
        ret_val->object_add(id);
        ret_val->object_add(op);
        ret_val->object_add(name);
        ret_val->object_add(rows);
        ret_val->object_add(cost);
//        ret_val->object_add(op_cost);
        ret_val->object_add(output);
        // child operator
        int64_t num_of_child = get_num_of_child();
        Pair *child = NULL;
        const uint64_t OB_MAX_JSON_CHILD_NAME_LENGTH = 64;
        char name_buf[OB_MAX_JSON_CHILD_NAME_LENGTH];
        int64_t name_buf_size = OB_MAX_JSON_CHILD_NAME_LENGTH;
        for (int64_t i = 0; OB_SUCC(ret) && i < num_of_child; ++i) {
          int64_t child_name_pos = snprintf(name_buf, name_buf_size, "CHILD_%ld", i+1);
          ObString child_name(child_name_pos, name_buf);
          if (OB_ISNULL(child = (Pair *)allocator->alloc(sizeof(Pair)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_ERROR("no memory");
          } else if (OB_ISNULL(child = new(child) Pair())) {
            ret = OB_ERROR;
            LOG_WARN("failed to new json Pair");
          } else if (OB_ISNULL(get_child(i))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get_child(i) returns null", K(ret), K(i));
          } else if (OB_FAIL(ob_write_string(*allocator, child_name, child->name_))) {
            LOG_WARN("failed to write string", K(ret));
            /* Do nothing */
          } else if (OB_FAIL(get_child(i)->to_json(buf, buf_len, pos, child->value_))) {
            LOG_WARN("to_json fails", K(ret), K(i));
          } else {
            ret_val->object_add(child);
          }
        }
      } else { /* Do nothing */ }
    }
  }

  return ret;
}

int ObLogicalOperator::mark_expr_produced(ObRawExpr *expr,
                                          uint64_t branch_id,
                                          uint64_t producer_id,
                                          ObAllocExprContext &gen_expr_ctx)
{
  int ret = OB_SUCCESS;
  ExprProducer *expr_producer = NULL;
  if (OB_ISNULL(expr) || OB_INVALID_ID == branch_id || OB_INVALID_ID == producer_id) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(ret), K(expr), K(branch_id), K(producer_id));
  } else if (OB_FAIL(gen_expr_ctx.find(expr, expr_producer))) {
    LOG_WARN("failed to find expr producer", K(ret));
  } else if (OB_ISNULL(expr_producer)) {
    ExprProducer new_expr_producer(expr, id_);
    new_expr_producer.producer_branch_ = branch_id;
    new_expr_producer.producer_id_ = producer_id;
    if (gen_expr_ctx.add(new_expr_producer)) {
      LOG_WARN("failed to add expr producer", K(ret));
    } else { /*do nothing*/ }
  } else {
    // mark as produced
    if (expr_producer->producer_branch_ == OB_INVALID_ID) {
      expr_producer->producer_branch_ = branch_id;
      expr_producer->producer_id_ = producer_id;
      LOG_TRACE("expr is marked as produced.", K(expr_producer->expr_), K(branch_id),
          K(expr_producer->consumer_id_), K(producer_id));
    }
  }
  return ret;
}

int ObLogicalOperator::allocate_expr_pre(ObAllocExprContext &ctx)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr*, 16> all_exprs;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FAIL(get_op_exprs(all_exprs))) {
    LOG_WARN("failed to get op exprs", K(ret));
  } else if (OB_FAIL(get_plan()->get_optimizer_context().get_all_exprs().append(all_exprs))) {
    LOG_WARN("failed to append exprs", K(ret));
  } else if (OB_FAIL(extract_non_const_exprs(all_exprs,
                                             op_exprs_))) {
    LOG_WARN("failed to extract const exprs", K(ret));
  } else if (OB_FAIL(add_exprs_to_ctx(ctx, op_exprs_))) {
    LOG_WARN("failed to add exprs to ctx", K(ret));
  } else {
    LOG_TRACE("succeed to allocate expr pre", K(id_), K(op_exprs_.count()),
        K(op_exprs_), K(get_name()), K(is_plan_root()));
  }
  return ret;
}

int ObLogicalOperator::get_op_exprs(ObIArray<ObRawExpr*> &all_exprs)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_array_no_dup(all_exprs, get_filter_exprs()))) {
    LOG_WARN("failed to append filter exprs", K(ret));
  } else if (OB_FAIL(append_array_no_dup(all_exprs, get_startup_exprs()))) {
    LOG_WARN("failed to get start up exprs", K(ret));
  } else if (is_plan_root() && OB_FAIL(append_array_no_dup(all_exprs, get_output_exprs()))) {
    LOG_WARN("failed to get output exprs", K(ret));
  } else { /*do noting*/ }
  return ret;
}

int ObLogicalOperator::get_next_producer_id(ObLogicalOperator *node,
                                            uint64_t &producer_id)
{
  int ret = OB_SUCCESS;
  producer_id = OB_INVALID_ID;
  if (OB_ISNULL(node)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else {
    bool is_find = false;
    while (NULL != node && !is_find) {
      if (IS_EXPR_PASSBY_OPER(node->get_type())) {
        node = node->get_child(first_child);
      } else {
        is_find = true;
        producer_id = node->get_operator_id();
      }
    }
    if (OB_SUCC(ret) && !is_find) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("can not find next producer id", K(ret));
    }
  }
  return ret;
}

int ObLogicalOperator::build_and_put_pack_expr(ObIArray<ObRawExpr*> &output_exprs)
{
  int ret = OB_SUCCESS;
  ObRawExpr *pack_expr = NULL;
  ObLogicalOperator *child = NULL;
  if (OB_ISNULL(get_plan()) || OB_ISNULL(get_plan()->get_optimizer_context().get_exec_ctx()) ||
      OB_ISNULL(get_plan()->get_optimizer_context().get_exec_ctx()->get_physical_plan_ctx()) ||
      OB_ISNULL(child = get_child(first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(get_plan()), K(child), K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_pack_expr(
       get_plan()->get_optimizer_context().get_expr_factory(),
       get_plan()->get_optimizer_context().is_ps_protocol(),
       get_plan()->get_optimizer_context().get_session_info(),
       get_plan()->get_optimizer_context().get_exec_ctx()->get_physical_plan_ctx()->get_field_array(),
       output_exprs,
       pack_expr))) {
    LOG_WARN("failed to build pack expr", K(ret));
  } else if (OB_ISNULL(pack_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FALSE_IT(output_exprs.reuse())) {
    /*do nothing*/
  } else if (OB_FAIL(output_exprs.push_back(pack_expr))) {
    LOG_WARN("failed to push back pack expr", K(ret));
  } else if (OB_FAIL(child->add_op_exprs(pack_expr))) {
    LOG_WARN("failed to add op exprs", K(ret));
  } else {
    get_plan()->get_optimizer_context().set_packed(true);
  }
  return ret;
}

int ObLogicalOperator::build_and_put_into_outfile_expr(const ObSelectIntoItem *into_item,
                                                       ObIArray<ObRawExpr*> &output_exprs)
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  ObRawExpr *to_outfile_expr = NULL;
  uint64_t producer_id = OB_INVALID_ID;
  if (OB_ISNULL(get_plan()) || OB_ISNULL(into_item) || OB_ISNULL(child = get_child(first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(get_plan()), K(into_item), K(child), K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_to_outfile_expr(
                                    get_plan()->get_optimizer_context().get_expr_factory(),
                                    get_plan()->get_optimizer_context().get_session_info(),
                                    into_item,
                                    output_exprs,
                                    to_outfile_expr))) {
    LOG_WARN("failed to build_to_outfile_expr", K(*into_item), K(ret));
  } else if (OB_ISNULL(to_outfile_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (FALSE_IT(output_exprs.reuse())) {
    /*do nothing*/
  } else if (OB_FAIL(output_exprs.push_back(to_outfile_expr))) {
    LOG_WARN("failed to push back exprs", K(ret));
  } else if (ObPhyPlanType::OB_PHY_PLAN_DISTRIBUTED == get_phy_plan_type() &&
             OB_FAIL(put_into_outfile_expr(to_outfile_expr))) {
    LOG_WARN("failed to push back expr", K(ret));
  } else { /*do nothing*/ }
  return ret;
}

int ObLogicalOperator::put_into_outfile_expr(ObRawExpr *to_outfile_expr)
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = NULL;
  if (OB_ISNULL(to_outfile_expr) ||
      OB_ISNULL(child = get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(to_outfile_expr), K(child), K(ret));
  } else {
    bool is_find = false;
    while (NULL != child && !is_find && 1 == child->get_num_of_child()) {
      if (IS_EXPR_PASSBY_OPER(child->get_type()) ||
          (log_op_def::LOG_EXCHANGE == (child->get_type()) &&
           static_cast<ObLogExchange*>(child)->is_consumer())) {
        child = child->get_child(first_child);
      } else {
        is_find = true;
      }
    }
    if (OB_ISNULL(child)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(child->add_op_exprs(to_outfile_expr))) {
      LOG_WARN("failed to add op exprs", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObLogicalOperator::add_exprs_to_ctx(ObAllocExprContext &ctx,
                                        const ObIArray<ObRawExpr*> &exprs)
{
  int ret = OB_SUCCESS;
  uint64_t producer_id = OB_INVALID_ID;
  if (OB_FAIL(get_next_producer_id(this, producer_id))) {
    LOG_WARN("failed to get next producer id", K(ret));
  } else if (OB_FAIL(add_exprs_to_ctx(ctx, exprs, producer_id))) {
    LOG_WARN("failed to add exprs to ctx", K(exprs), K(ret));
  } else { /*do nothing*/ }
  return ret;
}

int ObLogicalOperator::add_exprs_to_ctx(ObAllocExprContext &ctx,
                                        const ObIArray<ObRawExpr*> &input_exprs,
                                        uint64_t producer_id)
{
  int ret = OB_SUCCESS;
  uint64_t consumer_id = id_;
  ObSEArray<ObRawExpr*, 16> shared_exprs;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FAIL(extract_shared_exprs(input_exprs,
                                           ctx,
                                           shared_exprs))) {
    LOG_WARN("failed to extract column input_exprs", K(ret));
  } else {
    ObRawExpr *raw_expr = NULL;
    ExprProducer *raw_producer = NULL;
    // add shared exprs, should set both producer id and consumer id
    for (int64_t i = 0; OB_SUCC(ret) && i < shared_exprs.count(); i++) {
      uint64_t consumer_id = OB_INVALID_ID;
      if (OB_ISNULL(raw_expr = shared_exprs.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (OB_FAIL(ctx.find(raw_expr, raw_producer))) {
        LOG_WARN("failed to find raw expr producer", K(ret));
      } else if (NULL != raw_producer) {
        // update the raw_producer id
        raw_producer->is_shared_ = true;
        if (OB_INVALID_ID == raw_producer->producer_id_) {
          raw_producer->producer_id_ = producer_id;
        } else if (can_update_producer_id_for_shared_expr(raw_expr)) {
          raw_producer->producer_id_ = std::min(raw_producer->producer_id_, producer_id);
        } else { /*do nothing*/ }
        LOG_TRACE("succeed to update shared expr producer id", K(raw_expr),
                  K(*raw_expr), K(producer_id), K(consumer_id), KPC(raw_producer), K(get_name()));
      } else if (OB_FAIL(find_consumer_id_for_shared_expr(&ctx.expr_producers_, raw_expr, consumer_id))) {
        LOG_WARN("failed to find sharable expr consumer id", K(ret));
      } else if (OB_INVALID_ID == consumer_id) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected conusmer id", K(*raw_expr), K(ret));
      } else {
        ExprProducer expr_producer(raw_expr, consumer_id, producer_id);
        expr_producer.is_shared_ = true;
        if (OB_FAIL(ctx.add(expr_producer))) {
          LOG_WARN("failed to push balck raw_expr", K(ret));
        } else {
          LOG_TRACE("succeed to add shared exprs", K(raw_expr), K(*raw_expr),
              K(producer_id), K(expr_producer.consumer_id_), K(consumer_id), K(expr_producer),
              K(get_name()));
        }
      }
    }
    // add input expressions, should set both producer and consumer id
    for (int64_t i = 0; OB_SUCC(ret) && i < input_exprs.count(); i++) {
      if (OB_ISNULL(raw_expr = input_exprs.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (ObOptimizerUtil::find_item(shared_exprs, raw_expr)) {
        /*do nothing*/
      } else if (OB_FAIL(ctx.find(raw_expr, raw_producer))) {
        LOG_WARN("failed to raw expr producer", K(ret));
      } else if (NULL != raw_producer) {
        // update the raw_producer id
        if (OB_ISNULL(raw_producer)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret));
        } else {
          if (OB_INVALID_ID == raw_producer->producer_id_) {
            raw_producer->producer_id_ = producer_id;
          } else {
            raw_producer->producer_id_ = std::min(raw_producer->producer_id_, producer_id);
          }
          LOG_TRACE("succeed to update input expr producer id", K(raw_expr),
              K(*raw_expr), K(producer_id), K(consumer_id), KPC(raw_producer), K(get_name()));
        }
      } else {
        ExprProducer expr_producer(raw_expr, consumer_id, producer_id);
        if (OB_FAIL(ctx.add(expr_producer))) {
          LOG_WARN("failed to push balck raw_expr", K(ret));
        } else {
          LOG_TRACE("succeed to add input exprs", K(raw_expr), K(*raw_expr),
              K(producer_id), K(expr_producer.consumer_id_), K(consumer_id), K(expr_producer),
              K(get_name()));
        }
      }
    }
  }
  return ret;
}

bool ObLogicalOperator::can_update_producer_id_for_shared_expr(const ObRawExpr *expr)
{
  bool can = true;
  if (NULL != expr && expr->has_flag(IS_DYNAMIC_PARAM)) {
    if (log_op_def::LOG_SUBPLAN_FILTER == get_type()) {
      can = static_cast<ObLogSubPlanFilter*>(this)->is_my_exec_expr(expr);
    } else if (log_op_def::LOG_JOIN == get_type()) {
      can = static_cast<ObLogJoin*>(this)->is_my_exec_expr(expr);
    } else {
      can = false;
    }
  } else if (NULL != expr && expr->has_flag(IS_ROWNUM)) {
    can = (log_op_def::LOG_COUNT == get_type());
  }
  return can;
}

int ObLogicalOperator::extract_non_const_exprs(const ObIArray<ObRawExpr*> &input_exprs,
                                               ObIArray<ObRawExpr*> &non_const_exprs)
{
  int ret = OB_SUCCESS;
  ObRawExpr *expr = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < input_exprs.count(); i++) {
    if (OB_ISNULL(expr = input_exprs.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret), K(expr));
    } else if (expr->is_const_raw_expr() || 
               expr->has_flag(IS_USER_VARIABLE)) {
      /*do nothing*/
    } else if (OB_FAIL(non_const_exprs.push_back(expr))) {
      LOG_WARN("failed to push back expr", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObLogicalOperator::extract_shared_exprs(const ObIArray<ObRawExpr*> &exprs,
                                            ObAllocExprContext &ctx,
                                            ObIArray<ObRawExpr*> &shard_exprs)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < exprs.count(); i++) {
    if (OB_ISNULL(exprs.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(extract_shared_exprs(exprs.at(i),
                                             ctx,
                                             0,
                                             shard_exprs))) {
      LOG_WARN("failed to extract fix producer exprs", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObLogicalOperator::extract_shared_exprs(ObRawExpr *raw_expr,
                                             ObAllocExprContext &ctx,
                                             int64_t parent_ref_cnt,
                                             ObIArray<ObRawExpr*> &shard_exprs)
{
  int ret = OB_SUCCESS;
  int64_t ref_cnt = 0;
  if (OB_ISNULL(raw_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(raw_expr), K(ret));
  } else if (raw_expr->is_const_raw_expr()) {
    /*do nothing*/
  } else if (T_OP_ROW == raw_expr->get_expr_type()) {
    /*do nothing*/
  } else if (OB_FAIL(ctx.get_expr_ref_cnt(raw_expr, ref_cnt))) {
    LOG_WARN("failed to get expr ref cnt", K(ret));
  } else if (ref_cnt <= parent_ref_cnt) {
    /*do nothing*/
  } else if (OB_FAIL(add_var_to_array_no_dup(shard_exprs, raw_expr))) {
    LOG_WARN("failed to add var to array", K(ret));
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < raw_expr->get_param_count(); ++i) {
    ret = SMART_CALL(extract_shared_exprs(raw_expr->get_param_expr(i),
                                           ctx,
                                           ref_cnt,
                                           shard_exprs));
  }
  return ret;
}

int ObLogicalOperator::find_consumer_id_for_shared_expr(const ObIArray<ExprProducer> *ctx,
                                                        const ObRawExpr *expr,
                                                        uint64_t &consumer_id)
{
  int ret = OB_SUCCESS;
  consumer_id = OB_INVALID_ID;
  if (OB_ISNULL(ctx) || OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ctx->count(); i++) {
      if (OB_ISNULL(ctx->at(i).expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (ObOptimizerUtil::is_point_based_sub_expr(expr, ctx->at(i).expr_)) {
        if (OB_INVALID_ID == consumer_id) {
          consumer_id = ctx->at(i).consumer_id_;
        } else {
          consumer_id = std::max(consumer_id, ctx->at(i).consumer_id_);
        }
      } else { /*do nothing*/}
    }
  }
  return ret;
}

int ObLogicalOperator::allocate_expr_post(ObAllocExprContext &ctx)
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *parent = NULL;
  ObLogicalOperator *child = NULL;
  if (IS_EXPR_PASSBY_OPER(type_) && !is_plan_root()) {
    if (OB_ISNULL(child = get_child(ObLogicalOperator::first_child))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(output_exprs_.assign(child->get_output_exprs()))) {
      LOG_WARN("failed to assign output exprs", K(ret));
    } else { /*do nothing*/ }
  } else if (log_op_def::LOG_EXCHANGE == type_ &&
             static_cast<ObLogExchange*>(this)->get_is_remote() &&
             static_cast<ObLogExchange*>(this)->is_producer()) {
    if (OB_ISNULL(parent = get_parent()) ||
        OB_ISNULL(child = get_child(ObLogicalOperator::first_child))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(parent), K(child), K(ret));
    } else if (OB_FAIL(output_exprs_.assign(parent->get_output_exprs()))) {
      LOG_WARN("failed to assign output exprs", K(ret));
    } else if (OB_FAIL(child->get_output_exprs().assign(parent->get_output_exprs()))) {
      LOG_WARN("failed to push back exprs", K(ret));
    } else { /*do nothing*/ }
  } else {
    ObIArray<ExprProducer> &producers = ctx.expr_producers_;
    bool produced = false;
    do {
      produced = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < producers.count(); i++) {
        if (id_ >= producers.at(i).producer_id_) {
          ObRawExpr *expr = producers.at(i).expr_;
          if (OB_ISNULL(expr)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get unexpected null", K(ret));
          } else if (OB_INVALID_ID == producers.at(i).producer_branch_ &&
                     producers.at(i).producer_id_ == id_) { // not produced yet
            LOG_TRACE("try to produce expr", K(*expr), K(get_name()));
            bool can_be_produced = false;
            if (OB_FAIL(expr_can_be_produced(expr, ctx, can_be_produced))) {
              LOG_WARN("expr_can_be_produced fails", K(ret));
            } else if (can_be_produced) {
              produced = true;
              producers.at(i).producer_branch_ = branch_id_;
              producers.at(i).producer_id_ = id_;
              LOG_TRACE("expr can be produced now", K(*expr), K(get_name()), K(id_));
            } else {
              LOG_TRACE("expr can not be produced now", K(*expr), K(get_name()), K(id_));
            }
          } else { /*do nothing*/ }

          if (OB_SUCC(ret) && OB_INVALID_ID != producers.at(i).producer_branch_ && producers.at(i).consumer_id_ > id_) {
            if (!is_plan_root() && (is_child_output_exprs(expr) || producers.at(i).producer_id_ == id_)) {
              if (OB_FAIL(add_var_to_array_no_dup(output_exprs_, expr))) {
                LOG_WARN("failed to add expr to output", K(ret));
              } else {
                LOG_TRACE("expr is added into output expr", K(*expr), K(get_name()),
                    K(producers.at(i).consumer_id_), K(id_));
              }
            } else {
              LOG_TRACE("expr is not added into output expr",
                  K(*expr), K(get_name()), K(producers.at(i).consumer_id_), K(id_));
            }
          }
        } else { /* do nothing */ }
      }
    } while (OB_SUCC(ret) && produced);
  }
  return ret;
}

int ObLogicalOperator::expr_can_be_produced(const ObRawExpr *expr,
                                            ObAllocExprContext &expr_ctx,
                                            bool &can_be_produced)
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  const ObRawExpr *dependant_expr = NULL;
  can_be_produced = false;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(expr));
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("check stack overflow failed", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too deep recursive", K(ret));
  } else if (0 == expr->get_param_count()) {
    LOG_TRACE("checking if expr can be produced", K(expr), K(*expr), K(get_name()),
        K(id_), K(branch_id_));
    if (OB_FAIL(expr_has_been_produced(expr, expr_ctx, can_be_produced))) {
      LOG_WARN("failed to check whether expr_has_been_produced", K(ret));
    } else if (can_be_produced) {
      LOG_TRACE("expr has been produced", K(expr), K(get_name()), K(id_), K(branch_id_));
    } else if (!expr->has_flag(CNT_COLUMN)) {
      can_be_produced = true;
      LOG_TRACE("expr can be produced", K(expr), K(*expr), K(get_name()),
              K(id_), K(branch_id_));
    } else if (expr->is_column_ref_expr() &&
               NULL != (dependant_expr = static_cast<const ObColumnRefRawExpr*>(expr)->get_dependant_expr())) {
      if (OB_FAIL(expr_can_be_produced(dependant_expr, expr_ctx, can_be_produced))) {
        LOG_WARN("failed to check expr can be produced", K(ret));
      } else { /*do nothing*/ }
    } else {
      can_be_produced = false;
      LOG_TRACE("expr can not be produced", K(expr), K(*expr), K(get_name()),
          K(id_), K(branch_id_));
    }
  } else {
    LOG_TRACE("checking if expr can be produced", K(expr), K(*expr), K(expr->get_param_count()),
        K(get_name()), K(id_), K(branch_id_));
    can_be_produced = true;
    for (int64_t i = 0; OB_SUCC(ret) && can_be_produced && i < expr->get_param_count(); i++) {
      const ObRawExpr *param_expr = expr->get_param_expr(i);
      bool param_produced = false;
      if (OB_ISNULL(param_expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null ", K(ret));
      } else if (OB_FAIL(expr_has_been_produced(param_expr, expr_ctx, param_produced))) {
        LOG_WARN("failed to check whether expr_has_been_produced", K(ret));
      } else if (param_produced) {
        LOG_TRACE("input expr has been produced", K(param_expr), K(*param_expr),
            K(get_name()), K(id_), K(branch_id_));
      } else if (OB_FAIL(SMART_CALL(expr_can_be_produced(param_expr, expr_ctx, param_produced)))) {
        LOG_WARN("failed to check expr can be produced", K(ret));
      } else if (!param_produced) {
        // if any of their inputs is not produced yet, the expr cannot be produced
        can_be_produced = false;
        LOG_TRACE("input expr has not yet been produced", K(param_expr), K(*param_expr),
                  K(get_name()), K(id_), K(branch_id_));
      } else {
        LOG_TRACE("input expr not yet produced, but can be", K(param_expr), K(*param_expr),
                  K(get_name()), K(id_), K(branch_id_));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::expr_has_been_produced(const ObRawExpr *expr,
                                              ObAllocExprContext &expr_ctx,
                                              bool &has_been_produced)
{
  int ret = OB_SUCCESS;
  has_been_produced = false;
  ExprProducer *expr_producer = NULL;
  if (OB_FAIL(expr_ctx.find(expr, expr_producer)))  {
    LOG_WARN("failed to find expr producer", K(ret));
  } else if (NULL == expr_producer) {
    // not find
    /*
     *  The expression is only visible if its producer branch id is
     *  lager than the current branch. The reason is illustrated in
     *  the following graph:
     *
     *            op1(b_id = 0)
     *           /   \
     *          /     \
     *       op2(0)   op3(1)
     *
     *  In this case, 'b_id' is the branch id for each operator.
     *  op3 should not see any expressions that are produced
     *  by op2 whose branch id is smaller than op3's.
     *
     *  Note that we rely on the operator numbering to mark the branch
     *  id of each operator, without which the algorithm is not going to work.
     */
  } else if (OB_INVALID_ID != expr_producer->producer_branch_
              && expr_producer->producer_branch_ >= branch_id_) {
    has_been_produced = true;
  }
  return ret;
}

bool ObLogicalOperator::is_child_output_exprs(const ObRawExpr *expr) const
{
  bool is_child_output = false;
  for (int i = 0; !is_child_output && i < get_num_of_child(); i++) {
    if (NULL != get_child(i) &&
        ObOptimizerUtil::find_item(get_child(i)->get_output_exprs(), expr)) {
      is_child_output = true;
    } else { /*do nothing*/ }
  }
  return is_child_output;
}

int ObLogicalOperator::check_is_table_scan(const ObLogicalOperator &op,
                                           bool &is_table_scan)
{
  int ret = OB_SUCCESS;
  is_table_scan = false;
  const ObLogicalOperator *cur_op = &op;
  while (NULL != cur_op) {
    if (LOG_TABLE_SCAN == cur_op->get_type()) {
      is_table_scan = true;
      cur_op = NULL;
    } else if (LOG_MATERIAL == cur_op->get_type() ||
               LOG_JOIN_FILTER == cur_op->get_type() ||
               LOG_SORT == cur_op->get_type() ||
               LOG_TABLE_LOOKUP == cur_op->get_type() ||
               LOG_SUBPLAN_SCAN == cur_op->get_type()) {
      cur_op = cur_op->get_child(first_child);
    } else {
      cur_op = NULL;
    }
  }
  return ret;
}

int ObLogicalOperator::reorder_filter_exprs()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Get unexpeced null", K(ret), K(get_plan()));
  } else {
    FilterCompare filter_compare(get_plan()->get_predicate_selectivities());
    std::sort(filter_exprs_.begin(), filter_exprs_.end(), filter_compare);
  }
  return ret;
}

/**
 * 生成计划的location约束，有以下三种约束：
 * base_table_constraints_:
 *    基表location约束，包括TABLE_SCAN算子上的基表和INSERT算子上的基表
 * strict_pwj_constraint_:
 *    严格partition wise join约束，要求同一个分组内的基表分区逻辑上和物理上都相等。
 *    每个分组是一个array，保存了对应基表在base_table_constraints_中的偏移
 * non_strict_pwj_constraint_:
 *    严格partition wise join约束，要求用一个分组内的基表分区物理上相等。
 *    每个分组是一个array，保存了对应基表在base_table_constraints_中的偏移
 */
int ObLogicalOperator::gen_location_constraint(void *ctx)
{
  int ret = OB_SUCCESS;
  bool is_union_all = false;
  if (OB_ISNULL(ctx) || OB_ISNULL(get_stmt())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ctx is unexpected null", K(ret));
  } else {
    ObLocationConstraintContext *loc_cons_ctx = reinterpret_cast<ObLocationConstraintContext *>(ctx);

    if (get_num_of_child() == 0) {
      if (log_op_def::LOG_TABLE_SCAN == get_type()) {
        // base table constraints for TABLE SCAN
        LocationConstraint loc_cons;
        ObLogTableScan *log_scan_op = dynamic_cast<ObLogTableScan *>(this);
        if (log_scan_op->get_contains_fake_cte()) {
          // do nothing
        } else if (log_scan_op->use_das()) {
          // not add to constraints for das
        } else if (log_scan_op->get_dblink_id() != 0) {
          // dblink table, execute at other cluster
        } else if (OB_FAIL(get_tbl_loc_cons_for_scan(loc_cons))) {
          LOG_WARN("failed to get location constraint for table scan op", K(ret));
        } else if (OB_FAIL(loc_cons_ctx->base_table_constraints_.push_back(loc_cons))) {
          LOG_WARN("failed to push back location constraint", K(ret));
        } else if (OB_FAIL(strict_pwj_constraint_.push_back(
                    loc_cons_ctx->base_table_constraints_.count() - 1))) {
          LOG_WARN("failed to push back location constraint offset", K(ret));
        } else if (OB_FAIL(non_strict_pwj_constraint_.push_back(
                    loc_cons_ctx->base_table_constraints_.count() - 1))) {
          LOG_WARN("failed to push back location constraint offset", K(ret));
        }
      }
    } else if (get_num_of_child() > 0) {
      /**
       * 当前算子从孩子算子（insert包括自身）上继承的pwj约束的次数，如果继承次数超过1次，
       * 说明有多个pwj约束可以合并，就需要将当前算子的pwj约束添加到ctx中
       * e.g.        join3
       *            /    \         base_table_constraints = [t1,t2,t3,t4]
       *         join1   join2
       * 假设join1上有严格约束(0,1), join2上有严格约束(2,3), 那么join3应该继承并合并左右孩子节点的约束(0,1,2,3)
       */

      int64_t add_count = 0;
      bool is_pdml = false;
      if (log_op_def::LOG_SET == get_type()) {
        ObLogSet *set_op = static_cast<ObLogSet *>(this);
        is_union_all = (ObSelectStmt::UNION == set_op->get_set_op() && !set_op->is_set_distinct()) ||
                       set_op->is_recursive_union();
      }

      if (log_op_def::LOG_INSERT == get_type() &&
          !get_stmt()->has_instead_of_trigger() &&
          static_cast<ObLogInsert*>(this)->is_insert_select()) {
        // base table constraints for INSERT
        // multi part insert只记录基表location约束，非multi part insert需要同时记录
        // 基表location约束和partition wise join约束
        bool is_multi_part_dml = false;
        LocationConstraint loc_cons;
        if (OB_FAIL(get_tbl_loc_cons_for_insert(loc_cons, is_multi_part_dml))) {
          LOG_WARN("failed to get location constraint for insert op", K(ret));
        } else if (!is_multi_part_dml) {
          // 非multi part insert
          if (OB_FAIL(loc_cons_ctx->base_table_constraints_.push_back(loc_cons))) {
            LOG_WARN("failed to push back location constraint", K(ret));
          } else if (OB_FAIL(strict_pwj_constraint_.push_back(
                      loc_cons_ctx->base_table_constraints_.count() - 1))) {
            LOG_WARN("failed to push back location constraint offset", K(ret));
          } else if (OB_FAIL(non_strict_pwj_constraint_.push_back(
                      loc_cons_ctx->base_table_constraints_.count() - 1))) {
            LOG_WARN("failed to push back location constraint offset", K(ret));
          } else {
            ++add_count;
          }
        } else if (OB_FAIL(loc_cons_ctx->base_table_constraints_.push_back(loc_cons))) {
          LOG_WARN("failed to push back location constraint", K(ret));
        }
      }

      // 处理pdml index维护的location 约束
      // 目前支持的pdml logical op包含（insert，update，delete）
      // pdml logical op中需要维护phy table location的条件是：
      // 1. 当前逻辑算子是pdml
      // 2. 当前算子是用于维护global index table
      if (OB_SUCC(ret)) {
        if (log_op_def::LOG_DELETE == get_type()
            || log_op_def::LOG_UPDATE == get_type()) {
          ObLogDelUpd *dml_log_op = static_cast<ObLogDelUpd *>(this);
          if (dml_log_op->is_pdml()) {
            LocationConstraint loc_cons;
            is_pdml = true;
            if (OB_FAIL(get_tbl_loc_cons_for_pdml_index(loc_cons))) {
              LOG_WARN("failed to get table location constraint for pdml index",
                K(ret), K(get_name()));
            } else if (OB_FAIL(loc_cons_ctx->base_table_constraints_.push_back(loc_cons))) {
              LOG_WARN("failed to push back location constraint", K(ret));
            } else if (OB_FAIL(strict_pwj_constraint_.push_back(
                        loc_cons_ctx->base_table_constraints_.count() - 1))) {
              LOG_WARN("failed to push back location constraint offset", K(ret));
            } else if (OB_FAIL(non_strict_pwj_constraint_.push_back(
                        loc_cons_ctx->base_table_constraints_.count() - 1))) {
              LOG_WARN("failed to push back location constraint offset", K(ret));
            } else {
              ++add_count;
              LOG_TRACE("add location constraint for pdml index maintain op",
                K(ret), K(loc_cons), K(get_name()),
                "base_table_cons", loc_cons_ctx->base_table_constraints_);
            }
          }
        }
      }

      bool need_add_strict = true;
      bool need_add_non_strict = true;
      int64_t i = 0;
      if (log_op_def::LOG_TEMP_TABLE_TRANSFORMATION == get_type()) {
        // each branch of temp_table_transformation is isolated
        // only need to inherit the last branch
        i = get_num_of_child() - 1;
      }
      for (/* do nothing */; OB_SUCC(ret) && !is_pdml && i < get_num_of_child(); ++i) {
        if (OB_ISNULL(get_child(i))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret), K(i));
        } else if (LOG_EXCHANGE != get_child(i)->get_type()) {
          /*
          *
          *  MULTI PARTITION INSERT
          *     EXCHANGE IN DISTR
          *       EXCHANGE OUT DISTR
          *         MERGE JOIN
          *          TABLE SCAN    t1
          *          TABLE LOOKUP
          *            TABLE SCAN  t2(idx_t2)
          *
          *  t1的分区和idx_t2的分区需在同一个节点上，否则partition wise join 报4016
          *  因此table lookup 需要继承子节点的约束
          */
          LOG_TRACE("get child location constraint", K(i),
              K(get_child(i)->strict_pwj_constraint_), K(get_child(i)->non_strict_pwj_constraint_));
          if (get_child(i)->strict_pwj_constraint_.count() <= 0 &&
              get_child(i)->non_strict_pwj_constraint_.count() <= 0) {
            // 孩子节点没有记录pwj约束，说明这个分支上没有table scan或者存在exchange
          } else if (get_child(i)->strict_pwj_constraint_.count() <= 0 ||
                     get_child(i)->non_strict_pwj_constraint_.count() <= 0) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("strict pwj constraint and non strict pwj constraint count not valid",
                        K(ret), K(get_child(i)->strict_pwj_constraint_.count()),
                        K(get_child(i)->non_strict_pwj_constraint_.count()));
          } else {
            // 所有算子都会继承第一个可继承孩子节点的严格pwj约束和非严格pwj约束
            // union all算子会继承所有孩子节点的非严格pwj约束
            // 非union all算子会继承所有孩子节点的严格pwj约束
            ++add_count;
            if (need_add_strict) {
              if (OB_FAIL(append(strict_pwj_constraint_, get_child(i)->strict_pwj_constraint_))) {
                LOG_WARN("failed to append child pwj constraint", K(ret));
              } else if (is_union_all) {
                need_add_strict = false;
              }
            }
            if (need_add_non_strict) {
              if (OB_FAIL(append(non_strict_pwj_constraint_, get_child(i)->non_strict_pwj_constraint_))) {
                LOG_WARN("failed to append child pwj constraint", K(ret));
              } else if (!is_union_all) {
                need_add_non_strict = false;
              }
            }
          }
        }
      } // for end

      if (OB_SUCC(ret) && add_count > 1) {
        if (is_union_all) {
          if (OB_FAIL(loc_cons_ctx->non_strict_constraints_.push_back(&non_strict_pwj_constraint_))) {
            LOG_WARN("failed to push back pwj constraint");
          }
        } else if (OB_FAIL(loc_cons_ctx->strict_constraints_.push_back(&strict_pwj_constraint_))) {
          LOG_WARN("fail to push back pwj constraint", K(ret));
        }
      }
    } else { /* do nothing */ }
  }

  return ret;
}

int ObLogicalOperator::get_tbl_loc_cons_for_pdml_index(LocationConstraint &loc_cons)
{
  int ret = OB_SUCCESS;
  ObLogDelUpd *dml_log_op = static_cast<ObLogDelUpd *>(this);
  ObTableLocationType location_type = OB_TBL_LOCATION_UNINITIALIZED;
  ObShardingInfo *sharding = nullptr;
  if (OB_ISNULL(dml_log_op) || OB_ISNULL(sharding = dml_log_op->get_sharding())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dml logical operator is null", K(ret), K(dml_log_op));
  } else if (OB_FAIL(dml_log_op->get_table_location_type(location_type))) {
    LOG_WARN("failed to get location type", K(ret));
  } else {
    loc_cons.phy_loc_type_ = location_type;
    loc_cons.key_.table_id_ = dml_log_op->get_table_id();
    loc_cons.key_.ref_table_id_ = dml_log_op->get_index_tid();
    loc_cons.table_partition_info_ = dml_log_op->get_table_partition_info();
    if (sharding->get_part_cnt() > 1 && sharding->is_distributed()) {
      if (sharding->is_partition_single()) {
        loc_cons.add_constraint_flag(LocationConstraint::SinglePartition);
      } else if (sharding->is_subpartition_single()) {
        loc_cons.add_constraint_flag(LocationConstraint::SingleSubPartition);
      }
    }
  }
  return ret;
}

int ObLogicalOperator::get_tbl_loc_cons_for_scan(LocationConstraint &loc_cons)
{
  int ret = OB_SUCCESS;
  ObLogTableScan *log_scan_op = dynamic_cast<ObLogTableScan *>(this);
  ObShardingInfo *sharding = nullptr;
  if (OB_ISNULL(log_scan_op) || OB_ISNULL(sharding = log_scan_op->get_sharding())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(log_scan_op), K(sharding));
  } else if (OB_FAIL(log_scan_op->get_phy_location_type(loc_cons.phy_loc_type_))) {
    LOG_WARN("failed to get phy location type", K(ret));
  } else {
    loc_cons.key_.table_id_ = log_scan_op->get_table_id();
    loc_cons.table_partition_info_ = log_scan_op->get_table_partition_info();
    loc_cons.key_.ref_table_id_ = log_scan_op->get_real_index_table_id();
    if (sharding->get_part_cnt() > 1 && sharding->is_distributed()) {
      if (sharding->is_partition_single()) {
        loc_cons.add_constraint_flag(LocationConstraint::SinglePartition);
      } else if (sharding->is_subpartition_single()) {
        loc_cons.add_constraint_flag(LocationConstraint::SingleSubPartition);
      }
    }

    LOG_TRACE("initialized table's location constraint for table scan op", K(loc_cons));
  }
  return ret;
}

int ObLogicalOperator::get_tbl_loc_cons_for_insert(LocationConstraint &loc_cons, bool &is_multi_part_dml)
{
  int ret = OB_SUCCESS;
  ObTableLocationType location_type = OB_TBL_LOCATION_UNINITIALIZED;
  ObLogInsert *log_insert_op = static_cast<ObLogInsert *>(this);
  ObShardingInfo *sharding = nullptr;
  if (OB_ISNULL(log_insert_op) || OB_ISNULL(sharding = log_insert_op->get_sharding())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(log_insert_op), K(sharding));
  } else if (OB_FAIL(log_insert_op->get_table_location_type(location_type))) {
    LOG_WARN("failed to get location type", K(ret));
  } else {
    is_multi_part_dml = log_insert_op->is_multi_part_dml();
    loc_cons.phy_loc_type_ = location_type;
    loc_cons.key_.table_id_ = log_insert_op->get_loc_table_id();
    loc_cons.key_.ref_table_id_ = log_insert_op->get_index_tid();
    loc_cons.table_partition_info_ = log_insert_op->get_table_partition_info();
    if (is_multi_part_dml) {
      loc_cons.add_constraint_flag(LocationConstraint::IsMultiPartInsert);
    } else if (sharding->get_part_cnt() > 1 && sharding->is_distributed()) {
      if (sharding->is_partition_single()) {
        loc_cons.add_constraint_flag(LocationConstraint::SinglePartition);
      } else if (sharding->is_subpartition_single()) {
        loc_cons.add_constraint_flag(LocationConstraint::SingleSubPartition);
      }
    }
    LOG_TRACE("initialized table's location constraint for insert op", K(loc_cons), K(is_multi_part_dml));
  }

  return ret;
}

// under Gi operator, find the table scan whose table id is same as the argument table id
int ObLogicalOperator::get_table_scan(ObLogicalOperator *&tsc, uint64_t table_id)
{
  int ret = OB_SUCCESS;
  // Under GI, there are no GI, exchange
  if (LOG_GRANULE_ITERATOR == get_type() || LOG_EXCHANGE == get_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected operator type", K(get_type()), K(ret));
  } else if (nullptr == tsc && log_op_def::LOG_TABLE_SCAN == get_type() && table_id == static_cast<ObLogTableScan*>(this)->get_table_id()) {
    tsc = this;
  }
  for (int64_t i = 0; OB_SUCC(ret) && nullptr == tsc && i < get_num_of_child(); i++) {
    if (OB_ISNULL(get_child(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get_child(i) is null", K(ret), K(i));
    } else if (OB_FAIL(SMART_CALL(get_child(i)->get_table_scan(tsc, table_id)))) {
      LOG_WARN("failed to set op ordering in parts recursively", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObLogicalOperator::find_all_tsc(ObIArray<ObLogicalOperator *> &tsc_ops,
                                    ObLogicalOperator *root)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(root)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the log op is null", K(ret));
  } else if (log_op_def::LOG_EXCHANGE != get_type()) {
    for(int64_t i = 0; i < root->get_num_of_child(); i++) {
      ObLogicalOperator *child = root->get_child(i);
      if (OB_FAIL(find_all_tsc(tsc_ops, child))) {
        LOG_WARN("failed to find all tsc", K(ret));
      }
    }
  }

  if (OB_SUCC(ret) && log_op_def::LOG_TABLE_SCAN == root->get_type()) {
    if (OB_FAIL(tsc_ops.push_back(root))) {
      LOG_WARN("failed to push back tsc ops", K(ret));
    }
  }
  return ret;
}

int ObLogicalOperator::allocate_material(const int64_t index)
{
  int ret = OB_SUCCESS;
  ObLogPlan *plan = NULL;
  ObLogicalOperator *child = NULL;
  ObLogPlan *child_plan = NULL;
  ObLogicalOperator* mat_op = NULL;
  if (OB_ISNULL(plan = get_plan()) || OB_ISNULL(child = get_child(index)) ||
      OB_ISNULL(child_plan = child->get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(child), K(plan), K(child_plan));
  } else if (OB_ISNULL(mat_op = child_plan->get_log_op_factory().allocate(*child_plan, log_op_def::LOG_MATERIAL))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("failed to allocate material operator", K(ret));
  } else {
    mat_op->set_child(0, child);
    child->set_parent(mat_op);
    mat_op->set_parent(this);
    set_child(index, mat_op);
    if (OB_FAIL(mat_op->compute_property())) {
      LOG_WARN("failed to compute property");
    } else {
      mat_op->set_op_cost(0.0);
      mat_op->set_cost(child->get_cost());
    }
    //把Material当作下层LogPlan的顶点，否则没人会给这个Material分配表达式
    if (OB_SUCC(ret) && child->is_plan_root()) {
      if (OB_FAIL(mat_op->get_output_exprs().assign(child->get_output_exprs()))) {
        LOG_WARN("failed to assign output exprs", K(ret));
      } else {
        mat_op->mark_is_plan_root();
        child_plan->set_plan_root(mat_op);
        child->set_is_plan_root(false);
        child->get_output_exprs().reuse();
      }
    }
  }
  return ret;
}

int ObLogicalOperator::check_exchange_rescan(bool &need_rescan)
{
  int ret = OB_SUCCESS;
  need_rescan = false;
  if (LOG_EXCHANGE == get_type()) {
    need_rescan = true;
  } else if (LOG_MATERIAL == get_type()) { //TODO:遇到其他物化类算子也可以停止探测，但目前它们不支持真正意义的物化
    /*no nothing, stop checking deeply*/
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && !need_rescan && i < get_num_of_child(); ++i) {
      if (OB_ISNULL(get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get_child(i) returns null", K(i), K(ret));
      } else if (OB_FAIL(get_child(i)->check_exchange_rescan(need_rescan))) {
        LOG_WARN("get_child(i)->check_exchange_rescan() fails", K(ret));
      } else { /* Do nothing */ }
    }
  }
  return ret;
}

int ObLogicalOperator::check_has_exchange_below(bool &has_exchange) const
{
  int ret = OB_SUCCESS;
  has_exchange = false;
  if (LOG_EXCHANGE == get_type()) {
    has_exchange = true;
  } else {
    ObLogicalOperator *child_op = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && !has_exchange && i < get_num_of_child(); ++i) {
      if (OB_ISNULL(child_op = get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get_child(i) returns null", K(i), K(ret));
      } else if (OB_FAIL(SMART_CALL(child_op->check_has_exchange_below(has_exchange)))) {
        LOG_WARN("failed to check if child operator has exchange below", KPC(child_op), K(ret));
      } else { /* Do nothing */ }
    }
  }
  return ret;
}

bool ObLogicalOperator::is_all_expr_produced(const ObIArray<ExprProducer> &ctx)
{
  bool all_produced = true;
  for (int64_t i = 0; i < ctx.count(); i++) {
    if (ctx.at(i).not_produced()) {
      all_produced = false;
      LOG_WARN("failed to produce expression", K(ctx.at(i)));
    } else {
      LOG_TRACE("succ to produce expression", K(ctx.at(i)));
    }
  }
  return all_produced;
}

uint64_t ObLogicalOperator::hash(uint64_t seed) const
{
  seed = do_hash(type_, seed);
  seed = do_hash(id_, seed);
  LOG_TRACE("operator hash", K(get_op_name(type_)));
  return seed;
}

int ObLogicalOperator::numbering_operator_pre(NumberingCtx &ctx)
{
  int ret = OB_SUCCESS;
  op_id_ = ctx.op_id_++;
  plan_depth_ = ctx.plan_depth_++;
  if (LOG_COUNT == get_type()) {
    ObRawExpr *rownum_expr = NULL;
    if (OB_ISNULL(get_stmt())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("stmt is null", K(ret));
    } else if (OB_FAIL(get_stmt()->get_rownum_expr(rownum_expr))) {
      LOG_WARN("get rownum expr failed", K(ret));
    } else if (OB_ISNULL(rownum_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("no rownum expr in stmt of count operator", K(ret));
    } else {
      ObSysFunRawExpr *sys_rownum_expr = static_cast<ObSysFunRawExpr *>(rownum_expr);
      sys_rownum_expr->set_op_id(op_id_);
    }
  }
  if (ctx.going_up_) {
    ctx.branch_id_++;
    ctx.going_up_ = false;
  } else { /* Do nothing */ }

  if (OB_INVALID_ID == branch_id_) {
    branch_id_ = ctx.branch_id_;
  } else { /* Do nothing */ }
  return ret;
}

void ObLogicalOperator::numbering_operator_post(NumberingCtx &ctx)
{
  id_ = ctx.num_++;
  ctx.plan_depth_--;
  if (!ctx.going_up_) {
    ctx.going_up_ = true;
  } else { /* Do nothing */ }
}

int ObLogicalOperator::refine_dop_by_hint()
{
  int ret = OB_SUCCESS;
  ObQueryCtx *query_ctx = nullptr;
  if (OB_ISNULL(get_plan()) || OB_ISNULL(get_plan()->get_stmt()) ||
      OB_ISNULL(query_ctx = get_plan()->get_stmt()->get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt is NULL", K(ret));
  } else if (LOG_EXCHANGE == type_
             && static_cast<ObLogExchange*>(this)->is_producer()
             && get_plan()->get_optimizer_context().get_parallel() > 1) {
    // note: don't change single-step dfo sched. query's dop, which can't be parallized
    ObLogExchange *producer = static_cast<ObLogExchange*>(this);
    const ObIArray<ObDopHint>& dops = query_ctx->get_global_hint().dops_;
    ARRAY_FOREACH(dops, idx) {
      int64_t px_id = dops.at(idx).dfo_ / 10000;
      int64_t dfo_id = dops.at(idx).dfo_ % 10000;
      if (px_id == producer->get_px_id() && dfo_id == producer->get_dfo_id()) {
        producer->set_parallel(static_cast<int64_t>(dops.at(idx).dop_));
        LOG_DEBUG("XXXX: set op dop to hint value",
                 K(dops.at(idx).dop_), K(px_id), K(dfo_id), K(op_id_));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::alloc_md_post(AllocMDContext &ctx)
{
  int ret = OB_SUCCESS;
  UNUSED(ctx);
  ObQueryCtx *query_ctx = nullptr;
  if (OB_ISNULL(get_plan()) || OB_ISNULL(get_plan()->get_stmt()) ||
      OB_ISNULL(query_ctx = get_plan()->get_stmt()->get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt is NULL", K(ret));
  } else {
    const ObIArray<ObMonitorHint>& monitor_ids = query_ctx->get_global_hint().monitoring_ids_;
    ARRAY_FOREACH(monitor_ids, idx) {
      if (monitor_ids.at(idx).id_ == op_id_) {
        if (OB_FAIL(allocate_monitoring_dump_node_above(monitor_ids.at(idx).flags_, op_id_))) {
          LOG_WARN("Failed to allocate monitoring dump", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObLogicalOperator::numbering_exchange_pre(NumberingExchangeCtx &ctx)
{
  int ret = OB_SUCCESS;
  if (LOG_EXCHANGE == type_) {
    ObLogExchange *exchange_op = static_cast<ObLogExchange*>(this);
    if (exchange_op->is_px_consumer() && exchange_op->is_rescanable()) {
      ret = ctx.push_px(ctx.next_px());
    }
  }
  return ret;
}

int ObLogicalOperator::numbering_exchange_post(NumberingExchangeCtx &ctx)
{
  int ret = OB_SUCCESS;
  if (LOG_EXCHANGE == type_) {
    ObLogExchange *exchange_op = static_cast<ObLogExchange*>(this);
    if (exchange_op->is_px_producer()) {
      int64_t px_id = OB_INVALID_ID;
      int64_t dfo_id = OB_INVALID_ID;
      if (OB_FAIL(ctx.next_dfo(px_id, dfo_id))) {
        LOG_WARN("get next dfo id fail", K(ret));
      } else {
        exchange_op->set_dfo_id(dfo_id);
        exchange_op->set_px_id(px_id);
        // refine dop by DOP() hint
        if (OB_FAIL(refine_dop_by_hint())) {
          LOG_WARN("fail refine dfo dop", K(ret), K(dfo_id), K(px_id));
        }
      }
    } else if (exchange_op->is_px_consumer() && exchange_op->is_rescanable()) {
      ret = ctx.pop_px();
    }
  }
  return ret;
}

/*
 * 算法；每个节点在先序pre时进行push，后序post时pop，同时pop之前将自身的factor merge到parent上，
 * 这样可以将需要传递的factor传递给整颗树
 *            o(1)
 *          /       \
 *         o(2)     o(7)
 *      /       \
 *     o(3)     o(6)
 *    /   \
 *   o(4) o(5)
 * 数字表示入栈的顺序，即push操作
 */
int ObLogicalOperator::px_estimate_size_factor_pre()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(parent_)) {
    PxOpSizeFactor parent_factor = parent_->px_est_size_factor_;
    if (LOG_EXCHANGE != type_ && parent_factor.has_granule_child_factor()) {
      px_est_size_factor_.merge_factor(parent_factor.get_granule_child_factor());
    }
  }
  if (LOG_GRANULE_ITERATOR == type_) {
    bool partition_granule = false;
    ObLogGranuleIterator *log_gi = static_cast<ObLogGranuleIterator*>(this);
    if (OB_FAIL(log_gi->is_partition_gi(partition_granule))) {
      LOG_WARN("failed to judge partition gi", K(ret));
    } else {
      if (partition_granule) {
        px_est_size_factor_.partition_granule_child_ = true;
      } else {
        px_est_size_factor_.block_granule_child_ = true;
      }
    }
  }
  return ret;
}

// 这里统一将child的factor合并到parent，而不是通过从parent拉child的方式来处理facotr
int ObLogicalOperator::px_estimate_size_factor_post()
{
  int ret = OB_SUCCESS;
  // first process self, then some special process like exchange,join, then process parent
  if (LOG_TABLE_SCAN == type_ || LOG_TABLE_LOOKUP == type_) {
    if (!px_est_size_factor_.has_granule()) {
      px_est_size_factor_.single_partition_table_scan_ = true;
    }
  } else if (LOG_GRANULE_ITERATOR == type_) {
    bool partition_granule = false;
    ObLogGranuleIterator *log_gi = static_cast<ObLogGranuleIterator*>(this);
    px_est_size_factor_.revert_leaf_factor();
    if (OB_FAIL(log_gi->is_partition_gi(partition_granule))) {
      LOG_WARN("failed to judge partition gi", K(ret));
    } else {
      if (partition_granule) {
        px_est_size_factor_.partition_granule_parent_ = true;
      } else {
        px_est_size_factor_.block_granule_parent_ = true;
      }
    }
  } else if (LOG_EXCHANGE == type_) {
    ObLogExchange *exchange = static_cast<ObLogExchange*>(this);
    if (exchange->is_px_producer()) {
      // 跨shuffle，则reset所有，仅保留exchange相关
      px_est_size_factor_.revert_all();
      if (ObPQDistributeMethod::BROADCAST == exchange->get_dist_method()) {
        px_est_size_factor_.broadcast_exchange_ = true;
      } else if (ObPQDistributeMethod::PARTITION == exchange->get_dist_method()) {
        px_est_size_factor_.pk_exchange_ = true;
      }
    }
  }
  if (OB_SUCC(ret) && OB_NOT_NULL(parent_)) {
    PxOpSizeFactor child_factor = px_est_size_factor_;
    if (1 < parent_->get_num_of_child()) {
      if (LOG_JOIN == parent_->type_) {
        ObLogJoin *join = static_cast<ObLogJoin*>(parent_);
        // revert right of hash join exchange factor
        if (JoinAlgo::HASH_JOIN == join->get_join_algo()
          && join->get_child(second_child) == this) {
          child_factor.revert_exchange();
        }
      }
    }
    if (1 < get_num_of_child()) {
      // broadcast只服务于HashJoin，如果上层不是HashJoin的二元或多元Operator，则去掉Broadcast设置
      // 但其实有些plan也是基于Broadcast的，如
      //  HashJoin
      //    Union all
      //      Exchange(Broadcast)
      //        Ta
      //      Exchange(Broadcast)
      //        Tb
      //    Tc
      child_factor.revert_exchange();
    }
    parent_->px_est_size_factor_.merge_factor(child_factor);
    LOG_TRACE("trace estimate size factor", K(id_),
      "op factor", px_est_size_factor_,
      "parent factor", parent_->px_est_size_factor_);
  }
  return ret;
}

int ObLogicalOperator::px_rescan_pre()
{
  int ret = OB_SUCCESS;
  /* 一共有 3 中生成 QC 的场景：
   * 1. 顶层算子是 EXCHANGE，本身就是 QC
   * 2. 顶层算子下的一层 EXCHANGE 都是 QC
   * 3. SUBPLAN FILTER 下的一层 EXCHANGE 算子 (first_child 除外)
   * 4. Nested Loop Join (both children are pulled to local execution)
   * 5. Recursive union all
   */
  if ((is_plan_root() && nullptr == get_parent()) ||
       (NULL != get_parent() && get_parent()->get_type() == LOG_TEMP_TABLE_TRANSFORMATION)) {
    if (OB_FAIL(mark_child_exchange_rescanable())) {
      LOG_WARN("failed to mark child exchange rescanable", K(ret));
    } else { /*do nothing*/ }
  }
  if (OB_FAIL(ret)) {
  } else if (LOG_SUBPLAN_FILTER == type_) {
    if (OB_FAIL(check_subplan_filter_child_exchange_rescanable())) {
      LOG_WARN("mark child ex-receive as px op fail", K(ret));
    } else if (get_plan()->get_optimizer_context().get_parallel() <= 1 &&
        !static_cast<ObLogSubPlanFilter*>(this)->get_exec_params().empty() &&
        get_plan()->get_optimizer_context().enable_px_batch_rescan()) {
      common::ObIArray<bool>&enable_px_batch_rescans =
          static_cast<ObLogSubPlanFilter*>(this)->get_px_batch_rescans();
      bool find_px = false;
      bool nested_rescan = false;
      for (int i = 0; i < get_num_of_child() && OB_SUCC(ret); ++i) {
        find_px = false;
        nested_rescan = false;
        if (0 == i) {
          enable_px_batch_rescans.push_back(false);
        } else if (OB_FAIL(get_child(i)->find_nested_dis_rescan(nested_rescan, false))) {
          LOG_WARN("fail to find nested rescan", K(ret));
        } else if (nested_rescan) {
          /*do nothing*/
        } else if (OB_FAIL(get_child(i)->find_px_for_batch_rescan(log_op_def::LOG_SUBPLAN_FILTER,
              get_op_id(), find_px))) {
          LOG_WARN("fail to find px for batch rescan", K(ret));
        }
        if (OB_SUCC(ret) && 0 != i) {
          if (OB_FAIL(enable_px_batch_rescans.push_back(find_px))) {
            LOG_WARN("fail to push back find px", K(ret));
          }
        }
      }
    }
  } else if (LOG_JOIN == type_ &&
             JoinAlgo::NESTED_LOOP_JOIN == static_cast<ObLogJoin*>(this)->get_join_algo()
             && !(static_cast<ObLogJoin*>(this)->get_join_type() == CONNECT_BY_JOIN
                  && static_cast<ObLogJoin*>(this)->is_nlj_without_param_down())) {
    if (OB_ISNULL(get_child(second_child))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (LOG_MATERIAL == get_child(second_child)->get_type()) {
      /*do nothing*/
    } else if (OB_FAIL(get_child(second_child)->mark_child_exchange_rescanable())) {
      LOG_WARN("mark child ex-receive as px op fail", K(ret));
    } else if (static_cast<ObLogJoin*>(this)->get_join_type() != CONNECT_BY_JOIN &&
               static_cast<ObLogJoin*>(this)->is_nlj_with_param_down() &&
               !IS_SEMI_ANTI_JOIN(static_cast<ObLogJoin*>(this)->get_join_type()) &&
               get_plan()->get_optimizer_context().get_parallel() <= 1 &&
               get_plan()->get_optimizer_context().enable_px_batch_rescan()) {
      bool find_px = false;
      bool nested_rescan = false;
      if (OB_FAIL(get_child(second_child)->find_nested_dis_rescan(nested_rescan, false))) {
       LOG_WARN("fail to find nested rescan", K(ret));
      } else if (nested_rescan) {
        /*do nothing*/
      } else if (OB_FAIL(get_child(second_child)->find_px_for_batch_rescan(log_op_def::LOG_JOIN,
            get_op_id(), find_px))) {
          LOG_WARN("fail to find px for batch rescan", K(ret));
      } else if (find_px) {
        static_cast<ObLogJoin*>(this)->set_px_batch_rescan(true);
      }
    }
  } else if (LOG_SET == type_ &&
            static_cast<ObLogSet*>(this)->is_recursive_union()) {
    // recursive union all need to restart plan when rescan right child.
    if (OB_ISNULL(get_child(second_child))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(get_child(second_child)->mark_child_exchange_rescanable())) {
      LOG_WARN("mark child ex-receive as px op fail", K(ret));
    }
  }
  return ret;
}

int ObLogicalOperator::mark_child_exchange_rescanable()
{
  int ret = OB_SUCCESS;
  if (LOG_EXCHANGE == get_type()) {
    ObLogExchange *exchange_op = static_cast<ObLogExchange*>(this);
    if (exchange_op->is_consumer()) {
      exchange_op->set_rescanable(true);
    }
  } else {
    for (int64_t i = first_child; OB_SUCC(ret) && i < get_num_of_child(); ++i) {
      if (OB_ISNULL(get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get_child(i) returns null", K(i), K(ret));
      } else if (OB_FAIL(SMART_CALL(get_child(i)->mark_child_exchange_rescanable()))) {
        LOG_WARN("mark child ex-receive as px op fail", K(ret));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::check_output_dependance(ObIArray<ObRawExpr *> &child_output, PPDeps &deps)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr*, 8> exprs;
  LOG_TRACE("start to check output exprs", K(type_), K(child_output), K(deps));
  ObRawExprCheckDep dep_checker(child_output, deps, false);
  if (OB_FAIL(append(exprs, op_exprs_))) {
    LOG_WARN("failed to append exprs", K(ret));
  } else if (OB_FAIL(append_array_no_dup(exprs, output_exprs_))) {
    LOG_WARN("failed to append exprs", K(ret));
  } else if (OB_FAIL(dep_checker.check(exprs))) {
    LOG_WARN("failed to check output exprs", K(ret));
  } else {
    LOG_TRACE("succeed to check output exprs", K(exprs), K(type_), K(deps));
  }
  return ret;
}

int ObLogicalOperator::project_pruning_pre()
{
  int ret = OB_SUCCESS;
  // delete exprs who appeared in current op's output_exprs
  // but not used by it's parent's output_exprs_
  if (NULL != parent_ && !is_plan_root() && (LOG_EXPR_VALUES != type_) &&
      !(LOG_EXCHANGE == type_ && static_cast<ObLogExchange*>(this)->get_is_remote())) {
    PPDeps deps;
    if (OB_FAIL(parent_->check_output_dependance(get_output_exprs(), deps))) {
      LOG_WARN("parent_->check_output_dep() fails", K(ret));
    } else {
      do_project_pruning(get_output_exprs(), deps);
    }
  } else { /* do nothing */ }

  if (OB_FAIL(ret)) {
    /*do nothing*/
  } else if (LOG_TABLE_SCAN == type_) {
    ObLogTableScan *table_scan = static_cast<ObLogTableScan*>(this);
    PPDeps deps;
    if (OB_FAIL(check_output_dependance(table_scan->get_access_exprs(), deps))) {
      LOG_WARN("check_output_dep fails", K(ret));
    } else {
      do_project_pruning(table_scan->get_access_exprs(), deps);
    }
    if (OB_SUCC(ret) && OB_FAIL(table_scan->index_back_check())) {
      LOG_WARN("failed to check index back", K(ret));
    } else { /* Do nothing */ }
  } else if (LOG_SUBPLAN_SCAN == type_) {
    ObLogSubPlanScan *subplan_scan = static_cast<ObLogSubPlanScan*>(this);
    PPDeps deps;
    if (OB_FAIL(check_output_dependance(subplan_scan->get_access_exprs(), deps))) {
      LOG_WARN("check_output_dep fails", K(ret));
    } else {
      do_project_pruning(subplan_scan->get_access_exprs(), deps);
    }
  } else if (LOG_TEMP_TABLE_ACCESS == type_) {
    ObLogTempTableAccess *temp_scan = static_cast<ObLogTempTableAccess*>(this);
    PPDeps deps;
    if (OB_FAIL(check_output_dependance(temp_scan->get_access_exprs(), deps))) {
      LOG_WARN("check_output_dep fails", K(ret));
    } else {
      do_project_pruning(temp_scan->get_access_exprs(), deps);
    }
  } else if (LOG_TABLE_LOOKUP == type_) {
    ObLogTableLookup *tlu = static_cast<ObLogTableLookup*>(this);
    PPDeps deps;
    if (OB_FAIL(tlu->check_access_dependance(deps))) {
      LOG_WARN("failed to check access deps", K(ret));
    } else {
      do_project_pruning(tlu->get_access_exprs(), deps);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(try_add_remove_const_exprs())) {
    LOG_WARN("failed to add remove const exprs", K(ret));
  } else { /*do nothing*/ }

  return ret;
}

void ObLogicalOperator::do_project_pruning(ObIArray<ObRawExpr *> &exprs,
                                           PPDeps &deps)
{
  int64_t i = 0;
  int64_t j = 0;
  LOG_TRACE("start to do project pruning", K(type_), K(exprs), K(deps));
  for (i = 0, j = 0; i < exprs.count(); i++) {
    if (deps.has_member(static_cast<int32_t>(i))
        || T_ORA_ROWSCN == exprs.at(i)->get_expr_type()) {
      exprs.at(j++) = exprs.at(i);
    } else {
      LOG_TRACE("project pruning remove expr", K(exprs.at(i)), K(*exprs.at(i)),
          K(get_name()), K(lbt()));
    }
  }
  while (i > j) {
    exprs.pop_back();
    i--;
  }
}

int ObLogicalOperator::try_add_remove_const_exprs()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan()) || OB_ISNULL(get_plan()->get_optimizer_context().get_session_info())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(get_plan()), K(ret));
  } else {
    FOREACH_X(e, output_exprs_, OB_SUCC(ret)) {
      // Add remove_const() to above const expr, except:
      // - remove_const() already added. (has CNT_VOLATILE_CONST flag)
      // - is dynamic param store (has CNT_DYNAMIC_PARAM flag). Because question mark expr of
      //   dynamic param store may be passed by operator output. e.g.:
      //
      if (OB_ISNULL((*e))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if ((*e)->is_const_expr() &&
                 !(*e)->has_flag(CNT_VOLATILE_CONST) &&
                 !(log_op_def::LOG_EXCHANGE == get_type()
                   && static_cast<ObLogExchange*>(this)->is_producer()
                   && (*e)->has_flag(CNT_DYNAMIC_PARAM))) {
        ObRawExpr *remove_const_expr = NULL;
        if (OB_FAIL(ObRawExprUtils::build_remove_const_expr(
                                    get_plan()->get_optimizer_context().get_expr_factory(),
                                    *get_plan()->get_optimizer_context().get_session_info(),
                                    *e,
                                    remove_const_expr))) {
          LOG_WARN("failed to build remove const expr", K(output_exprs_), K(*e), K(ret));
        } else if (OB_ISNULL(remove_const_expr)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret));
        } else if (OB_FAIL(get_plan()->get_optimizer_context().get_all_exprs().append(remove_const_expr))) {
          LOG_WARN("faield to append exprs", K(ret));
        } else {
          *e = remove_const_expr;
        }
      }
    }
  }
  return ret;
}

int ObLogicalOperator::adjust_plan_root_output_exprs()
{
  int ret = OB_SUCCESS;
  const ObDMLStmt *stmt = NULL;
  ObSelectIntoItem *into_item = NULL;
  if (OB_ISNULL(stmt = get_stmt())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(stmt), K(ret));
  } else if (output_exprs_.empty()) {
    /*do nothing*/
  } else if (stmt->is_select_stmt() &&
             FALSE_IT(into_item = static_cast<const ObSelectStmt*>(get_stmt())->get_select_into())) {
    /*do nothing*/
  } else if (NULL == get_parent() && NULL != into_item && T_INTO_OUTFILE == into_item->into_type_) {
    if (OB_FAIL(build_and_put_into_outfile_expr(into_item, output_exprs_))) {
      LOG_WARN("failed to add into outfile expr to ctx", K(ret));
    } else {
      LOG_TRACE("succeed to add into outfile expr to ctx", K(ret));
    }
  } else if (NULL == get_parent() && check_stmt_can_be_packed(stmt)) {
    if (OB_FAIL(build_and_put_pack_expr(output_exprs_))) {
      LOG_WARN("failed to add pack expr to context", K(ret));
    } else {
      LOG_TRACE("succeed to add plan root output exprs", K(output_exprs_));
    }
  } else { /*do nothing*/ }

  return ret;
}

int ObLogicalOperator::set_plan_root_output_exprs()
{
  int ret = OB_SUCCESS;
  const ObDMLStmt *stmt = NULL;
  if (OB_ISNULL(stmt = get_stmt())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(stmt), K(ret));
  } else if (stmt->is_select_stmt()) {
    const ObSelectStmt *sel_stmt = static_cast<const ObSelectStmt*>(get_stmt());
    bool is_unpivot = (LOG_UNPIVOT == type_ && sel_stmt->is_unpivot_select());
    if (OB_FAIL(sel_stmt->get_select_exprs(output_exprs_, is_unpivot))) {
      LOG_WARN("failed to get select exprs", K(ret));
    } else { /*do nothing*/ }
  } else if (stmt->is_returning()) {
    const ObDelUpdStmt *del_upd_stmt = static_cast<const ObDelUpdStmt *>(stmt);
    if (OB_FAIL(append(output_exprs_, del_upd_stmt->get_returning_exprs()))) {
      LOG_WARN("failed to append returning exprs into output", K(ret));
    }
  } else { /*do nothing*/ }

  return ret;
}

bool ObLogicalOperator::check_stmt_can_be_packed(const ObDMLStmt *stmt)
{
  bool need_pack = false;
  ObSQLSessionInfo *session_info = NULL;
  if (NULL != stmt && NULL != get_plan() &&
      NULL != (session_info = get_plan()->get_optimizer_context().get_session_info())) {
    need_pack = stmt->is_select_stmt() && (!session_info->is_inner()) && LOG_EXCHANGE == type_
                 && (ObPhyPlanType::OB_PHY_PLAN_DISTRIBUTED == get_phy_plan_type());
  }
  return need_pack;
}

int ObLogicalOperator::replace_generated_agg_expr(
        const ObIArray<std::pair<ObRawExpr *, ObRawExpr *>  >&to_replace_exprs)
{
  int ret = OB_SUCCESS;
  if (0 < to_replace_exprs.count()) {
    FOREACH_CNT_X(it, get_op_ordering(), OB_SUCC(ret)) {
      if (OB_FAIL(replace_expr_action(to_replace_exprs, it->expr_))) {
        LOG_WARN("replace agg expr failed", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(replace_exprs_action(to_replace_exprs, get_filter_exprs()))) {
      LOG_WARN("failed to replace agg expr", K(ret));
    } else if (OB_FAIL(replace_exprs_action(to_replace_exprs, get_output_exprs()))) {
      LOG_WARN("failed to replace agg expr", K(ret));
    } else if (OB_FAIL(inner_replace_generated_agg_expr(to_replace_exprs))) {
      LOG_WARN("failed to inner replace agg expr", K(ret));
    } else { /* Do nothing */ }
  } else { /* Do nothing */ }
  return ret;
}

int ObLogicalOperator::inner_replace_generated_agg_expr(
        const ObIArray<std::pair<ObRawExpr *, ObRawExpr *>   >&to_replace_exprs)
{
  int ret = OB_SUCCESS;
  //do operator specific
  UNUSED(to_replace_exprs);
  return ret;
}

/*
 * pair: orig_expr  new_expr
 */
int ObLogicalOperator::replace_exprs_action(
        const ObIArray<std::pair<ObRawExpr *, ObRawExpr *>  >&to_replace_exprs,
        ObIArray<ObRawExpr *> &dest_exprs)
{
  int ret = OB_SUCCESS;
  int64_t src_num = to_replace_exprs.count();
  int64_t dest_num = dest_exprs.count();
  if (src_num > 0) {
    for (int64_t i = 0; OB_SUCC(ret) && i < dest_num; ++i) {
      ObRawExpr *&cur_expr = dest_exprs.at(i);
      if (OB_FAIL(replace_expr_action(to_replace_exprs, cur_expr))) {
        LOG_WARN("failed to do replace agg expr action", K(ret));
      } else { /* Do nothing */ }
    }
  } else { /* Do nothing */ }
  return ret;
}

int ObLogicalOperator::replace_expr_action(
        const ObIArray<std::pair<ObRawExpr *, ObRawExpr *>   >&to_replace_exprs,
        ObRawExpr *&dest_expr)
{
  int ret = OB_SUCCESS;
  int64_t src_num = to_replace_exprs.count();
  if (src_num > 0) {
    int64_t N = to_replace_exprs.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < N; ++i) {
      if (to_replace_exprs.at(i).first == to_replace_exprs.at(i).second) {
        // skip
      } else if (dest_expr == to_replace_exprs.at(i).first) {
        dest_expr = to_replace_exprs.at(i).second;
      } else {
        ObRawExprReplacer replacer(to_replace_exprs.at(i).first, to_replace_exprs.at(i).second);
        if (OB_ISNULL(dest_expr)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("dest_expr is null", K(ret));
        } else if (OB_FAIL(replacer.replace(*dest_expr))) {
          LOG_WARN("failed to replace expr", K(ret));
        } else { /* Do nothing */ }
      }
    }
  } else { /* Do nothing */ }
  return ret;
}

int ObLogicalOperator::check_sharding_compatible_with_reduce_expr(
                       const ObIArray<ObRawExpr*> &reduce_exprs,
                       bool &compatible) const
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr*, 4> part_exprs;
  ObSEArray<ObRawExpr*, 4> part_column_exprs;
  compatible = false;
  if (NULL == strong_sharding_) {
    /*do nothing*/
  } else if (OB_FAIL(strong_sharding_->get_all_partition_keys(part_exprs, true))) {
    LOG_WARN("failed to get all part keys", K(ret));
  } else if (0 == part_exprs.count()) {
    compatible = false;
  } else if (ObOptimizerUtil::subset_exprs(part_exprs,
                                           reduce_exprs,
                                           get_output_equal_sets())) {
    compatible = true;
  } else if (ObRawExprUtils::is_all_column_exprs(part_exprs)) {
    /*do nothing*/
  } else if (OB_FAIL(ObRawExprUtils::extract_column_exprs(part_exprs,
                                                          part_column_exprs))) {
    LOG_WARN("failed to extract column exprs", K(ret));
  } else if (ObOptimizerUtil::subset_exprs(part_column_exprs,
                                           reduce_exprs,
                                           get_output_equal_sets())) {
    compatible = true;
  } else { /*do nothing*/ }

  if (OB_SUCC(ret)) {
    LOG_TRACE("succeed to check sharding compatiable info", K(compatible), K(part_column_exprs),
        K(reduce_exprs));
  }
  return ret;
}

struct Compare
{
  Compare() {}
  bool operator()(const ObLogicalOperator::PartInfo &l, const ObLogicalOperator::PartInfo &r)
  {
    bool less = false;
    if (l.part_id_ < r.part_id_) {
      less = true;
    } else if (l.part_id_  == r.part_id_ && l.subpart_id_ < r.subpart_id_) {
      less = true;
    }
    return less;
  }
private:
  DISALLOW_COPY_AND_ASSIGN(Compare);
};

struct CopyableComparer
{
  CopyableComparer(Compare &compare) : compare_(compare) {}
  bool operator()(const ObLogicalOperator::PartInfo &l, const ObLogicalOperator::PartInfo &r)
  {
    return compare_(l, r);
  }
  Compare &compare_;
};

int ObLogicalOperator::explain_print_partitions(ObTablePartitionInfo &table_partition_info,
                                                char *buf, int64_t &buf_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  const ObCandiTabletLocIArray &partitions =
    table_partition_info.get_phy_tbl_location_info().get_phy_part_loc_info_list();
  uint64_t ref_table_id = table_partition_info.get_phy_tbl_location_info().get_ref_table_id();
  uint64_t table_id = table_partition_info.get_table_id();
  const bool two_level = (share::schema::PARTITION_LEVEL_TWO
                          == table_partition_info.get_part_level());
  ObSEArray<ObLogicalOperator::PartInfo, 64> part_infos;
  ObSqlSchemaGuard *schema_guard = NULL;
  ObOptimizerContext *opt_ctx = NULL;
  const share::schema::ObTableSchema *table_schema = NULL;
  const ObDMLStmt *stmt = NULL;
  if (OB_ISNULL(get_plan())
      || OB_ISNULL(opt_ctx = &get_plan()->get_optimizer_context())
      || OB_ISNULL(schema_guard = opt_ctx->get_sql_schema_guard())
      || OB_ISNULL(stmt = get_plan()->get_stmt())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("NULL pointer error", K(get_plan()), K(opt_ctx), K(schema_guard), K(ret));
  } else if (OB_FAIL(schema_guard->get_table_schema(ref_table_id, table_schema, ObSqlSchemaGuard::is_link_table(stmt, table_id)))) {
    LOG_WARN("fail to get index schema", K(ret), K(ref_table_id), K(table_id));
  }
  int64_t N = partitions.count();
  for (int64_t i = 0; OB_SUCC(ret) && i < N; i++) {
    ObLogicalOperator::PartInfo part_info;
    const ObOptTabletLoc &part_loc = partitions.at(i).get_partition_location();
    if (is_virtual_table(ref_table_id)) {
      if (VirtualSvrPair::EMPTY_VIRTUAL_TABLE_TABLET_ID == part_loc.get_partition_id()) {

      } else {
        part_info.part_id_ = part_loc.get_partition_id();
        OZ(part_infos.push_back(part_info));
      }
    } else if (!table_schema->is_partitioned_table()) {
      part_info.part_id_ = 0;
      OZ(part_infos.push_back(part_info));
    } else {
      const ObTabletID &tablet_id = part_loc.get_tablet_id();
      OZ(table_schema->get_part_idx_by_tablet(tablet_id, part_info.part_id_, part_info.subpart_id_));
      OZ(part_infos.push_back(part_info));
      LOG_TRACE("explain print partition", K(tablet_id), K(part_info), K(ref_table_id));
    }
  }
  if (OB_SUCC(ret)) {
    Compare cmp;
    std::sort(part_infos.begin(), part_infos.end(), CopyableComparer(cmp));
    if (OB_FAIL(ObLogicalOperator::explain_print_partitions(part_infos, two_level,
                                                            buf, buf_len, pos))) {
      LOG_WARN("Failed to print partitions");
    } else { }//do nothing
  }
  return ret;
}

int ObLogicalOperator::explain_print_partitions(
    const ObIArray<ObLogicalOperator::PartInfo> &part_infos,
    const bool two_level,
    char *buf,
    int64_t &buf_len,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  const int64_t count = part_infos.count();
  if (OB_ISNULL(buf)
      || buf_len <=0
      || pos < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Input arguments erro", K(ret), K(buf), K(buf_len), K(pos));
  } else if (OB_FAIL(BUF_PRINTF("partitions("))) {
    LOG_WARN("Failed to printf partitions", K(ret));
  } else if (0 == count){
    ret = BUF_PRINTF("nil");
  } else {
    bool continuous = false;
    bool cont_flag = false;
    for (int64_t idx = 0; OB_SUCC(ret) && idx < count; ++idx) {
      cont_flag = false;
      const int64_t id = part_infos.at(idx).part_id_;
      if (!continuous) {
        if (idx < count -1) {
          const int64_t n_id = part_infos.at(idx + 1).part_id_;
          if (two_level) {
            const int64_t p_id = id;
            const int64_t s_id = part_infos.at(idx).subpart_id_;
            const int64_t np_id = n_id;
            const int64_t ns_id = part_infos.at(idx + 1).subpart_id_;
            if (p_id == np_id && s_id == ns_id - 1) {
              ret = BUF_PRINTF("p%lusp[%lu-", p_id, s_id);
              continuous = true;
            } else {
              ret = BUF_PRINTF("p%lusp%lu", p_id, s_id);
            }
          } else if (id == n_id -1) {
            ret = BUF_PRINTF("p[%lu-", id);
            continuous = true;
          } else {
            ret = BUF_PRINTF("p%lu", id);
          }
        } else if(two_level) {
          const int64_t p_id = id;
          const int64_t s_id = part_infos.at(idx).subpart_id_;
          ret = BUF_PRINTF("p%lusp%lu", p_id, s_id);
        } else {
          ret = BUF_PRINTF("p%lu", id);
        }
      } else {
        if (idx >= count -1) {
          continuous = false;
        } else if (two_level) {
          const int64_t n_id = part_infos.at(idx + 1).part_id_;
          const int64_t p_id = id;
          const int64_t s_id = part_infos.at(idx).subpart_id_;
          const int64_t np_id = n_id;
          const int64_t ns_id = part_infos.at(idx + 1).subpart_id_;
          if (p_id != np_id ||
              s_id != (ns_id - 1)) {
            continuous = false;
          }
        } else if (id != part_infos.at(idx + 1).part_id_ - 1) {
          continuous = false;
        } else { }

        if (continuous) {
          cont_flag = true;
        } else if (two_level) {
          const int64_t s_id = part_infos.at(idx).subpart_id_;
          ret = BUF_PRINTF("%lu]", s_id);
        } else {
          ret = BUF_PRINTF("%lu]", id);
        }
      }
      if (!cont_flag && !continuous && (idx < count -1)) {
        ret = BUF_PRINTF(", ");
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = BUF_PRINTF(")");
  }
  return ret;
}

int ObLogicalOperator::allocate_granule_pre(AllocGIContext &ctx)
{
  int ret = OB_SUCCESS;
  UNUSED(ctx);
  return ret;
}

int ObLogicalOperator::allocate_granule_post(AllocGIContext &ctx)
{
	int ret = OB_SUCCESS;
	UNUSED(ctx);
	return ret;
}

int ObLogicalOperator::px_pipe_blocking_pre(ObPxPipeBlockingCtx &ctx)
{
  typedef ObPxPipeBlockingCtx::OpCtx OpCtx;
  UNUSED(ctx);
  int ret = OB_SUCCESS;
  OpCtx *op_ctx = static_cast<OpCtx *>(traverse_ctx_);
  OpCtx *child_op_ctx = NULL;
  if (OB_ISNULL(op_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("NULL traverse ctx", K(ret));
  }
  for (int64_t i = 0; i < get_num_of_child() && OB_SUCC(ret); i++) {
    if (NULL == get_child(i)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("NULL child", K(ret));
    } else if (OB_ISNULL(child_op_ctx = static_cast<OpCtx *>(
        get_child(i)->traverse_ctx_))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("NULL traverse ctx", K(ret));
    } else {
      child_op_ctx->dfo_depth_ = op_ctx->dfo_depth_;
      child_op_ctx->out_.set_exch(op_ctx->out_.is_exch() && !is_block_input(i));
    }
  }
  return ret;
}

int ObLogicalOperator::px_pipe_blocking_post(ObPxPipeBlockingCtx &ctx)
{
  typedef ObPxPipeBlockingCtx::OpCtx OpCtx;
  UNUSED(ctx);
  int ret = OB_SUCCESS;
  OpCtx *op_ctx = static_cast<OpCtx *>(traverse_ctx_);
  if (OB_ISNULL(op_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("NULL traverse ctx", K(ret));
  } else if (op_ctx->dfo_depth_ < 0) {
    // nothing to do, the current operator is not processed by PX
  } else {
    bool got_in_exch = false;
    int64_t child_dfo_cnt = 0;
    for (int64_t i = get_num_of_child() - 1; OB_SUCC(ret) && i >= 0; i--) {
      auto child = get_child(i);
      OpCtx *child_op_ctx = NULL;
      if (OB_ISNULL(child)
          || OB_ISNULL(child_op_ctx = static_cast<OpCtx *>(child->traverse_ctx_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("NULL child or NULL child traverse ctx", K(ret));
      } else {
        if (child_op_ctx->has_dfo_below_) {
          child_dfo_cnt += 1;
        }
        if (child_op_ctx->in_.is_exch() && !is_block_input(i)) {
          if (child_dfo_cnt > 1 && !is_consume_child_1by1()) {
            if (child->get_type() == LOG_DISTINCT
                && static_cast<ObLogDistinct*>(child)->get_algo() ==  HASH_AGGREGATE
                && !static_cast<ObLogDistinct *>(child)->is_push_down()) {
              static_cast<ObLogDistinct*>(child)->set_block_mode(true);
              LOG_DEBUG("distinct block mode", K(lbt()));
            } else if (OB_FAIL(allocate_material(i))) {
              LOG_WARN("allocate material failed", K(ret));
            }
          } else if (!got_in_exch) {
            got_in_exch = true;
          }
        }
        if (OB_SUCC(ret)) {
          if (LOG_JOIN == child->get_type()) {
            ObLogJoin *join = static_cast<ObLogJoin*>(child);
            bool need_alloc = false;
            // why we need allocate material op for BLOCK operator HJ?
            // Shared hash join requires strong synchronization between threads
            // If thread 1 get data from SHJ, others will wait it finish processing
            // Now dfc server block channel of thread 1, dead lock is happending
            if (join->is_shared_hash_join() && OB_FAIL(need_alloc_material_for_shared_hj(*child, need_alloc))) {
              LOG_WARN("check need allocate material failed", K(ret));
            } else if (need_alloc) {
              OZ (allocate_material(i));
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      op_ctx->in_.set_exch(got_in_exch);
      op_ctx->has_dfo_below_ = child_dfo_cnt > 0;
      LOG_TRACE("pipe blocking ctx", K(get_name()), K(*op_ctx));
    }
  }
  return ret;
}

int ObLogicalOperator::has_block_parent_for_shj(bool &has_non_block_shj)
{
  int ret = OB_SUCCESS;
  if (LOG_EXCHANGE == type_) {
    // don't recursive to do
  } else {
    if (LOG_JOIN == type_) {
      ObLogJoin *join = static_cast<ObLogJoin*>(this);
      if (join->is_shared_hash_join()) {
        has_non_block_shj = true;
      }
    }
    for (int64_t i = get_num_of_child() - 1; OB_SUCC(ret) && !has_non_block_shj && i >= 0; i--) {
      bool tmp_has_shj = false;
      auto child = get_child(i);
      if (OB_FAIL(child->has_block_parent_for_shj(tmp_has_shj))) {
        LOG_WARN("failed to process block parent", K(ret));
      } else if (tmp_has_shj) {
        if (!is_block_input(i)) {
          // need to find whether the parent is blocked
          has_non_block_shj = true;
          break;
        }
      }
    } // end for
  }
  return ret;
}

int ObLogicalOperator::check_has_temp_table_access(ObLogicalOperator *cur,
                                                   bool &has_temp_table_access)
{
  int ret = OB_SUCCESS;
  has_temp_table_access = false;
  ObLogicalOperator *first_child = NULL;
  if (NULL == cur) {
    /* do nothing */
  } else if (log_op_def::LOG_TEMP_TABLE_ACCESS == cur->get_type()) {
    has_temp_table_access = true;
  } else if (0 >= cur->get_num_of_child()) {
    /* do nothing */
  } else if (OB_ISNULL(first_child = cur->get_child(0))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("child is null", K(ret));
  } else if (OB_FAIL(SMART_CALL(check_has_temp_table_access(first_child, has_temp_table_access)))) {
    LOG_WARN("failed to check has temp table access", K(ret));
  }
  return ret;
}

int ObLogicalOperator::allocate_granule_nodes_above(AllocGIContext &ctx)
{
	int ret = OB_SUCCESS;
	bool partition_granule = false;
  bool has_temp_table_access = false;
  //  op    granule iterator
  //   |    ->    |
  //  other      op
  //              |
  //             other
	if (!ctx.alloc_gi_) {
		//do nothing
	} else if (OB_ISNULL(get_plan()) || OB_ISNULL(get_sharding())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Get unexpected null", K(ret), K(get_plan()), K(get_sharding()));
  } else if (!get_plan()->get_optimizer_context().get_temp_table_infos().empty() &&
             OB_FAIL(check_has_temp_table_access(this, has_temp_table_access))) {
    LOG_WARN("failed to check has temp table access", K(ret));
  } else if (has_temp_table_access || get_contains_fake_cte()) {
    // do not allocate granule nodes above temp table access now
    LOG_TRACE("do not allocate granule iterator due to temp table", K(get_name()));
  } else if (LOG_TABLE_SCAN != get_type()
             && LOG_JOIN != get_type()
             && LOG_SET != get_type()
             && LOG_GROUP_BY != get_type()
             && LOG_DISTINCT != get_type()
             && LOG_SUBPLAN_FILTER != get_type()
             && LOG_WINDOW_FUNCTION != get_type()
             && LOG_UPDATE != get_type()
             && LOG_DELETE != get_type()
             && LOG_INSERT != get_type()
             && LOG_MERGE != get_type()
             && LOG_FOR_UPD != get_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Only special op can allocate a granule iterator", K(get_type()));
  } else {
    ObLogicalOperator *log_op = NULL;
    ObLogOperatorFactory &factory = get_plan()->get_log_op_factory();
    if (OB_ISNULL(log_op = factory.allocate(*(get_plan()), LOG_GRANULE_ITERATOR))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("failed to allocate exchange nodes", K(log_op));
    } else {
      ObLogGranuleIterator *gi_op = static_cast<ObLogGranuleIterator *>(log_op);
      if (NULL != get_parent()) {
        bool found_child = false;
        for (int64_t i = 0; OB_SUCC(ret) && !found_child && i < get_parent()->get_num_of_child(); ++i) {
          if (get_parent()->get_child(i) == this) {
            get_parent()->set_child(i, gi_op);
            found_child = true;
          } else { /*do nothing*/ }
        }
        gi_op->set_parent(get_parent());
      } else { /*do nothing*/ }
      set_parent(gi_op);
      gi_op->set_child(first_child, this);
      gi_op->set_cost(get_cost());
      gi_op->set_card(get_card());
      gi_op->set_width(get_width());
      gi_op->set_parallel(get_parallel());
      gi_op->set_partition_count(ctx.partition_count_);
      gi_op->set_hash_part(ctx.hash_part_);
      gi_op->set_tablet_size(ctx.tablet_size_);

      if (ctx.is_in_pw_affinity_state()) {
        gi_op->add_flag(GI_AFFINITIZE);
        gi_op->add_flag(GI_PARTITION_WISE);
      }
      if (LOG_TABLE_SCAN == get_type() &&
          static_cast<ObLogTableScan *>(this)->get_join_filter_info().is_inited_) {
        ObLogTableScan *table_scan = static_cast<ObLogTableScan*>(this);
        ObOpPseudoColumnRawExpr *tablet_id_expr = NULL;
        if (OB_FAIL(generate_pseudo_partition_id_expr(tablet_id_expr))) {
          LOG_WARN("fail alloc partition id expr", K(ret));
        } else {
          gi_op->set_tablet_id_expr(tablet_id_expr);
          gi_op->set_join_filter_info(table_scan->get_join_filter_info());
          table_scan->set_tablet_id_expr(tablet_id_expr);
        }
      } else if (LOG_GROUP_BY == get_type()) {
        if (static_cast<ObLogGroupBy*>(this)->force_partition_gi()) {
          gi_op->add_flag(GI_PARTITION_WISE);
        }
      } else if (LOG_DISTINCT == get_type()) {
        if (static_cast<ObLogDistinct*>(this)->force_partition_gi()) {
          gi_op->add_flag(GI_PARTITION_WISE);
        }
      } else if (ctx.is_in_partition_wise_state()) {
        gi_op->add_flag(GI_PARTITION_WISE);
      } else { /*do nothing*/ }

      if (ctx.is_in_slave_mapping()) {
        gi_op->add_flag(GI_SLAVE_MAPPING);
      }

      if (OB_SUCC(ret)) {
        if (OB_FAIL(gi_op->is_partition_gi(partition_granule))) {
          LOG_WARN("failed judge partition granule", K(ret));
        }
      }

      if (ctx.force_partition() || partition_granule) {
        gi_op->add_flag(GI_FORCE_PARTITION_GRANULE);
      }

      if (OB_SUCC(ret)) {
        if (OB_FAIL(gi_op->compute_property())) {
          LOG_WARN("failed to compute equal sets", K(ret));
        }
      }

      if (OB_SUCC(ret) && is_plan_root()) {
        if (OB_FAIL(gi_op->get_output_exprs().assign(output_exprs_))) {
          LOG_WARN("failed to assign output exprs", K(ret));
        } else {
          gi_op->mark_is_plan_root();
          get_plan()->set_plan_root(gi_op);
          set_is_plan_root(false);
          output_exprs_.reuse();
        }
      } else { /*do nothing*/ }

      LOG_TRACE("succ to allocate granule iterator nodes above operator", K(get_name()),
                K(get_cost()), K(get_card()), K(ctx.is_in_pw_affinity_state()),
                K(ctx.is_in_partition_wise_state()));
    }
  }
	/*
	 * we try to allocate gi. whether success or failed, we reset the alloc_gi_.
	 * if we forget reset this var, may get some wrong log plan.
	 * */
  ctx.alloc_gi_ = false;
  return ret;
}

int ObLogicalOperator::set_granule_nodes_affinity(AllocGIContext &ctx, int64_t child_index)
{
	int ret = OB_SUCCESS;
	UNUSED(ctx);
	if (child_index >= get_num_of_child()) {
		ret = OB_ERR_UNEXPECTED;
		LOG_WARN("set granule affinity failed", K(ret));
	} else {
		ObLogicalOperator *child_op = get_child(child_index);
		if (LOG_GRANULE_ITERATOR == child_op->get_type()) {
			static_cast<ObLogGranuleIterator*>(child_op)->add_flag(GI_AFFINITIZE);
      if (ctx.enable_gi_partition_pruning_) {
			  static_cast<ObLogGranuleIterator*>(child_op)->add_flag(GI_ENABLE_PARTITION_PRUNING);
      }
    } else if (0 == child_op->get_num_of_child()) {
      // 单分区单parallel的情况下，我们不会分配GI；单分区本身就具有affinize的属性。
      LOG_TRACE("No GI operator found");
		} else if (child_op->get_num_of_child() != 1) {
			ret = OB_ERR_UNEXPECTED;
			LOG_WARN("set granule affinity failed, the child must be single child operator", K(ret), K(child_op->get_num_of_child()), K(child_op->get_type()));
		} else if (OB_FAIL(child_op->set_granule_nodes_affinity(ctx, 0))) {
			LOG_WARN("set granule affinity failed", K(ret));
		}
	}
	return ret;
}

int ObLogicalOperator::find_first_recursive(
    const log_op_def::ObLogOpType type, ObLogicalOperator *&op)
{
  int ret = OB_SUCCESS;
  op = NULL;
  if (type == get_type()) {
    op = this;
  } else if (NULL == get_child(first_child)) {
    /*do nothing*/
  } else {
    ret = get_child(first_child)->find_first_recursive(type, op);
  }
  return ret;
}

void ObLogicalOperator::clear_all_traverse_ctx()
{
  traverse_ctx_ = NULL;
  for (int64_t i = 0; i < get_num_of_child(); i++) {
    if (NULL != get_child(i)) {
      get_child(i)->clear_all_traverse_ctx();
    }
  }
}

int ObLogicalOperator::allocate_gi_recursively(AllocGIContext &ctx)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); i++) {
    if (OB_ISNULL(get_child(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get_child(i) returns unexpected null", K(ret), K(i));
    } else if (LOG_EXCHANGE == get_child(i)->get_type()) {
      //do not allocate gi over a dfo bound.
    } else if (OB_FAIL(get_child(i)->allocate_gi_recursively(ctx))) {
      LOG_WARN("failed to bottom-up traverse operator", K(ret), K(get_name()));
    } else { /* Do nothing */ }
  }
  if (OB_SUCC(ret)) {
    if (LOG_TABLE_SCAN == get_type()) {
      ObLogTableScan *scan = static_cast<ObLogTableScan*>(this);
      if (!scan->use_das()) {
        ctx.alloc_gi_ = true;
      }
      if (OB_FAIL(allocate_granule_nodes_above(ctx))) {
        LOG_WARN("allocate gi above table scan failed", K(ret));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::pw_allocate_granule_pre(AllocGIContext &ctx)
{
  int ret = OB_SUCCESS;
  /* (partition wise join/union/group/window function below)
   *                   |
   *                 OP(1)
   *                   |
   *             --------------
   *             |            |
   *            OP(2)        ...
   *             |
   *            ...
   *   OP(1) will set his id to gi allocate ctx as reset token,
   *   and in the post stage OP(1) will reset the state of
   *   gi allocate ctx.
   * */
  if (!ctx.exchange_above()) {
    LOG_TRACE("no exchange above, do nothing");
  } else if (!ctx.is_in_partition_wise_state()
      && !ctx.is_in_pw_affinity_state()
      && is_partition_wise()) {
    ctx.set_in_partition_wise_state(this);
    LOG_TRACE("in find partition wise state", K(*this));
  }
  return ret;
}

int ObLogicalOperator::pw_allocate_granule_post(AllocGIContext &ctx)
{
  int ret = OB_SUCCESS;
  /*
   * (partition wise join/union/group/window function below)
   *                   |
   *                 OP(1)
   *                   |
   *             --------------
   *             |            |
   *            OP(2)     ...
   *             |
   *            ...
   *   OP(1) will reset the state of gi allocate ctx.
   *   As the ctx has record the state was changed by OP(1),
   *   so OP(2) cann't reset this state.
   * */
  if (!ctx.exchange_above()) {
    LOG_TRACE("no exchange above, do nothing", K(ctx));
  } else if (ctx.is_op_set_pw(this)) {
    // In partition-wise join case, when GI is above group by/window function with pw attribute,
    // it doesn't support rescan before,
    // so it allocates the GI just above tsc and set attribute gi_random.
    // However, at most one gi_random can be supported within a DFO,
    // so that won't work in the plan found in this bug.
    // Now we support GI rescan in partition_wise_state so we just push up the GI here in such case
    // https://aone.alibaba-inc.com/task/32370920
    if (ctx.is_in_partition_wise_state() && is_fully_paratition_wise()) {
      ctx.alloc_gi_ = true;
      if (OB_FAIL(allocate_granule_nodes_above(ctx))) {
        LOG_WARN("allocate gi above table scan failed", K(ret));
      }
    } else if (ctx.is_in_partition_wise_state() || ctx.is_in_pw_affinity_state()) {
      if (OB_FAIL(allocate_gi_recursively(ctx))) {
        LOG_WARN("failed to allocate gi recursively", K(ret));
      }
    }
    IGNORE_RETURN ctx.reset_info();
  }
  return ret;
}

int ObLogicalOperator::allocate_link_post()
{
  /**
   * if all child nodes have same dblink id, just set dblink id of current node,
   * otherwise, add link op above those childs whose dblink id is not OB_INVALID_ID.
   */
  int ret = OB_SUCCESS;
  bool support_dblink = false;
  switch(get_type()) {  // white list of supported logical operator.
    case LOG_COUNT:         /*no break*/
    case LOG_DISTINCT:      /*no break*/
    case LOG_GROUP_BY:      /*no break*/
    case LOG_SET:
      support_dblink = true;
      break;
    case LOG_TABLE_SCAN:
      if (0 != get_dblink_id() &&
          OB_NOT_NULL(static_cast<ObLogTableScan*>(this)->get_offset_expr())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("dblink not support offset push down to table scan", K(ret), K(type_));
      } else {
        support_dblink = true;
      }
      break;
    case LOG_JOIN:
      support_dblink = CONNECT_BY_JOIN != static_cast<ObLogJoin*>(this)->get_join_type();
      break;
    case LOG_SORT:
      support_dblink = NULL == static_cast<ObLogSort*>(this)->get_topn_expr();
      break;
    case LOG_SUBPLAN_SCAN:
      /**
       * Temporarily close the reverse spelling of subplanscan op
       * until the optimizer team completes the col_name of the outputs of the set op
       * and the col_name of the outputs of its(set op) parent node.
      */
      support_dblink = false;
      break;
    default:
      support_dblink = false;
      break;
  }
  if (support_dblink) {
    uint64_t dblink_id = (get_num_of_child() > 0 && OB_NOT_NULL(get_child(0))) ?
                         get_child(0)->get_dblink_id() : 0;
    for (int64_t i = 1; OB_SUCC(ret) && i < get_num_of_child(); i++) {
      if (OB_ISNULL(get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get_child(i) returns unexpected null", K(ret), K(i));
      } else if (dblink_id != get_child(i)->get_dblink_id()) {
        dblink_id = 0;
        break;
      }
    }
    if (OB_SUCC(ret) && 0 != dblink_id) {
      set_dblink_id(dblink_id);
    }
  }
  // the code above is used to op that have child,
  // the code below is used to all op.
  if (OB_SUCC(ret)) {
    if (0 != get_dblink_id()) {
      /*
       * ---------------------------------------------
       * |0 |MERGE UNION DISTINCT|    |6        |75  |
       * |1 | TABLE SCAN         |T1  |3        |37  |
       * |2 | TABLE SCAN         |T2  |3        |37  |
       * =============================================
       * DO NOT use is_plan_root(), op 0/1/2 all return true, use 'NULL == get_parent()'.
       */
      if (NULL == get_parent() && OB_FAIL(allocate_link_node_above(-1))) {
        LOG_WARN("allocate link node above failed", K(ret));
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); i++) {
        if (OB_ISNULL(get_child(i))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get_child(i) returns unexpected null", K(ret), K(i));
        } else if (0 == get_child(i)->get_dblink_id()) {
          // skip.
        } else if (OB_FAIL(get_child(i)->allocate_link_node_above(i))) {
          LOG_WARN("allocate link node above failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObLogicalOperator::allocate_startup_expr_post()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); ++i) {
    if (OB_FAIL(allocate_startup_expr_post(i))) {
      LOG_WARN("failed to allocate startup expr post", K(i), K(ret));
    }
  }
  return ret;
}

int ObLogicalOperator::allocate_startup_expr_post(int64_t child_idx)
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *child = get_child(child_idx);
  if (OB_ISNULL(child)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpect null child", K(ret));
  } else if (is_dml_operator() ||
            log_op_def::LOG_TEMP_TABLE_INSERT == get_type()) {
    //do nothing
  } else if (child->get_startup_exprs().empty()) {
    //do nothing
  } else {
    ObSEArray<ObRawExpr*, 4> non_startup_exprs, new_startup_exprs;
    ObIArray<ObRawExpr*> &startup_exprs = child->get_startup_exprs();
    for (int64_t i = 0; OB_SUCC(ret) && i < startup_exprs.count(); ++i) {
      if (OB_ISNULL(startup_exprs.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpect null expr", K(ret));
      } else if (startup_exprs.at(i)->has_flag(CNT_ROWNUM) ||
                 startup_exprs.at(i)->has_flag(CNT_DYNAMIC_PARAM)) {
        if (OB_FAIL(non_startup_exprs.push_back(startup_exprs.at(i)))) {
          LOG_WARN("failed to push back expr", K(ret));
        }
      } else if (OB_FAIL(new_startup_exprs.push_back(startup_exprs.at(i)))) {
        LOG_WARN("failed to push back expr", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(add_startup_exprs(new_startup_exprs))) {
        LOG_WARN("failed to add startup exprs", K(ret));
      } else {
        //exchange out上面的startup filter保留，用于控制当前dfo提前终止
        bool mark_exchange_out = false;
        if (log_op_def::LOG_EXCHANGE == child->get_type()) {
          ObLogExchange *exchange_out = static_cast<ObLogExchange*>(child);
          if (exchange_out->is_px_producer()) {
            if (log_op_def::LOG_EXCHANGE == get_type()) {
              ObLogExchange *exchange_in = static_cast<ObLogExchange*>(this);
              if (!exchange_in->is_rescanable()) {
                mark_exchange_out = true;
              }
            }
          }
        }
        if (!mark_exchange_out) {
          if (OB_FAIL(child->get_startup_exprs().assign(non_startup_exprs))) {
            LOG_WARN("failed to assign exprs", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObLogicalOperator::collect_link_sql_context_pre(GenLinkStmtPostContext &link_ctx)
{
  UNUSED(link_ctx);
  return OB_SUCCESS;
}

int ObLogicalOperator::copy_part_expr_pre(CopyPartExprCtx &ctx)
{
  UNUSED(ctx);
  return OB_SUCCESS;
}

int ObLogicalOperator::copy_part_expr(CopyPartExprCtx &ctx, ObRawExpr* &calc_part_id_expr)
{
  int ret = OB_SUCCESS;
  bool is_on_null_side = false;
  int64_t idx = OB_INVALID_INDEX;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpect null plan", K(ret));
  } else if (!ObOptimizerUtil::find_item(ctx.used_part_exprs_, calc_part_id_expr)) {
    if (OB_FAIL(ctx.used_part_exprs_.push_back(calc_part_id_expr))) {
      LOG_WARN("failed to push back expr", K(ret));
    }
  } else if (ObOptimizerUtil::find_item(ctx.cache_lookup_calc_part_id_exprs_,
                                        calc_part_id_expr, &idx)) {
    if (OB_UNLIKELY(idx < 0 || idx >= ctx.new_lookup_calc_part_id_exprs_.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get invalid idx", K(ret));
    } else {
      calc_part_id_expr = ctx.new_lookup_calc_part_id_exprs_.at(idx);
    }
  } else if (OB_FAIL(check_is_on_null_side(this, is_on_null_side))) {
    LOG_WARN("failed to check is on null side", K(ret));
  } else if (is_on_null_side && (log_op_def::LOG_TABLE_LOOKUP != get_type())) {
    // the calc_part_id_expr in tlu is copied at ObLogTableLookup::do_copy_calc_part_id_expr
    if (OB_FAIL(ObRawExprUtils::copy_expr(get_plan()->get_optimizer_context().get_expr_factory(),
                                              calc_part_id_expr,
                                              calc_part_id_expr,
                                              COPY_REF_DEFAULT))) {
      LOG_WARN("failed to copy expr", K(ret));
    }
  }
  LOG_TRACE("succeed to copy calc_part_id_expr", K(ret), K(get_name()),
            K(idx), K(is_on_null_side), K(*calc_part_id_expr), K(calc_part_id_expr));
  return ret;
}

int ObLogicalOperator::check_is_on_null_side(ObLogicalOperator *op, bool &is_on_null_side)
{
  int ret = OB_SUCCESS;
  is_on_null_side = false;
  if (OB_ISNULL(op)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpect null log op", K(ret));
  } else if (op->is_plan_root()) {
    //do nothing
  } else {
    ObLogicalOperator *parent = op->get_parent();
    ObLogJoin *join_op = static_cast<ObLogJoin*>(parent);
    if (OB_ISNULL(parent)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect null log op", K(ret));
    } else if (log_op_def::LOG_JOIN != parent->get_type()) {
      //do nothing
    } else if (!IS_OUTER_JOIN(join_op->get_join_type())) {
      //do nothing
    } else if (FULL_OUTER_JOIN == join_op->get_join_type()) {
      is_on_null_side = true;
    } else if (LEFT_OUTER_JOIN == join_op->get_join_type()) {
      is_on_null_side = op == join_op->get_child(second_child);
    } else if (RIGHT_OUTER_JOIN == join_op->get_join_type()) {
      is_on_null_side = op == join_op->get_child(first_child);
    }
    if (OB_SUCC(ret) && !is_on_null_side) {
      if (OB_FAIL(SMART_CALL(check_is_on_null_side(parent, is_on_null_side)))) {
        LOG_WARN("failed to check is on null side", K(ret));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::generate_link_sql_post(GenLinkStmtPostContext &link_ctx)
{
  UNUSED(link_ctx);
  return OB_SUCCESS;
}

int ObLogicalOperator::allocate_link_node_above(int64_t child_idx)
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *link = NULL;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Get unexpected null", K(ret), K(get_plan()));
  } else if (OB_ISNULL(link = get_plan()->get_log_op_factory().allocate(*(get_plan()), LOG_LINK))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("failed to allocate link node");
  } else {
    /**
     * now we pull all link table rows to local, then add exchange op if need repart, like:
     *
     *         join                        join
     *           |                           |
     *     -------------       =>      -------------
     *     |           |               |           |
     * part_table  link_table      part_table   exchange
     *               (local)                    (repart)
     *                                             |
     *                                         link_table
     *                                           (local)
     *
     * the problem is the data from link table maybe transfer twice on network,
     * because we can only use sql to communicate between different clusters,
     * and sql have no repart ability, so we must repart all data in local cluster.
     * BUT, the problem may be solved in certain situation, such as part_table and link_table
     * can wise join regardless phy location, the transmit op only do data-transfer-job
     * without any repart job.
     * ps:
     * to achieve this optimization, we need add partition info into link table schema,
     * then every local part read another part from remote cluster using "from t1 partition(p0)",
     * YES we use from clause "partition(p0)" instead of exchange op, remember that we can not
     * execute exchange op in remote cluster even if there is no need to repart.
     */
    if (NULL != get_parent()) {
      get_parent()->set_child(child_idx, link);
      link->set_parent(get_parent());
    }
    link->set_child(0, this);
    this->set_parent(link);
    link->set_dblink_id(get_dblink_id());
    if (OB_FAIL(link->compute_property())) {
      LOG_WARN("failed to compute property", K(ret));
    }

    if (OB_SUCC(ret) && is_plan_root()) {
      if (OB_FAIL(link->get_output_exprs().assign(output_exprs_))) {
        LOG_WARN("failed to assign output exprs", K(ret));
      } else {
        link->mark_is_plan_root();
        get_plan()->set_plan_root(link);
        this->set_is_plan_root(false);
        output_exprs_.reuse();
      }
    }
  }
  return ret;
}

bool AllocGIContext::is_in_partition_wise_state()
{
  return GIS_IN_PARTITION_WISE == state_;
};

void AllocGIContext::set_in_partition_wise_state(ObLogicalOperator *op_ptr)
{
  state_ = GIS_IN_PARTITION_WISE;
  pw_op_ptr_ = op_ptr;
}

bool AllocGIContext::is_op_set_pw(ObLogicalOperator *op_ptr)
{
  return pw_op_ptr_ == op_ptr;
}

bool AllocGIContext::try_set_out_partition_wise_state(ObLogicalOperator *op_ptr)
{
  bool result = false;
  if (pw_op_ptr_ == op_ptr) {
    state_ = GIS_NORMAL;
    result = true;
  }
  return result;
}

int AllocGIContext::set_pw_affinity_state()
{
  int ret = OB_SUCCESS;
  if (GIS_IN_PARTITION_WISE == state_) {
    state_ = GIS_PARTITION_WITH_AFFINITY;
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("set partition-wise-affinity state without partition-wise state is forbidden", K(ret));
  }
  return ret;
}

void AllocGIContext::reset_info()
{
  state_ = GIS_NORMAL;
  multi_child_op_above_count_in_dfo_ = 0;
  enable_gi_partition_pruning_ = false;
}

AllocGIContext::GIState AllocGIContext::get_state()
{
  return state_;
}

bool AllocGIContext::managed_by_gi()
{
  return GIS_NORMAL != state_;
}

bool AllocGIContext::is_in_pw_affinity_state()
{
  return GIS_PARTITION_WITH_AFFINITY == state_;
}

void ObLogicalOperator::set_parent(ObLogicalOperator *parent)
{
  if (parent) {
  LOG_TRACE("set parent",
           "op_name", this->get_name(),
           "parent_op_name", parent->get_name(),
           "before", parent_, "after", parent, K(lbt()));
  } else {
    LOG_TRACE("set parent",
             "op_name", this->get_name(),
             "before", parent_, "after", parent, K(lbt()));
  }
  parent_ = parent;
}

int ObLogicalOperator::allocate_monitoring_dump_node_above(uint64_t flags, uint64_t dst_op_id)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Get unexpected null", K(ret), K(get_plan()));
  } else if (LOG_EXCHANGE == get_type() &&
             (static_cast<ObLogExchange*>(this)->is_producer() ||
             (static_cast<ObLogExchange*>(this)->is_consumer() && static_cast<ObLogExchange*>(this)->get_is_remote()))) {
    // Do nothing.
  } else {
    ObLogicalOperator *log_op = NULL;
    ObLogOperatorFactory &factory = get_plan()->get_log_op_factory();
    if (OB_ISNULL(log_op = factory.allocate(*(get_plan()), LOG_MONITORING_DUMP))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("Failed to allocate exchange nodes", K(log_op));
    } else {
      ObLogMonitoringDump *monitoring_dump = static_cast<ObLogMonitoringDump *>(log_op);
      if (NULL != get_parent()) {
        bool found_child = false;
        for (int64_t i = 0; OB_SUCC(ret) && !found_child && i < get_parent()->get_num_of_child(); ++i) {
          if (get_parent()->get_child(i) == this) {
            get_parent()->set_child(i, monitoring_dump);
            found_child = true;
          } else { /*do nothing*/ }
        }
        monitoring_dump->set_parent(get_parent());
      } else { /*do nothing*/ }
      set_parent(monitoring_dump);
      monitoring_dump->set_child(first_child, this);
      monitoring_dump->set_cost(get_cost());
      monitoring_dump->set_card(get_card());
      monitoring_dump->set_width(get_width());
      monitoring_dump->set_flags(flags);
      monitoring_dump->set_dst_op_id(dst_op_id);

      if (OB_SUCC(ret)) {
        if (OB_FAIL(monitoring_dump->compute_property())) {
          LOG_WARN("Failed to compute equal sets", K(ret));
        }
      }

      if (OB_SUCC(ret) && is_plan_root()) {
        if (OB_FAIL(monitoring_dump->get_output_exprs().assign(output_exprs_))) {
          LOG_WARN("failed to assign output exprs", K(ret));
        } else {
          monitoring_dump->mark_is_plan_root();
          get_plan()->set_plan_root(monitoring_dump);
          set_is_plan_root(false);
          output_exprs_.reuse();
        }
      }
      LOG_TRACE("succ to allocate monitoring dump node above operator", K(get_name()),
                K(get_cost()), K(get_card()), K(flags));
    }
  }
  return ret;
}

int ObLogicalOperator::push_down_bloom_filter_expr(ObLogicalOperator *op,
    ObLogicalOperator *join_filter_op,  double join_filter_rate)
{
  int ret = OB_SUCCESS;
  CK(OB_NOT_NULL(op) && OB_NOT_NULL(join_filter_op));
  if (OB_SUCC(ret)) {
    ObLogJoinFilter *join_filter_use = static_cast<ObLogJoinFilter *>(join_filter_op);
    common::ObIArray<ObRawExpr *> &exprs = op->get_filter_exprs();
    ObRawExprFactory &expr_factory = get_plan()->get_optimizer_context().get_expr_factory();
    common::ObIArray<ObRawExpr *> &join_exprs = join_filter_use->get_join_exprs();
    ObOpRawExpr *join_filter_expr = NULL;
    ObSQLSessionInfo *session_info = get_plan()->get_optimizer_context().get_session_info();
    if (OB_FAIL(expr_factory.create_raw_expr(T_OP_JOIN_BLOOM_FILTER, join_filter_expr))) {
      LOG_WARN("fail to create raw expr", K(ret));
    } else {
      for (int i = 0; i < join_exprs.count() && OB_SUCC(ret); ++i) {
        if (OB_FAIL(join_filter_expr->add_param_expr(join_exprs.at(i)))) {
          LOG_WARN("fail to add param expr", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(join_filter_expr->formalize(session_info))) {
          LOG_WARN("fail to formalize expr", K(ret));
        } else if (OB_FAIL(exprs.push_back(join_filter_expr))) {
          LOG_WARN("fail to to push back expr", K(ret));
        } else if (OB_FAIL(add_var_to_array_no_dup(get_plan()->get_predicate_selectivities(),
            ObExprSelPair(join_filter_expr, join_filter_rate)))) {
          LOG_WARN("fail to add join filter expr", K(ret));
        } else {
          join_filter_use->set_join_filter_expr(join_filter_expr);
        }
      }
    }
  }
  return ret;
}

int ObLogicalOperator::find_table_scan(ObLogicalOperator* root_op,
                                       uint64_t table_id,
                                       ObLogicalOperator* &scan_op,
                                       bool& table_scan_has_exchange)
{
  int ret = OB_SUCCESS;
  scan_op = NULL;
  if (OB_ISNULL(root_op)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpect null logical operator", K(ret));
  } else if (log_op_def::LOG_TABLE_SCAN == root_op->get_type()) {
    ObLogTableScan *scan = static_cast<ObLogTableScan *>(root_op);
    if (scan->get_table_id() == table_id) {
      scan_op = root_op;
    }
  } else if (log_op_def::LOG_TEMP_TABLE_ACCESS == root_op->get_type()) {
    ObLogTempTableAccess *scan = static_cast<ObLogTempTableAccess *>(root_op);
    if (scan->get_table_id() == table_id) {
      scan_op = root_op;
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && NULL == scan_op && i < root_op->get_num_of_child(); ++i) {
      ObLogicalOperator *child = root_op->get_child(i);
      if (OB_FAIL(SMART_CALL(find_table_scan(child, table_id, scan_op, table_scan_has_exchange)))) {
        LOG_WARN("failed to find operator", K(ret));
      }
    }
    if (OB_SUCC(ret) && NULL != scan_op) {
      if (log_op_def::LOG_EXCHANGE == root_op->get_type()) {
        table_scan_has_exchange = true;
      }
    }
  }
  return ret;
}

int ObLogicalOperator::allocate_partition_join_filter(const ObIArray<JoinFilterInfo> &infos,
                                                      int64_t &filter_id)
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *filter_create = NULL;
  ObLogJoinFilter *join_filter_create = NULL;
  ObLogOperatorFactory &factory = get_plan()->get_log_op_factory();
  CK(LOG_JOIN == get_type());
  for (int i = 0; i < infos.count() && OB_SUCC(ret); ++i) {
    filter_create = NULL;
    bool dummy_has_exchange = false;
    const JoinFilterInfo &info = infos.at(i);
    ObLogTableScan *scan_op = NULL;
    ObLogicalOperator *node = NULL;
    if (!info.need_partition_join_filter_) {
      continue;
    } else if (OB_ISNULL(info.sharding_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect null sharding", K(ret));
    } else if (OB_FAIL(find_table_scan(get_child(second_child),
                                       info.table_id_,
                                       node,
                                       dummy_has_exchange))) {
      LOG_WARN("failed to find table scan", K(ret));
    } else if (OB_ISNULL(node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect null table scan", K(ret));
    } else if (log_op_def::LOG_TABLE_SCAN != node->get_type()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect operator type", K(ret));
    } else if (OB_ISNULL(filter_create = factory.allocate(*(get_plan()), LOG_JOIN_FILTER))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("failed to allocate exchange nodes", K(ret));
    } else {
      bool is_shared_hash_join = static_cast<ObLogJoin*>(this)->is_shared_hash_join();
      scan_op = static_cast<ObLogTableScan*>(node);
      ObPxBFStaticInfo bf_info;
      join_filter_create = static_cast<ObLogJoinFilter *>(filter_create);
      join_filter_create->set_is_create_filter(true);
      join_filter_create->set_filter_id(filter_id);
      join_filter_create->set_child(first_child, get_child(first_child));
      get_child(first_child)->set_parent(join_filter_create);
      join_filter_create->set_parent(this);
      set_child(first_child, join_filter_create);
      join_filter_create->set_filter_length(info.sharding_->get_part_cnt() * 2);
      if (is_shared_hash_join) {
        join_filter_create->set_is_shared_partition_join_filter();
      } else {
        join_filter_create->set_is_no_shared_partition_join_filter();
      }
      join_filter_create->set_tablet_id_expr(info.calc_part_id_expr_);
      join_filter_create->set_is_use_filter_shuffle(false);
      OZ(join_filter_create->compute_property());
      OZ(bf_info.init(get_plan()->get_optimizer_context().get_session_info()->get_effective_tenant_id(),
          filter_id, GCTX.server_id_, is_shared_hash_join, info.skip_subpart_));
      scan_op->set_join_filter_info(bf_info);
      filter_id++;
      for (int j = 0; j < info.lexprs_.count() && OB_SUCC(ret); ++j) {
        ObRawExpr *expr = info.lexprs_.at(j);
        CK(OB_NOT_NULL(expr));
        OZ(join_filter_create->get_join_exprs().push_back(expr));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::allocate_normal_join_filter(const ObIArray<JoinFilterInfo> &infos,
                                                   int64_t &filter_id)
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *filter_create = NULL;
  ObLogicalOperator *filter_use = NULL;
  ObLogJoinFilter *join_filter_create = NULL;
  ObLogJoinFilter *join_filter_use = NULL;
  ObLogOperatorFactory &factory = get_plan()->get_log_op_factory();
  CK(LOG_JOIN == get_type());
  if (OB_SUCC(ret)) {
    DistAlgo join_dist_algo = static_cast<ObLogJoin*>(this)->get_join_distributed_method();
    for (int i = 0; i < infos.count() && OB_SUCC(ret); ++i) {
      bool right_has_exchange = false;
      filter_create = NULL;
      filter_use = NULL;
      const JoinFilterInfo &info = infos.at(i);
      ObLogicalOperator *node = NULL;
      if (!info.can_use_join_filter_) {
        //do nothing
      } else if (OB_ISNULL(filter_create = factory.allocate(*(get_plan()), LOG_JOIN_FILTER))
          || OB_ISNULL(filter_use = factory.allocate(*(get_plan()), LOG_JOIN_FILTER))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("failed to allocate exchange nodes", K(ret));
      } else if (OB_FAIL(find_table_scan(get_child(second_child),
                                         info.table_id_,
                                         node,
                                         right_has_exchange))) {
        LOG_WARN("failed to find table scan", K(ret));
      } else if (OB_ISNULL(node)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpect null table scan", K(ret));
      } else {
        join_filter_create = static_cast<ObLogJoinFilter *>(filter_create);
        join_filter_use = static_cast<ObLogJoinFilter *>(filter_use);
        join_filter_create->set_is_create_filter(true);
        join_filter_use->set_is_create_filter(false);
        join_filter_create->set_filter_id(filter_id);
        join_filter_use->set_filter_id(filter_id);
        join_filter_create->set_child(first_child, get_child(first_child));
        get_child(first_child)->set_parent(join_filter_create);
        join_filter_create->set_parent(this);
        set_child(first_child, join_filter_create);
        join_filter_create->set_filter_length(join_filter_create->
            get_child(first_child)->get_card());
        join_filter_use->set_filter_length(
            join_filter_create->get_child(first_child)->get_card());
        for (int64_t i = 0; OB_SUCC(ret) && i < node->get_parent()->get_num_of_child(); ++i) {
          if (node->get_parent()->get_child(i) == node) {
            node->get_parent()->set_child(i, join_filter_use);
	          break;
          }
        }
        if (OB_SUCC(ret)) {
          join_filter_use->set_parent(node->get_parent());
          node->set_parent(join_filter_use);
          join_filter_use->set_child(first_child, node);
          if (right_has_exchange) {
            join_filter_create->set_is_use_filter_shuffle(true);
            join_filter_use->set_is_use_filter_shuffle(true);
            if (OB_FAIL(mark_bloom_filter_id_to_receive_op(join_filter_use, filter_id))) {
              LOG_WARN("failed to mark bloom filter id to receive op", K(filter_id), K(join_filter_use));
            }
          }
          if ((is_partition_wise_ && !right_has_exchange) || DIST_PARTITION_NONE == join_dist_algo) {
            join_filter_create->set_is_non_shared_join_filter();
            join_filter_use->set_is_non_shared_join_filter();
          } else {
            join_filter_create->set_is_shared_join_filter();
            join_filter_use->set_is_shared_join_filter();
          }
          OZ(join_filter_create->compute_property());
          OZ(join_filter_use->compute_property());
        }
        filter_id++;
        for (int j = 0; j < info.lexprs_.count() && OB_SUCC(ret); ++j) {
          ObRawExpr *lexpr = info.lexprs_.at(j);
          ObRawExpr *rexpr = info.rexprs_.at(j);
          CK(OB_NOT_NULL(lexpr) && OB_NOT_NULL(rexpr));
          OZ(join_filter_create->get_join_exprs().push_back(lexpr));
          OZ(join_filter_use->get_join_exprs().push_back(rexpr));
        }
        OZ(push_down_bloom_filter_expr(node, join_filter_use, info.join_filter_selectivity_));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::mark_bloom_filter_id_to_receive_op(ObLogicalOperator *filter_use, int64_t filter_id)
{
  int ret = OB_SUCCESS;
  ObLogicalOperator *parent = NULL;
  if (OB_ISNULL(filter_use)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpect null", K(ret));
  } else if (FALSE_IT(parent = filter_use->get_parent())) {
  } else if (NULL != parent) {
    if (log_op_def::LOG_EXCHANGE == parent->get_type() &&
        static_cast<ObLogExchange *>(parent)->is_consumer()) {
      if (OB_FAIL(static_cast<ObLogExchange *>(parent)->get_bloom_filter_ids().push_back(filter_id))) {
        LOG_WARN("ObLogExchange failed to record bloom filter id", K(parent), K(filter_id), K(ret));
      } else {
        LOG_DEBUG("ObLogExchange succ to record bloom filter id", K(parent), K(filter_id));
      }
    } else if (OB_FAIL(SMART_CALL(mark_bloom_filter_id_to_receive_op(parent, filter_id)))) {
      LOG_WARN("mark bloom filter id to receive op failed", K(ret));
    }
  }
  return ret;
}

int ObLogicalOperator::allocate_bf_node_for_hash_join(AllocBloomFilterContext &ctx)
{
  int ret = OB_SUCCESS;
  ObLogJoin *join_op = static_cast<ObLogJoin*>(this);
  if (LOG_JOIN != get_type()) {
    //do nothing
  } else if (join_op->get_join_filter_infos().empty()) {
    //do nothing
  } else if (OB_FAIL(allocate_partition_join_filter(join_op->get_join_filter_infos(),
                                                    ctx.filter_id_))) {
    LOG_WARN("fail to allocate partition join filter", K(ret));
  } else if (OB_FAIL(allocate_normal_join_filter(join_op->get_join_filter_infos(),
                                                 ctx.filter_id_))) {
    LOG_WARN("fail to allocate normal join filter", K(ret));
  }
  return ret;
}

int ObLogicalOperator::generate_pseudo_partition_id_expr(ObOpPseudoColumnRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  ObExprResType res_type;
  res_type.set_type(ObIntType);
  res_type.set_accuracy(ObAccuracy::MAX_ACCURACY[ObIntType]);

  ObOptimizerContext &ctx = get_plan()->get_optimizer_context();
  if (OB_FAIL(ObRawExprUtils::build_op_pseudo_column_expr(ctx.get_expr_factory(),
                                                          T_PDML_PARTITION_ID,
                                                          "PARTITION_ID",
                                                          res_type,
                                                          expr))) {
    LOG_WARN("build operator pseudo column failed", K(ret));
  } else if (OB_FAIL(expr->formalize(ctx.get_session_info()))) {
    LOG_WARN("expr formalize failed", K(ret));
  }

  return ret;
}

int ObLogicalOperator::find_nested_dis_rescan(bool &find, bool nested)
{
  int ret = OB_SUCCESS;
  bool is_end = false;
  if (find) {
  } else if (LOG_EXCHANGE == get_type()) {
    if (nested) {
      find = true;
    }
    is_end = true;
  } else if (LOG_SUBPLAN_FILTER == get_type() ||
        (LOG_JOIN == get_type() &&
        JoinAlgo::NESTED_LOOP_JOIN == static_cast<ObLogJoin*>(this)->get_join_algo())) {
    nested = true;
  }
  if (!is_end) {
    for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); i++) {
      ObLogicalOperator *child = NULL;
      if (OB_ISNULL(child = get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(get_child(i)), K(ret));
      } else if (OB_FAIL(SMART_CALL(child->find_nested_dis_rescan(find, nested)))) {
        LOG_WARN("fail to find px for batch rescan", K(ret));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::check_subplan_filter_child_exchange_rescanable()
{
  int ret = OB_SUCCESS;
  // 对于subplan filter右孩子如果是onetime expr,
  // 则不需要将该子孩子exhange标记为px coord, 此时subpaln filter左孩子必须标记为px coord.
  // 对于subplan filter右孩子没有onetime expr的场景,
  // 从second child起, 均需要标记为px coord.
  // 右孩子们是否标记为px coord取决于是否需要rescan.
  // 左孩子是否标记为px, 取决于右子孩子是否有onetime expr, 原因是需要先获取expr值, 再下压至左孩子,
  // 详见issue https://work.aone.alibaba-inc.com/issue/30827301
  if (OB_UNLIKELY(LOG_SUBPLAN_FILTER != type_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected operator type", K(ret), K(type_));
  } else {
    bool has_onetime_expr = false;
    ObLogSubPlanFilter *sub_plan_filter = static_cast<ObLogSubPlanFilter*>(this);
    for (int64_t i = get_num_of_child() - 1; OB_SUCC(ret) && i >= 0; --i) {
      if (i != 0) {
        if (sub_plan_filter->get_onetime_idxs().has_member(i)) {
          has_onetime_expr = true;
        } else if (OB_FAIL(get_child(i)->mark_child_exchange_rescanable())) {
          LOG_WARN("mark child ex-receive as px op fail", K(ret));
        } else { /*do nothing*/ }
      } else if (has_onetime_expr && OB_FAIL(get_child(i)->mark_child_exchange_rescanable())) {
        LOG_WARN("mark child ex-receive as px op fail", K(ret));
      } else { /*do nothing*/ }
    }
  }
  return ret;
}

int ObLogicalOperator::get_part_column_exprs(const uint64_t table_id,
                                             const uint64_t ref_table_id,
                                             ObIArray<ObRawExpr *> &part_exprs) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("plan is nullptr");
  } else {
    ret = get_plan()->get_part_column_exprs(table_id, ref_table_id, part_exprs);
  }
  return ret;
}

int ObLogicalOperator::find_px_for_batch_rescan(const log_op_def::ObLogOpType op_type,
    const int64_t op_id, bool &find)
{
  int ret = OB_SUCCESS;
  if (LOG_SUBPLAN_FILTER == get_type() ||
      (LOG_JOIN == get_type() &&
       JoinAlgo::NESTED_LOOP_JOIN == static_cast<ObLogJoin*>(this)->get_join_algo())) {
    /*do nothing*/
  } else if (LOG_EXCHANGE == get_type()) {
    ObLogExchange *op = static_cast<ObLogExchange *>(this);
    if (op->is_rescanable()) {
      op->set_px_batch_op_id(op_id);
      op->set_px_batch_op_type(op_type);
      find = true;
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < get_num_of_child(); i++) {
      ObLogicalOperator *child = NULL;
      if (OB_ISNULL(child = get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(get_child(i)), K(ret));
      } else if (OB_FAIL(SMART_CALL(child->find_px_for_batch_rescan(op_type, op_id, find)))) {
        LOG_WARN("fail to find px for batch rescan", K(ret));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::add_op_exprs(ObRawExpr *expr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FAIL(op_exprs_.push_back(expr))) {
    LOG_WARN("failed to push back exprs", K(ret));
  } else { /*do nothing*/ }
  return ret;
}

int ObLogicalOperator::find_shuffle_join_filter(bool &find) const
{
  int ret = OB_SUCCESS;
  find = false;
  if (log_op_def::LOG_EXCHANGE == get_type()) {
    /* do nothing */
  } else if (log_op_def::LOG_JOIN == get_type()) {
    const ObIArray<JoinFilterInfo> &infos = static_cast<const ObLogJoin*>(this)->get_join_filter_infos();
    for (int64_t i = 0; OB_SUCC(ret) && !find && i < infos.count(); ++i) {
      find = !infos.at(i).in_current_dfo_;
    }
    const ObLogicalOperator *child = NULL;
    for (int64_t i = 0; !find && OB_SUCC(ret) && i < get_num_of_child(); i++) {
      if (OB_ISNULL(child = get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null", K(ret), K(child));
      } else if (OB_FAIL(SMART_CALL(child->find_shuffle_join_filter(find)))) {
        LOG_WARN("failed to find shuffle join filter", K(ret));
      }
    }
  } else {
    const ObLogicalOperator *child = NULL;
    for (int64_t i = 0; !find && OB_SUCC(ret) && i < get_num_of_child(); i++) {
      if (OB_ISNULL(child = get_child(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null", K(ret), K(child));
      } else if (OB_FAIL(SMART_CALL(child->find_shuffle_join_filter(find)))) {
        LOG_WARN("failed to find shuffle join filter", K(ret));
      }
    }
  }
  return ret;
}

int ObLogicalOperator::has_window_function_below(bool &has_win_func) const
{
  int ret = OB_SUCCESS;
  has_win_func = LOG_WINDOW_FUNCTION == get_type();
  const ObLogicalOperator *child = NULL;
  for (int64_t i = 0; !has_win_func && OB_SUCC(ret) && i < get_num_of_child(); i++) {
    if (OB_ISNULL(child = get_child(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null", K(ret), K(child));
    } else if (OB_FAIL(SMART_CALL(child->has_window_function_below(has_win_func)))) {
      LOG_WARN("failed to check has window function below", K(ret));
    }
  }
  return ret;
}

int ObLogicalOperator::get_pushdown_op(log_op_def::ObLogOpType op_type, const ObLogicalOperator *&op) const
{
  int ret = OB_SUCCESS;
  op = NULL;
  const ObLogicalOperator *child = NULL;
  if (get_type() == op_type) {
    op = this;
  } else if (get_num_of_child() < 1) {
    /* do nothing */
  } else if (LOG_EXCHANGE != get_type() && LOG_SORT != get_type()
             && LOG_GRANULE_ITERATOR != get_type()
             && LOG_TOPK != get_type()
             && LOG_MATERIAL != get_type()) {
    /* do nothing */
  } else if (OB_ISNULL(child = get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(child));
  } else if (OB_FAIL(SMART_CALL(child->get_pushdown_op(op_type, op)))) {
    LOG_WARN("failed to check push down", K(ret));
  }
  return ret;
}

int ObLogicalOperator::need_alloc_material_for_shared_hj(ObLogicalOperator &curr_op, bool &need_alloc)
{
  int ret = OB_SUCCESS;
  need_alloc = false;
  ObLogicalOperator *parent = curr_op.get_parent();
  bool stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(stack_overflow))) {
    LOG_WARN("failed to check stack overflow", K(ret));
  } else if (stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too deep recursive", K(ret));
  } else if (nullptr == parent || LOG_EXCHANGE == curr_op.type_) {
    //do nothing
  } else {
    bool end_traverse = false;
    int64_t i = 0;
    for (; OB_SUCC(ret) && i < parent->get_num_of_child(); ++i) {
      if (OB_ISNULL(parent->get_child(i))) {
        ret =  OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected got nullptr child", K(ret), K(i));
      } else if (parent->get_child(i) == &curr_op) {
        if (parent->is_block_input(i)) {
          end_traverse = true;
        }
        break;
      }
    }
    for (; OB_SUCC(ret) && !end_traverse && i < parent->get_num_of_child(); ++i) {
      if (OB_ISNULL(parent->get_child(i))) {
        ret =  OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected got nullptr child", K(ret), K(i));
      } else if (parent->get_child(i) == &curr_op) {
      } else if (parent->get_child(i)->is_exchange_allocated()) {
        end_traverse = true;
        need_alloc = true;
      }
    }
    if (OB_SUCC(ret) && !end_traverse) {
      OZ (need_alloc_material_for_shared_hj(*parent, need_alloc));
    }
  }
  return ret;
}
