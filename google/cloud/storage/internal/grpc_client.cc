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

#include "google/cloud/storage/internal/grpc_client.h"
#include "google/cloud/storage/grpc_plugin.h"
#include "google/cloud/storage/internal/grpc_object_read_source.h"
#include "google/cloud/storage/internal/grpc_resumable_upload_session.h"
#include "google/cloud/storage/internal/openssl_util.h"
#include "google/cloud/storage/internal/resumable_upload_session.h"
#include "google/cloud/storage/internal/sha256_hash.h"
#include "google/cloud/storage/internal/storage_auth.h"
#include "google/cloud/storage/internal/storage_round_robin.h"
#include "google/cloud/storage/internal/storage_stub.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/internal/big_endian.h"
#include "google/cloud/internal/format_time_point.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/internal/invoke_result.h"
#include "google/cloud/internal/time_utils.h"
#include "google/cloud/internal/unified_grpc_credentials.h"
#include "google/cloud/log.h"
#include "absl/algorithm/container.h"
#include "absl/strings/str_split.h"
#include "absl/time/time.h"
#include <crc32c/crc32c.h>
#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <cinttypes>

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace internal {

using ::google::cloud::internal::GrpcAuthenticationStrategy;
using ::google::cloud::internal::MakeBackgroundThreadsFactory;

auto constexpr kDirectPathConfig = R"json({
    "loadBalancingConfig": [{
      "grpclb": {
        "childPolicy": [{
          "pick_first": {}
        }]
      }
    }]
  })json";

int DefaultGrpcNumChannels() {
  auto constexpr kMinimumChannels = 4;
  auto const count = std::thread::hardware_concurrency();
  return (std::max)(kMinimumChannels, static_cast<int>(count));
}

Options DefaultOptionsGrpc(Options options) {
  options = DefaultOptionsWithCredentials(std::move(options));
  if (!options.has<UnifiedCredentialsOption>() &&
      !options.has<GrpcCredentialOption>()) {
    options.set<UnifiedCredentialsOption>(
        google::cloud::MakeGoogleDefaultCredentials());
  }
  if (!options.has<EndpointOption>()) {
    options.set<EndpointOption>("storage.googleapis.com");
  }
  auto env = google::cloud::internal::GetEnv("CLOUD_STORAGE_GRPC_ENDPOINT");
  if (env.has_value()) {
    options.set<UnifiedCredentialsOption>(MakeInsecureCredentials());
    options.set<EndpointOption>(*env);
  }
  if (!options.has<GrpcNumChannelsOption>()) {
    options.set<GrpcNumChannelsOption>(DefaultGrpcNumChannels());
  }
  return options;
}

std::shared_ptr<grpc::Channel> CreateGrpcChannel(
    GrpcAuthenticationStrategy& auth, Options const& options, int channel_id) {
  grpc::ChannelArguments args;
  auto const& config = options.get<storage_experimental::GrpcPluginOption>();
  if (config.empty() || config == "default" || config == "none") {
    // Just configure for the regular path.
    args.SetInt("grpc.channel_id", channel_id);
    return auth.CreateChannel(options.get<EndpointOption>(), std::move(args));
  }
  std::set<absl::string_view> settings = absl::StrSplit(config, ',');
  auto const dp = settings.count("dp") != 0 || settings.count("alts") != 0;
  if (dp || settings.count("pick-first-lb") != 0) {
    args.SetServiceConfigJSON(kDirectPathConfig);
  }
  if (dp || settings.count("enable-dns-srv-queries") != 0) {
    args.SetInt("grpc.dns_enable_srv_queries", 1);
  }
  if (settings.count("disable-dns-srv-queries") != 0) {
    args.SetInt("grpc.dns_enable_srv_queries", 0);
  }
  if (settings.count("exclusive") != 0) {
    args.SetInt("grpc.channel_id", channel_id);
  }
  if (settings.count("alts") != 0) {
    grpc::experimental::AltsCredentialsOptions alts_opts;
    return grpc::CreateCustomChannel(
        options.get<EndpointOption>(),
        grpc::CompositeChannelCredentials(
            grpc::experimental::AltsCredentials(alts_opts),
            grpc::GoogleComputeEngineCredentials()),
        std::move(args));
  }
  return auth.CreateChannel(options.get<EndpointOption>(), std::move(args));
}

std::shared_ptr<GrpcAuthenticationStrategy> CreateAuthenticationStrategy(
    CompletionQueue cq, Options const& opts) {
  if (opts.has<UnifiedCredentialsOption>()) {
    return google::cloud::internal::CreateAuthenticationStrategy(
        opts.get<UnifiedCredentialsOption>(), std::move(cq), opts);
  }
  return google::cloud::internal::CreateAuthenticationStrategy(
      opts.get<google::cloud::GrpcCredentialOption>());
}

