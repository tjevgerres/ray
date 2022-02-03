#pragma once
#include <grpcpp/server.h>

#include "ray/common/asio/instrumented_io_context.h"
#include "src/ray/protobuf/syncer.grpc.pb.h"

namespace ray {
namespace syncing {

struct Reporter {
  virtual ray::rpc::syncer::RaySyncMessage Snapshot() const = 0;
};

struct Receiver {
  virtual void Update(ray::rpc::syncer::RaySyncMessage &) = 0;
  virtual void Snapshot(ray::rpc::syncer::RaySyncMessage &) const = 0;
};

class RaySyncer {
 public:
  using RayComponentId = ray::rpc::syncer::RayComponentId;
  using RaySyncMessage = ray::rpc::syncer::RaySyncMessage;
  using RaySyncMessages = ray::rpc::syncer::RaySyncMessages;
  using RaySyncMessageType = ray::rpc::syncer::RaySyncMessageType;
  using ServerReactor = grpc::ServerBidiReactor<ray::rpc::syncer::RaySyncMessages,
                                                ray::rpc::syncer::RaySyncMessages>;
  using ClientReactor = grpc::ClientBidiReactor<ray::rpc::syncer::RaySyncMessages,
                                                ray::rpc::syncer::RaySyncMessages>;
  static constexpr size_t kComponentArraySize =
      static_cast<size_t>(ray::rpc::syncer::RayComponentId_ARRAYSIZE);

  RaySyncer(std::string node_id, instrumented_io_context &io_context)
      : node_id_(std::move(node_id)), io_context_(io_context) {
    cluster_messages_.emplace(node_id_,
                              absl::flat_hash_map<std::pair<std::string, RayComponentId>,
                                                  std::shared_ptr<RaySyncMessage>>());
  }

  void Follow(std::shared_ptr<grpc::Channel> channel) {
    // We don't allow change the follower for now.
    RAY_CHECK(leader_ == nullptr);
    leader_stub_ = ray::rpc::syncer::RaySyncer::NewStub(channel);
    leader_ = std::make_unique<SyncerClientReactor>(*this, node_id_, *leader_stub_);
  }

  void Register(RayComponentId component_id, const Reporter *reporter,
                Receiver *receiver) {
    reporters_[component_id] = reporter;
    receivers_[component_id] = receiver;
  }

  void Update(const std::string &from_node_id, RaySyncMessage message) {
    auto iter = cluster_messages_.find(from_node_id);
    if (iter == cluster_messages_.end()) {
      return;
    }
    auto component_key = std::make_pair(message.node_id(), message.component_id());
    auto &current_message = iter->second[component_key];
    if (current_message == nullptr || message.version() < current_message->version()) {
      return;
    }

    current_message = std::make_shared<RaySyncMessage>(std::move(message));
    if (receivers_[message.component_id()] != nullptr && message.node_id() != node_id_) {
      receivers_[message.component_id()]->Update(*current_message);
    }
  }

  void Update(const std::string &from_node_id, RaySyncMessages messages) {
    for (auto &message : *messages.mutable_sync_messages()) {
      Update(from_node_id, std::move(message));
    }
  }

  std::vector<std::shared_ptr<RaySyncMessage>> SyncMessages(
      const std::string &node_id) const {
    std::vector<std::shared_ptr<RaySyncMessage>> messages;
    for (auto &node_message : cluster_messages_) {
      if (node_message.first == node_id) {
        continue;
      }
      for (auto &component_message : node_message.second) {
        if (component_message.first.first != node_id) {
          messages.emplace_back(component_message.second);
        }
      }
    }
    return messages;
  }

  ServerReactor *Accept(const std::string &node_id) {
    auto reactor = std::make_unique<SyncerServerReactor>(*this, node_id);
    followers_.emplace(node_id, std::move(reactor));
    return followers_[node_id].get();
  }

 private:
  struct SyncContext {
    void ResetOutMessage() {
      arena.Reset();
      out_message =
          google::protobuf::Arena::CreateMessage<ray::rpc::syncer::RaySyncMessages>(
              &arena);
    }

    std::string node_id;
    google::protobuf::Arena arena;
    ray::rpc::syncer::RaySyncMessages in_message;
    ray::rpc::syncer::RaySyncMessages *out_message;
    std::vector<std::shared_ptr<RaySyncMessage>> buffer;
  };

  // TODO(yic): Find a way to remove the duplicate code.
  class SyncerClientReactor : public ClientReactor {
   public:
    SyncerClientReactor(RaySyncer &instance, const std::string &node_id,
                        ray::rpc::syncer::RaySyncer::Stub &stub)
        : instance_(instance), timer_(instance.io_context_) {
      rpc_context_.AddMetadata("node_id", node_id);
      stub.async()->StartSync(&rpc_context_, this);
      context_.ResetOutMessage();
      StartRead(&context_.in_message);
      instance_.io_context_.dispatch([this] { SendMessage(); }, "RaySyncWrite");
      StartCall();
    }

    const std::string &GetNodeId() const { return context_.node_id; }

