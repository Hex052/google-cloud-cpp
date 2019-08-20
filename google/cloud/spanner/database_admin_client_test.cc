// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/spanner/database_admin_client.h"
#include "google/cloud/testing_util/assert_ok.h"
#include <gmock/gmock.h>

namespace google {
namespace cloud {
namespace spanner {
inline namespace SPANNER_CLIENT_NS {
namespace {

using ::testing::_;
using ::testing::Invoke;
namespace gcsa = ::google::spanner::admin::database::v1;

class MockDatabaseAdminClientStub : public internal::DatabaseAdminStub {
 public:
  MOCK_METHOD2(CreateDatabase,
               StatusOr<google::longrunning::Operation>(
                   grpc::ClientContext&, gcsa::CreateDatabaseRequest const&));

  MOCK_METHOD1(AwaitCreateDatabase, future<StatusOr<gcsa::Database>>(
                                        google::longrunning::Operation));

  MOCK_METHOD2(UpdateDatabase, StatusOr<google::longrunning::Operation>(
                                   grpc::ClientContext&,
                                   gcsa::UpdateDatabaseDdlRequest const&));

  MOCK_METHOD1(AwaitUpdateDatabase,
               future<StatusOr<gcsa::UpdateDatabaseDdlMetadata>>(
                   google::longrunning::Operation));

  MOCK_METHOD2(DropDatabase,
               Status(grpc::ClientContext&, gcsa::DropDatabaseRequest const&));

  MOCK_METHOD2(GetOperation,
               StatusOr<google::longrunning::Operation>(
                   grpc::ClientContext&,
                   google::longrunning::GetOperationRequest const&));
};

/// @test Verify that successful case works.
TEST(DatabaseAdminClientTest, CreateDatabaseSuccess) {
  auto mock = std::make_shared<MockDatabaseAdminClientStub>();

  EXPECT_CALL(*mock, CreateDatabase(_, _))
      .WillOnce(
          Invoke([](grpc::ClientContext&, gcsa::CreateDatabaseRequest const&) {
            gcsa::Database database;
            database.set_name("test-db");
            google::longrunning::Operation op;
            op.set_name("test-operation-name");
            op.set_done(true);
            op.mutable_response()->PackFrom(database);
            return make_status_or(op);
          }));
  EXPECT_CALL(*mock, AwaitCreateDatabase(_))
      .WillOnce(Invoke([](google::longrunning::Operation const& op) {
        EXPECT_EQ("test-operation-name", op.name());
        EXPECT_TRUE(op.done());
        EXPECT_TRUE(op.has_response());
        gcsa::Database database;
        op.response().UnpackTo(&database);
        return make_ready_future(make_status_or(database));
      }));

  DatabaseAdminClient client(mock);
  Database dbase("test-project", "test-instance", "test-db");
  auto fut = client.CreateDatabase(dbase);
  EXPECT_EQ(std::future_status::ready, fut.wait_for(std::chrono::seconds(0)));
  auto db = fut.get();
  EXPECT_STATUS_OK(db);

  EXPECT_EQ("test-db", db->name());
}

/// @test Verify that a permanent error in CreateDatabase is immediately
/// reported.
TEST(DatabaseAdminClientTest, HandleCreateDatabaseError) {
  auto mock = std::make_shared<MockDatabaseAdminClientStub>();

  EXPECT_CALL(*mock, CreateDatabase(_, _))
      .WillOnce(
          Invoke([](grpc::ClientContext&, gcsa::CreateDatabaseRequest const&) {
            return StatusOr<google::longrunning::Operation>(
                Status(StatusCode::kPermissionDenied, "uh-oh"));
          }));

  DatabaseAdminClient client(mock);
  Database dbase("test-project", "test-instance", "test-db");
  auto fut = client.CreateDatabase(dbase);
  EXPECT_EQ(std::future_status::ready, fut.wait_for(std::chrono::seconds(0)));
  auto db = fut.get();
  EXPECT_EQ(StatusCode::kPermissionDenied, db.status().code());
}

/// @test Verify that successful case works.
TEST(DatabaseAdminClientTest, UpdateDatabaseSuccess) {
  auto mock = std::make_shared<MockDatabaseAdminClientStub>();

  EXPECT_CALL(*mock, UpdateDatabase(_, _))
      .WillOnce(Invoke(
          [](grpc::ClientContext&, gcsa::UpdateDatabaseDdlRequest const&) {
            gcsa::UpdateDatabaseDdlMetadata metadata;
            metadata.set_database("test-db");
            google::longrunning::Operation op;
            op.set_name("test-operation-name");
            op.set_done(true);
            op.mutable_response()->PackFrom(metadata);
            return make_status_or(op);
          }));
  EXPECT_CALL(*mock, AwaitUpdateDatabase(_))
      .WillOnce(Invoke([](google::longrunning::Operation const& op) {
        EXPECT_EQ("test-operation-name", op.name());
        EXPECT_TRUE(op.done());
        EXPECT_TRUE(op.has_response());
        gcsa::UpdateDatabaseDdlMetadata metadata;
        op.response().UnpackTo(&metadata);
        return make_ready_future(make_status_or(metadata));
      }));

  DatabaseAdminClient client(mock);
  Database dbase("test-project", "test-instance", "test-db");
  auto fut = client.UpdateDatabase(
      dbase, {"ALTER TABLE Albums ADD COLUMN MarketingBudget INT64"});
  EXPECT_EQ(std::future_status::ready, fut.wait_for(std::chrono::seconds(0)));
  auto metadata = fut.get();
  EXPECT_STATUS_OK(metadata);

  EXPECT_EQ("test-db", metadata->database());
}

/// @test Verify that a permanent error in UpdateDatabase is immediately
/// reported.
TEST(DatabaseAdminClientTest, HandleUpdateDatabaseError) {
  auto mock = std::make_shared<MockDatabaseAdminClientStub>();

  EXPECT_CALL(*mock, UpdateDatabase(_, _))
      .WillOnce(Invoke(
          [](grpc::ClientContext&, gcsa::UpdateDatabaseDdlRequest const&) {
            return StatusOr<google::longrunning::Operation>(
                Status(StatusCode::kPermissionDenied, "uh-oh"));
          }));

  DatabaseAdminClient client(mock);
  Database dbase("test-project", "test-instance", "test-db");
  auto fut = client.UpdateDatabase(
      dbase, {"ALTER TABLE Albums ADD COLUMN MarketingBudget INT64"});
  EXPECT_EQ(std::future_status::ready, fut.wait_for(std::chrono::seconds(0)));
  auto db = fut.get();
  EXPECT_EQ(StatusCode::kPermissionDenied, db.status().code());
}

/// @test Verify that errors in the polling loop are reported.
TEST(DatabaseAdminClientTest, HandleAwaitCreateDatabaseError) {
  auto mock = std::make_shared<MockDatabaseAdminClientStub>();

  EXPECT_CALL(*mock, CreateDatabase(_, _))
      .WillOnce(
          Invoke([](grpc::ClientContext&, gcsa::CreateDatabaseRequest const&) {
            google::longrunning::Operation op;
            op.set_name("test-operation-name");
            op.set_done(false);
            return make_status_or(std::move(op));
          }));
  EXPECT_CALL(*mock, AwaitCreateDatabase(_))
      .WillOnce(Invoke([](google::longrunning::Operation const& op) {
        EXPECT_EQ("test-operation-name", op.name());
        return make_ready_future(
            StatusOr<gcsa::Database>(Status(StatusCode::kAborted, "oh noes")));
      }));

  DatabaseAdminClient client(mock);
  Database dbase("test-project", "test-instance", "test-db");
  auto db = client.CreateDatabase(dbase).get();
  EXPECT_EQ(StatusCode::kAborted, db.status().code());
}

/// @test Verify that errors in the polling loop are reported.
TEST(DatabaseAdminClientTest, HandleAwaitUpdateDatabaseError) {
  auto mock = std::make_shared<MockDatabaseAdminClientStub>();

  EXPECT_CALL(*mock, UpdateDatabase(_, _))
      .WillOnce(Invoke(
          [](grpc::ClientContext&, gcsa::UpdateDatabaseDdlRequest const&) {
            google::longrunning::Operation op;
            op.set_name("test-operation-name");
            op.set_done(false);
            return make_status_or(std::move(op));
          }));
  EXPECT_CALL(*mock, AwaitUpdateDatabase(_))
      .WillOnce(Invoke([](google::longrunning::Operation const& op) {
        EXPECT_EQ("test-operation-name", op.name());
        return make_ready_future(StatusOr<gcsa::UpdateDatabaseDdlMetadata>(
            Status(StatusCode::kAborted, "oh noes")));
      }));

  DatabaseAdminClient client(mock);
  Database dbase("test-project", "test-instance", "test-db");
  auto db =
      client
          .UpdateDatabase(
              dbase, {"ALTER TABLE Albums ADD COLUMN MarketingBudget INT64"})
          .get();
  EXPECT_EQ(StatusCode::kAborted, db.status().code());
}

}  // namespace
}  // namespace SPANNER_CLIENT_NS
}  // namespace spanner
}  // namespace cloud
}  // namespace google
