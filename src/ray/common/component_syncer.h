#pragma once
#include <grpcpp/server.h>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "ray/common/asio/instrumented_io_context.h"
#include "ray/common/asio/periodical_runner.h"
#include "ray/common/id.h"
#include "src/ray/protobuf/syncer.grpc.pb.h"

namespace ray {
namespace syncing {

using ServerBidiReactor = grpc::ServerBidiReactor<ray::rpc::syncer::RaySyncMessages,
                                                  ray::rpc::syncer::RaySyncMessages>;
using ClientBidiReactor = grpc::ClientBidiReactor<ray::rpc::syncer::RaySyncMessages,
                                                  ray::rpc::syncer::RaySyncMessages>;

using RayComponentId = ray::rpc::syncer::RayComponentId;
using RaySyncMessage = ray::rpc::syncer::RaySyncMessage;
using RaySyncMessages = ray::rpc::syncer::RaySyncMessages;
using RaySyncMessageType = ray::rpc::syncer::RaySyncMessageType;

static constexpr size_t kComponentArraySize =
    static_cast<size_t>(ray::rpc::syncer::RayComponentId_ARRAYSIZE);

struct Reporter {
  virtual std::optional<RaySyncMessage> Snapshot(uint64_t current_version) const = 0;
  virtual ~Reporter() {}
};

struct Receiver {
  virtual void Update(const RaySyncMessage &message) = 0;
  virtual ~Receiver() {}
};

struct SyncClientReactor;
struct SyncServerReactor;

class RaySyncer {
 public:
  RaySyncer(std::string node_id, instrumented_io_context &io_context);
  ~RaySyncer();

  // Follower will send its message to leader
  // Leader will broadcast what it received to followers
  void ConnectTo(std::unique_ptr<ray::rpc::syncer::RaySyncer::Stub> stub);

  SyncServerReactor *ConnectFrom(grpc::CallbackServerContext *context);
  void DisconnectFrom(std::string node_id);

  // Register a component
  void Register(RayComponentId component_id, const Reporter *reporter,
                Receiver *receiver, int64_t publish_ms = 100) {
    reporters_[component_id] = reporter;
    receivers_[component_id] = receiver;
    if(reporter != nullptr) {
      RAY_CHECK(publish_ms > 0);
      timer_.RunFnPeriodically(
          [this, component_id]() {
            const auto &local_view = cluster_view_[GetNodeId()];
            auto reporter = reporters_[component_id];
            if (reporter != nullptr) {
              auto version = local_view[component_id] ? local_view[component_id]->version() : 0;
              auto update = reporter->Snapshot(version);
              if (update) {
                Update(*update);
              }
            }
          },
          publish_ms);
    }
  }

  void Update(RaySyncMessage message) {
    if(message.message_type() == RaySyncMessageType::AGGREGATE) {
      BroadcastMessage(make_shared<RaySyncMessage>(std::move(message)));
      return;
    }

    auto &current_message = cluster_view_[message.node_id()][message.component_id()];
    if (current_message && current_message->version() >= message.version()) {
      // We've already got the newer messages. Skip this.
      return;
    }
    current_message = std::make_shared<RaySyncMessage>(std::move(message));
    BroadcastMessage(current_message);
  }

  void Update(RaySyncMessages messages) {
    for (RaySyncMessage &message : *messages.mutable_sync_messages()) {
      Update(std::move(message));
    }
  }

  const std::string &GetNodeId() const { return node_id_; }

 private:
  template <typename T>
  using Array = std::array<T, kComponentArraySize>;

  void BroadcastMessage(std::shared_ptr<RaySyncMessage> message);
  const std::string node_id_;
  std::unique_ptr<ray::rpc::syncer::RaySyncer::Stub> leader_stub_;
  SyncClientReactor *leader_ = nullptr;

  absl::flat_hash_map<std::string, Array<std::shared_ptr<RaySyncMessage>>> cluster_view_;

  // Manage connections
  absl::flat_hash_map<std::string, std::unique_ptr<SyncServerReactor>> followers_;

  // For local nodes
  std::array<const Reporter *, kComponentArraySize> reporters_;
  std::array<Receiver *, kComponentArraySize> receivers_;
  instrumented_io_context &io_context_;
  ray::PeriodicalRunner timer_;
};

class RaySyncerService : public ray::rpc::syncer::RaySyncer::CallbackService {
 public:
  RaySyncerService(RaySyncer &syncer) : syncer_(syncer) {}

  grpc::ServerBidiReactor<RaySyncMessages, RaySyncMessages> *StartSync(
      grpc::CallbackServerContext *context) override;

 private:
  RaySyncer &syncer_;
};

template <typename T>
class NodeSyncContext : public T {
 public:
  using T::StartRead;
  using T::StartWrite;

  constexpr static bool kIsServer = std::is_same_v<T, ServerBidiReactor>;
  using C =
      std::conditional_t<kIsServer, grpc::CallbackServerContext, grpc::ClientContext>;

  NodeSyncContext(RaySyncer &syncer, instrumented_io_context &io_context, C *rpc_context)
      : rpc_context_(rpc_context), io_context_(io_context), instance_(syncer) {
    // write_opts_.set_corked();
  }

