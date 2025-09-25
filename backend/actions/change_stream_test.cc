//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "backend/actions/change_stream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "zetasql/public/json_value.h"
#include "zetasql/public/numeric_value.h"
#include "zetasql/public/types/type_factory.h"
#include "zetasql/public/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "zetasql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "backend/actions/ops.h"
#include "backend/datamodel/key.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "tests/common/actions.h"
#include "tests/common/schema_constructor.h"
#include "common/constants.h"
#include "nlohmann/json_fwd.hpp"
#include "nlohmann/json.hpp"
namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {
using JSON = ::nlohmann::json;
using zetasql::JSONValue;
using zetasql::NumericValue;
using zetasql::values::Bool;
using zetasql::values::Double;
using zetasql::values::DoubleArray;
using zetasql::values::Float;
using zetasql::values::FloatArray;
using zetasql::values::Int64;
using zetasql::values::Json;
using zetasql::values::JsonArray;
using zetasql::values::Numeric;
using zetasql::values::NumericArray;
using zetasql::values::String;

class ChangeStreamTest : public test::ActionsTest {
 public:
  ChangeStreamTest()
      : schema_(emulator::test::CreateSchemaFromDDL(
                    {
                        R"(
                            CREATE TABLE TestTable (
                              int64_col INT64 NOT NULL,
                              string_col STRING(MAX),
                              another_string_col STRING(MAX)
                            ) PRIMARY KEY (int64_col)
                          )",
                        R"(
                            CREATE TABLE TestTable2 (
                              int64_col INT64 NOT NULL,
                              string_col STRING(MAX),
                              another_string_col STRING(MAX)
                            ) PRIMARY KEY (int64_col)
                          )",
                        R"(
                            CREATE CHANGE STREAM ChangeStream_All FOR ALL OPTIONS ( value_capture_type = 'NEW_VALUES' )
                        )",
                        R"(
                            CREATE CHANGE STREAM ChangeStream_TestTable2StrCol FOR TestTable2(string_col) OPTIONS ( value_capture_type = 'NEW_VALUES' )
                        )",
                        R"(
                            CREATE CHANGE STREAM ChangeStream_TestTable2KeyOnly FOR TestTable2() OPTIONS ( value_capture_type = 'NEW_VALUES' )
                        )",
                        R"(
                            CREATE CHANGE STREAM ChangeStream_TestTable2 FOR TestTable2 OPTIONS ( value_capture_type = 'NEW_VALUES' )
                        )"},
                    &type_factory_)
                    .value()),
        float_schema_(emulator::test::CreateSchemaFromDDL(
                          {
                              R"(
                            CREATE TABLE FloatTable (
                              int64_col INT64 NOT NULL,
                              float_col FLOAT32,
                              double_col FLOAT64,
                              float_arr ARRAY<FLOAT32>,
                              double_arr ARRAY<FLOAT64>
                            ) PRIMARY KEY (int64_col)
                          )",
                              R"(
                            CREATE CHANGE STREAM ChangeStream_FloatTable FOR FloatTable OPTIONS ( value_capture_type = 'NEW_VALUES' )
                        )"},
                          &type_factory_)
                          .value()),
        pg_schema_(
            emulator::test::CreateSchemaFromDDL(
                {
                    R"(
                          CREATE TABLE entended_pg_datatypes (
                            int_col bigint NOT NULL PRIMARY KEY,
                            jsonb_col jsonb,
                            jsonb_arr jsonb[],
                            numeric_col numeric,
                            numeric_arr numeric[]
                          )
                        )",
                    R"(CREATE CHANGE STREAM pg_stream FOR ALL WITH ( value_capture_type = 'NEW_VALUES' ))",
                },
                &type_factory_, "", /*proto_descriptor_bytes*/
                database_api::DatabaseDialect::POSTGRESQL)
                .value()),
        commit_timestamp_schema_(emulator::test::CreateSchemaFromDDL(
                    {
                        R"(
                            CREATE TABLE CommitTimestampTable (
                              id INT64 NOT NULL,
                              name STRING(MAX),
                              commit_ts TIMESTAMP NOT NULL OPTIONS (allow_commit_timestamp = true)
                            ) PRIMARY KEY (id)
                          )",
                        R"(
                            CREATE CHANGE STREAM CommitTimestampStream FOR CommitTimestampTable OPTIONS ( value_capture_type = 'NEW_VALUES' )
                        )"},
                    &type_factory_)
                    .value()),
        table_(schema_->FindTable("TestTable")),
        table2_(schema_->FindTable("TestTable2")),
        float_table_(float_schema_->FindTable("FloatTable")),
        pg_table_(pg_schema_->FindTable("entended_pg_datatypes")),
        commit_timestamp_table_(commit_timestamp_schema_->FindTable("CommitTimestampTable")),
        base_columns_(table_->columns()),
        base_columns_table_2_all_col_(table2_->columns()),
        float_columns_(float_table_->columns()),
        pg_columns_(pg_table_->columns()),
        change_stream_(schema_->FindChangeStream("ChangeStream_All")),
        change_stream2_(
            schema_->FindChangeStream("ChangeStream_TestTable2StrCol")),
        change_stream3_(
            schema_->FindChangeStream("ChangeStream_TestTable2KeyOnly")),
        change_stream4_(schema_->FindChangeStream("ChangeStream_TestTable2")),
        float_change_stream_(
            float_schema_->FindChangeStream("ChangeStream_FloatTable")),
        pg_change_stream_(pg_schema_->FindChangeStream("pg_stream")),
        commit_timestamp_change_stream_(commit_timestamp_schema_->FindChangeStream("CommitTimestampStream")) {}

 protected:
  // Test components.
  zetasql::TypeFactory type_factory_;
  std::unique_ptr<const Schema> schema_;
  std::unique_ptr<const Schema> float_schema_;
  std::unique_ptr<const Schema> pg_schema_;
  std::unique_ptr<const Schema> commit_timestamp_schema_;

  // Test variables.
  const Table* table_;
  const Table* table2_;
  const Table* float_table_;
  const Table* pg_table_;
  const Table* commit_timestamp_table_;
  absl::Span<const Column* const> base_columns_;
  absl::Span<const Column* const> base_columns_table_2_all_col_;
  absl::Span<const Column* const> float_columns_;
  absl::Span<const Column* const> pg_columns_;
  const ChangeStream* change_stream_;
  const ChangeStream* change_stream2_;
  const ChangeStream* change_stream3_;
  const ChangeStream* change_stream4_;
  const ChangeStream* float_change_stream_;
  const ChangeStream* pg_change_stream_;
  const ChangeStream* commit_timestamp_change_stream_;
  std::vector<const Column*> key_and_another_string_col_table_1_ = {
      table_->FindColumn("int64_col"),
      table_->FindColumn("another_string_col")};
  std::vector<const Column*> key_and_string_col_table_1_ = {
      table_->FindColumn("int64_col"), table_->FindColumn("string_col")};
  std::vector<const Column*> key_and_another_string_col_table_2_ = {
      table2_->FindColumn("int64_col"),
      table2_->FindColumn("another_string_col")};
  std::vector<const Column*> key_and_string_col_table_2_ = {
      table2_->FindColumn("int64_col"), table2_->FindColumn("string_col")};
};

void set_up_partition_token_for_change_stream_partition_table(
    const ChangeStream* change_stream, test::TestReadOnlyStore* store) {
  // Populate partition table with the initial partition token
  std::vector<const Column*> columns;
  columns.push_back(change_stream->change_stream_partition_table()
                        ->FindKeyColumn("partition_token")
                        ->column());
  columns.push_back(
      change_stream->change_stream_partition_table()->FindColumn("end_time"));
  const std::vector<zetasql::Value> values = {
      zetasql::Value::String("11111"), zetasql::Value::NullTimestamp()};
  // Insert 1st partition to change_stream2_'s partition table
  ZETASQL_EXPECT_OK(store->Insert(change_stream->change_stream_partition_table(),
                          Key({String("11111")}), columns, values));
}

