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

#include "hci/facade/acl_manager_facade.h"

#include <condition_variable>
#include <memory>
#include <mutex>

#include "common/bind.h"
#include "grpc/grpc_event_queue.h"
#include "hci/acl_manager.h"
#include "hci/facade/acl_manager_facade.grpc.pb.h"
#include "hci/facade/acl_manager_facade.pb.h"
#include "hci/hci_packets.h"
#include "packet/raw_builder.h"

using ::grpc::ServerAsyncResponseWriter;
using ::grpc::ServerAsyncWriter;
using ::grpc::ServerContext;

using ::bluetooth::packet::RawBuilder;

namespace bluetooth {
namespace hci {
namespace facade {

using acl_manager::ClassicAclConnection;
using acl_manager::ConnectionCallbacks;
using acl_manager::ConnectionManagementCallbacks;

class AclManagerFacadeService : public AclManagerFacade::Service, public ConnectionCallbacks {
 public:
  AclManagerFacadeService(AclManager* acl_manager, ::bluetooth::os::Handler* facade_handler)
      : acl_manager_(acl_manager), facade_handler_(facade_handler) {
    acl_manager_->RegisterCallbacks(this, facade_handler_);
  }

  ~AclManagerFacadeService() override {
    std::unique_lock<std::mutex> lock(acl_connections_mutex_);
    for (auto connection : acl_connections_) {
      connection.second.connection_->GetAclQueueEnd()->UnregisterDequeue();
    }
  }

  ::grpc::Status CreateConnection(::grpc::ServerContext* context, const ConnectionMsg* request,
                                  ::grpc::ServerWriter<ConnectionEvent>* writer) override {
    Address peer;
    ASSERT(Address::FromString(request->address(), peer));
    acl_manager_->CreateConnection(peer);
    if (per_connection_events_.size() > current_connection_request_) {
      return ::grpc::Status(::grpc::StatusCode::RESOURCE_EXHAUSTED, "Only one outstanding request is supported");
    }
    per_connection_events_.emplace_back(std::make_unique<::bluetooth::grpc::GrpcEventQueue<ConnectionEvent>>(
        std::string("connection attempt ") + std::to_string(current_connection_request_)));
    return per_connection_events_[current_connection_request_]->RunLoop(context, writer);
  }

  ::grpc::Status Disconnect(::grpc::ServerContext* context, const HandleMsg* request,
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

  ::grpc::Status AuthenticationRequested(::grpc::ServerContext* context, const HandleMsg* request,
                                         ::google::protobuf::Empty* response) override {
    std::unique_lock<std::mutex> lock(acl_connections_mutex_);
    auto connection = acl_connections_.find(request->handle());
    if (connection == acl_connections_.end()) {
      LOG_ERROR("Invalid handle");
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "Invalid handle");
    } else {
      connection->second.connection_->AuthenticationRequested();
      return ::grpc::Status::OK;
    }
  };

  ::grpc::Status FetchIncomingConnection(::grpc::ServerContext* context, const google::protobuf::Empty* request,
                                         ::grpc::ServerWriter<ConnectionEvent>* writer) override {
    if (per_connection_events_.size() > current_connection_request_) {
      return ::grpc::Status(::grpc::StatusCode::RESOURCE_EXHAUSTED, "Only one outstanding connection is supported");
    }
    per_connection_events_.emplace_back(std::make_unique<::bluetooth::grpc::GrpcEventQueue<ConnectionEvent>>(
        std::string("incoming connection ") + std::to_string(current_connection_request_)));
    return per_connection_events_[current_connection_request_]->RunLoop(context, writer);
  }

  ::grpc::Status SendAclData(::grpc::ServerContext* context, const AclData* request,
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
        // TODO: This is unsafe because connection may have gone
        connection->second.connection_->GetAclQueueEnd()->RegisterEnqueue(
            facade_handler_, common::Bind(&AclManagerFacadeService::enqueue_packet, common::Unretained(this),
                                          common::Unretained(request), common::Passed(std::move(promise))));
      }
      auto status = future.wait_for(std::chrono::milliseconds(1000));
      if (status != std::future_status::ready) {
        return ::grpc::Status(::grpc::StatusCode::RESOURCE_EXHAUSTED, "Can't send packet");
      }
    }
    return ::grpc::Status::OK;
  }

  std::unique_ptr<BasePacketBuilder> enqueue_packet(const AclData* request, std::promise<void> promise) {
    auto connection = acl_connections_.find(request->handle());
    ASSERT_LOG(connection != acl_connections_.end(), "handle %d", request->handle());
    connection->second.connection_->GetAclQueueEnd()->UnregisterEnqueue();
    std::unique_ptr<RawBuilder> packet =
        std::make_unique<RawBuilder>(std::vector<uint8_t>(request->payload().begin(), request->payload().end()));
    promise.set_value();
    return packet;
  }