    void OnReadDone(bool ok) override {
      if (ok) {
        instance_.io_context_.dispatch(
            [this] {
              instance_.Update(context_.node_id, std::move(context_.in_message));
              context_.in_message.Clear();
              StartRead(&context_.in_message);
            },
            "ReadDone");
      } else {
        StartRead(&context_.in_message);
      }
    }

    void OnWriteDone(bool ok) override {
      if (ok) {
        timer_.expires_from_now(boost::posix_time::milliseconds(100));
        timer_.async_wait([this](const boost::system::error_code &error) {
          if (error == boost::asio::error::operation_aborted) {
            return;
          }
          RAY_CHECK(!error) << error.message();
          SendMessage();
        });
      } else {
        instance_.io_context_.dispatch([this] { SendMessage(); }, "RaySyncWrite");
      }
    }

    void OnDone(const grpc::Status &status) override {
      instance_.io_context_.dispatch(
          [this] {
            auto node_id = context_.node_id;
            instance_.followers_.erase(node_id);
          },
          "RaySyncDone");
    }

   private:
    void SendMessage() {
      for (size_t i = 0; i < kComponentArraySize; ++i) {
        if (instance_.reporters_[i] != nullptr) {
          instance_.Update(context_.node_id, instance_.reporters_[i]->Snapshot());
        }
      }
      context_.buffer = instance_.SyncMessages(context_.node_id);
      context_.ResetOutMessage();
      for (auto &message : context_.buffer) {
        context_.out_message->mutable_sync_messages()->UnsafeArenaAddAllocated(
            message.get());
      }
      StartWrite(context_.out_message);
    }

    RaySyncer &instance_;
    boost::asio::deadline_timer timer_;
    SyncContext context_;
    grpc::ClientContext rpc_context_;
  };

  class SyncerServerReactor : public ServerReactor {
   public:
    SyncerServerReactor(RaySyncer &instance, const std::string &node_id)
        : instance_(instance), timer_(instance.io_context_) {
      context_.node_id = node_id;
      StartRead(&context_.in_message);
      instance_.io_context_.dispatch([this] { SendMessage(); }, "RaySyncWrite");
    }

    const std::string &GetNodeId() const { return context_.node_id; }

    void OnReadDone(bool ok) override {
      if (ok) {
        instance_.io_context_.dispatch(
            [this] {
              instance_.Update(context_.node_id, std::move(context_.in_message));
              context_.in_message.Clear();
              StartRead(&context_.in_message);
            },
            "RaySyncReadDone");
      } else {
        Finish(grpc::Status::OK);
      }
    }

    void OnWriteDone(bool ok) override {
      if (ok) {
        timer_.expires_from_now(boost::posix_time::milliseconds(100));
        timer_.async_wait([this](const boost::system::error_code &error) {
          if (error == boost::asio::error::operation_aborted) {
            return;
          }
          RAY_CHECK(!error) << error.message();
          SendMessage();
        });
      } else {
        Finish(grpc::Status::OK);
      }
    }

    void OnDone() override {
      instance_.io_context_.dispatch(
          [this] {
            auto node_id = context_.node_id;
            instance_.followers_.erase(node_id);
          },
          "RaySyncDone");
    }

   private:
    void SendMessage() {
      context_.buffer = instance_.SyncMessages(context_.node_id);
      context_.ResetOutMessage();
      for (auto &message : context_.buffer) {
        context_.out_message->mutable_sync_messages()->UnsafeArenaAddAllocated(
            message.get());
      }
      StartWrite(context_.out_message);
    }
    RaySyncer &instance_;
    boost::asio::deadline_timer timer_;
    SyncContext context_;
  };

 private:
  const std::string node_id_;

  std::unique_ptr<ray::rpc::syncer::RaySyncer::Stub> leader_stub_;
  std::unique_ptr<ClientReactor> leader_;
  // Manage messages
  absl::flat_hash_map<std::string,
                      absl::flat_hash_map<std::pair<std::string, RayComponentId>,
                                          std::shared_ptr<RaySyncMessage>>>
      cluster_messages_;

  // Manage connections
  absl::flat_hash_map<std::string, std::unique_ptr<ServerReactor>> followers_;

  // For local nodes
  std::array<const Reporter *, kComponentArraySize> reporters_;
  std::array<Receiver *, kComponentArraySize> receivers_;
  instrumented_io_context &io_context_;
};

class RaySyncerService : public ray::rpc::syncer::RaySyncer::CallbackService {
 public:
  RaySyncerService(RaySyncer &syncer) : syncer_(syncer) {}

  grpc::ServerBidiReactor<ray::rpc::syncer::RaySyncMessages,
                          ray::rpc::syncer::RaySyncMessages>
      *StartSync(grpc::CallbackServerContext *context) override {
    const auto &metadata = context->client_metadata();
    auto iter = metadata.find("node_id");
    RAY_CHECK(iter != metadata.end());
    auto node_id = std::string(iter->second.begin(), iter->second.end());
    return syncer_.Accept(node_id);
  }

 private:
  RaySyncer &syncer_;
};

}  // namespace syncing
}  // namespace ray