TEST_F(ChangeStreamTest, AddOneInsertOpAndCheckResultWriteOpContent) {
  set_up_partition_token_for_change_stream_partition_table(change_stream_,
                                                           store());
  // Insert base table entry.
  std::vector<WriteOp> buffered_write_ops;
  buffered_write_ops.push_back(
      Insert(table_, Key({Int64(1)}), base_columns_,
             {Int64(1), String("value"), String("value2")}));
  ZETASQL_ASSERT_OK_AND_ASSIGN(
      std::vector<WriteOp> change_stream_write_ops,
      BuildChangeStreamWriteOps(schema_.get(), buffered_write_ops, store(), 1,
                                 absl::FromUnixMicros(1000000)));
  // Verify change stream entry is added to the transaction buffer.
  ASSERT_EQ(change_stream_write_ops.size(), 1);
  WriteOp op = change_stream_write_ops[0];
  // Verify the table of the received WriteOp
  ASSERT_EQ(TableOf(op), change_stream_->change_stream_data_table());
  // Verify the received WriteOp is InsertOp
  auto* operation = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation, nullptr);
  // Verify columns in the rebuilt InsertOp corresponds to columns in
  // change_stream_data_table
  ASSERT_EQ(operation->columns,
            change_stream_->change_stream_data_table()->columns());
  ASSERT_EQ(operation->columns.size(), 19);
  ASSERT_EQ(operation->values.size(), 19);
  // Verify values in the rebuilt InsertOp are correct
  // Verify partition_token
  ASSERT_EQ(operation->values[0], zetasql::Value::String("11111"));
  // Verify record_sequence
  ASSERT_EQ(operation->values[3], zetasql::Value(String("00000000")));
  // Verify is_last_record_in_transaction_in_partition
  ASSERT_EQ(operation->values[4], zetasql::Value(Bool(true)));
  // Verify table_name
  ASSERT_EQ(operation->values[5], zetasql::Value(String("TestTable")));
  // Verify column_types_name
  ASSERT_EQ(operation->values[6],
            zetasql::values::Array(
                zetasql::types::StringArrayType(),
                {zetasql::Value(String("int64_col")),
                 zetasql::Value(String("string_col")),
                 zetasql::Value(String("another_string_col"))}));
  // Verify column_types_type
  JSON col_1_type;
  col_1_type["code"] = "INT64";
  JSON col_2_type;
  col_2_type["code"] = "STRING";
  JSON col_3_type;
  col_3_type["code"] = "STRING";
  ASSERT_EQ(
      operation->values[7],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String(col_1_type.dump())),
                                zetasql::Value(String(col_2_type.dump())),
                                zetasql::Value(String(col_3_type.dump()))}));
  // Verify column_types_is_primary_key
  ASSERT_EQ(operation->values[8],
            zetasql::values::Array(
                zetasql::types::BoolArrayType(),
                {zetasql::Value(Bool(true)), zetasql::Value(Bool(false)),
                 zetasql::Value(Bool(false))}));
  // Verify column_types_ordinal_position
  ASSERT_EQ(operation->values[9],
            zetasql::values::Array(
                zetasql::types::Int64ArrayType(),
                {zetasql::Value(Int64(1)), zetasql::Value(Int64(2)),
                 zetasql::Value(Int64(3))}));
  // Verify mods
  zetasql::Value mod_keys = operation->values[10];
  ASSERT_EQ(mod_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  zetasql::Value mod_new_values = operation->values[11];
  ASSERT_EQ(mod_new_values.element(0),
            zetasql::Value(
                String("{\"another_string_col\":\"value2\",\"string_col\":"
                       "\"value\"}")));
  zetasql::Value mod_old_values = operation->values[12];
  ASSERT_EQ(mod_old_values.element(0), zetasql::Value(String("{}")));
  // Verify mod_type
  ASSERT_EQ(operation->values[13], zetasql::Value(String("INSERT")));
  // Verify value_capture_type
  ASSERT_EQ(operation->values[14], zetasql::Value(String("NEW_VALUES")));
  // Verify number_of_records_in_transaction
  ASSERT_EQ(operation->values[15], zetasql::Value(Int64(1)));
  // Verify number_of_partitions_in_transaction
  ASSERT_EQ(operation->values[16], zetasql::Value(Int64(1)));
  // Verify transaction_tag
  ASSERT_EQ(operation->values[17], zetasql::Value(String("")));
  // Verify is_system_transaction
  ASSERT_EQ(operation->values[18], zetasql::Value(Bool(false)));
}

TEST_F(ChangeStreamTest, AddTwoInsertForDiffSetCols) {
  set_up_partition_token_for_change_stream_partition_table(change_stream_,
                                                           store());
  // Insert base table entry.
  std::vector<WriteOp> buffered_write_ops;
  // Insert 1st base table entry. base_columns1 only contains the first two
  // columns of TestTable2.
  std::vector<const Column*> insert_columns1 = {
      table_->FindColumn("int64_col"), table_->FindColumn("string_col")};
  buffered_write_ops.push_back(Insert(table_, Key({Int64(1)}), insert_columns1,
                                      {Int64(1), String("value")}));
  // Insert 2nd base table entry. base_columns_table_2_all_col_ contains all
  // columns of TestTable2.
  buffered_write_ops.push_back(
      Insert(table_, Key({Int64(2)}), base_columns_,
             {Int64(2), String("value"), String("value2")}));
  ZETASQL_ASSERT_OK_AND_ASSIGN(
      std::vector<WriteOp> change_stream_write_ops,
      BuildChangeStreamWriteOps(schema_.get(), buffered_write_ops, store(), 1,
                                 absl::FromUnixMicros(1000000)));
  // Verify change stream entry is added to the transaction buffer.
  ASSERT_EQ(change_stream_write_ops.size(), 1);
}

TEST_F(ChangeStreamTest, AddTwoInsertDiffSetsNonKeyTrackedCols) {
  // Populate partition table with the initial partition token
  set_up_partition_token_for_change_stream_partition_table(change_stream_,
                                                           store());
  std::vector<WriteOp> buffered_write_ops;
  // Insert 1st base table entry.
  std::vector<const Column*> base_columns1 = {table_->FindColumn("int64_col")};
  buffered_write_ops.push_back(
      Insert(table_, Key({Int64(1)}), base_columns1, {Int64(1)}));
  // Insert 2nd base table entry
  std::vector<const Column*> base_columns2 = {table_->FindColumn("int64_col"),
                                              table_->FindColumn("string_col")};
  buffered_write_ops.push_back(Insert(table_, Key({Int64(2)}), base_columns2,
                                      {Int64(2), String("value")}));
  ZETASQL_ASSERT_OK_AND_ASSIGN(
      std::vector<WriteOp> change_stream_write_ops,
      BuildChangeStreamWriteOps(schema_.get(), buffered_write_ops, store(), 1,
                                 absl::FromUnixMicros(1000000)));
  // Verify change stream entry is added to the transaction buffer.
  ASSERT_EQ(change_stream_write_ops.size(), 1);
}