  void Init() {
    if constexpr (kIsServer) {
      // Init server
      const auto &metadata = rpc_context_->client_metadata();
      auto iter = metadata.find("node_id");
      RAY_CHECK(iter != metadata.end());
      node_id_ =
          NodeID::FromHex(std::string(iter->second.begin(), iter->second.end())).Binary();
      T::StartSendInitialMetadata();
    } else {
      T::StartCall();
    }
  }

  const std::string &GetNodeId() const { return node_id_; }

  void Send(std::shared_ptr<RaySyncMessage> message) {
    auto &node_versions = GetNodeComponentVersions(message->node_id());

    if (node_versions[message->component_id()] < message->version()) {
      out_buffer_.push_back(message);
      node_versions[message->component_id()] = message->version();
    }

    if (out_message_ == nullptr) {
      SendNextMessage();
    }
  }

  void OnReadDone(bool ok) override {
    if (ok) {
      io_context_.dispatch(
          [this] {
            for (auto &message : in_message_.sync_messages()) {
              auto &node_versions = this->GetNodeComponentVersions(message.node_id());
              if (node_versions[message.component_id()] < message.version()) {
                node_versions[message.component_id()] = message.version();
              }
            }
            instance_.Update(std::move(in_message_));
            in_message_.Clear();
            StartRead(&in_message_);
          },
          "ReadDone");
    } else {
      HandleFailure();
    }
  }

  void OnWriteDone(bool ok) override {
    if (ok) {
      io_context_.dispatch([this] { this->SendNextMessage(); }, "RaySyncWrite");
    } else {
      HandleFailure();
    }
  }

 protected:
  void SendNextMessage() {
    if (out_message_ != nullptr) {
      out_buffer_.erase(out_buffer_.begin(), out_buffer_.begin() + consumed_messages_);
      consumed_messages_ = 0;
      arena_.Reset();
    }

    if (out_buffer_.empty()) {
      if (out_message_ != nullptr) {
        StartWrite(nullptr);
      }
      out_message_ = nullptr;
    } else {
      out_message_ =
          google::protobuf::Arena::CreateMessage<ray::rpc::syncer::RaySyncMessages>(
              &arena_);
      absl::flat_hash_set<std::string> inserted;
      for (auto iter = out_buffer_.rbegin(); iter != out_buffer_.rend(); ++iter) {
        if (inserted.find((*iter)->node_id()) != inserted.end()) {
          continue;
        }
        inserted.insert((*iter)->node_id());
        out_message_->mutable_sync_messages()->UnsafeArenaAddAllocated((*iter).get());
      }
      consumed_messages_ = out_buffer_.size();
      StartWrite(out_message_, write_opts_);
    }
  }

  std::array<uint64_t, kComponentArraySize> &GetNodeComponentVersions(
      const std::string &node_id) {
    auto iter = node_versions_.find(node_id);
    if (iter == node_versions_.end()) {
      iter =
          node_versions_.emplace(node_id, std::array<uint64_t, kComponentArraySize>({}))
              .first;
    }
    return iter->second;
  }

  void HandleFailure() {
    RAY_LOG(ERROR) << "Sync with " << GetNodeId() << " failed";
    if constexpr (kIsServer) {
      T::Finish(grpc::Status::OK);
    } else {
      T::StartWritesDone();
    }
  }

  C *rpc_context_;
  instrumented_io_context &io_context_;
  RaySyncer &instance_;
  std::string node_id_;

  google::protobuf::Arena arena_;
  ray::rpc::syncer::RaySyncMessages in_message_;
  ray::rpc::syncer::RaySyncMessages *out_message_ = nullptr;
  size_t consumed_messages_ = 0;
  std::vector<std::shared_ptr<RaySyncMessage>> out_buffer_;

  absl::flat_hash_map<std::string, std::array<uint64_t, kComponentArraySize>>
      node_versions_;
  grpc::WriteOptions write_opts_;
};

struct SyncServerReactor : public NodeSyncContext<ServerBidiReactor> {
  using NodeSyncContext<ServerBidiReactor>::NodeSyncContext;

  void OnSendInitialMetadataDone(bool ok) override {
    if (ok) {
      StartRead(&in_message_);
    } else {
      Finish(grpc::Status::OK);
    }
  }

  void OnDone() override { instance_.DisconnectFrom(node_id_); }
};

struct SyncClientReactor : public NodeSyncContext<ClientBidiReactor> {
  using NodeSyncContext<ClientBidiReactor>::NodeSyncContext;
  void OnReadInitialMetadataDone(bool ok) override {
    if (ok) {
      const auto &metadata = rpc_context_->GetServerInitialMetadata();
      auto iter = metadata.find("node_id");
      RAY_CHECK(iter != metadata.end());
      RAY_LOG(INFO) << "Start to follow " << iter->second;
      node_id_ =
          NodeID::FromHex(std::string(iter->second.begin(), iter->second.end())).Binary();
      StartRead(&in_message_);
    } else {
      HandleFailure();
    }
  }

  void OnDone(const grpc::Status &status) override {
    RAY_LOG(INFO) << "NodeId: " << GetNodeId()
                  << " disconnects from sync server with status "
                  << status.error_message();
    delete this;
  }

  void OnWritesDoneDone(bool ok) override {
    if (!ok) {
      RAY_LOG(ERROR) << "Failed to send WritesDone to server";
    }
  }
};

}  // namespace syncing
}  // namespace ray