std::shared_ptr<StorageStub> CreateStorageStub(CompletionQueue cq,
                                               Options const& opts) {
  auto auth = CreateAuthenticationStrategy(std::move(cq), opts);
  std::vector<std::shared_ptr<StorageStub>> children(
      (std::max)(1, opts.get<GrpcNumChannelsOption>()));
  int id = 0;
  std::generate(children.begin(), children.end(), [&id, &auth, opts] {
    return MakeDefaultStorageStub(CreateGrpcChannel(*auth, opts, id++));
  });
  std::shared_ptr<StorageStub> stub =
      std::make_shared<StorageRoundRobin>(std::move(children));
  if (auth->RequiresConfigureContext()) {
    stub = std::make_shared<StorageAuth>(std::move(auth), std::move(stub));
  }
  return stub;
}

std::shared_ptr<GrpcClient> GrpcClient::Create(Options const& opts) {
  // Cannot use std::make_shared<> as the constructor is private.
  return std::shared_ptr<GrpcClient>(new GrpcClient(opts));
}

std::shared_ptr<GrpcClient> GrpcClient::CreateMock(
    std::shared_ptr<StorageStub> stub, Options opts) {
  return std::shared_ptr<GrpcClient>(
      new GrpcClient(std::move(stub), DefaultOptionsGrpc(std::move(opts))));
}

GrpcClient::GrpcClient(Options const& opts)
    : backwards_compatibility_options_(
          MakeBackwardsCompatibleClientOptions(opts)),
      background_(MakeBackgroundThreadsFactory(opts)()),
      stub_(CreateStorageStub(background_->cq(), opts)) {}

GrpcClient::GrpcClient(std::shared_ptr<StorageStub> stub, Options const& opts)
    : backwards_compatibility_options_(
          MakeBackwardsCompatibleClientOptions(opts)),
      background_(MakeBackgroundThreadsFactory(opts)()),
      stub_(std::move(stub)) {}

std::unique_ptr<GrpcClient::InsertStream> GrpcClient::CreateUploadWriter(
    std::unique_ptr<grpc::ClientContext> context) {
  return stub_->InsertObjectMedia(std::move(context));
}

StatusOr<ResumableUploadResponse> GrpcClient::QueryResumableUpload(
    QueryResumableUploadRequest const& request) {
  grpc::ClientContext context;
  auto response = stub_->QueryWriteStatus(context, ToProto(request));
  if (!response) return std::move(response).status();

  return ResumableUploadResponse{
      {},
      static_cast<std::uint64_t>(response->committed_size()),
      // TODO(b/146890058) - `response` should include the object metadata.
      ObjectMetadata{},
      response->complete() ? ResumableUploadResponse::kDone
                           : ResumableUploadResponse::kInProgress,
      {}};
}

ClientOptions const& GrpcClient::client_options() const {
  return backwards_compatibility_options_;
}