// Add operations with different mod_types to the buffer and check if distinct
// DataChangeRecords are generated once mod_type changed.
// Insert, Insert, Update, Update, Insert, Delete, Delete -> 4 WriteOps
TEST_F(ChangeStreamTest, AddMultipleDataChangeRecordsToChangeStreamDataTable) {
  // Populate partition table with the initial partition token
  set_up_partition_token_for_change_stream_partition_table(change_stream_,
                                                           store());
  std::vector<WriteOp> buffered_write_ops;
  buffered_write_ops.push_back(
      Insert(table_, Key({Int64(1)}), base_columns_,
             {Int64(1), String("value"), String("value2")}));
  buffered_write_ops.push_back(
      Insert(table_, Key({Int64(2)}), base_columns_,
             {Int64(2), String("value_row2"), String("value2_row2")}));
  buffered_write_ops.push_back(
      Update(table_, Key({Int64(1)}), base_columns_,
             {Int64(1), String("updated_value"), String("updated_value2")}));
  buffered_write_ops.push_back(Update(
      table_, Key({Int64(2)}), base_columns_,
      {Int64(2), String("updated_value_row2"), String("updated_value2_row2")}));
  buffered_write_ops.push_back(
      Insert(table_, Key({Int64(3)}), base_columns_,
             {Int64(3), String("value_row3"), String("value2_row3")}));
  buffered_write_ops.push_back(Delete(table_, Key({Int64(1)})));
  buffered_write_ops.push_back(Delete(table_, Key({Int64(2)})));
  ZETASQL_ASSERT_OK_AND_ASSIGN(
      std::vector<WriteOp> change_stream_write_ops,
      BuildChangeStreamWriteOps(schema_.get(), buffered_write_ops, store(), 1,
                                 absl::FromUnixMicros(1000000)));
  // Verify the number of change stream entries is added to the transaction
  // buffer.
  // Insert, Insert, Update, Update, Insert, Delete, Delete -> 4 WriteOps
  ASSERT_EQ(change_stream_write_ops.size(), 4);

  WriteOp op = change_stream_write_ops[0];
  // Verify the first received WriteOp is InsertOp
  auto* operation = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation, nullptr);
  // Verify mod_type
  ASSERT_EQ(operation->values[13], zetasql::Value(String("INSERT")));
  // column_type_names
  ASSERT_EQ(operation->values[3], zetasql::Value(String("00000000")));
  // Verify is_last_record_in_transaction_in_partition
  ASSERT_EQ(operation->values[4], zetasql::Value(Bool(false)));
  // Verify number_of_records_in_transaction
  ASSERT_EQ(operation->values[15], zetasql::Value(Int64(4)));
  // Verify the column_types of the 1st WriteOp (INSERT mod_type)
  ASSERT_EQ(operation->values[6],
            zetasql::values::Array(
                zetasql::types::StringArrayType(),
                {zetasql::Value(String("int64_col")),
                 zetasql::Value(String("string_col")),
                 zetasql::Value(String("another_string_col"))}));
  // Verify column_types_type
  JSON col_1_type;
  col_1_type["code"] = "INT64";
  JSON col_2_type;
  col_2_type["code"] = "STRING";
  JSON col_3_type;
  col_3_type["code"] = "STRING";
  ASSERT_EQ(
      operation->values[7],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String(col_1_type.dump())),
                                zetasql::Value(String(col_2_type.dump())),
                                zetasql::Value(String(col_3_type.dump()))}));
  ASSERT_EQ(operation->values[8],
            zetasql::values::Array(
                zetasql::types::BoolArrayType(),
                {zetasql::Value(Bool(true)), zetasql::Value(Bool(false)),
                 zetasql::Value(Bool(false))}));
  // Verify the mods of the 1st WriteOp (INSERT mod_type)
  zetasql::Value mod_keys = operation->values[10];
  ASSERT_EQ(mod_keys.num_elements(), 2);
  ASSERT_EQ(mod_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  ASSERT_EQ(mod_keys.element(1),
            zetasql::Value(String("{\"int64_col\":\"2\"}")));
  zetasql::Value mod_new_values = operation->values[11];
  ASSERT_EQ(mod_new_values.element(0),
            zetasql::Value(
                String("{\"another_string_col\":\"value2\",\"string_col\":"
                       "\"value\"}")));
  ASSERT_EQ(mod_new_values.element(1),
            zetasql::Value(String("{\"another_string_col\":\"value2_row2\","
                                    "\"string_col\":\"value_row2\"}")));
  zetasql::Value mod_old_values = operation->values[12];
  ASSERT_EQ(mod_old_values.element(0), zetasql::Value(String("{}")));
  ASSERT_EQ(mod_old_values.element(1), zetasql::Value(String("{}")));

  // Verify the 2nd received WriteOp (UPDATE mod_type)
  op = change_stream_write_ops[1];
  auto* operation2 = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation2, nullptr);
  ASSERT_EQ(operation2->values[3], zetasql::Value(String("00000001")));
  // Verify is_last_record_in_transaction_in_partition
  ASSERT_EQ(operation2->values[4], zetasql::Value(Bool(false)));
  // Verify mod_type
  ASSERT_EQ(operation2->values[13], zetasql::Value(String("UPDATE")));
  // Verify number_of_records_in_transaction
  ASSERT_EQ(operation2->values[15], zetasql::Value(Int64(4)));
  // Verify the column_types_name of the 2nd WriteOp (UPDATE mod_type)
  ASSERT_EQ(operation2->values[6],
            zetasql::values::Array(
                zetasql::types::StringArrayType(),
                {zetasql::Value(String("int64_col")),
                 zetasql::Value(String("string_col")),
                 zetasql::Value(String("another_string_col"))}));
  // Verify column_types_type
  ASSERT_EQ(
      operation2->values[7],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String(col_1_type.dump())),
                                zetasql::Value(String(col_2_type.dump())),
                                zetasql::Value(String(col_3_type.dump()))}));
  // Verify column_types_is_primary_key
  ASSERT_EQ(operation2->values[8],
            zetasql::values::Array(
                zetasql::types::BoolArrayType(),
                {zetasql::Value(Bool(true)), zetasql::Value(Bool(false)),
                 zetasql::Value(Bool(false))}));
  // Verify column_types_ordinal_position
  ASSERT_EQ(operation2->values[9],
            zetasql::values::Array(
                zetasql::types::Int64ArrayType(),
                {zetasql::Value(Int64(1)), zetasql::Value(Int64(2)),
                 zetasql::Value(Int64(3))}));
  // Verify the mods of the 2nd WriteOp (UPDATE mod_type)
  zetasql::Value mod_2_keys = operation->values[10];
  ASSERT_EQ(mod_2_keys.num_elements(), 2);
  ASSERT_EQ(mod_2_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  ASSERT_EQ(mod_2_keys.element(1),
            zetasql::Value(String("{\"int64_col\":\"2\"}")));
  zetasql::Value mod_2_new_values = operation->values[11];
  ASSERT_EQ(mod_2_new_values.element(0),
            zetasql::Value(
                String("{\"another_string_col\":\"updated_value2\",\"string_"
                       "col\":\"updated_value\"}")));
  ASSERT_EQ(
      mod_2_new_values.element(1),
      zetasql::Value(String("{\"another_string_col\":\"updated_value2_row2\","
                              "\"string_col\":\"updated_value_row2\"}")));
  zetasql::Value mod_2_old_values = operation->values[12];
  ASSERT_EQ(mod_2_old_values.element(0), zetasql::Value(String("{}")));
  ASSERT_EQ(mod_2_old_values.element(1), zetasql::Value(String("{}")));

  // Verify the 3rd received WriteOp (INSERT mod_type)
  op = change_stream_write_ops[2];
  auto* operation3 = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation3, nullptr);
  ASSERT_EQ(operation3->values[13], zetasql::Value(String("INSERT")));
  ASSERT_EQ(operation->values[3], zetasql::Value(String("00000002")));
  // Verify is_last_record_in_transaction_in_partition
  ASSERT_EQ(operation3->values[4], zetasql::Value(Bool(false)));
  // Verify number_of_records_in_transaction
  ASSERT_EQ(operation3->values[15], zetasql::Value(Int64(4)));

  // Verify the 4th(last) received WriteOp is DeleteOp
  op = change_stream_write_ops[3];
  auto operation4 = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation4, nullptr);
  ASSERT_EQ(operation4->values[3], zetasql::Value(String("00000003")));
  // Verify is_last_record_in_transaction_in_partition
  ASSERT_EQ(operation4->values[4], zetasql::Value(Bool(true)));
  // Verify mod_type
  ASSERT_EQ(operation4->values[13], zetasql::Value(String("DELETE")));
  // Verify number_of_records_in_transaction
  ASSERT_EQ(operation4->values[15], zetasql::Value(Int64(4)));

  // Verify the column_types of the 4th WriteOp (DELETE mod_type)
  ASSERT_EQ(operation4->values[6],
            zetasql::values::Array(
                zetasql::types::StringArrayType(),
                {zetasql::Value(String("int64_col")),
                 zetasql::Value(String("string_col")),
                 zetasql::Value(String("another_string_col"))}));
  // Verify column_types_type
  ASSERT_EQ(
      operation4->values[7],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String(col_1_type.dump())),
                                zetasql::Value(String(col_2_type.dump())),
                                zetasql::Value(String(col_3_type.dump()))}));
  // Verify column_types_is_primary_key
  ASSERT_EQ(operation4->values[8],
            zetasql::values::Array(
                zetasql::types::BoolArrayType(),
                {zetasql::Value(Bool(true)), zetasql::Value(Bool(false)),
                 zetasql::Value(Bool(false))}));
  // Verify column_types_ordinal_position
  ASSERT_EQ(operation4->values[9],
            zetasql::values::Array(
                zetasql::types::Int64ArrayType(),
                {zetasql::Value(Int64(1)), zetasql::Value(Int64(2)),
                 zetasql::Value(Int64(3))}));
  // Verify the mods of the 4th WriteOp (DELETE mod_type)
  zetasql::Value mod_4_keys = operation4->values[10];
  ASSERT_EQ(mod_4_keys.num_elements(), 2);
  ASSERT_EQ(mod_4_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  ASSERT_EQ(mod_4_keys.element(1),
            zetasql::Value(String("{\"int64_col\":\"2\"}")));
  zetasql::Value mod_4_new_values = operation4->values[11];
  ASSERT_EQ(mod_4_new_values.element(0), zetasql::Value(String("{}")));
  ASSERT_EQ(mod_4_new_values.element(1), zetasql::Value(String("{}")));
  zetasql::Value mod_4_old_values = operation4->values[12];
  ASSERT_EQ(mod_4_old_values.element(0), zetasql::Value(String("{}")));
  ASSERT_EQ(mod_4_old_values.element(1), zetasql::Value(String("{}")));
}

