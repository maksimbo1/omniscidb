/*
 * Copyright 2017 MapD Technologies, Inc.
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

/**
 * @file    Analyzer.h
 * @author  Wei Hong <wei@map-d.com>
 * @brief   Defines data structures for the semantic analysis phase of query processing
 *
 * Copyright (c) 2014 MapD Technologies, Inc.  All rights reserved.
 **/
#ifndef ANALYZER_H
#define ANALYZER_H

#include <cstdint>
#include <iostream>
#include <list>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Geospatial/Types.h"
#include "Logger/Logger.h"
#include "Shared/sqldefs.h"
#include "Shared/sqltypes.h"

namespace Analyzer {
class Expr;
}

namespace Catalog_Namespace {
class Catalog;
}

template <typename Tp, typename... Args>
inline typename std::enable_if<std::is_base_of<Analyzer::Expr, Tp>::value,
                               std::shared_ptr<Tp>>::type
makeExpr(Args&&... args) {
  return std::make_shared<Tp>(std::forward<Args>(args)...);
}

namespace Analyzer {

class ColumnVar;
class TargetEntry;
class Expr;
using DomainSet = std::list<const Expr*>;

/*
 * @type Expr
 * @brief super class for all expressions in parse trees and in query plans
 */
class Expr : public std::enable_shared_from_this<Expr> {
 public:
  Expr(SQLTypes t, bool notnull) : type_info(t, notnull), contains_agg(false) {}
  Expr(SQLTypes t, int d, bool notnull)
      : type_info(t, d, 0, notnull), contains_agg(false) {}
  Expr(SQLTypes t, int d, int s, bool notnull)
      : type_info(t, d, s, notnull), contains_agg(false) {}
  Expr(const SQLTypeInfo& ti, bool has_agg = false)
      : type_info(ti), contains_agg(has_agg) {}
  virtual ~Expr() {}
  std::shared_ptr<Analyzer::Expr> get_shared_ptr() { return shared_from_this(); }
  const SQLTypeInfo& get_type_info() const { return type_info; }
  void set_type_info(const SQLTypeInfo& ti) { type_info = ti; }
  bool get_contains_agg() const { return contains_agg; }
  void set_contains_agg(bool a) { contains_agg = a; }
  virtual std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info);
  virtual void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const {};
  virtual std::shared_ptr<Analyzer::Expr> deep_copy()
      const = 0;  // make a deep copy of self
                  /*
                   * @brief normalize_simple_predicate only applies to boolean expressions.
                   * it checks if it is an expression comparing a column
                   * with a constant.  if so, it returns a normalized copy of the predicate with ColumnVar
                   * always as the left operand with rte_idx set to the rte_idx of the ColumnVar.
                   * it returns nullptr with rte_idx set to -1 otherwise.
                   */
  virtual std::shared_ptr<Analyzer::Expr> normalize_simple_predicate(int& rte_idx) const {
    rte_idx = -1;
    return nullptr;
  }
  /*
   * @brief seperate conjunctive predicates into scan predicates, join predicates and
   * constant predicates.
   */
  virtual void group_predicates(std::list<const Expr*>& scan_predicates,
                                std::list<const Expr*>& join_predicates,
                                std::list<const Expr*>& const_predicates) const {}
  /*
   * @brief collect_rte_idx collects the indices of all the range table
   * entries involved in an expression
   */
  virtual void collect_rte_idx(std::set<int>& rte_idx_set) const {}
  /*
   * @brief collect_column_var collects all unique ColumnVar nodes in an expression
   * If include_agg = false, it does not include to ColumnVar nodes inside
   * the argument to AggExpr's.  Otherwise, they are included.
   * It does not make copies of the ColumnVar
   */
  virtual void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const {}
  /*
   * @brief rewrite_with_targetlist rewrite ColumnVar's in expression with entries in a
   * targetlist. targetlist expressions are expected to be only Var's or AggExpr's.
   * returns a new expression copy
   */
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const {
    return deep_copy();
  };
  /*
   * @brief rewrite_with_child_targetlist rewrite ColumnVar's in expression with entries
   * in a child plan's targetlist. targetlist expressions are expected to be only Var's or
   * ColumnVar's returns a new expression copy
   */
  virtual std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const {
    return deep_copy();
  };
  /*
   * @brief rewrite_agg_to_var rewrite ColumnVar's in expression with entries in an
   * AggPlan's targetlist. targetlist expressions are expected to be only Var's or
   * ColumnVar's or AggExpr's All AggExpr's are written into Var's. returns a new
   * expression copy
   */
  virtual std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const {
    return deep_copy();
  }
  virtual bool operator==(const Expr& rhs) const = 0;
  virtual std::string toString() const = 0;
  virtual void print() const { std::cout << toString(); }

  virtual void add_unique(std::list<const Expr*>& expr_list) const;
  /*
   * @brief find_expr traverse Expr hierarchy and adds the node pointer to
   * the expr_list if the function f returns true.
   * Duplicate Expr's are not added the list.
   * Cannot use std::set because we don't have an ordering function.
   */
  virtual void find_expr(bool (*f)(const Expr*),
                         std::list<const Expr*>& expr_list) const {
    if (f(this)) {
      add_unique(expr_list);
    }
  }
  /*
   * @brief decompress adds cast operator to decompress encoded result
   */
  std::shared_ptr<Analyzer::Expr> decompress();
  /*
   * @brief perform domain analysis on Expr and fill in domain
   * information in domain_set.  Empty domain_set means no information.
   */
  virtual void get_domain(DomainSet& domain_set) const { domain_set.clear(); }

 protected:
  SQLTypeInfo type_info;  // SQLTypeInfo of the return result of this expression
  bool contains_agg;
};

using ExpressionPtr = std::shared_ptr<Analyzer::Expr>;
using ExpressionPtrList = std::list<ExpressionPtr>;
using ExpressionPtrVector = std::vector<ExpressionPtr>;

/*
 * @type ColumnVar
 * @brief expression that evaluates to the value of a column in a given row from a base
 * table. It is used in parse trees and is only used in Scan nodes in a query plan for
 * scanning a table while Var nodes are used for all other plans.
 */
