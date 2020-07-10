/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hci/facade/le_acl_manager_facade.h"

#include <condition_variable>
#include <memory>
#include <mutex>

#include "common/bind.h"
#include "grpc/grpc_event_queue.h"
#include "hci/acl_manager.h"
#include "hci/facade/le_acl_manager_facade.grpc.pb.h"
#include "hci/facade/le_acl_manager_facade.pb.h"
#include "hci/hci_packets.h"
#include "packet/raw_builder.h"

using ::grpc::ServerAsyncResponseWriter;
using ::grpc::ServerAsyncWriter;
using ::grpc::ServerContext;

using ::bluetooth::packet::RawBuilder;

namespace bluetooth {
namespace hci {
namespace facade {

using acl_manager::LeAclConnection;
using acl_manager::LeConnectionCallbacks;
using acl_manager::LeConnectionManagementCallbacks;

class LeAclManagerFacadeService : public LeAclManagerFacade::Service, public LeConnectionCallbacks {
 public:
  LeAclManagerFacadeService(AclManager* acl_manager, ::bluetooth::os::Handler* facade_handler)
      : acl_manager_(acl_manager), facade_handler_(facade_handler) {
    acl_manager_->RegisterLeCallbacks(this, facade_handler_);
  }

  ~LeAclManagerFacadeService() override {
    std::unique_lock<std::mutex> lock(acl_connections_mutex_);
    for (auto& conn : acl_connections_) {
      if (conn.second.connection_ != nullptr) {
        conn.second.connection_->GetAclQueueEnd()->UnregisterDequeue();
        conn.second.connection_.reset();
      }
    }
  }

  ::grpc::Status CreateConnection(::grpc::ServerContext* context, const LeConnectionMsg* request,
                                  ::grpc::ServerWriter<LeConnectionEvent>* writer) override {
    Address peer_address;
    ASSERT(Address::FromString(request->address(), peer_address));
    AddressWithType peer(peer_address, static_cast<AddressType>(request->address_type()));
    acl_manager_->CreateLeConnection(peer);
    if (per_connection_events_.size() > current_connection_request_) {
      return ::grpc::Status(::grpc::StatusCode::RESOURCE_EXHAUSTED, "Only one outstanding request is supported");
    }
    per_connection_events_.emplace_back(std::make_unique<::bluetooth::grpc::GrpcEventQueue<LeConnectionEvent>>(
        std::string("connection attempt ") + std::to_string(current_connection_request_)));
    return per_connection_events_[current_connection_request_]->RunLoop(context, writer);
  }

  ::grpc::Status Disconnect(::grpc::ServerContext* context, const LeHandleMsg* request,
                            ::google::protobuf::Empty* response) override {
    std::unique_lock<std::mutex> lock(acl_connections_mutex_);
    auto connection = acl_connections_.find(request->handle());
    if (connection == acl_connections_.end()) {
      LOG_ERROR("Invalid handle");
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "Invalid handle");
    } else {
      connection->second.connection_->Disconnect(DisconnectReason::REMOTE_USER_TERMINATED_CONNECTION);
      return ::grpc::Status::OK;
    }
  }

  ::grpc::Status FetchIncomingConnection(::grpc::ServerContext* context, const google::protobuf::Empty* request,
                                         ::grpc::ServerWriter<LeConnectionEvent>* writer) override {
    if (per_connection_events_.size() > current_connection_request_) {
      return ::grpc::Status(::grpc::StatusCode::RESOURCE_EXHAUSTED, "Only one outstanding connection is supported");
    }
    per_connection_events_.emplace_back(std::make_unique<::bluetooth::grpc::GrpcEventQueue<LeConnectionEvent>>(
        std::string("incoming connection ") + std::to_string(current_connection_request_)));
    return per_connection_events_[current_connection_request_]->RunLoop(context, writer);
  }