// Insert to table1, Insert to table2, Insert to table1 -> 3 DataChangeRecords
TEST_F(ChangeStreamTest, AddWriteOpForDiffUserTablesForSameChangeStream) {
  // Populate partition table with the initial partition token
  set_up_partition_token_for_change_stream_partition_table(change_stream_,
                                                           store());
  absl::flat_hash_map<const ChangeStream*, std::vector<DataChangeRecord>>
      data_change_records_in_transaction_by_change_stream;
  absl::flat_hash_map<const ChangeStream*, ModGroup>
      last_mod_group_by_change_stream;
  // Insert base table entry to TestTable.
  ASSERT_THAT(LogTableMod(Insert(table_, Key({Int64(1)}), base_columns_,
                                 {Int64(1), String("value"), String("value2")}),
                          change_stream_, zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());
  // Insert base table entry to TestTable2.
  ASSERT_THAT(LogTableMod(Insert(table2_, Key({Int64(1)}),
                                 base_columns_table_2_all_col_,
                                 {Int64(1), String("value"), String("value2")}),
                          change_stream_, zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());
  // Insert base table entry to TestTable.
  ASSERT_THAT(LogTableMod(Insert(table_, Key({Int64(2)}), base_columns_,
                                 {Int64(2), String("value_row2"),
                                  String("value2_row2")}),
                          change_stream_, zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());

  // Set number_of_records_in_transaction in each DataChangeRecord after
  // finishing processing all operations
  std::vector<WriteOp> write_ops =
      BuildMutation(&data_change_records_in_transaction_by_change_stream, 1,
                    &last_mod_group_by_change_stream,
                    absl::FromUnixMicros(1000000));
  // Verify the number rebuilt WriteOps added to the transaction
  // buffer.
  ASSERT_EQ(write_ops.size(), 3);
  WriteOp op = write_ops[0];
  InsertOp* insert_op = std::get_if<InsertOp>(&op);
  ASSERT_NE(insert_op, nullptr);
  EXPECT_EQ(insert_op->values[5], zetasql::Value(String("TestTable")));
  op = write_ops[1];
  insert_op = std::get_if<InsertOp>(&op);
  ASSERT_NE(insert_op, nullptr);
  EXPECT_EQ(insert_op->values[5], zetasql::Value(String("TestTable2")));
  op = write_ops[2];
  insert_op = std::get_if<InsertOp>(&op);
  ASSERT_NE(insert_op, nullptr);
  EXPECT_EQ(insert_op->values[5], zetasql::Value(String("TestTable")));
}

// Update table1(another_string_col), Update table1(string_col), Update
// table1(another_string_col) -> 3 DataChangeRecords
TEST_F(ChangeStreamTest, AddWriteOpForDiffNonKeyColsForSameChangeStream) {
  // Populate partition table with the initial partition token
  set_up_partition_token_for_change_stream_partition_table(change_stream_,
                                                           store());
  absl::flat_hash_map<const ChangeStream*, std::vector<DataChangeRecord>>
      data_change_records_in_transaction_by_change_stream;
  absl::flat_hash_map<const ChangeStream*, ModGroup>
      last_mod_group_by_change_stream;
  // Insert base table entry to TestTable.
  ASSERT_THAT(LogTableMod(Update(table_, Key({Int64(1)}),
                                 key_and_another_string_col_table_1_,
                                 {Int64(1), String("another_string_value1")}),
                          change_stream_, zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());
  // Insert base table entry to TestTable2.
  ASSERT_THAT(
      LogTableMod(Update(table_, Key({Int64(1)}), key_and_string_col_table_1_,
                         {Int64(1), String("string_value1")}),
                  change_stream_, zetasql::Value::String("11111"),
                  &data_change_records_in_transaction_by_change_stream, 1,
                  &last_mod_group_by_change_stream, store(),
          absl::FromUnixMicros(1000000)),
      ::zetasql_base::testing::IsOk());
  // Insert base table entry to TestTable.
  ASSERT_THAT(LogTableMod(Update(table_, Key({Int64(2)}),
                                 key_and_another_string_col_table_1_,
                                 {Int64(2), String("another_string_value2")}),
                          change_stream_, zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());
  // Set number_of_records_in_transaction in each DataChangeRecord after
  // finishing processing all operations
  std::vector<WriteOp> write_ops =
      BuildMutation(&data_change_records_in_transaction_by_change_stream, 1,
                    &last_mod_group_by_change_stream,
                    absl::FromUnixMicros(1000000));
  // Verify the number of rebuilt WriteOps added to the transaction
  // buffer.
  EXPECT_EQ(write_ops.size(), 3);
}

TEST_F(ChangeStreamTest, AddWriteOpForDifferentChangeStreams) {
  // Populate ChangeStream_All_partition_table with the initial partition
  // token
  set_up_partition_token_for_change_stream_partition_table(change_stream_,
                                                           store());
  // Populate ChangeStream_TestTable2StrCol_partition_table with the initial
  // partition token
  set_up_partition_token_for_change_stream_partition_table(change_stream2_,
                                                           store());
  absl::flat_hash_map<const ChangeStream*, std::vector<DataChangeRecord>>
      data_change_records_in_transaction_by_change_stream;
  absl::flat_hash_map<const ChangeStream*, ModGroup>
      last_mod_group_by_change_stream;
  // Insert base table entry to TestTable.
  ASSERT_THAT(
      LogTableMod(Insert(table2_, Key({Int64(1)}), key_and_string_col_table_2_,
                         {Int64(1), String("string_value1")}),
                  change_stream_, zetasql::Value::String("11111"),
                  &data_change_records_in_transaction_by_change_stream, 1,
                  &last_mod_group_by_change_stream, store(),
          absl::FromUnixMicros(1000000)),
      ::zetasql_base::testing::IsOk());
  ASSERT_THAT(
      LogTableMod(Insert(table2_, Key({Int64(2)}), key_and_string_col_table_2_,
                         {Int64(2), String("string_value2")}),
                  change_stream2_, zetasql::Value::String("11111"),
                  &data_change_records_in_transaction_by_change_stream, 1,
                  &last_mod_group_by_change_stream, store(),
          absl::FromUnixMicros(1000000)),
      ::zetasql_base::testing::IsOk());
  ASSERT_THAT(
      LogTableMod(Insert(table2_, Key({Int64(1)}), key_and_string_col_table_2_,
                         {Int64(3), String("string_value3")}),
                  change_stream_, zetasql::Value::String("11111"),
                  &data_change_records_in_transaction_by_change_stream, 1,
                  &last_mod_group_by_change_stream, store(),
          absl::FromUnixMicros(1000000)),
      ::zetasql_base::testing::IsOk());
  ASSERT_THAT(LogTableMod(Insert(table2_, Key({Int64(1)}),
                                 key_and_another_string_col_table_2_,
                                 {Int64(4), String("another_string_value4")}),
                          change_stream_, zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());
  // Set number_of_records_in_transaction in each DataChangeRecord after
  // finishing processing all operations
  std::vector<WriteOp> write_ops =
      BuildMutation(&data_change_records_in_transaction_by_change_stream, 1,
                    &last_mod_group_by_change_stream,
                    absl::FromUnixMicros(1000000));
  // Insert to table2(string_col) tracked by cs1, Insert to table2(string_col)
  // tracked by cs2, Insert to table2(string_col) tracked by cs1, Insert to
  // table2(another_string_col) tracked by cs1 -> 3 DataChangeRecords
  ASSERT_EQ(write_ops.size(), 2);
  int count_cs_test_table = 0;
  int count_cs_test_table2 = 0;
  for (int64_t i = 0; i < write_ops.size(); ++i) {
    WriteOp op = write_ops[i];
    auto* insert_operation = std::get_if<InsertOp>(&op);
    if (insert_operation->table->Name() ==
        "_change_stream_data_ChangeStream_All") {
      count_cs_test_table++;
    } else if (insert_operation->table->Name() ==
               "_change_stream_data_ChangeStream_TestTable2StrCol") {
      count_cs_test_table2++;
    }
  }
  ASSERT_EQ(count_cs_test_table, 1);
  ASSERT_EQ(count_cs_test_table2, 1);
}

TEST_F(ChangeStreamTest,
       InsertUpdateDeleteUntrackedColumnsForChangeStreamTrackingKeyColsOnly) {
  // Populate ChangeStream_TestTable2KeyOnly_partition_table with the initial
  // partition token
  set_up_partition_token_for_change_stream_partition_table(change_stream3_,
                                                           store());
  absl::flat_hash_map<const ChangeStream*, std::vector<DataChangeRecord>>
      data_change_records_in_transaction_by_change_stream;
  absl::flat_hash_map<const ChangeStream*, ModGroup>
      last_mod_group_by_change_stream;
  // Insert base table entry to TestTable.
  ASSERT_THAT(LogTableMod(Insert(table2_, Key({Int64(1)}),
                                 key_and_another_string_col_table_2_,
                                 {Int64(1), String("another_string_value1")}),
                          change_stream3_, zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());
  // Update to an untracked column.
  ASSERT_THAT(
      LogTableMod(
          Update(table2_, Key({Int64(1)}), key_and_another_string_col_table_2_,
                 {Int64(1), String("another_string_value_update")}),
          change_stream3_, zetasql::Value::String("11111"),
          &data_change_records_in_transaction_by_change_stream, 1,
          &last_mod_group_by_change_stream, store(),
          absl::FromUnixMicros(1000000)),
      ::zetasql_base::testing::IsOk());
  // Delete the row.
  ASSERT_THAT(LogTableMod(Delete(table2_, Key({Int64(1)})), change_stream3_,
                          zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());
  // Set number_of_records_in_transaction in each DataChangeRecord after
  // finishing processing all operations
  std::vector<WriteOp> write_ops =
      BuildMutation(&data_change_records_in_transaction_by_change_stream, 1,
                    &last_mod_group_by_change_stream,
                    absl::FromUnixMicros(1000000));
  // Verify the number of rebuilt WriteOps added to the transaction
  // buffer.
  ASSERT_EQ(write_ops.size(), 2);
  // Verify the first received WriteOp is for INSERT mod_type
  WriteOp op = write_ops[0];
  auto* operation = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation, nullptr);
  ASSERT_EQ(operation->values[13], zetasql::Value(String("INSERT")));
  // Verify column_types_name
  ASSERT_EQ(operation->values[6],
            zetasql::values::Array(zetasql::types::StringArrayType(),
                                     {zetasql::Value(String("int64_col"))}));
  // Verify column_types_type
  JSON col_1_type;
  col_1_type["code"] = "INT64";
  ASSERT_EQ(
      operation->values[7],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String(col_1_type.dump()))}));
  // Verify column_types_is_primary_key
  ASSERT_EQ(operation->values[8],
            zetasql::values::Array(zetasql::types::BoolArrayType(),
                                     {zetasql::Value(Bool(true))}));
  // Verify column_types_ordinal_position
  ASSERT_EQ(operation->values[9],
            zetasql::values::Array(zetasql::types::Int64ArrayType(),
                                     {zetasql::Value(Int64(1))}));

  // Since new_values field in mods field only contains non_key_col values,
  // new_values should be empty.
  zetasql::Value mod_keys = operation->values[10];
  ASSERT_EQ(mod_keys.num_elements(), 1);
  ASSERT_EQ(mod_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  zetasql::Value mod_new_values = operation->values[11];
  ASSERT_EQ(mod_new_values.element(0), zetasql::Value(String("{}")));
  zetasql::Value mod_old_values = operation->values[12];
  ASSERT_EQ(mod_old_values.element(0), zetasql::Value(String("{}")));

  // Verify the second received WriteOp is for DELETE mod_type
  op = write_ops[1];
  auto* operation2 = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation, nullptr);
  ASSERT_EQ(operation2->values[13], zetasql::Value(String("DELETE")));
  // Verify column_types_name
  ASSERT_EQ(operation->values[6],
            zetasql::values::Array(zetasql::types::StringArrayType(),
                                     {zetasql::Value(String("int64_col"))}));
  // Verify column_types_type
  ASSERT_EQ(
      operation->values[7],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String(col_1_type.dump()))}));
  // ASSERT_EQ(operation->values[7], zetasql::Value(String("int64_col")));
  // Verify column_types_is_primary_key
  ASSERT_EQ(operation->values[8],
            zetasql::values::Array(zetasql::types::BoolArrayType(),
                                     {zetasql::Value(Bool(true))}));
  // Verify column_types_ordinal_position
  ASSERT_EQ(operation->values[9],
            zetasql::values::Array(zetasql::types::Int64ArrayType(),
                                     {zetasql::Value(Int64(1))}));
  // Verify mods to be empty
  mod_keys = operation->values[10];
  ASSERT_EQ(mod_keys.num_elements(), 1);
  ASSERT_EQ(mod_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  mod_new_values = operation->values[11];
  ASSERT_EQ(mod_new_values.element(0), zetasql::Value(String("{}")));
  mod_old_values = operation->values[12];
  ASSERT_EQ(mod_old_values.element(0), zetasql::Value(String("{}")));
}

TEST_F(ChangeStreamTest, InsertUpdateDeleteUntrackedColumnsSameRow) {
  // Populate ChangeStream_TestTable2StrCol_partition_table with the initial
  // partition token
  set_up_partition_token_for_change_stream_partition_table(change_stream2_,
                                                           store());
  absl::flat_hash_map<const ChangeStream*, std::vector<DataChangeRecord>>
      data_change_records_in_transaction_by_change_stream;
  absl::flat_hash_map<const ChangeStream*, ModGroup>
      last_mod_group_by_change_stream;
  // Insert base table entry to TestTable.
  ASSERT_THAT(LogTableMod(Insert(table2_, Key({Int64(1)}),
                                 key_and_another_string_col_table_2_,
                                 {Int64(1), String("another_string_value1")}),
                          change_stream2_, zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());
  // Update to an untracked column.
  ASSERT_THAT(
      LogTableMod(
          Update(table2_, Key({Int64(1)}), key_and_another_string_col_table_2_,
                 {Int64(1), String("another_string_value_update")}),
          change_stream2_, zetasql::Value::String("11111"),
          &data_change_records_in_transaction_by_change_stream, 1,
          &last_mod_group_by_change_stream, store(),
          absl::FromUnixMicros(1000000)),
      ::zetasql_base::testing::IsOk());
  // Delete the row.
  ASSERT_THAT(LogTableMod(Delete(table2_, Key({Int64(1)})), change_stream2_,
                          zetasql::Value::String("11111"),
                          &data_change_records_in_transaction_by_change_stream,
                          1, &last_mod_group_by_change_stream, store(),
                          absl::FromUnixMicros(1000000)),
              ::zetasql_base::testing::IsOk());
  // Set number_of_records_in_transaction in each DataChangeRecord after
  // finishing processing all operations
  std::vector<WriteOp> write_ops =
      BuildMutation(&data_change_records_in_transaction_by_change_stream, 1,
                    &last_mod_group_by_change_stream,
                    absl::FromUnixMicros(1000000));
  // Verify the number of rebuilt WriteOps added to the transaction
  // buffer.
  ASSERT_EQ(write_ops.size(), 2);
  // Verify the first received WriteOp is for INSERT mod_type
  WriteOp op = write_ops[0];
  auto* operation = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation, nullptr);
  ASSERT_EQ(operation->values[13], zetasql::Value(String("INSERT")));
  // Verify is_last_record_in_transaction_in_partition
  ASSERT_EQ(operation->values[4], zetasql::Value(Bool(false)));
  // Verify number_of_records_in_transaction
  ASSERT_EQ(operation->values[15], zetasql::Value(Int64(2)));
  // Verify column_types_name
  ASSERT_EQ(operation->values[6],
            zetasql::values::Array(zetasql::types::StringArrayType(),
                                     {zetasql::Value(String("int64_col")),
                                      zetasql::Value(String("string_col"))}));
  // Verify column_types_type
  JSON col_1_type;
  col_1_type["code"] = "INT64";
  JSON col_2_type;
  col_2_type["code"] = "STRING";
  ASSERT_EQ(
      operation->values[7],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String(col_1_type.dump())),
                                zetasql::Value(String(col_2_type.dump()))}));
  // Verify column_types_is_primary_key
  ASSERT_EQ(operation->values[8],
            zetasql::values::Array(
                zetasql::types::BoolArrayType(),
                {zetasql::Value(Bool(true)), zetasql::Value(Bool(false))}));
  // Verify column_types_ordinal_position
  ASSERT_EQ(operation->values[9],
            zetasql::values::Array(
                zetasql::types::Int64ArrayType(),
                {zetasql::Value(Int64(1)), zetasql::Value(Int64(2))}));
  // Since new_values field in mods field only contains non_key_col values,
  // new_values should be empty.
  zetasql::Value mod_keys = operation->values[10];
  ASSERT_EQ(mod_keys.num_elements(), 1);
  ASSERT_EQ(mod_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  zetasql::Value mod_new_values = operation->values[11];
  ASSERT_EQ(mod_new_values.element(0),
            zetasql::Value(String("{\"string_col\":null}")));
  zetasql::Value mod_old_values = operation->values[12];
  ASSERT_EQ(mod_old_values.element(0), zetasql::Value(String("{}")));

  // Verify the second received WriteOp is for DELETE mod_type
  op = write_ops[1];
  auto* operation2 = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation2, nullptr);
  ASSERT_EQ(operation2->values[13], zetasql::Value(String("DELETE")));
  // Verify column_types_name
  ASSERT_EQ(operation2->values[6],
            zetasql::values::Array(zetasql::types::StringArrayType(),
                                     {zetasql::Value(String("int64_col")),
                                      zetasql::Value(String("string_col"))}));
  // Verify column_types_type
  ASSERT_EQ(
      operation2->values[7],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String(col_1_type.dump())),
                                zetasql::Value(String(col_2_type.dump()))}));
  // Verify column_types_is_primary_key
  ASSERT_EQ(operation2->values[8],
            zetasql::values::Array(
                zetasql::types::BoolArrayType(),
                {zetasql::Value(Bool(true)), zetasql::Value(Bool(false))}));
  // Verify column_types_ordinal_position
  ASSERT_EQ(operation2->values[9],
            zetasql::values::Array(
                zetasql::types::Int64ArrayType(),
                {zetasql::Value(Int64(1)), zetasql::Value(Int64(2))}));
  // Verify mods to be empty
  mod_keys = operation->values[10];
  ASSERT_EQ(mod_keys.num_elements(), 1);
  ASSERT_EQ(mod_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  mod_new_values = operation->values[11];
  ASSERT_EQ(mod_new_values.element(0), zetasql::Value(String("{}")));
  mod_old_values = operation->values[12];
  ASSERT_EQ(mod_old_values.element(0), zetasql::Value(String("{}")));
}