class ColumnVar : public Expr {
 public:
  ColumnVar(const SQLTypeInfo& ti, int r, int c, int i)
      : Expr(ti), table_id(r), column_id(c), rte_idx(i) {}
  int get_table_id() const { return table_id; }
  int get_column_id() const { return column_id; }
  int get_rte_idx() const { return rte_idx; }
  EncodingType get_compression() const { return type_info.get_compression(); }
  int get_comp_param() const { return type_info.get_comp_param(); }
  void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const override;
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    rte_idx_set.insert(rte_idx);
  }
  static bool colvar_comp(const ColumnVar* l, const ColumnVar* r) {
    return l->get_table_id() < r->get_table_id() ||
           (l->get_table_id() == r->get_table_id() &&
            l->get_column_id() < r->get_column_id());
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    colvar_set.insert(this);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;

 protected:
  int table_id;   // the global table id
  int column_id;  // the column id
  int rte_idx;    // 0-based range table index, used for table ordering in multi-joins
};

/*
 * @type ExpressionTuple
 * @brief A tuple of expressions on the side of an equi-join on multiple columns.
 * Not to be used in any other context.
 */
class ExpressionTuple : public Expr {
 public:
  ExpressionTuple(const std::vector<std::shared_ptr<Analyzer::Expr>>& tuple)
      : Expr(SQLTypeInfo()), tuple_(tuple){};

  const std::vector<std::shared_ptr<Analyzer::Expr>>& getTuple() const { return tuple_; }

  void collect_rte_idx(std::set<int>& rte_idx_set) const override;

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;

 private:
  const std::vector<std::shared_ptr<Analyzer::Expr>> tuple_;
};

/*
 * @type Var
 * @brief expression that evaluates to the value of a column in a given row generated
 * from a query plan node.  It is only used in plan nodes above Scan nodes.
 * The row can be produced by either the inner or the outer plan in case of a join.
 * It inherits from ColumnVar to keep track of the lineage through the plan nodes.
 * The table_id will be set to 0 if the Var does not correspond to an original column
 * value.
 */
class Var : public ColumnVar {
 public:
  enum WhichRow { kINPUT_OUTER, kINPUT_INNER, kOUTPUT, kGROUPBY };
  Var(const SQLTypeInfo& ti, int r, int c, int i, WhichRow o, int v)
      : ColumnVar(ti, r, c, i), which_row(o), varno(v) {}
  Var(const SQLTypeInfo& ti, WhichRow o, int v)
      : ColumnVar(ti, 0, 0, -1), which_row(o), varno(v) {}
  WhichRow get_which_row() const { return which_row; }
  void set_which_row(WhichRow r) { which_row = r; }
  int get_varno() const { return varno; }
  void set_varno(int n) { varno = n; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  std::string toString() const override;
  void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    rte_idx_set.insert(-1);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return deep_copy();
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return deep_copy();
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;

 private:
  WhichRow which_row;  // indicate which row this Var should project from.  It can be from
                       // the outer input plan or the inner input plan (for joins) or the
                       // output row in the current plan.
  int varno;           // the column number in the row.  1-based
};

/*
 * @type Constant
 * @brief expression for a constant value
 */
class Constant : public Expr {
 public:
  Constant(SQLTypes t, bool n) : Expr(t, !n), is_null(n) {
    if (n) {
      set_null_value();
    } else {
      type_info.set_notnull(true);
    }
  }
  Constant(SQLTypes t, bool n, Datum v) : Expr(t, !n), is_null(n), constval(v) {
    if (n) {
      set_null_value();
    } else {
      type_info.set_notnull(true);
    }
  }
  Constant(const SQLTypeInfo& ti, bool n, Datum v) : Expr(ti), is_null(n), constval(v) {
    if (n) {
      set_null_value();
    } else {
      type_info.set_notnull(true);
    }
  }
  Constant(const SQLTypeInfo& ti,
           bool n,
           const std::list<std::shared_ptr<Analyzer::Expr>>& l)
      : Expr(ti), is_null(n), constval(Datum{0}), value_list(l) {}
  ~Constant() override;
  bool get_is_null() const { return is_null; }
  Datum get_constval() const { return constval; }
  void set_constval(Datum d) { constval = d; }
  const std::list<std::shared_ptr<Analyzer::Expr>>& get_value_list() const {
    return value_list;
  }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info) override;
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;

 protected:
  bool is_null;    // constant is NULL
  Datum constval;  // the constant value
  const std::list<std::shared_ptr<Analyzer::Expr>> value_list;
  void cast_number(const SQLTypeInfo& new_type_info);
  void cast_string(const SQLTypeInfo& new_type_info);
  void cast_from_string(const SQLTypeInfo& new_type_info);
  void cast_to_string(const SQLTypeInfo& new_type_info);
  void do_cast(const SQLTypeInfo& new_type_info);
  void set_null_value();
};

/*
 * @type UOper
 * @brief represents unary operator expressions.  operator types include
 * kUMINUS, kISNULL, kEXISTS, kCAST
 */