  ::grpc::Status FetchAclData(::grpc::ServerContext* context, const ::google::protobuf::Empty* request,
                              ::grpc::ServerWriter<AclData>* writer) override {
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

  void on_incoming_classic_acl(std::shared_ptr<ClassicAclConnection> connection, uint16_t handle) {
    auto packet = connection->GetAclQueueEnd()->TryDequeue();
    AclData acl_data;
    acl_data.set_handle(handle);
    acl_data.set_payload(std::string(packet->begin(), packet->end()));
    pending_acl_data_.OnIncomingEvent(acl_data);
  }

  void on_disconnect(std::shared_ptr<ClassicAclConnection> connection, uint32_t entry, ErrorCode code) {
    connection->GetAclQueueEnd()->UnregisterDequeue();
    std::unique_ptr<BasePacketBuilder> builder =
        DisconnectBuilder::Create(to_handle(entry), static_cast<DisconnectReason>(code));
    ConnectionEvent disconnection;
    disconnection.set_event(builder_to_string(std::move(builder)));
    per_connection_events_[entry]->OnIncomingEvent(disconnection);
  }

  void OnConnectSuccess(std::unique_ptr<ClassicAclConnection> connection) override {
    std::unique_lock<std::mutex> lock(acl_connections_mutex_);
    auto addr = connection->GetAddress();
    std::shared_ptr<ClassicAclConnection> shared_connection = std::move(connection);
    uint16_t handle = to_handle(current_connection_request_);
    acl_connections_.emplace(std::pair(handle, Connection(handle, shared_connection)));
    auto remote_address = shared_connection->GetAddress().ToString();
    shared_connection->GetAclQueueEnd()->RegisterDequeue(
        facade_handler_, common::Bind(&AclManagerFacadeService::on_incoming_classic_acl, common::Unretained(this),
                                      shared_connection, handle));
    auto callbacks = acl_connections_.find(handle)->second.GetCallbacks();
    shared_connection->RegisterCallbacks(callbacks, facade_handler_);
    std::unique_ptr<BasePacketBuilder> builder =
        ConnectionCompleteBuilder::Create(ErrorCode::SUCCESS, handle, addr, LinkType::ACL, Enable::DISABLED);
    ConnectionEvent success;
    success.set_event(builder_to_string(std::move(builder)));
    per_connection_events_[current_connection_request_]->OnIncomingEvent(success);
    current_connection_request_++;
  }

  void OnConnectFail(Address address, ErrorCode reason) override {
    std::unique_ptr<BasePacketBuilder> builder =
        ConnectionCompleteBuilder::Create(reason, 0, address, LinkType::ACL, Enable::DISABLED);
    ConnectionEvent fail;
    fail.set_event(builder_to_string(std::move(builder)));
    per_connection_events_[current_connection_request_]->OnIncomingEvent(fail);
    current_connection_request_++;
  }

  class Connection : public ConnectionManagementCallbacks {
   public:
    Connection(uint16_t handle, std::shared_ptr<ClassicAclConnection> connection)
        : handle_(handle), connection_(std::move(connection)) {}

    ConnectionManagementCallbacks* GetCallbacks() {
      return this;
    }

    void OnMasterLinkKeyComplete(KeyFlag key_flag) override {
      LOG_DEBUG("key_flag:%s", KeyFlagText(key_flag).c_str());
    }

    void OnRoleChange(Role new_role) override {
      LOG_DEBUG("new_role:%d", (uint8_t)new_role);
    }

    void OnReadLinkPolicySettingsComplete(uint16_t link_policy_settings) override {
      LOG_DEBUG("link_policy_settings:%d", link_policy_settings);
    }

    void OnConnectionPacketTypeChanged(uint16_t packet_type) override {
      LOG_DEBUG("OnConnectionPacketTypeChanged packet_type:%d", packet_type);
    }

    void OnAuthenticationComplete() override {
      LOG_DEBUG("OnAuthenticationComplete");
    }

    void OnEncryptionChange(EncryptionEnabled enabled) override {
      LOG_DEBUG("OnConnectionPacketTypeChanged enabled:%d", (uint8_t)enabled);
    }

    void OnChangeConnectionLinkKeyComplete() override {
      LOG_DEBUG("OnChangeConnectionLinkKeyComplete");
    };

    void OnReadClockOffsetComplete(uint16_t clock_offset) override {
      LOG_DEBUG("OnReadClockOffsetComplete clock_offset:%d", clock_offset);
    };

    void OnModeChange(Mode current_mode, uint16_t interval) override {
      LOG_DEBUG("OnModeChange Mode:%d, interval:%d", (uint8_t)current_mode, interval);
    };

    void OnQosSetupComplete(ServiceType service_type, uint32_t token_rate, uint32_t peak_bandwidth, uint32_t latency,
                            uint32_t delay_variation) override {
      LOG_DEBUG("OnQosSetupComplete service_type:%d, token_rate:%d, peak_bandwidth:%d, latency:%d, delay_variation:%d",
                (uint8_t)service_type, token_rate, peak_bandwidth, latency, delay_variation);
    }

    void OnFlowSpecificationComplete(FlowDirection flow_direction, ServiceType service_type, uint32_t token_rate,
                                     uint32_t token_bucket_size, uint32_t peak_bandwidth,
                                     uint32_t access_latency) override {
      LOG_DEBUG(
          "OnFlowSpecificationComplete flow_direction:%d. service_type:%d, token_rate:%d, token_bucket_size:%d, "
          "peak_bandwidth:%d, access_latency:%d",
          (uint8_t)flow_direction, (uint8_t)service_type, token_rate, token_bucket_size, peak_bandwidth,
          access_latency);
    }

    void OnFlushOccurred() override {
      LOG_DEBUG("OnFlushOccurred");
    }

    void OnRoleDiscoveryComplete(Role current_role) override {
      LOG_DEBUG("OnRoleDiscoveryComplete current_role:%d", (uint8_t)current_role);
    }

    void OnReadAutomaticFlushTimeoutComplete(uint16_t flush_timeout) override {
      LOG_DEBUG("OnReadAutomaticFlushTimeoutComplete flush_timeout:%d", flush_timeout);
    }

    void OnReadTransmitPowerLevelComplete(uint8_t transmit_power_level) override {
      LOG_DEBUG("OnReadTransmitPowerLevelComplete transmit_power_level:%d", transmit_power_level);
    }

    void OnReadLinkSupervisionTimeoutComplete(uint16_t link_supervision_timeout) override {
      LOG_DEBUG("OnReadLinkSupervisionTimeoutComplete link_supervision_timeout:%d", link_supervision_timeout);
    }

    void OnReadFailedContactCounterComplete(uint16_t failed_contact_counter) override {
      LOG_DEBUG("OnReadFailedContactCounterComplete failed_contact_counter:%d", failed_contact_counter);
    }

    void OnReadLinkQualityComplete(uint8_t link_quality) override {
      LOG_DEBUG("OnReadLinkQualityComplete link_quality:%d", link_quality);
    }

    void OnReadAfhChannelMapComplete(AfhMode afh_mode, std::array<uint8_t, 10> afh_channel_map) override {
      LOG_DEBUG("OnReadAfhChannelMapComplete afh_mode:%d", (uint8_t)afh_mode);
    }

    void OnReadRssiComplete(uint8_t rssi) override {
      LOG_DEBUG("OnReadRssiComplete rssi:%d", rssi);
    }

    void OnReadClockComplete(uint32_t clock, uint16_t accuracy) override {
      LOG_DEBUG("OnReadClockComplete clock:%d, accuracy:%d", clock, accuracy);
    }

    void OnDisconnection(ErrorCode reason) override {
      LOG_DEBUG("OnDisconnection reason: %s", ErrorCodeText(reason).c_str());
    }
    uint16_t handle_;
    std::shared_ptr<ClassicAclConnection> connection_;
  };

 private:
  AclManager* acl_manager_;
  ::bluetooth::os::Handler* facade_handler_;
  mutable std::mutex acl_connections_mutex_;
  std::map<uint16_t, Connection> acl_connections_;
  ::bluetooth::grpc::GrpcEventQueue<AclData> pending_acl_data_{"FetchAclData"};
  std::vector<std::unique_ptr<::bluetooth::grpc::GrpcEventQueue<ConnectionEvent>>> per_connection_events_;
  uint32_t current_connection_request_{0};
};

void AclManagerFacadeModule::ListDependencies(ModuleList* list) {
  ::bluetooth::grpc::GrpcFacadeModule::ListDependencies(list);
  list->add<AclManager>();
}

void AclManagerFacadeModule::Start() {
  ::bluetooth::grpc::GrpcFacadeModule::Start();
  service_ = new AclManagerFacadeService(GetDependency<AclManager>(), GetHandler());
}

void AclManagerFacadeModule::Stop() {
  delete service_;
  ::bluetooth::grpc::GrpcFacadeModule::Stop();
}

::grpc::Service* AclManagerFacadeModule::GetService() const {
  return service_;
}

const ModuleFactory AclManagerFacadeModule::Factory =
    ::bluetooth::ModuleFactory([]() { return new AclManagerFacadeModule(); });

}  // namespace facade
}  // namespace hci
}  // namespace bluetooth