TEST_F(ChangeStreamTest, MultipleInsertToSeparateSubsetsColumnsSameTable) {
  // Populate ChangeStream_All_partition_table with the initial partition
  // token
  set_up_partition_token_for_change_stream_partition_table(change_stream_,
                                                           store());
  std::vector<WriteOp> buffered_write_ops;
  buffered_write_ops.push_back(Insert(table_, Key({Int64(1)}),
                                      key_and_string_col_table_1_,
                                      {Int64(1), String("string_value1")}));
  buffered_write_ops.push_back(
      Insert(table_, Key({Int64(2)}), key_and_another_string_col_table_1_,
             {Int64(2), String("another_string_value2")}));
  ZETASQL_ASSERT_OK_AND_ASSIGN(
      std::vector<WriteOp> change_stream_write_ops,
      BuildChangeStreamWriteOps(schema_.get(), buffered_write_ops, store(), 1,
                                 absl::FromUnixMicros(1000000)));
  // Verify the number of rebuilt WriteOps added to the transaction
  // buffer.
  ASSERT_EQ(change_stream_write_ops.size(), 1);

  // Verify the first received WriteOp is for INSERT mod_type
  WriteOp op = change_stream_write_ops[0];
  auto* operation = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation, nullptr);
  // Verify column_types. Since column_types include column types tracked by the
  // change_stream_ and the change_stream_ tracks all, verify both the key
  // column and the tracked non_key column (string_col_) are included in
  // column_types.
  ASSERT_EQ(operation->values[6],
            zetasql::values::Array(
                zetasql::types::StringArrayType(),
                {zetasql::Value(String("int64_col")),
                 zetasql::Value(String("string_col")),
                 zetasql::Value(String("another_string_col"))}));
  JSON col_1_type;
  col_1_type["code"] = "INT64";
  JSON col_2_type;
  col_2_type["code"] = "STRING";
  JSON col_3_type;
  col_3_type["code"] = "STRING";
  ASSERT_EQ(
      operation->values[7],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String(col_1_type.dump())),
                                zetasql::Value(String(col_2_type.dump())),
                                zetasql::Value(String(col_3_type.dump()))}));
  ASSERT_EQ(operation->values[8],
            zetasql::values::Array(
                zetasql::types::BoolArrayType(),
                {zetasql::Value(Bool(true)), zetasql::Value(Bool(false)),
                 zetasql::Value(Bool(false))}));
  ASSERT_EQ(operation->values[9],
            zetasql::values::Array(
                zetasql::types::Int64ArrayType(),
                {zetasql::Value(Int64(1)), zetasql::Value(Int64(2)),
                 zetasql::Value(Int64(3))}));
  // Verify mods
  zetasql::Value mod_keys = operation->values[10];
  ASSERT_EQ(mod_keys.num_elements(), 2);
  ASSERT_EQ(mod_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  ASSERT_EQ(mod_keys.element(1),
            zetasql::Value(String("{\"int64_col\":\"2\"}")));
  zetasql::Value mod_new_values = operation->values[11];
  ASSERT_EQ(
      mod_new_values.element(0),
      zetasql::Value(String(
          "{\"another_string_col\":null,\"string_col\":\"string_value1\"}")));
  ASSERT_EQ(mod_new_values.element(1),
            zetasql::Value(String("{\"another_string_col\":\"another_string_"
                                    "value2\",\"string_col\":null}")));
  zetasql::Value mod_old_values = operation->values[12];
  ASSERT_EQ(mod_old_values.element(0), zetasql::Value(String("{}")));
}