class UOper : public Expr {
 public:
  UOper(const SQLTypeInfo& ti, bool has_agg, SQLOps o, std::shared_ptr<Analyzer::Expr> p)
      : Expr(ti, has_agg), optype(o), operand(p) {}
  UOper(SQLTypes t, SQLOps o, std::shared_ptr<Analyzer::Expr> p)
      : Expr(t, o == kISNULL ? true : p->get_type_info().get_notnull())
      , optype(o)
      , operand(p) {}
  SQLOps get_optype() const { return optype; }
  const Expr* get_operand() const { return operand.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_operand() const { return operand; }
  void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const override;
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    operand->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    operand->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<UOper>(
        type_info, contains_agg, optype, operand->rewrite_with_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<UOper>(
        type_info, contains_agg, optype, operand->rewrite_with_child_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<UOper>(
        type_info, contains_agg, optype, operand->rewrite_agg_to_var(tlist));
  }
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;
  std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info) override;

 protected:
  SQLOps optype;  // operator type, e.g., kUMINUS, kISNULL, kEXISTS
  std::shared_ptr<Analyzer::Expr> operand;  // operand expression
};

/*
 * @type BinOper
 * @brief represents binary operator expressions.  it includes all
 * comparison, arithmetic and boolean binary operators.  it handles ANY/ALL qualifiers
 * in case the right_operand is a subquery.
 */
class BinOper : public Expr {
 public:
  BinOper(const SQLTypeInfo& ti,
          bool has_agg,
          SQLOps o,
          SQLQualifier q,
          std::shared_ptr<Analyzer::Expr> l,
          std::shared_ptr<Analyzer::Expr> r)
      : Expr(ti, has_agg), optype(o), qualifier(q), left_operand(l), right_operand(r) {}
  BinOper(SQLTypes t,
          SQLOps o,
          SQLQualifier q,
          std::shared_ptr<Analyzer::Expr> l,
          std::shared_ptr<Analyzer::Expr> r)
      : Expr(t, l->get_type_info().get_notnull() && r->get_type_info().get_notnull())
      , optype(o)
      , qualifier(q)
      , left_operand(l)
      , right_operand(r) {}
  SQLOps get_optype() const { return optype; }
  bool is_overlaps_oper() const { return optype == kOVERLAPS; }
  SQLQualifier get_qualifier() const { return qualifier; }
  const Expr* get_left_operand() const { return left_operand.get(); }
  const Expr* get_right_operand() const { return right_operand.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_left_operand() const {
    return left_operand;
  }
  const std::shared_ptr<Analyzer::Expr> get_own_right_operand() const {
    return right_operand;
  }
  static SQLTypeInfo analyze_type_info(SQLOps op,
                                       const SQLTypeInfo& left_type,
                                       const SQLTypeInfo& right_type,
                                       SQLTypeInfo* new_left_type,
                                       SQLTypeInfo* new_right_type);
  static SQLTypeInfo common_numeric_type(const SQLTypeInfo& type1,
                                         const SQLTypeInfo& type2);
  static SQLTypeInfo common_string_type(const SQLTypeInfo& type1,
                                        const SQLTypeInfo& type2);
  void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const override;
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  std::shared_ptr<Analyzer::Expr> normalize_simple_predicate(int& rte_idx) const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    left_operand->collect_rte_idx(rte_idx_set);
    right_operand->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    left_operand->collect_column_var(colvar_set, include_agg);
    right_operand->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<BinOper>(type_info,
                             contains_agg,
                             optype,
                             qualifier,
                             left_operand->rewrite_with_targetlist(tlist),
                             right_operand->rewrite_with_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<BinOper>(type_info,
                             contains_agg,
                             optype,
                             qualifier,
                             left_operand->rewrite_with_child_targetlist(tlist),
                             right_operand->rewrite_with_child_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<BinOper>(type_info,
                             contains_agg,
                             optype,
                             qualifier,
                             left_operand->rewrite_agg_to_var(tlist),
                             right_operand->rewrite_agg_to_var(tlist));
  }
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;
  static bool simple_predicate_has_simple_cast(
      const std::shared_ptr<Analyzer::Expr> cast_operand,
      const std::shared_ptr<Analyzer::Expr> const_operand);

 private:
  SQLOps optype;           // operator type, e.g., kLT, kAND, kPLUS, etc.
  SQLQualifier qualifier;  // qualifier kANY, kALL or kONE.  Only relevant with
                           // right_operand is Subquery
  std::shared_ptr<Analyzer::Expr> left_operand;   // the left operand expression
  std::shared_ptr<Analyzer::Expr> right_operand;  // the right operand expression
};

class Query;

/*
 * @type Subquery
 * @brief subquery expression.  Note that the type of the expression is the type of the
 * TargetEntry in the subquery instead of the set.
 */
class Subquery : public Expr {
 public:
  Subquery(const SQLTypeInfo& ti, Query* q)
      : Expr(ti), parsetree(q) /*, plan(nullptr)*/ {}
  ~Subquery() override;
  const Query* get_parsetree() const { return parsetree; }
  // const Plan *get_plan() const { return plan; }
  // void set_plan(Plan *p) { plan = p; } // subquery plan is set by the optimizer
  std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info) override;
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override {
    CHECK(false);
  }
  void collect_rte_idx(std::set<int>& rte_idx_set) const override { CHECK(false); }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    CHECK(false);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    abort();
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    abort();
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    abort();
  }
  bool operator==(const Expr& rhs) const override {
    CHECK(false);
    return false;
  }
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override {
    CHECK(false);
  }

 private:
  Query* parsetree;  // parse tree of the subquery
};

/*
 * @type InValues
 * @brief represents predicate expr IN (v1, v2, ...)
 * v1, v2, ... are can be either Constant or Parameter.
 */
class InValues : public Expr {
 public:
  InValues(std::shared_ptr<Analyzer::Expr> a,
           const std::list<std::shared_ptr<Analyzer::Expr>>& l);
  const Expr* get_arg() const { return arg.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }
  const std::list<std::shared_ptr<Analyzer::Expr>>& get_value_list() const {
    return value_list;
  }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    arg->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    arg->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  std::shared_ptr<Analyzer::Expr> arg;  // the argument left of IN
  const std::list<std::shared_ptr<Analyzer::Expr>>
      value_list;  // the list of values right of IN
};

/*
 * @type InIntegerSet
 * @brief represents predicate expr IN (v1, v2, ...) for the case where the right
 *        hand side is a list of integers or dictionary-encoded strings generated
 *        by a IN subquery. Avoids the overhead of storing a list of shared pointers
 *        to Constant objects, making it more suitable for IN sub-queries usage.
 * v1, v2, ... are integers
 */
class InIntegerSet : public Expr {
 public:
  InIntegerSet(const std::shared_ptr<const Analyzer::Expr> a,
               const std::vector<int64_t>& values,
               const bool not_null);

  const Expr* get_arg() const { return arg.get(); }

  const std::vector<int64_t>& get_value_list() const { return value_list; }

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;

