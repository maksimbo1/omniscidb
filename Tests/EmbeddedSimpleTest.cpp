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

#include <gtest/gtest.h>
#include "Embedded/DBEngine.h"
#include "EmbeddedTest.h"
#include <boost/program_options.hpp>

#include <iostream>

using namespace std;
using namespace EmbeddedDatabase;


TEST_F(DBEngineSQLTest, SimpleTest) {
  EXPECT_NO_THROW(SetUp("(t int not null)"));
  // EXPECT_NO_THROW(run_dml("insert into dbe_test values (5), (6), (1), (2);"));
  cout << "Set up" << endl;
  EXPECT_NO_THROW(run_dml("insert into dbe_test values (5);"));
  cout << "Dml ran" << endl;

  ASSERT_EQ(1, select_int("select count(t) from dbe_test;"));
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);

  namespace po = boost::program_options;

  po::options_description desc("Options");

  // these two are here to allow passing correctly google testing parameters
  desc.add_options()("gtest_list_tests", "list all tests");
  desc.add_options()("gtest_filter", "filters tests, use --help for details");

  desc.add_options()("test-help",
                     "Print all EmbeddedDbTest specific options (for gtest "
                     "options use `--help`).");

  logger::LogOptions log_options(argv[0]);
  log_options.max_files_ = 0;  // stderr only by default
  desc.add(log_options.get_options());

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
  po::notify(vm);

  if (vm.count("test-help")) {
    std::cout << "Usage: EmbeddedDbTest" << std::endl << std::endl;
    std::cout << desc << std::endl;
    return 0;
  }

  logger::init(log_options);

  std::map<std::string, std::string> parameters = {
    {"path", std::string(BASE_PATH)}};

  engine = DBEngine::create(parameters);

  int err{0};

  try {
    err = RUN_ALL_TESTS();
    engine.reset();
  } catch (const std::exception& e) {
    LOG(ERROR) << e.what();
  }
  return err;
}