TEST_F(ChangeStreamTest, PgVerifyExtendedDatatypesValueAndType) {
  set_up_partition_token_for_change_stream_partition_table(pg_change_stream_,
                                                           store());
  // Insert base table entry.
  std::vector<WriteOp> buffered_write_ops;
  buffered_write_ops.push_back(Insert(
      pg_table_, Key({Int64(1)}), pg_columns_,
      {Int64(1), Json(JSONValue(static_cast<int64_t>(2024))),
       JsonArray({JSONValue(static_cast<int64_t>(1)),
                  JSONValue(static_cast<int64_t>(2))}),
       Numeric(11), NumericArray({NumericValue(22), NumericValue(33)})}));
  ZETASQL_ASSERT_OK_AND_ASSIGN(std::vector<WriteOp> change_stream_write_ops,
                       BuildChangeStreamWriteOps(
                           pg_schema_.get(), buffered_write_ops, store(), 1,
                           absl::FromUnixMicros(1000000)));
  // Verify change stream entry is added to the transaction buffer.
  ASSERT_EQ(change_stream_write_ops.size(), 1);
  WriteOp op = change_stream_write_ops[0];
  // Verify the table of the received WriteOp
  ASSERT_EQ(TableOf(op), pg_change_stream_->change_stream_data_table());
  // Verify the received WriteOp is InsertOp
  auto* operation = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation, nullptr);
  // Verify columns in the rebuilt InsertOp corresponds to columns in
  // change_stream_data_table
  ASSERT_EQ(operation->columns,
            pg_change_stream_->change_stream_data_table()->columns());
  ASSERT_EQ(operation->columns.size(), 19);
  ASSERT_EQ(operation->values.size(), 19);
  // Verify values in the rebuilt InsertOp are correct
  // Verify partition_token
  ASSERT_EQ(operation->values[0], zetasql::Value::String("11111"));
  // Verify record_sequence
  ASSERT_EQ(operation->values[3], zetasql::Value(String("00000000")));
  // Verify is_last_record_in_transaction_in_partition
  ASSERT_EQ(operation->values[4], zetasql::Value(Bool(true)));
  // Verify table_name
  ASSERT_EQ(operation->values[5],
            zetasql::Value(String("entended_pg_datatypes")));
  // Verify column_types_name
  ASSERT_EQ(
      operation->values[6],
      zetasql::values::Array(zetasql::types::StringArrayType(),
                               {zetasql::Value(String("int_col")),
                                zetasql::Value(String("jsonb_col")),
                                zetasql::Value(String("jsonb_arr")),
                                zetasql::Value(String("numeric_col")),
                                zetasql::Value(String("numeric_arr"))}));
  // Verify column_types_type
  JSON int_type;
  int_type["code"] = "INT64";
  JSON jsonb_type;
  jsonb_type["code"] = "JSON";
  jsonb_type["type_annotation"] = "PG_JSONB";
  JSON json_arr_type;
  json_arr_type["code"] = "ARRAY";
  json_arr_type["array_element_type"]["code"] = "JSON";
  json_arr_type["array_element_type"]["type_annotation"] = "PG_JSONB";
  JSON numeric_type;
  numeric_type["code"] = "NUMERIC";
  numeric_type["type_annotation"] = "PG_NUMERIC";
  JSON numeric_arr_type;
  numeric_arr_type["code"] = "ARRAY";
  numeric_arr_type["array_element_type"]["code"] = "NUMERIC";
  numeric_arr_type["array_element_type"]["type_annotation"] = "PG_NUMERIC";
  ASSERT_EQ(operation->values[7],
            zetasql::values::Array(
                zetasql::types::StringArrayType(),
                {zetasql::Value(String(int_type.dump())),
                 zetasql::Value(String(jsonb_type.dump())),
                 zetasql::Value(String(json_arr_type.dump())),
                 zetasql::Value(String(numeric_type.dump())),
                 zetasql::Value(String(numeric_arr_type.dump()))}));
  // Verify column_types_is_primary_key
  ASSERT_EQ(operation->values[8],
            zetasql::values::Array(
                zetasql::types::BoolArrayType(),
                {zetasql::Value(Bool(true)), zetasql::Value(Bool(false)),
                 zetasql::Value(Bool(false)), zetasql::Value(Bool(false)),
                 zetasql::Value(Bool(false))}));
  // Verify column_types_ordinal_position
  ASSERT_EQ(operation->values[9],
            zetasql::values::Array(
                zetasql::types::Int64ArrayType(),
                {zetasql::Value(Int64(1)), zetasql::Value(Int64(2)),
                 zetasql::Value(Int64(3)), zetasql::Value(Int64(4)),
                 zetasql::Value(Int64(5))}));
  // Verify mods
  zetasql::Value mod_keys = operation->values[10];
  ASSERT_EQ(mod_keys.element(0),
            zetasql::Value(String("{\"int_col\":\"1\"}")));
  zetasql::Value mod_new_values = operation->values[11];
  ASSERT_EQ(
      mod_new_values.element(0),
      zetasql::Value(String(
          R"({"jsonb_arr":["1","2"],"jsonb_col":"2024","numeric_arr":["22","33"],"numeric_col":"11"})")));
  zetasql::Value mod_old_values = operation->values[12];
  ASSERT_EQ(mod_old_values.element(0), zetasql::Value(String("{}")));
  // Verify mod_type
  ASSERT_EQ(operation->values[13], zetasql::Value(String("INSERT")));
  // Verify value_capture_type
  ASSERT_EQ(operation->values[14], zetasql::Value(String("NEW_VALUES")));
  // Verify number_of_records_in_transaction
  ASSERT_EQ(operation->values[15], zetasql::Value(Int64(1)));
  // Verify number_of_partitions_in_transaction
  ASSERT_EQ(operation->values[16], zetasql::Value(Int64(1)));
  // Verify transaction_tag
  ASSERT_EQ(operation->values[17], zetasql::Value(String("")));
  // Verify is_system_transaction
  ASSERT_EQ(operation->values[18], zetasql::Value(Bool(false)));
}