 private:
  const std::shared_ptr<const Analyzer::Expr> arg;  // the argument left of IN
  const std::vector<int64_t> value_list;            // the list of values right of IN
};

/*
 * @type CharLengthExpr
 * @brief expression for the CHAR_LENGTH expression.
 * arg must evaluate to char, varchar or text.
 */
class CharLengthExpr : public Expr {
 public:
  CharLengthExpr(std::shared_ptr<Analyzer::Expr> a, bool e)
      : Expr(kINT, a->get_type_info().get_notnull()), arg(a), calc_encoded_length(e) {}
  const Expr* get_arg() const { return arg.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }
  bool get_calc_encoded_length() const { return calc_encoded_length; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    arg->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    arg->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<CharLengthExpr>(arg->rewrite_with_targetlist(tlist),
                                    calc_encoded_length);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<CharLengthExpr>(arg->rewrite_with_child_targetlist(tlist),
                                    calc_encoded_length);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<CharLengthExpr>(arg->rewrite_agg_to_var(tlist), calc_encoded_length);
  }
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  std::shared_ptr<Analyzer::Expr> arg;
  bool calc_encoded_length;
};

/*
 * @type KeyForStringExpr
 * @brief expression for the KEY_FOR_STRING expression.
 * arg must be a dict encoded column, not str literal.
 */
class KeyForStringExpr : public Expr {
 public:
  KeyForStringExpr(std::shared_ptr<Analyzer::Expr> a)
      : Expr(kINT, a->get_type_info().get_notnull()), arg(a) {}
  const Expr* get_arg() const { return arg.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    arg->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    arg->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<KeyForStringExpr>(arg->rewrite_with_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<KeyForStringExpr>(arg->rewrite_with_child_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<KeyForStringExpr>(arg->rewrite_agg_to_var(tlist));
  }
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  std::shared_ptr<Analyzer::Expr> arg;
};

/*
 * @type SampleRatioExpr
 * @brief expression for the SAMPLE_RATIO expression. Argument range is expected to be
 * between 0 and 1.
 */
class SampleRatioExpr : public Expr {
 public:
  SampleRatioExpr(std::shared_ptr<Analyzer::Expr> a)
      : Expr(kBOOLEAN, a->get_type_info().get_notnull()), arg(a) {}
  const Expr* get_arg() const { return arg.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    arg->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    arg->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<SampleRatioExpr>(arg->rewrite_with_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<SampleRatioExpr>(arg->rewrite_with_child_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<SampleRatioExpr>(arg->rewrite_agg_to_var(tlist));
  }
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  std::shared_ptr<Analyzer::Expr> arg;
};

/**
 * @brief Expression class for the LOWER (lowercase) string function.
 * The "arg" constructor parameter must be an expression that resolves to a string
 * datatype (e.g. TEXT).
 */
class LowerExpr : public Expr {
 public:
  LowerExpr(std::shared_ptr<Analyzer::Expr> arg) : Expr(arg->get_type_info()), arg(arg) {}

  const Expr* get_arg() const { return arg.get(); }

  const std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }

  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    arg->collect_rte_idx(rte_idx_set);
  }

  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    arg->collect_column_var(colvar_set, include_agg);
  }

  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<LowerExpr>(arg->rewrite_with_targetlist(tlist));
  }

  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<LowerExpr>(arg->rewrite_with_child_targetlist(tlist));
  }

  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<LowerExpr>(arg->rewrite_agg_to_var(tlist));
  }

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;

  bool operator==(const Expr& rhs) const override;

  std::string toString() const override;

  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  std::shared_ptr<Analyzer::Expr> arg;
};

/*
 * @type CardinalityExpr
 * @brief expression for the CARDINALITY expression.
 * arg must evaluate to array (or multiset when supported).
 */
class CardinalityExpr : public Expr {
 public:
  CardinalityExpr(std::shared_ptr<Analyzer::Expr> a)
      : Expr(kINT, a->get_type_info().get_notnull()), arg(a) {}
  const Expr* get_arg() const { return arg.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    arg->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    arg->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<CardinalityExpr>(arg->rewrite_with_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<CardinalityExpr>(arg->rewrite_with_child_targetlist(tlist));
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<CardinalityExpr>(arg->rewrite_agg_to_var(tlist));
  }
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  std::shared_ptr<Analyzer::Expr> arg;
};

/*
 * @type LikeExpr
 * @brief expression for the LIKE predicate.
 * arg must evaluate to char, varchar or text.
 */
class LikeExpr : public Expr {
 public:
  LikeExpr(std::shared_ptr<Analyzer::Expr> a,
           std::shared_ptr<Analyzer::Expr> l,
           std::shared_ptr<Analyzer::Expr> e,
           bool i,
           bool s)
      : Expr(kBOOLEAN, a->get_type_info().get_notnull())
      , arg(a)
      , like_expr(l)
      , escape_expr(e)
      , is_ilike(i)
      , is_simple(s) {}
  const Expr* get_arg() const { return arg.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }
  const Expr* get_like_expr() const { return like_expr.get(); }
  const Expr* get_escape_expr() const { return escape_expr.get(); }
  bool get_is_ilike() const { return is_ilike; }
  bool get_is_simple() const { return is_simple; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    arg->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    arg->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<LikeExpr>(arg->rewrite_with_targetlist(tlist),
                              like_expr->deep_copy(),
                              escape_expr ? escape_expr->deep_copy() : nullptr,
                              is_ilike,
                              is_simple);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<LikeExpr>(arg->rewrite_with_child_targetlist(tlist),
                              like_expr->deep_copy(),
                              escape_expr ? escape_expr->deep_copy() : nullptr,
                              is_ilike,
                              is_simple);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<LikeExpr>(arg->rewrite_agg_to_var(tlist),
                              like_expr->deep_copy(),
                              escape_expr ? escape_expr->deep_copy() : nullptr,
                              is_ilike,
                              is_simple);
  }
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  std::shared_ptr<Analyzer::Expr> arg;        // the argument to the left of LIKE
  std::shared_ptr<Analyzer::Expr> like_expr;  // expression that evaluates to like string
  std::shared_ptr<Analyzer::Expr>
      escape_expr;  // expression that evaluates to escape string, can be nullptr
  bool is_ilike;    // is this ILIKE?
  bool is_simple;   // is this simple, meaning we can use fast path search (fits '%str%'
                    // pattern with no inner '%','_','[',']'
};

/*
 * @type RegexpExpr
 * @brief expression for REGEXP.
 * arg must evaluate to char, varchar or text.
 */
class RegexpExpr : public Expr {
 public:
  RegexpExpr(std::shared_ptr<Analyzer::Expr> a,
             std::shared_ptr<Analyzer::Expr> p,
             std::shared_ptr<Analyzer::Expr> e)
      : Expr(kBOOLEAN, a->get_type_info().get_notnull())
      , arg(a)
      , pattern_expr(p)
      , escape_expr(e) {}
  const Expr* get_arg() const { return arg.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }
  const Expr* get_pattern_expr() const { return pattern_expr.get(); }
  const Expr* get_escape_expr() const { return escape_expr.get(); }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    arg->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    arg->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<RegexpExpr>(arg->rewrite_with_targetlist(tlist),
                                pattern_expr->deep_copy(),
                                escape_expr ? escape_expr->deep_copy() : nullptr);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<RegexpExpr>(arg->rewrite_with_child_targetlist(tlist),
                                pattern_expr->deep_copy(),
                                escape_expr ? escape_expr->deep_copy() : nullptr);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<RegexpExpr>(arg->rewrite_agg_to_var(tlist),
                                pattern_expr->deep_copy(),
                                escape_expr ? escape_expr->deep_copy() : nullptr);
  }
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  std::shared_ptr<Analyzer::Expr> arg;  // the argument to the left of REGEXP
  std::shared_ptr<Analyzer::Expr>
      pattern_expr;  // expression that evaluates to pattern string
  std::shared_ptr<Analyzer::Expr>
      escape_expr;  // expression that evaluates to escape string, can be nullptr
};