  ::grpc::Status SendAclData(::grpc::ServerContext* context, const LeAclData* request,
                             ::google::protobuf::Empty* response) override {
    std::promise<void> promise;
    auto future = promise.get_future();
    {
      std::unique_lock<std::mutex> lock(acl_connections_mutex_);
      auto connection = acl_connections_.find(request->handle());
      if (connection == acl_connections_.end()) {
        LOG_ERROR("Invalid handle");
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "Invalid handle");
      } else {
        connection->second.connection_->GetAclQueueEnd()->RegisterEnqueue(
            facade_handler_, common::Bind(&LeAclManagerFacadeService::enqueue_packet, common::Unretained(this),
                                          common::Unretained(request), common::Passed(std::move(promise))));
      }
    }
    future.wait();
    return ::grpc::Status::OK;
  }

  ::grpc::Status SetInitiatorAddress(::grpc::ServerContext* context,
                                     const ::bluetooth::facade::BluetoothAddressWithType* request,
                                     ::google::protobuf::Empty* writer) override {
    Address address;
    ASSERT(Address::FromString(request->address().address(), address));
    acl_manager_->SetLeInitiatorAddress(AddressWithType(address, static_cast<AddressType>(request->type())));
    return ::grpc::Status::OK;
  }

  std::unique_ptr<BasePacketBuilder> enqueue_packet(const LeAclData* request, std::promise<void> promise) {
    auto connection = acl_connections_.find(request->handle());
    ASSERT_LOG(connection != acl_connections_.end(), "handle %d", request->handle());
    connection->second.connection_->GetAclQueueEnd()->UnregisterEnqueue();
    std::unique_ptr<RawBuilder> packet =
        std::make_unique<RawBuilder>(std::vector<uint8_t>(request->payload().begin(), request->payload().end()));
    promise.set_value();
    return packet;
  }

  ::grpc::Status FetchAclData(::grpc::ServerContext* context, const ::google::protobuf::Empty* request,
                              ::grpc::ServerWriter<LeAclData>* writer) override {
    return pending_acl_data_.RunLoop(context, writer);
  }

  static inline uint16_t to_handle(uint32_t current_request) {
    return (current_request + 0x10) % 0xe00;
  }

  static inline std::string builder_to_string(std::unique_ptr<BasePacketBuilder> builder) {
    std::vector<uint8_t> bytes;
    BitInserter bit_inserter(bytes);
    builder->Serialize(bit_inserter);
    return std::string(bytes.begin(), bytes.end());
  }

  void on_incoming_acl(std::shared_ptr<LeAclConnection> connection, uint16_t handle) {
    auto packet = connection->GetAclQueueEnd()->TryDequeue();
    LeAclData acl_data;
    acl_data.set_handle(handle);
    acl_data.set_payload(std::string(packet->begin(), packet->end()));
    pending_acl_data_.OnIncomingEvent(acl_data);
  }

  void OnDisconnection(uint32_t entry, ErrorCode code) {
    std::unique_ptr<BasePacketBuilder> builder =
        DisconnectionCompleteBuilder::Create(ErrorCode::SUCCESS, to_handle(entry), code);
    LeConnectionEvent disconnection;
    disconnection.set_event(builder_to_string(std::move(builder)));
    per_connection_events_[entry]->OnIncomingEvent(disconnection);
  }