StatusOr<ListBucketsResponse> GrpcClient::ListBuckets(
    ListBucketsRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<BucketMetadata> GrpcClient::CreateBucket(CreateBucketRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<BucketMetadata> GrpcClient::GetBucketMetadata(
    GetBucketMetadataRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<EmptyResponse> GrpcClient::DeleteBucket(DeleteBucketRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<BucketMetadata> GrpcClient::UpdateBucket(UpdateBucketRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<BucketMetadata> GrpcClient::PatchBucket(PatchBucketRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<IamPolicy> GrpcClient::GetBucketIamPolicy(
    GetBucketIamPolicyRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<NativeIamPolicy> GrpcClient::GetNativeBucketIamPolicy(
    GetBucketIamPolicyRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<IamPolicy> GrpcClient::SetBucketIamPolicy(
    SetBucketIamPolicyRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<NativeIamPolicy> GrpcClient::SetNativeBucketIamPolicy(
    SetNativeBucketIamPolicyRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<TestBucketIamPermissionsResponse> GrpcClient::TestBucketIamPermissions(
    TestBucketIamPermissionsRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<BucketMetadata> GrpcClient::LockBucketRetentionPolicy(
    LockBucketRetentionPolicyRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectMetadata> GrpcClient::InsertObjectMedia(
    InsertObjectMediaRequest const& request) {
  auto r = ToProto(request);
  if (!r) return std::move(r).status();
  auto proto_request = *r;

  auto stream =
      stub_->InsertObjectMedia(absl::make_unique<grpc::ClientContext>());

  auto const& contents = request.contents();
  auto const contents_size = static_cast<std::int64_t>(contents.size());
  std::int64_t const maximum_buffer_size =
      google::storage::v1::ServiceConstants::MAX_WRITE_CHUNK_BYTES;

  // This loop must run at least once because we need to send at least one
  // Write() call for empty objects.
  for (std::int64_t offset = 0, n = 0; offset <= contents_size; offset += n) {
    proto_request.set_write_offset(offset);
    auto& data = *proto_request.mutable_checksummed_data();
    n = (std::min)(contents_size - offset, maximum_buffer_size);
    data.set_content(
        contents.substr(static_cast<std::string::size_type>(offset),
                        static_cast<std::string::size_type>(n)));
    data.mutable_crc32c()->set_value(crc32c::Crc32c(data.content()));

    if (offset + n >= contents_size) {
      proto_request.set_finish_write(true);
      stream->Write(proto_request, grpc::WriteOptions{}.set_last_message());
      break;
    }
    if (!stream->Write(proto_request, grpc::WriteOptions{})) break;
    // After the first message, clear the object specification and checksums,
    // there is no need to resend it.
    proto_request.clear_insert_object_spec();
    proto_request.clear_object_checksums();
  }

  auto response = stream->Close();
  if (!response) return std::move(response).status();
  return FromProto(*std::move(response));
}

StatusOr<ObjectMetadata> GrpcClient::CopyObject(CopyObjectRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectMetadata> GrpcClient::GetObjectMetadata(
    GetObjectMetadataRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<std::unique_ptr<ObjectReadSource>> GrpcClient::ReadObject(
    ReadObjectRangeRequest const& request) {
  // With the REST API this condition was detected by the server as an error,
  // generally we prefer the server to detect errors because its answers are
  // authoritative. In this case, the server cannot: with gRPC '0' is the same
  // as "not set" and the server would send back the full file, which was
  // unlikely to be the customer's intent.
  if (request.HasOption<ReadLast>() &&
      request.GetOption<ReadLast>().value() == 0) {
    return Status(
        StatusCode::kOutOfRange,
        "ReadLast(0) is invalid in REST and produces incorrect output in gRPC");
  }
  auto context = absl::make_unique<grpc::ClientContext>();
  if (backwards_compatibility_options_.download_stall_timeout().count() != 0) {
    context->set_deadline(
        std::chrono::system_clock::now() +
        backwards_compatibility_options_.download_stall_timeout());
  }
  return std::unique_ptr<ObjectReadSource>(
      absl::make_unique<GrpcObjectReadSource>(
          stub_->GetObjectMedia(std::move(context), ToProto(request))));
}

StatusOr<ListObjectsResponse> GrpcClient::ListObjects(
    ListObjectsRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<EmptyResponse> GrpcClient::DeleteObject(DeleteObjectRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectMetadata> GrpcClient::UpdateObject(UpdateObjectRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectMetadata> GrpcClient::PatchObject(PatchObjectRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectMetadata> GrpcClient::ComposeObject(
    ComposeObjectRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<RewriteObjectResponse> GrpcClient::RewriteObject(
    RewriteObjectRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<std::unique_ptr<ResumableUploadSession>>
GrpcClient::CreateResumableSession(ResumableUploadRequest const& request) {
  if (request.HasOption<UseResumableUploadSession>()) {
    auto session_id = request.GetOption<UseResumableUploadSession>().value();
    if (!session_id.empty()) {
      return RestoreResumableSession(session_id);
    }
  }

  grpc::ClientContext context;
  auto response = stub_->StartResumableWrite(context, ToProto(request));
  if (!response.ok()) return std::move(response).status();

  auto self = shared_from_this();
  return std::unique_ptr<ResumableUploadSession>(new GrpcResumableUploadSession(
      self,
      {request.bucket_name(), request.object_name(), response->upload_id()}));
}

StatusOr<std::unique_ptr<ResumableUploadSession>>
GrpcClient::RestoreResumableSession(std::string const& upload_url) {
  auto self = shared_from_this();
  auto upload_session_params = DecodeGrpcResumableUploadSessionUrl(upload_url);
  if (!upload_session_params) {
    return upload_session_params.status();
  }
  auto session = std::unique_ptr<ResumableUploadSession>(
      new GrpcResumableUploadSession(self, *upload_session_params));
  auto response = session->ResetSession();
  if (response.status().ok()) {
    return session;
  }
  return std::move(response).status();
}

StatusOr<EmptyResponse> GrpcClient::DeleteResumableUpload(
    DeleteResumableUploadRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ListBucketAclResponse> GrpcClient::ListBucketAcl(
    ListBucketAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<BucketAccessControl> GrpcClient::GetBucketAcl(
    GetBucketAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<BucketAccessControl> GrpcClient::CreateBucketAcl(
    CreateBucketAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<EmptyResponse> GrpcClient::DeleteBucketAcl(
    DeleteBucketAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ListObjectAclResponse> GrpcClient::ListObjectAcl(
    ListObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<BucketAccessControl> GrpcClient::UpdateBucketAcl(
    UpdateBucketAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<BucketAccessControl> GrpcClient::PatchBucketAcl(
    PatchBucketAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectAccessControl> GrpcClient::CreateObjectAcl(
    CreateObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<EmptyResponse> GrpcClient::DeleteObjectAcl(
    DeleteObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectAccessControl> GrpcClient::GetObjectAcl(
    GetObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectAccessControl> GrpcClient::UpdateObjectAcl(
    UpdateObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectAccessControl> GrpcClient::PatchObjectAcl(
    PatchObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ListDefaultObjectAclResponse> GrpcClient::ListDefaultObjectAcl(
    ListDefaultObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectAccessControl> GrpcClient::CreateDefaultObjectAcl(
    CreateDefaultObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<EmptyResponse> GrpcClient::DeleteDefaultObjectAcl(
    DeleteDefaultObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectAccessControl> GrpcClient::GetDefaultObjectAcl(
    GetDefaultObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectAccessControl> GrpcClient::UpdateDefaultObjectAcl(
    UpdateDefaultObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ObjectAccessControl> GrpcClient::PatchDefaultObjectAcl(
    PatchDefaultObjectAclRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ServiceAccount> GrpcClient::GetServiceAccount(
    GetProjectServiceAccountRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ListHmacKeysResponse> GrpcClient::ListHmacKeys(
    ListHmacKeysRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<CreateHmacKeyResponse> GrpcClient::CreateHmacKey(
    CreateHmacKeyRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<EmptyResponse> GrpcClient::DeleteHmacKey(DeleteHmacKeyRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<HmacKeyMetadata> GrpcClient::GetHmacKey(GetHmacKeyRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<HmacKeyMetadata> GrpcClient::UpdateHmacKey(
    UpdateHmacKeyRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<SignBlobResponse> GrpcClient::SignBlob(SignBlobRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<ListNotificationsResponse> GrpcClient::ListNotifications(
    ListNotificationsRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<NotificationMetadata> GrpcClient::CreateNotification(
    CreateNotificationRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<NotificationMetadata> GrpcClient::GetNotification(
    GetNotificationRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

StatusOr<EmptyResponse> GrpcClient::DeleteNotification(
    DeleteNotificationRequest const&) {
  return Status(StatusCode::kUnimplemented, __func__);
}

template <typename GrpcRequest, typename StorageRequest>
void SetCommonParameters(GrpcRequest& request, StorageRequest const& req) {
  if (req.template HasOption<UserProject>()) {
    request.mutable_common_request_params()->set_user_project(
        req.template GetOption<UserProject>().value());
  }
  // The gRPC has a single field for the `QuotaUser` parameter, while the JSON
  // API has two:
  //    https://cloud.google.com/storage/docs/json_api/v1/parameters#quotaUser
  // Fortunately the semantics are to use `quotaUser` if set, so we can set
  // the `UserIp` value into the `quota_user` field, and overwrite it if
  // `QuotaUser` is also set. A bit bizarre, but at least it is backwards
  // compatible.
  if (req.template HasOption<UserIp>()) {
    request.mutable_common_request_params()->set_quota_user(
        req.template GetOption<UserIp>().value());
  }
  if (req.template HasOption<QuotaUser>()) {
    request.mutable_common_request_params()->set_quota_user(
        req.template GetOption<QuotaUser>().value());
  }
  // TODO(#4215) - what do we do with FieldMask, as the representation for
  // `fields` is different.
}

template <typename GrpcRequest, typename StorageRequest>
void SetCommonObjectParameters(GrpcRequest& request,
                               StorageRequest const& req) {
  if (req.template HasOption<EncryptionKey>()) {
    auto data = req.template GetOption<EncryptionKey>().value();
    request.mutable_common_object_request_params()->set_encryption_algorithm(
        std::move(data.algorithm));
    request.mutable_common_object_request_params()->set_encryption_key(
        std::move(data.key));
    request.mutable_common_object_request_params()->set_encryption_key_sha256(
        std::move(data.sha256));
  }
}

template <typename GrpcRequest, typename StorageRequest>
void SetProjection(GrpcRequest& request, StorageRequest const& req) {
  if (req.template HasOption<Projection>()) {
    request.set_projection(
        GrpcClient::ToProto(req.template GetOption<Projection>()));
  }
}

template <typename GrpcRequest>
struct GetPredefinedAcl {
  auto operator()(GrpcRequest const& q) -> decltype(q.predefined_acl());
};

template <
    typename GrpcRequest, typename StorageRequest,
    typename std::enable_if<
        std::is_same<google::storage::v1::CommonEnums::PredefinedObjectAcl,
                     google::cloud::internal::invoke_result_t<
                         GetPredefinedAcl<GrpcRequest>, GrpcRequest>>::value,
        int>::type = 0>
void SetPredefinedAcl(GrpcRequest& request, StorageRequest const& req) {
  if (req.template HasOption<PredefinedAcl>()) {
    request.set_predefined_acl(
        GrpcClient::ToProtoObject(req.template GetOption<PredefinedAcl>()));
  }
}

template <typename GrpcRequest, typename StorageRequest>
void SetPredefinedDefaultObjectAcl(GrpcRequest& request,
                                   StorageRequest const& req) {
  if (req.template HasOption<PredefinedDefaultObjectAcl>()) {
    request.set_predefined_default_object_acl(GrpcClient::ToProto(
        req.template GetOption<PredefinedDefaultObjectAcl>()));
  }
}

template <typename GrpcRequest, typename StorageRequest>
void SetMetagenerationConditions(GrpcRequest& request,
                                 StorageRequest const& req) {
  if (req.template HasOption<IfMetagenerationMatch>()) {
    request.mutable_if_metageneration_match()->set_value(
        req.template GetOption<IfMetagenerationMatch>().value());
  }
  if (req.template HasOption<IfMetagenerationNotMatch>()) {
    request.mutable_if_metageneration_not_match()->set_value(
        req.template GetOption<IfMetagenerationNotMatch>().value());
  }
}

template <typename GrpcRequest, typename StorageRequest>
void SetGenerationConditions(GrpcRequest& request, StorageRequest const& req) {
  if (req.template HasOption<IfGenerationMatch>()) {
    request.mutable_if_generation_match()->set_value(
        req.template GetOption<IfGenerationMatch>().value());
  }
  if (req.template HasOption<IfGenerationNotMatch>()) {
    request.mutable_if_generation_not_match()->set_value(
        req.template GetOption<IfGenerationNotMatch>().value());
  }
}

template <typename StorageRequest>
void SetResourceOptions(google::storage::v1::Object& resource,
                        StorageRequest const& request) {
  if (request.template HasOption<ContentEncoding>()) {
    resource.set_content_encoding(
        request.template GetOption<ContentEncoding>().value());
  }
  if (request.template HasOption<ContentType>()) {
    resource.set_content_type(
        request.template GetOption<ContentType>().value());
  }
  if (request.template HasOption<KmsKeyName>()) {
    resource.set_kms_key_name(request.template GetOption<KmsKeyName>().value());
  }
}

template <typename StorageRequest>
void SetObjectMetadata(google::storage::v1::Object& resource,
                       StorageRequest const& req) {
  if (!req.template HasOption<WithObjectMetadata>()) {
    return;
  }
  auto metadata = req.template GetOption<WithObjectMetadata>().value();
  if (!metadata.content_encoding().empty()) {
    resource.set_content_encoding(metadata.content_encoding());
  }
  if (!metadata.content_disposition().empty()) {
    resource.set_content_disposition(metadata.content_disposition());
  }
  if (!metadata.cache_control().empty()) {
    resource.set_cache_control(metadata.cache_control());
  }
  for (auto const& acl : metadata.acl()) {
    *resource.add_acl() = GrpcClient::ToProto(acl);
  }
  if (!metadata.content_language().empty()) {
    resource.set_content_language(metadata.content_language());
  }
  if (!metadata.content_type().empty()) {
    resource.set_content_type(metadata.content_type());
  }
  if (metadata.event_based_hold()) {
    resource.mutable_event_based_hold()->set_value(metadata.event_based_hold());
  }

  for (auto const& kv : metadata.metadata()) {
    (*resource.mutable_metadata())[kv.first] = kv.second;
  }

  if (!metadata.storage_class().empty()) {
    resource.set_storage_class(metadata.storage_class());
  }
  resource.set_temporary_hold(metadata.temporary_hold());
}

CustomerEncryption GrpcClient::FromProto(
    google::storage::v1::Object::CustomerEncryption rhs) {
  CustomerEncryption result;
  result.encryption_algorithm = std::move(*rhs.mutable_encryption_algorithm());
  result.key_sha256 = std::move(*rhs.mutable_key_sha256());
  return result;
}

google::storage::v1::Object::CustomerEncryption GrpcClient::ToProto(
    CustomerEncryption rhs) {
  google::storage::v1::Object::CustomerEncryption result;
  result.set_encryption_algorithm(std::move(rhs.encryption_algorithm));
  result.set_key_sha256(std::move(rhs.key_sha256));
  return result;
}

ObjectMetadata GrpcClient::FromProto(google::storage::v1::Object object) {
  ObjectMetadata metadata;
  metadata.etag_ = std::move(*object.mutable_etag());
  metadata.id_ = std::move(*object.mutable_id());
  metadata.kind_ = "storage#object";
  metadata.metageneration_ = object.metageneration();
  metadata.name_ = std::move(*object.mutable_name());
  if (object.has_owner()) {
    metadata.owner_ = FromProto(*object.mutable_owner());
  }
  metadata.storage_class_ = std::move(*object.mutable_storage_class());
  if (object.has_time_created()) {
    metadata.time_created_ =
        google::cloud::internal::ToChronoTimePoint(object.time_created());
  }
  if (object.has_updated()) {
    metadata.updated_ =
        google::cloud::internal::ToChronoTimePoint(object.updated());
  }
  std::vector<ObjectAccessControl> acl;
  acl.reserve(object.acl_size());
  for (auto& item : *object.mutable_acl()) {
    acl.push_back(FromProto(std::move(item)));
  }
  metadata.acl_ = std::move(acl);
  metadata.bucket_ = std::move(*object.mutable_bucket());
  metadata.cache_control_ = std::move(*object.mutable_cache_control());
  metadata.component_count_ = object.component_count();
  metadata.content_disposition_ =
      std::move(*object.mutable_content_disposition());
  metadata.content_encoding_ = std::move(*object.mutable_content_encoding());
  metadata.content_language_ = std::move(*object.mutable_content_language());
  metadata.content_type_ = std::move(*object.mutable_content_type());
  if (object.has_crc32c()) {
    metadata.crc32c_ = Crc32cFromProto(object.crc32c());
  }
  if (object.has_customer_encryption()) {
    metadata.customer_encryption_ =
        FromProto(std::move(*object.mutable_customer_encryption()));
  }
  if (object.has_event_based_hold()) {
    metadata.event_based_hold_ = object.event_based_hold().value();
  }
  metadata.generation_ = object.generation();
  metadata.kms_key_name_ = std::move(*object.mutable_kms_key_name());
  metadata.md5_hash_ = object.md5_hash();
  for (auto const& kv : object.metadata()) {
    metadata.metadata_[kv.first] = kv.second;
  }
  if (object.has_retention_expiration_time()) {
    metadata.retention_expiration_time_ =
        google::cloud::internal::ToChronoTimePoint(
            object.retention_expiration_time());
  }
  metadata.size_ = static_cast<std::uint64_t>(object.size());
  metadata.temporary_hold_ = object.temporary_hold();
  if (object.has_time_deleted()) {
    metadata.time_deleted_ =
        google::cloud::internal::ToChronoTimePoint(object.time_deleted());
  }
  if (object.has_time_storage_class_updated()) {
    metadata.time_storage_class_updated_ =
        google::cloud::internal::ToChronoTimePoint(
            object.time_storage_class_updated());
  }
  // TODO(#4893) - support customTime for GCS+gRPC

  return metadata;
}

google::storage::v1::ObjectAccessControl GrpcClient::ToProto(
    ObjectAccessControl const& acl) {
  google::storage::v1::ObjectAccessControl result;
  result.set_role(acl.role());
  result.set_etag(acl.etag());
  result.set_id(acl.id());
  result.set_bucket(acl.bucket());
  result.set_object(acl.object());
  result.set_generation(acl.generation());
  result.set_entity(acl.entity());
  result.set_entity_id(acl.entity_id());
  result.set_email(acl.email());
  result.set_domain(acl.domain());
  if (acl.has_project_team()) {
    result.mutable_project_team()->set_project_number(
        acl.project_team().project_number);
    result.mutable_project_team()->set_team(acl.project_team().team);
  }
  return result;
}

ObjectAccessControl GrpcClient::FromProto(
    google::storage::v1::ObjectAccessControl acl) {
  ObjectAccessControl result;
  result.bucket_ = std::move(*acl.mutable_bucket());
  result.domain_ = std::move(*acl.mutable_domain());
  result.email_ = std::move(*acl.mutable_email());
  result.entity_ = std::move(*acl.mutable_entity());
  result.entity_id_ = std::move(*acl.mutable_entity_id());
  result.etag_ = std::move(*acl.mutable_etag());
  result.id_ = std::move(*acl.mutable_id());
  result.kind_ = "storage#objectAccessControl";
  if (acl.has_project_team()) {
    result.project_team_ = ProjectTeam{
        std::move(*acl.mutable_project_team()->mutable_project_number()),
        std::move(*acl.mutable_project_team()->mutable_team()),
    };
  }
  result.role_ = std::move(*acl.mutable_role());
  result.self_link_.clear();
  result.object_ = std::move(*acl.mutable_object());
  result.generation_ = acl.generation();

  return result;
}

google::storage::v1::Owner GrpcClient::ToProto(Owner rhs) {
  google::storage::v1::Owner result;
  *result.mutable_entity() = std::move(rhs.entity);
  *result.mutable_entity_id() = std::move(rhs.entity_id);
  return result;
}

Owner GrpcClient::FromProto(google::storage::v1::Owner rhs) {
  Owner result;
  result.entity = std::move(*rhs.mutable_entity());
  result.entity_id = std::move(*rhs.mutable_entity_id());
  return result;
}

google::storage::v1::CommonEnums::Projection GrpcClient::ToProto(
    Projection const& p) {
  if (p.value() == Projection::NoAcl().value()) {
    return google::storage::v1::CommonEnums::NO_ACL;
  }
  if (p.value() == Projection::Full().value()) {
    return google::storage::v1::CommonEnums::FULL;
  }
  GCP_LOG(ERROR) << "Unknown projection value " << p;
  return google::storage::v1::CommonEnums::FULL;
}

google::storage::v1::CommonEnums::PredefinedObjectAcl GrpcClient::ToProtoObject(
    PredefinedAcl const& acl) {
  if (acl.value() == PredefinedAcl::BucketOwnerFullControl().value()) {
    return google::storage::v1::CommonEnums::
        OBJECT_ACL_BUCKET_OWNER_FULL_CONTROL;
  }
  if (acl.value() == PredefinedAcl::BucketOwnerRead().value()) {
    return google::storage::v1::CommonEnums::OBJECT_ACL_BUCKET_OWNER_READ;
  }
  if (acl.value() == PredefinedAcl::AuthenticatedRead().value()) {
    return google::storage::v1::CommonEnums::OBJECT_ACL_AUTHENTICATED_READ;
  }
  if (acl.value() == PredefinedAcl::Private().value()) {
    return google::storage::v1::CommonEnums::OBJECT_ACL_PRIVATE;
  }
  if (acl.value() == PredefinedAcl::ProjectPrivate().value()) {
    return google::storage::v1::CommonEnums::OBJECT_ACL_PROJECT_PRIVATE;
  }
  if (acl.value() == PredefinedAcl::PublicRead().value()) {
    return google::storage::v1::CommonEnums::OBJECT_ACL_PUBLIC_READ;
  }
  if (acl.value() == PredefinedAcl::PublicReadWrite().value()) {
    GCP_LOG(ERROR) << "Invalid predefinedAcl value " << acl;
    return google::storage::v1::CommonEnums::PREDEFINED_OBJECT_ACL_UNSPECIFIED;
  }
  GCP_LOG(ERROR) << "Unknown predefinedAcl value " << acl;
  return google::storage::v1::CommonEnums::PREDEFINED_OBJECT_ACL_UNSPECIFIED;
}

StatusOr<google::storage::v1::InsertObjectRequest> GrpcClient::ToProto(
    InsertObjectMediaRequest const& request) {
  google::storage::v1::InsertObjectRequest r;
  auto& object_spec = *r.mutable_insert_object_spec();
  auto& resource = *object_spec.mutable_resource();
  SetResourceOptions(resource, request);
  SetObjectMetadata(resource, request);
  SetPredefinedAcl(object_spec, request);
  SetGenerationConditions(object_spec, request);
  SetMetagenerationConditions(object_spec, request);
  SetProjection(object_spec, request);
  SetCommonObjectParameters(r, request);
  SetCommonParameters(r, request);

  resource.set_bucket(request.bucket_name());
  resource.set_name(request.object_name());
  r.set_write_offset(0);

  auto& checksums = *r.mutable_object_checksums();
  if (request.HasOption<Crc32cChecksumValue>()) {
    // The client library accepts CRC32C checksums in the format required by the
    // REST APIs (base64-encoded big-endian, 32-bit integers). We need to
    // convert this to the format expected by proto, which is just a 32-bit
    // integer. But the value received by the application might be incorrect, so
    // we need to validate it.
    auto as_proto =
        Crc32cToProto(request.GetOption<Crc32cChecksumValue>().value());
    if (!as_proto.ok()) return std::move(as_proto).status();
    checksums.mutable_crc32c()->set_value(*as_proto);
  } else if (request.GetOption<DisableCrc32cChecksum>().value_or(false)) {
    // Nothing to do, the option is disabled (mostly useful in tests).
  } else {
    checksums.mutable_crc32c()->set_value(crc32c::Crc32c(request.contents()));
  }

  if (request.HasOption<MD5HashValue>()) {
    auto as_proto = MD5ToProto(request.GetOption<MD5HashValue>().value());
    if (!as_proto.ok()) return std::move(as_proto).status();
    checksums.set_md5_hash(*std::move(as_proto));
  } else if (request.GetOption<DisableMD5Hash>().value_or(false)) {
    // Nothing to do, the option is disabled.
  } else {
    checksums.set_md5_hash(ComputeMD5Hash(request.contents()));
  }

  return r;
}

google::storage::v1::StartResumableWriteRequest GrpcClient::ToProto(
    ResumableUploadRequest const& request) {
  google::storage::v1::StartResumableWriteRequest result;

  auto& object_spec = *result.mutable_insert_object_spec();
  auto& resource = *object_spec.mutable_resource();
  SetResourceOptions(resource, request);
  SetObjectMetadata(resource, request);
  SetPredefinedAcl(object_spec, request);
  SetGenerationConditions(object_spec, request);
  SetMetagenerationConditions(object_spec, request);
  SetProjection(object_spec, request);
  SetCommonParameters(result, request);
  SetCommonObjectParameters(result, request);

  resource.set_bucket(request.bucket_name());
  resource.set_name(request.object_name());

  return result;
}

google::storage::v1::QueryWriteStatusRequest GrpcClient::ToProto(
    QueryResumableUploadRequest const& request) {
  google::storage::v1::QueryWriteStatusRequest r;
  r.set_upload_id(request.upload_session_url());
  return r;
}

google::storage::v1::GetObjectMediaRequest GrpcClient::ToProto(
    ReadObjectRangeRequest const& request) {
  google::storage::v1::GetObjectMediaRequest r;
  r.set_object(request.object_name());
  r.set_bucket(request.bucket_name());
  if (request.HasOption<Generation>()) {
    r.set_generation(request.GetOption<Generation>().value());
  }
  if (request.HasOption<ReadRange>()) {
    auto const range = request.GetOption<ReadRange>().value();
    r.set_read_offset(range.begin);
    r.set_read_limit(range.end - range.begin);
  }
  if (request.HasOption<ReadLast>()) {
    auto const offset = request.GetOption<ReadLast>().value();
    r.set_read_offset(-offset);
  }
  if (request.HasOption<ReadFromOffset>()) {
    auto const offset = request.GetOption<ReadFromOffset>().value();
    if (offset > r.read_offset()) {
      if (r.read_limit() > 0) {
        r.set_read_limit(offset - r.read_offset());
      }
      r.set_read_offset(offset);
    }
  }
  SetGenerationConditions(r, request);
  SetMetagenerationConditions(r, request);
  SetCommonObjectParameters(r, request);
  SetCommonParameters(r, request);

  return r;
}

std::string GrpcClient::Crc32cFromProto(
    google::protobuf::UInt32Value const& v) {
  auto endian_encoded = google::cloud::internal::EncodeBigEndian(v.value());
  return Base64Encode(endian_encoded);
}

StatusOr<std::uint32_t> GrpcClient::Crc32cToProto(std::string const& v) {
  auto decoded = Base64Decode(v);
  if (!decoded) return std::move(decoded).status();
  return google::cloud::internal::DecodeBigEndian<std::uint32_t>(
      std::string(decoded->begin(), decoded->end()));
}

std::string GrpcClient::MD5FromProto(std::string const& v) {
  if (v.empty()) return {};
  auto binary = internal::HexDecode(v);
  return internal::Base64Encode(binary);
}

StatusOr<std::string> GrpcClient::MD5ToProto(std::string const& v) {
  if (v.empty()) return {};
  auto binary = internal::Base64Decode(v);
  if (!binary) return std::move(binary).status();
  return internal::HexEncode(*binary);
}

std::string GrpcClient::ComputeMD5Hash(const std::string& payload) {
  return internal::HexEncode(internal::MD5Hash(payload));
}

}  // namespace internal
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google