/*
 * @type LikelihoodExpr
 * @brief expression for LIKELY and UNLIKELY boolean identity functions.
 */
class LikelihoodExpr : public Expr {
 public:
  LikelihoodExpr(std::shared_ptr<Analyzer::Expr> a, float l = 0.5)
      : Expr(kBOOLEAN, a->get_type_info().get_notnull()), arg(a), likelihood(l) {}
  const Expr* get_arg() const { return arg.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }
  float get_likelihood() const { return likelihood; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    arg->collect_rte_idx(rte_idx_set);
  }
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    arg->collect_column_var(colvar_set, include_agg);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<LikelihoodExpr>(arg->rewrite_with_targetlist(tlist), likelihood);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<LikelihoodExpr>(arg->rewrite_with_child_targetlist(tlist),
                                    likelihood);
  }
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override {
    return makeExpr<LikelihoodExpr>(arg->rewrite_agg_to_var(tlist), likelihood);
  }
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  std::shared_ptr<Analyzer::Expr> arg;  // the argument to LIKELY, UNLIKELY
  float likelihood;
};

/*
 * @type AggExpr
 * @brief expression for builtin SQL aggregates.
 */
class AggExpr : public Expr {
 public:
  AggExpr(const SQLTypeInfo& ti,
          SQLAgg a,
          std::shared_ptr<Analyzer::Expr> g,
          bool d,
          std::shared_ptr<Analyzer::Constant> e)
      : Expr(ti, true), aggtype(a), arg(g), is_distinct(d), arg1(e) {}
  AggExpr(SQLTypes t,
          SQLAgg a,
          Expr* g,
          bool d,
          std::shared_ptr<Analyzer::Constant> e,
          int idx)
      : Expr(SQLTypeInfo(t, g == nullptr ? true : g->get_type_info().get_notnull()), true)
      , aggtype(a)
      , arg(g)
      , is_distinct(d)
      , arg1(e) {}
  SQLAgg get_aggtype() const { return aggtype; }
  Expr* get_arg() const { return arg.get(); }
  std::shared_ptr<Analyzer::Expr> get_own_arg() const { return arg; }
  bool get_is_distinct() const { return is_distinct; }
  std::shared_ptr<Analyzer::Constant> get_arg1() const { return arg1; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override {
    if (arg) {
      arg->collect_rte_idx(rte_idx_set);
    }
  };
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override {
    if (include_agg && arg != nullptr) {
      arg->collect_column_var(colvar_set, include_agg);
    }
  }
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  SQLAgg aggtype;                       // aggregate type: kAVG, kMIN, kMAX, kSUM, kCOUNT
  std::shared_ptr<Analyzer::Expr> arg;  // argument to aggregate
  bool is_distinct;                     // true only if it is for COUNT(DISTINCT x)
  // APPROX_COUNT_DISTINCT error_rate, APPROX_QUANTILE quantile
  std::shared_ptr<Analyzer::Constant> arg1;
};

/*
 * @type CaseExpr
 * @brief the CASE-WHEN-THEN-ELSE expression
 */
class CaseExpr : public Expr {
 public:
  CaseExpr(const SQLTypeInfo& ti,
           bool has_agg,
           const std::list<std::pair<std::shared_ptr<Analyzer::Expr>,
                                     std::shared_ptr<Analyzer::Expr>>>& w,
           std::shared_ptr<Analyzer::Expr> e)
      : Expr(ti, has_agg), expr_pair_list(w), else_expr(e) {}
  const std::list<
      std::pair<std::shared_ptr<Analyzer::Expr>, std::shared_ptr<Analyzer::Expr>>>&
  get_expr_pair_list() const {
    return expr_pair_list;
  }
  const Expr* get_else_expr() const { return else_expr.get(); }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override;
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;
  std::shared_ptr<Analyzer::Expr> add_cast(const SQLTypeInfo& new_type_info) override;
  void get_domain(DomainSet& domain_set) const override;