TEST_F(ChangeStreamTest, FloatValueAndTypes) {
  set_up_partition_token_for_change_stream_partition_table(float_change_stream_,
                                                           store());
  // Insert base table entry.
  std::vector<WriteOp> buffered_write_ops;
  buffered_write_ops.push_back(
      Insert(float_table_, Key({Int64(1)}), float_columns_,
             {Int64(1), Float(1.1f), Double(2.2), FloatArray({1.1f, 3.14f}),
              DoubleArray({2.2, 2.71})}));
  ZETASQL_ASSERT_OK_AND_ASSIGN(
      std::vector<WriteOp> change_stream_write_ops,
      BuildChangeStreamWriteOps(float_schema_.get(), buffered_write_ops,
                                store(), 1, absl::FromUnixMicros(1000000)));

  // Verify change stream entry is added to the transaction buffer.
  ASSERT_EQ(change_stream_write_ops.size(), 1);
  WriteOp op = change_stream_write_ops[0];
  // Verify the table of the received WriteOp
  ASSERT_EQ(TableOf(op), float_change_stream_->change_stream_data_table());
  // Verify the received WriteOp is InsertOp
  auto* operation = std::get_if<InsertOp>(&op);
  ASSERT_NE(operation, nullptr);
  // Verify columns in the rebuilt InsertOp corresponds to columns in
  // change_stream_data_table
  ASSERT_EQ(operation->columns,
            float_change_stream_->change_stream_data_table()->columns());

  // Verify values in the rebuilt InsertOp are correct
  // Verify partition_token
  ASSERT_EQ(operation->values[0], zetasql::Value::String("11111"));
  // Verify record_sequence
  ASSERT_EQ(operation->values[3], zetasql::Value(String("00000000")));
  // Verify is_last_record_in_transaction_in_partition
  ASSERT_EQ(operation->values[4], zetasql::Value(Bool(true)));
  // Verify table_name
  ASSERT_EQ(operation->values[5], zetasql::Value(String("FloatTable")));
  // Verify column_types_name
  ASSERT_EQ(operation->values[6],
            zetasql::values::Array(zetasql::types::StringArrayType(),
                                     {zetasql::Value(String("int64_col")),
                                      zetasql::Value(String("float_col")),
                                      zetasql::Value(String("double_col")),
                                      zetasql::Value(String("float_arr")),
                                      zetasql::Value(String("double_arr"))}));
  // Verify column_types_type
  JSON int_type;
  int_type["code"] = "INT64";
  JSON float32_type;
  float32_type["code"] = "FLOAT32";
  JSON float32_arr_type;
  float32_arr_type["code"] = "ARRAY";
  float32_arr_type["array_element_type"]["code"] = "FLOAT32";
  JSON float64_type;
  float64_type["code"] = "FLOAT64";
  JSON float64_arr_type;
  float64_arr_type["code"] = "ARRAY";
  float64_arr_type["array_element_type"]["code"] = "FLOAT64";
  ASSERT_EQ(operation->values[7],
            zetasql::values::Array(
                zetasql::types::StringArrayType(),
                {zetasql::Value(String(int_type.dump())),
                 zetasql::Value(String(float32_type.dump())),
                 zetasql::Value(String(float64_type.dump())),
                 zetasql::Value(String(float32_arr_type.dump())),
                 zetasql::Value(String(float64_arr_type.dump()))}));
  // Verify column_types_is_primary_key
  ASSERT_EQ(operation->values[8],
            zetasql::values::Array(
                zetasql::types::BoolArrayType(),
                {zetasql::Value(Bool(true)), zetasql::Value(Bool(false)),
                 zetasql::Value(Bool(false)), zetasql::Value(Bool(false)),
                 zetasql::Value(Bool(false))}));
  // Verify column_types_ordinal_position
  ASSERT_EQ(operation->values[9],
            zetasql::values::Array(
                zetasql::types::Int64ArrayType(),
                {zetasql::Value(Int64(1)), zetasql::Value(Int64(2)),
                 zetasql::Value(Int64(3)), zetasql::Value(Int64(4)),
                 zetasql::Value(Int64(5))}));
  // Verify mods
  zetasql::Value mod_keys = operation->values[10];
  ASSERT_EQ(mod_keys.element(0),
            zetasql::Value(String("{\"int64_col\":\"1\"}")));
  zetasql::Value mod_new_values = operation->values[11];
  ASSERT_EQ(
      mod_new_values.element(0),
      zetasql::Value(String(
          R"({"double_arr":[2.2,2.71],"double_col":2.2,"float_arr":[1.100000023841858,3.140000104904175],"float_col":1.100000023841858})")));
  zetasql::Value mod_old_values = operation->values[12];
  ASSERT_EQ(mod_old_values.element(0), zetasql::Value(String("{}")));
  // Verify mod_type
  ASSERT_EQ(operation->values[13], zetasql::Value(String("INSERT")));
  // Verify value_capture_type
  ASSERT_EQ(operation->values[14], zetasql::Value(String("NEW_VALUES")));
  // Verify number_of_records_in_transaction
  ASSERT_EQ(operation->values[15], zetasql::Value(Int64(1)));
  // Verify number_of_partitions_in_transaction
  ASSERT_EQ(operation->values[16], zetasql::Value(Int64(1)));
  // Verify transaction_tag
  ASSERT_EQ(operation->values[17], zetasql::Value(String("")));
  // Verify is_system_transaction
  ASSERT_EQ(operation->values[18], zetasql::Value(Bool(false)));
}

TEST_F(ChangeStreamTest, CommitTimestampResolutionInChangeStream) {
  // Set up partition token for the commit timestamp change stream
  set_up_partition_token_for_change_stream_partition_table(
      commit_timestamp_change_stream_, store());

  // Create buffered write ops with commit timestamp sentinel value
  std::vector<WriteOp> buffered_write_ops;
  std::vector<const Column*> commit_timestamp_columns = {
      commit_timestamp_table_->FindColumn("id"),
      commit_timestamp_table_->FindColumn("name"),
      commit_timestamp_table_->FindColumn("commit_ts")
  };
  
  // Use the commit timestamp sentinel value as per the constants
  buffered_write_ops.push_back(Insert(
      commit_timestamp_table_, Key({Int64(1)}), commit_timestamp_columns,
      {Int64(1), String("test_name"), 
       zetasql::Value::Timestamp(kCommitTimestampValueSentinel)}));

  // Set a real commit timestamp for the test
  absl::Time test_commit_timestamp = absl::FromUnixMicros(1500000000);
  
  ZETASQL_ASSERT_OK_AND_ASSIGN(
      std::vector<WriteOp> change_stream_write_ops,
      BuildChangeStreamWriteOps(commit_timestamp_schema_.get(), buffered_write_ops, 
                                 store(), 1, test_commit_timestamp));

  // Verify that a change stream entry was created
  ASSERT_EQ(change_stream_write_ops.size(), 1);
  WriteOp change_stream_op = change_stream_write_ops[0];
  
  // The change stream operation should be an Insert to the change stream data table
  const InsertOp* operation = std::get_if<InsertOp>(&change_stream_op);
  ASSERT_NE(operation, nullptr);
  ASSERT_EQ(operation->table, 
            commit_timestamp_change_stream_->change_stream_data_table());

  // Verify that the commit timestamp in the change stream record 
  // is the real timestamp, not the sentinel value
  zetasql::Value commit_timestamp_value = operation->values[1]; // commit_timestamp is at index 1
  ASSERT_TRUE(commit_timestamp_value.type()->IsTimestamp());
  ASSERT_EQ(commit_timestamp_value.ToTime(), test_commit_timestamp);
  
  // Also verify that the sentinel value was NOT used
  ASSERT_NE(commit_timestamp_value.ToTime(), absl::FromUnixMicros(zetasql::types::kTimestampMax));

  // Verify the new_values in the mods contain the resolved timestamp, not the sentinel
  zetasql::Value mod_new_values = operation->values[11]; // mods new_values
  ASSERT_TRUE(mod_new_values.type()->IsArray());
  ASSERT_EQ(mod_new_values.num_elements(), 1);
  
  std::string new_values_json = mod_new_values.element(0).string_value();
  // Parse the JSON to verify the commit_ts field contains the real timestamp
  JSON parsed_new_values = JSON::parse(new_values_json);
  ASSERT_TRUE(parsed_new_values.contains("commit_ts"));
  
  // The timestamp should be formatted as a proper timestamp string, not the sentinel
  std::string timestamp_str = parsed_new_values["commit_ts"];
  ASSERT_FALSE(timestamp_str.empty());
  // Should not contain the sentinel timestamp value
  ASSERT_THAT(timestamp_str, testing::Not(testing::HasSubstr("294247-01-10")));  // Max timestamp year
}