  void OnLeConnectSuccess(AddressWithType address_with_type, std::unique_ptr<LeAclConnection> connection) override {
    LOG_DEBUG("%s", address_with_type.ToString().c_str());

    std::unique_lock<std::mutex> lock(acl_connections_mutex_);
    auto addr = address_with_type.GetAddress();
    std::shared_ptr<LeAclConnection> shared_connection = std::move(connection);
    uint16_t handle = to_handle(current_connection_request_);
    acl_connections_.emplace(
        std::pair(handle, Connection(current_connection_request_, handle, shared_connection, this)));
    shared_connection->GetAclQueueEnd()->RegisterDequeue(
        facade_handler_, common::Bind(&LeAclManagerFacadeService::on_incoming_acl, common::Unretained(this),
                                      shared_connection, to_handle(current_connection_request_)));
    auto callbacks = acl_connections_.find(handle)->second.GetCallbacks();
    shared_connection->RegisterCallbacks(callbacks, facade_handler_);
    {
      std::unique_ptr<BasePacketBuilder> builder =
          LeConnectionCompleteBuilder::Create(ErrorCode::SUCCESS, to_handle(current_connection_request_), Role::MASTER,
                                              address_with_type.GetAddressType(), addr, 1, 2, 3, ClockAccuracy::PPM_20);
      LeConnectionEvent success;
      success.set_event(builder_to_string(std::move(builder)));
      per_connection_events_[current_connection_request_]->OnIncomingEvent(success);
    }
    current_connection_request_++;
  }

  void OnLeConnectFail(AddressWithType address, ErrorCode reason) override {
    std::unique_ptr<BasePacketBuilder> builder = LeConnectionCompleteBuilder::Create(
        reason, 0, Role::MASTER, address.GetAddressType(), address.GetAddress(), 0, 0, 0, ClockAccuracy::PPM_20);
    LeConnectionEvent fail;
    fail.set_event(builder_to_string(std::move(builder)));
    per_connection_events_[current_connection_request_]->OnIncomingEvent(fail);
    current_connection_request_++;
  }

  class Connection : public LeConnectionManagementCallbacks {
   public:
    Connection(uint32_t entry, uint16_t handle, std::shared_ptr<LeAclConnection> connection,
               LeAclManagerFacadeService* facade)
        : entry_(entry), handle_(handle), connection_(std::move(connection)), facade_(facade) {}
    void OnConnectionUpdate(uint16_t connection_interval, uint16_t connection_latency,
                            uint16_t supervision_timeout) override {
      LOG_DEBUG("interval: 0x%hx, latency: 0x%hx, timeout 0x%hx", connection_interval, connection_latency,
                supervision_timeout);
    }

    void OnDisconnection(ErrorCode reason) override {
      connection_->GetAclQueueEnd()->UnregisterDequeue();
      connection_.reset();
      facade_->OnDisconnection(entry_, reason);
    }

    LeConnectionManagementCallbacks* GetCallbacks() {
      return this;
    }

    uint32_t entry_;
    uint16_t handle_;
    std::shared_ptr<LeAclConnection> connection_;
    LeAclManagerFacadeService* facade_;
  };

 private:
  AclManager* acl_manager_;
  ::bluetooth::os::Handler* facade_handler_;
  mutable std::mutex acl_connections_mutex_;
  std::map<uint16_t, Connection> acl_connections_;
  ::bluetooth::grpc::GrpcEventQueue<LeAclData> pending_acl_data_{"FetchAclData"};
  std::vector<std::unique_ptr<::bluetooth::grpc::GrpcEventQueue<LeConnectionEvent>>> per_connection_events_;
  uint32_t current_connection_request_{0};
};

void LeAclManagerFacadeModule::ListDependencies(ModuleList* list) {
  ::bluetooth::grpc::GrpcFacadeModule::ListDependencies(list);
  list->add<AclManager>();
}

void LeAclManagerFacadeModule::Start() {
  ::bluetooth::grpc::GrpcFacadeModule::Start();
  service_ = new LeAclManagerFacadeService(GetDependency<AclManager>(), GetHandler());
}

void LeAclManagerFacadeModule::Stop() {
  delete service_;
  ::bluetooth::grpc::GrpcFacadeModule::Stop();
}

::grpc::Service* LeAclManagerFacadeModule::GetService() const {
  return service_;
}

const ModuleFactory LeAclManagerFacadeModule::Factory =
    ::bluetooth::ModuleFactory([]() { return new LeAclManagerFacadeModule(); });

}  // namespace facade
}  // namespace hci
}  // namespace bluetooth