 private:
  std::list<std::pair<std::shared_ptr<Analyzer::Expr>, std::shared_ptr<Analyzer::Expr>>>
      expr_pair_list;  // a pair of expressions for each WHEN expr1 THEN expr2.  expr1
                       // must be of boolean type.  all expr2's must be of compatible
                       // types and will be promoted to the common type.
  std::shared_ptr<Analyzer::Expr> else_expr;  // expression for ELSE.  nullptr if omitted.
};

/*
 * @type ExtractExpr
 * @brief the EXTRACT expression
 */
class ExtractExpr : public Expr {
 public:
  ExtractExpr(const SQLTypeInfo& ti,
              bool has_agg,
              ExtractField f,
              std::shared_ptr<Analyzer::Expr> e)
      : Expr(ti, has_agg), field_(f), from_expr_(e) {}
  ExtractField get_field() const { return field_; }
  const Expr* get_from_expr() const { return from_expr_.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_from_expr() const { return from_expr_; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override;
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  ExtractField field_;
  std::shared_ptr<Analyzer::Expr> from_expr_;
};

/*
 * @type DateaddExpr
 * @brief the DATEADD expression
 */
class DateaddExpr : public Expr {
 public:
  DateaddExpr(const SQLTypeInfo& ti,
              const DateaddField f,
              const std::shared_ptr<Analyzer::Expr> number,
              const std::shared_ptr<Analyzer::Expr> datetime)
      : Expr(ti, false), field_(f), number_(number), datetime_(datetime) {}
  DateaddField get_field() const { return field_; }
  const Expr* get_number_expr() const { return number_.get(); }
  const Expr* get_datetime_expr() const { return datetime_.get(); }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override;
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  const DateaddField field_;
  const std::shared_ptr<Analyzer::Expr> number_;
  const std::shared_ptr<Analyzer::Expr> datetime_;
};

/*
 * @type DatediffExpr
 * @brief the DATEDIFF expression
 */
class DatediffExpr : public Expr {
 public:
  DatediffExpr(const SQLTypeInfo& ti,
               const DatetruncField f,
               const std::shared_ptr<Analyzer::Expr> start,
               const std::shared_ptr<Analyzer::Expr> end)
      : Expr(ti, false), field_(f), start_(start), end_(end) {}
  DatetruncField get_field() const { return field_; }
  const Expr* get_start_expr() const { return start_.get(); }
  const Expr* get_end_expr() const { return end_.get(); }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override;
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  const DatetruncField field_;
  const std::shared_ptr<Analyzer::Expr> start_;
  const std::shared_ptr<Analyzer::Expr> end_;
};

/*
 * @type DatetruncExpr
 * @brief the DATE_TRUNC expression
 */
class DatetruncExpr : public Expr {
 public:
  DatetruncExpr(const SQLTypeInfo& ti,
                bool has_agg,
                DatetruncField f,
                std::shared_ptr<Analyzer::Expr> e)
      : Expr(ti, has_agg), field_(f), from_expr_(e) {}
  DatetruncField get_field() const { return field_; }
  const Expr* get_from_expr() const { return from_expr_.get(); }
  const std::shared_ptr<Analyzer::Expr> get_own_from_expr() const { return from_expr_; }
  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void check_group_by(
      const std::list<std::shared_ptr<Analyzer::Expr>>& groupby) const override;
  void group_predicates(std::list<const Expr*>& scan_predicates,
                        std::list<const Expr*>& join_predicates,
                        std::list<const Expr*>& const_predicates) const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override;
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_with_child_targetlist(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  std::shared_ptr<Analyzer::Expr> rewrite_agg_to_var(
      const std::vector<std::shared_ptr<TargetEntry>>& tlist) const override;
  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
  void find_expr(bool (*f)(const Expr*),
                 std::list<const Expr*>& expr_list) const override;

 private:
  DatetruncField field_;
  std::shared_ptr<Analyzer::Expr> from_expr_;
};

class FunctionOper : public Expr {
 public:
  FunctionOper(const SQLTypeInfo& ti,
               const std::string& name,
               const std::vector<std::shared_ptr<Analyzer::Expr>>& args)
      : Expr(ti, false), name_(name), args_(args) {}

  std::string getName() const { return name_; }

  size_t getArity() const { return args_.size(); }

  const Analyzer::Expr* getArg(const size_t i) const {
    CHECK_LT(i, args_.size());
    return args_[i].get();
  }

  std::shared_ptr<Analyzer::Expr> getOwnArg(const size_t i) const {
    CHECK_LT(i, args_.size());
    return args_[i];
  }

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;
  void collect_rte_idx(std::set<int>& rte_idx_set) const override;
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override;

  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;

 private:
  const std::string name_;
  const std::vector<std::shared_ptr<Analyzer::Expr>> args_;
};

class FunctionOperWithCustomTypeHandling : public FunctionOper {
 public:
  FunctionOperWithCustomTypeHandling(
      const SQLTypeInfo& ti,
      const std::string& name,
      const std::vector<std::shared_ptr<Analyzer::Expr>>& args)
      : FunctionOper(ti, name, args) {}

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  bool operator==(const Expr& rhs) const override;
};

/*
 * @type OffsetInFragment
 * @brief The offset of a row in the current fragment. To be used by updates.
 */
class OffsetInFragment : public Expr {
 public:
  OffsetInFragment() : Expr(SQLTypeInfo(kBIGINT, true)){};

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;
};

/*
 * @type OrderEntry
 * @brief represents an entry in ORDER BY clause.
 */
struct OrderEntry {
  OrderEntry(int t, bool d, bool nf) : tle_no(t), is_desc(d), nulls_first(nf){};
  ~OrderEntry() {}
  std::string toString() const;
  void print() const { std::cout << toString(); }
  int tle_no;       /* targetlist entry number: 1-based */
  bool is_desc;     /* true if order is DESC */
  bool nulls_first; /* true if nulls are ordered first.  otherwise last. */
};

/*
 * @type WindowFunction
 * @brief A window function.
 */
class WindowFunction : public Expr {
 public:
  WindowFunction(const SQLTypeInfo& ti,
                 const SqlWindowFunctionKind kind,
                 const std::vector<std::shared_ptr<Analyzer::Expr>>& args,
                 const std::vector<std::shared_ptr<Analyzer::Expr>>& partition_keys,
                 const std::vector<std::shared_ptr<Analyzer::Expr>>& order_keys,
                 const std::vector<OrderEntry>& collation)
      : Expr(ti)
      , kind_(kind)
      , args_(args)
      , partition_keys_(partition_keys)
      , order_keys_(order_keys)
      , collation_(collation){};

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;

  SqlWindowFunctionKind getKind() const { return kind_; }

  const std::vector<std::shared_ptr<Analyzer::Expr>>& getArgs() const { return args_; }

  const std::vector<std::shared_ptr<Analyzer::Expr>>& getPartitionKeys() const {
    return partition_keys_;
  }

  const std::vector<std::shared_ptr<Analyzer::Expr>>& getOrderKeys() const {
    return order_keys_;
  }

  const std::vector<OrderEntry>& getCollation() const { return collation_; }

 private:
  const SqlWindowFunctionKind kind_;
  const std::vector<std::shared_ptr<Analyzer::Expr>> args_;
  const std::vector<std::shared_ptr<Analyzer::Expr>> partition_keys_;
  const std::vector<std::shared_ptr<Analyzer::Expr>> order_keys_;
  const std::vector<OrderEntry> collation_;
};

/*
 * @type ArrayExpr
 * @brief Corresponds to ARRAY[] statements in SQL
 */

class ArrayExpr : public Expr {
 public:
  ArrayExpr(SQLTypeInfo const& array_ti,
            ExpressionPtrVector const& array_exprs,
            bool is_null = false,
            bool local_alloc = false)
      : Expr(array_ti)
      , contained_expressions_(array_exprs)
      , local_alloc_(local_alloc)
      , is_null_(is_null) {}

  Analyzer::ExpressionPtr deep_copy() const override;
  std::string toString() const override;
  bool operator==(Expr const& rhs) const override;
  size_t getElementCount() const { return contained_expressions_.size(); }
  bool isLocalAlloc() const { return local_alloc_; }
  bool isNull() const { return is_null_; }

  const Analyzer::Expr* getElement(const size_t i) const {
    CHECK_LT(i, contained_expressions_.size());
    return contained_expressions_[i].get();
  }

  void collect_rte_idx(std::set<int>& rte_idx_set) const override;
  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const override;

 private:
  ExpressionPtrVector contained_expressions_;
  bool local_alloc_;
  bool is_null_;  // constant is NULL
};

/*
 * @type GeoUOper
 * @brief Geo unary operation
 */
class GeoUOper : public Expr {
 public:
  GeoUOper(const Geospatial::GeoBase::GeoOp op,
           const SQLTypeInfo& ti,
           const SQLTypeInfo& ti0,
           const std::vector<std::shared_ptr<Analyzer::Expr>>& args)
      : Expr(ti), op_(op), ti0_(ti0), args0_(args){};

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;

  Geospatial::GeoBase::GeoOp getOp() const { return op_; }
  const SQLTypeInfo getTypeInfo0() const { return ti0_; }
  const std::vector<std::shared_ptr<Analyzer::Expr>>& getArgs0() const { return args0_; }

 private:
  const Geospatial::GeoBase::GeoOp op_;
  SQLTypeInfo ti0_;  // Type of geo input 0 (or geo output)
  const std::vector<std::shared_ptr<Analyzer::Expr>> args0_;
};

/*
 * @type GeoBinOper
 * @brief Geo binary operation
 */
class GeoBinOper : public Expr {
 public:
  GeoBinOper(const Geospatial::GeoBase::GeoOp op,
             const SQLTypeInfo& ti,
             const SQLTypeInfo& ti0,
             const SQLTypeInfo& ti1,
             const std::vector<std::shared_ptr<Analyzer::Expr>>& args0,
             const std::vector<std::shared_ptr<Analyzer::Expr>>& args1)
      : Expr(ti), op_(op), ti0_(ti0), ti1_(ti1), args0_(args0), args1_(args1){};

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  bool operator==(const Expr& rhs) const override;
  std::string toString() const override;

  Geospatial::GeoBase::GeoOp getOp() const { return op_; }
  const SQLTypeInfo getTypeInfo0() const { return ti0_; }
  const SQLTypeInfo getTypeInfo1() const { return ti1_; }
  const std::vector<std::shared_ptr<Analyzer::Expr>>& getArgs0() const { return args0_; }
  const std::vector<std::shared_ptr<Analyzer::Expr>>& getArgs1() const { return args1_; }

 private:
  const Geospatial::GeoBase::GeoOp op_;
  SQLTypeInfo ti0_;  // Type of geo input 0 (or geo output)
  SQLTypeInfo ti1_;  // Type of geo input 1
  const std::vector<std::shared_ptr<Analyzer::Expr>> args0_;
  const std::vector<std::shared_ptr<Analyzer::Expr>> args1_;
};

/*
 * @type TargetEntry
 * @brief Target list defines a relational projection.  It is a list of TargetEntry's.
 */
class TargetEntry {
 public:
  TargetEntry(const std::string& n, std::shared_ptr<Analyzer::Expr> e, bool u)
      : resname(n), expr(e), unnest(u) {}
  virtual ~TargetEntry() {}
  const std::string& get_resname() const { return resname; }
  void set_resname(const std::string& name) { resname = name; }
  Expr* get_expr() const { return expr.get(); }
  std::shared_ptr<Expr> get_own_expr() const { return expr; }
  void set_expr(std::shared_ptr<Analyzer::Expr> e) { expr = e; }
  bool get_unnest() const { return unnest; }
  std::string toString() const;
  void print() const { std::cout << toString(); }

 private:
  std::string resname;  // alias name, e.g., SELECT salary + bonus AS compensation,
  std::shared_ptr<Analyzer::Expr> expr;  // expression to evaluate for the value
  bool unnest;                           // unnest a collection type
};

class RangeTableEntry;

/*
 * @type Query
 * @brief parse tree for a query
 */
class Query {
 public:
  Query()
      : is_distinct(false)
      , where_predicate(nullptr)
      , having_predicate(nullptr)
      , order_by(nullptr)
      , next_query(nullptr)
      , is_unionall(false)
      , stmt_type(kSELECT)
      , num_aggs(0)
      , result_table_id(0)
      , limit(0)
      , offset(0) {}
  virtual ~Query();
  bool get_is_distinct() const { return is_distinct; }
  int get_num_aggs() const { return num_aggs; }
  const std::vector<std::shared_ptr<TargetEntry>>& get_targetlist() const {
    return targetlist;
  }
  std::vector<std::shared_ptr<TargetEntry>>& get_targetlist_nonconst() {
    return targetlist;
  }
  const std::vector<RangeTableEntry*>& get_rangetable() const { return rangetable; }
  const Expr* get_where_predicate() const { return where_predicate.get(); }
  const std::list<std::shared_ptr<Analyzer::Expr>>& get_group_by() const {
    return group_by;
  };
  const Expr* get_having_predicate() const { return having_predicate.get(); }
  const std::list<OrderEntry>* get_order_by() const { return order_by; }
  const Query* get_next_query() const { return next_query; }
  SQLStmtType get_stmt_type() const { return stmt_type; }
  bool get_is_unionall() const { return is_unionall; }
  int get_result_table_id() const { return result_table_id; }
  const std::list<int>& get_result_col_list() const { return result_col_list; }
  void set_result_col_list(const std::list<int>& col_list) { result_col_list = col_list; }
  void set_result_table_id(int id) { result_table_id = id; }
  void set_is_distinct(bool d) { is_distinct = d; }
  void set_where_predicate(std::shared_ptr<Analyzer::Expr> p) { where_predicate = p; }
  void set_group_by(std::list<std::shared_ptr<Analyzer::Expr>>& g) { group_by = g; }
  void set_having_predicate(std::shared_ptr<Analyzer::Expr> p) { having_predicate = p; }
  void set_order_by(std::list<OrderEntry>* o) { order_by = o; }
  void set_next_query(Query* q) { next_query = q; }
  void set_is_unionall(bool u) { is_unionall = u; }
  void set_stmt_type(SQLStmtType t) { stmt_type = t; }
  void set_num_aggs(int a) { num_aggs = a; }
  int get_rte_idx(const std::string& range_var_name) const;
  RangeTableEntry* get_rte(int rte_idx) const { return rangetable[rte_idx]; }
  void add_rte(RangeTableEntry* rte);
  void add_tle(std::shared_ptr<TargetEntry> tle) { targetlist.push_back(tle); }
  int64_t get_limit() const { return limit; }
  void set_limit(int64_t l) { limit = l; }
  int64_t get_offset() const { return offset; }
  void set_offset(int64_t o) { offset = o; }

 private:
  bool is_distinct;                                      // true only if SELECT DISTINCT
  std::vector<std::shared_ptr<TargetEntry>> targetlist;  // represents the SELECT clause
  std::vector<RangeTableEntry*> rangetable;  // represents the FROM clause for SELECT. For
                                             // INSERT, DELETE, UPDATE the result table is
                                             // always the first entry in rangetable.
  std::shared_ptr<Analyzer::Expr> where_predicate;      // represents the WHERE clause
  std::list<std::shared_ptr<Analyzer::Expr>> group_by;  // represents the GROUP BY clause
  std::shared_ptr<Analyzer::Expr> having_predicate;     // represents the HAVING clause
  std::list<OrderEntry>* order_by;                      // represents the ORDER BY clause
  Query* next_query;                                    // the next query to UNION
  bool is_unionall;                                     // true only if it is UNION ALL
  SQLStmtType stmt_type;
  int num_aggs;                    // number of aggregate functions in query
  int result_table_id;             // for INSERT statements only
  std::list<int> result_col_list;  // for INSERT statement only
  int64_t limit;                   // row count for LIMIT clause.  0 means ALL
  int64_t offset;                  // offset in OFFSET clause.  0 means no offset.
};

class GeoExpr : public Expr {
 public:
  GeoExpr(const SQLTypeInfo& ti) : Expr(ti) {}

  // geo expressions might hide child expressions (e.g. constructors). Centralize logic
  // for pulling out child expressions for simplicity in visitors.
  virtual std::vector<Analyzer::Expr*> getChildExprs() const { return {}; }
};

/**
 * An umbrella column var holding all information required to generate code for
 * subcolumns. Note that this currently inherits from ColumnVar, not the GeoExpr umbrella
 * type, because we use the same codegen paths regardless of columnvar type.
 */
class GeoColumnVar : public ColumnVar {
 public:
  GeoColumnVar(const SQLTypeInfo& ti,
               const int table_id,
               const int column_id,
               const int range_table_index,
               const bool with_bounds,
               const bool with_render_group)
      : ColumnVar(ti, table_id, column_id, range_table_index)
      , with_bounds_(with_bounds)
      , with_render_group_(with_render_group) {}

  GeoColumnVar(const Analyzer::ColumnVar* column_var,
               const bool with_bounds,
               const bool with_render_group)
      : ColumnVar(column_var->get_type_info(),
                  column_var->get_table_id(),
                  column_var->get_column_id(),
                  column_var->get_rte_idx())
      , with_bounds_(with_bounds)
      , with_render_group_(with_render_group) {}

 protected:
  bool with_bounds_;
  bool with_render_group_;
};

class GeoConstant : public GeoExpr {
 public:
  GeoConstant(std::unique_ptr<Geospatial::GeoBase>&& geo, const SQLTypeInfo& ti);

  std::shared_ptr<Analyzer::Expr> deep_copy() const final;

  std::string toString() const final;

  bool operator==(const Expr&) const final;

  size_t physicalCols() const;

  std::shared_ptr<Analyzer::Constant> makePhysicalConstant(const size_t index) const;

 private:
  std::unique_ptr<Geospatial::GeoBase> geo_;
};

/**
 * GeoOperator: A geo expression that transforms or accesses an input. Typically a
 * geospatial function prefixed with ST_
 */
class GeoOperator : public GeoExpr {
 public:
  GeoOperator(const SQLTypeInfo& ti,
              const std::string& name,
              const std::vector<std::shared_ptr<Analyzer::Expr>>& args);

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  void collect_rte_idx(std::set<int>& rte_idx_set) const final;

  void collect_column_var(
      std::set<const ColumnVar*, bool (*)(const ColumnVar*, const ColumnVar*)>&
          colvar_set,
      bool include_agg) const final;

  std::string toString() const override;

  bool operator==(const Expr&) const override;

  size_t size() const;

  Analyzer::Expr* getOperand(const size_t index) const;

  const std::string& getName() const { return name_; }

  std::vector<Analyzer::Expr*> getChildExprs() const override {
    std::vector<Analyzer::Expr*> ret;
    ret.reserve(args_.size());
    for (const auto& arg : args_) {
      ret.push_back(arg.get());
    }
    return ret;
  }

  std::shared_ptr<Analyzer::Expr> cloneWithUpdatedTypeInfo(const SQLTypeInfo& ti);

 protected:
  const std::string name_;
  const std::vector<std::shared_ptr<Analyzer::Expr>> args_;
};

class GeoTransformOperator : public GeoOperator {
 public:
  GeoTransformOperator(const SQLTypeInfo& ti,
                       const std::string& name,
                       const std::vector<std::shared_ptr<Analyzer::Expr>>& args,
                       const int32_t input_srid,
                       const int32_t output_srid)
      : GeoOperator(ti, name, args), input_srid_(input_srid), output_srid_(output_srid) {}

  std::shared_ptr<Analyzer::Expr> deep_copy() const override;

  std::string toString() const override;

  bool operator==(const Expr&) const override;

  int32_t getInputSRID() const { return input_srid_; }

  int32_t getOutputSRID() const { return output_srid_; }

 private:
  const int32_t input_srid_;
  const int32_t output_srid_;
};

}  // namespace Analyzer

inline std::shared_ptr<Analyzer::Var> var_ref(const Analyzer::Expr* expr,
                                              const Analyzer::Var::WhichRow which_row,
                                              const int varno) {
  const auto col_expr = dynamic_cast<const Analyzer::ColumnVar*>(expr);
  const int table_id = col_expr ? col_expr->get_table_id() : 0;
  const int column_id = col_expr ? col_expr->get_column_id() : 0;
  const int rte_idx = col_expr ? col_expr->get_rte_idx() : -1;
  return makeExpr<Analyzer::Var>(
      expr->get_type_info(), table_id, column_id, rte_idx, which_row, varno);
}

// Returns true iff the two expression lists are equal (same size and each element are
// equal).
bool expr_list_match(const std::vector<std::shared_ptr<Analyzer::Expr>>& lhs,
                     const std::vector<std::shared_ptr<Analyzer::Expr>>& rhs);

// Remove a cast operator if present.
std::shared_ptr<Analyzer::Expr> remove_cast(const std::shared_ptr<Analyzer::Expr>& expr);

#endif  // ANALYZER_H