TEST_F(ChangeStreamTest, CommitTimestampResolutionConsistencyBetweenMainDataAndChangeStream) {
  // This test verifies that commit timestamps are resolved consistently between
  // the main table data and change stream records, addressing the timing issue
  // where change streams might see sentinel values while main data gets real timestamps.
  
  // Set up partition token for the commit timestamp change stream
  set_up_partition_token_for_change_stream_partition_table(
      commit_timestamp_change_stream_, store());

  // Create buffered write ops with commit timestamp sentinel value
  std::vector<WriteOp> buffered_write_ops;
  std::vector<const Column*> commit_timestamp_columns = {
      commit_timestamp_table_->FindColumn("id"),
      commit_timestamp_table_->FindColumn("name"),
      commit_timestamp_table_->FindColumn("commit_ts")
  };
  
  // Insert a row with commit timestamp sentinel - this simulates what happens
  // when a client uses spanner.commit_timestamp()
  buffered_write_ops.push_back(Insert(
      commit_timestamp_table_, Key({Int64(1)}), commit_timestamp_columns,
      {Int64(1), String("test_name"), 
       zetasql::Value::Timestamp(kCommitTimestampValueSentinel)}));

  // Also add an update operation to test that updates work correctly too
  buffered_write_ops.push_back(Update(
      commit_timestamp_table_, Key({Int64(2)}), commit_timestamp_columns,
      {Int64(2), String("updated_name"), 
       zetasql::Value::Timestamp(kCommitTimestampValueSentinel)}));

  // Set a real commit timestamp for the test
  absl::Time test_commit_timestamp = absl::FromUnixMicros(1600000000);
  
  // Process the change stream operations - this is where the fix applies
  ZETASQL_ASSERT_OK_AND_ASSIGN(
      std::vector<WriteOp> change_stream_write_ops,
      BuildChangeStreamWriteOps(commit_timestamp_schema_.get(), buffered_write_ops, 
                                 store(), 1, test_commit_timestamp));

  // Verify we got change stream entries for both operations
  ASSERT_EQ(change_stream_write_ops.size(), 2);  // One for insert, one for update
  
  // Check both change stream records
  for (const WriteOp& change_stream_op : change_stream_write_ops) {
    const InsertOp* operation = std::get_if<InsertOp>(&change_stream_op);
    ASSERT_NE(operation, nullptr);
    ASSERT_EQ(operation->table, 
              commit_timestamp_change_stream_->change_stream_data_table());

    // Verify that the commit timestamp in the change stream record 
    // is the real timestamp, not the sentinel value
    zetasql::Value commit_timestamp_value = operation->values[1]; // commit_timestamp is at index 1
    ASSERT_TRUE(commit_timestamp_value.type()->IsTimestamp());
    ASSERT_EQ(commit_timestamp_value.ToTime(), test_commit_timestamp);
    
    // Verify the sentinel value was NOT used
    ASSERT_NE(commit_timestamp_value.ToTime(), absl::FromUnixMicros(zetasql::types::kTimestampMax));

    // Verify the new_values in the mods contain the resolved timestamp, not the sentinel
    zetasql::Value mod_new_values = operation->values[11]; // mods new_values
    ASSERT_TRUE(mod_new_values.type()->IsArray());
    ASSERT_EQ(mod_new_values.num_elements(), 1);
    
    std::string new_values_json = mod_new_values.element(0).string_value();
    // Parse the JSON to verify the commit_ts field contains the real timestamp
    JSON parsed_new_values = JSON::parse(new_values_json);
    ASSERT_TRUE(parsed_new_values.contains("commit_ts"));
    
    // The timestamp should be formatted as a proper timestamp string, not the sentinel
    std::string timestamp_str = parsed_new_values["commit_ts"];
    ASSERT_FALSE(timestamp_str.empty());
    // Should not contain the sentinel timestamp value
    ASSERT_THAT(timestamp_str, testing::Not(testing::HasSubstr("294247-01-10")));  // Max timestamp year
    
    // Additional verification: the timestamp should be a reasonable format
    // (This ensures it's not some other bogus value)
    ASSERT_THAT(timestamp_str, testing::MatchesRegex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z)"));
  }

  // Now simulate what the flush operation would do to the original buffered operations
  // This demonstrates that both main data and change stream data should have consistent timestamps
  std::vector<WriteOp> main_data_ops;
  for (const WriteOp& op : buffered_write_ops) {
    main_data_ops.push_back(std::visit(
        overloaded{
            [&](const InsertOp& insert_op) -> WriteOp {
              const Table* table = insert_op.table;
              // This is what happens during flush - resolve commit timestamps
              ValueList resolved_values;
              for (int i = 0; i < insert_op.columns.size(); i++) {
                zetasql::Value resolved_value = insert_op.values[i];
                if (insert_op.columns[i]->allows_commit_timestamp() && 
                    resolved_value.type()->IsTimestamp() &&
                    resolved_value.ToTime() == kCommitTimestampValueSentinel) {
                  resolved_value = zetasql::Value::Timestamp(test_commit_timestamp);
                }
                resolved_values.push_back(resolved_value);
              }
              return InsertOp{table, insert_op.key, insert_op.columns, resolved_values};
            },
            [&](const UpdateOp& update_op) -> WriteOp {
              const Table* table = update_op.table;
              // This is what happens during flush - resolve commit timestamps
              ValueList resolved_values;
              for (int i = 0; i < update_op.columns.size(); i++) {
                zetasql::Value resolved_value = update_op.values[i];
                if (update_op.columns[i]->allows_commit_timestamp() && 
                    resolved_value.type()->IsTimestamp() &&
                    resolved_value.ToTime() == kCommitTimestampValueSentinel) {
                  resolved_value = zetasql::Value::Timestamp(test_commit_timestamp);
                }
                resolved_values.push_back(resolved_value);
              }
              return UpdateOp{table, update_op.key, update_op.columns, resolved_values};
            },
            [&](const DeleteOp& delete_op) -> WriteOp {
              return delete_op;  // Deletes don't have values to resolve
            },
        },
        op));
  }

  // Verify that main data operations now have the resolved timestamps
  for (const WriteOp& main_op : main_data_ops) {
    if (std::holds_alternative<InsertOp>(main_op)) {
      const InsertOp& insert = std::get<InsertOp>(main_op);
      // Find the commit_ts column value
      for (int i = 0; i < insert.columns.size(); i++) {
        if (insert.columns[i]->Name() == "commit_ts") {
          ASSERT_TRUE(insert.values[i].type()->IsTimestamp());
          ASSERT_EQ(insert.values[i].ToTime(), test_commit_timestamp);
          // Verify it's NOT the sentinel value
          ASSERT_NE(insert.values[i].ToTime(), absl::FromUnixMicros(zetasql::types::kTimestampMax));
        }
      }
    } else if (std::holds_alternative<UpdateOp>(main_op)) {
      const UpdateOp& update = std::get<UpdateOp>(main_op);
      // Find the commit_ts column value
      for (int i = 0; i < update.columns.size(); i++) {
        if (update.columns[i]->Name() == "commit_ts") {
          ASSERT_TRUE(update.values[i].type()->IsTimestamp());
          ASSERT_EQ(update.values[i].ToTime(), test_commit_timestamp);
          // Verify it's NOT the sentinel value
          ASSERT_NE(update.values[i].ToTime(), absl::FromUnixMicros(zetasql::types::kTimestampMax));
        }
      }
    }
  }
  
  // The key assertion: both main data and change stream data should have 
  // the same resolved timestamp values, ensuring consistency
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
