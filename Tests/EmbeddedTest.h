/*
 * Copyright 2020 OmniSci, Inc.
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

#include <gtest/gtest.h>
#include "Embedded/DBEngine.h"
#include <boost/program_options.hpp>

#ifndef BASE_PATH
#define BASE_PATH "./tmp"
#endif


std::shared_ptr<EmbeddedDatabase::DBEngine> engine;

class DBEngineSQLTest : public ::testing::Test {
protected:
  DBEngineSQLTest() {}
  virtual ~DBEngineSQLTest() {}

  void SetUp(const std::string &table_spec) {
    run_ddl("DROP TABLE IF EXISTS dbe_test;");
    run_ddl("CREATE TABLE dbe_test " + table_spec + ";");
  }

  virtual void TearDown() {
    run_ddl("DROP TABLE IF EXISTS dbe_test;");
  }

  int select_int(const std::string& query_str) {
    auto cursor = ::engine->executeDML(query_str);
    auto row = cursor->getNextRow();
    return row.getInt(0);
  }

  float select_float(const std::string& query_str) {
    auto cursor = ::engine->executeDML(query_str);
    auto row = cursor->getNextRow();
    return row.getFloat(0);
  }

  double select_double(const std::string& query_str) {
    auto cursor = ::engine->executeDML(query_str);
    auto row = cursor->getNextRow();
    return row.getDouble(0);
  }

  void run_ddl(const std::string& query_str) {
    ::engine->executeDDL(query_str);
  }

  std::shared_ptr<arrow::RecordBatch> run_dml(const std::string& query_str) {
    auto cursor = ::engine->executeDML(query_str);
    return cursor ? cursor->getArrowRecordBatch(): nullptr;
  }
};
