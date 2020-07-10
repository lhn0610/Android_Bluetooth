/*
 * Copyright 2020 The Android Open Source Project
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

#pragma once

#include "common/bind.h"
#include "crypto_toolbox/crypto_toolbox.h"
#include "hci/acl_manager/assembler.h"
#include "hci/acl_manager/disconnector_for_le.h"
#include "hci/acl_manager/round_robin_scheduler.h"
#include "os/alarm.h"
#include "os/rand.h"

using bluetooth::crypto_toolbox::Octet16;

namespace bluetooth {
namespace hci {
namespace acl_manager {

using common::BindOnce;

struct le_acl_connection {
  le_acl_connection(AddressWithType address_with_type, AclConnection::QueueDownEnd* queue_down_end,
                    os::Handler* handler)
      : assembler_(address_with_type, queue_down_end, handler) {}
  ~le_acl_connection() = default;
  struct acl_manager::assembler assembler_;
  LeConnectionManagementCallbacks* le_connection_management_callbacks_ = nullptr;
};

struct le_impl {
  le_impl(HciLayer* hci_layer, Controller* controller, os::Handler* handler, RoundRobinScheduler* round_robin_scheduler,
          DisconnectorForLe* disconnector)
      : hci_layer_(hci_layer), controller_(controller), round_robin_scheduler_(round_robin_scheduler),
        disconnector_(disconnector) {
    hci_layer_ = hci_layer;
    controller_ = controller;
    handler_ = handler;
    le_acl_connection_interface_ = hci_layer_->GetLeAclConnectionInterface(
        handler_->BindOn(this, &le_impl::on_le_event), handler_->BindOn(this, &le_impl::on_le_disconnect));
    le_initiator_address_ =
        AddressWithType(Address{{0x00, 0x11, 0xFF, 0xFF, 0x33, 0x22}}, AddressType::RANDOM_DEVICE_ADDRESS);

    if (le_initiator_address_.GetAddressType() == AddressType::RANDOM_DEVICE_ADDRESS) {
      address_rotation_alarm_ = std::make_unique<os::Alarm>(handler_);
      RotateRandomAddress();
    }
  }

  ~le_impl() {
    for (auto subevent_code : LeConnectionManagementEvents) {
      hci_layer_->UnregisterLeEventHandler(subevent_code);
    }
    // Address might have been already canceled if public address was used
    if (address_rotation_alarm_) {
      address_rotation_alarm_->Cancel();
      address_rotation_alarm_.reset();
    }
    le_acl_connections_.clear();
  }

  void on_le_event(LeMetaEventView event_packet) {
    SubeventCode code = event_packet.GetSubeventCode();
    switch (code) {
      case SubeventCode::CONNECTION_COMPLETE:
        on_le_connection_complete(event_packet);
        break;
      case SubeventCode::ENHANCED_CONNECTION_COMPLETE:
        on_le_enhanced_connection_complete(event_packet);
        break;
      case SubeventCode::CONNECTION_UPDATE_COMPLETE:
        on_le_connection_update_complete(event_packet);
        break;
      default:
        LOG_ALWAYS_FATAL("Unhandled event code %s", SubeventCodeText(code).c_str());
    }
  }

  void on_le_disconnect(uint16_t handle, ErrorCode reason) {
    if (le_acl_connections_.count(handle) == 1) {
      auto& connection = le_acl_connections_.find(handle)->second;
      round_robin_scheduler_->Unregister(handle);
      connection.le_connection_management_callbacks_->OnDisconnection(reason);
      le_acl_connections_.erase(handle);
    }
  }

  void on_common_le_connection_complete(AddressWithType address_with_type) {
    auto connecting_addr_with_type = connecting_le_.find(address_with_type);
    if (connecting_addr_with_type == connecting_le_.end()) {
      LOG_WARN("No prior connection request for %s", address_with_type.ToString().c_str());
    } else {
      connecting_le_.erase(connecting_addr_with_type);
    }
  }

  void on_le_connection_complete(LeMetaEventView packet) {
    LeConnectionCompleteView connection_complete = LeConnectionCompleteView::Create(packet);
    ASSERT(connection_complete.IsValid());
    auto status = connection_complete.GetStatus();
    auto address = connection_complete.GetPeerAddress();
    auto peer_address_type = connection_complete.GetPeerAddressType();
    // TODO: find out which address and type was used to initiate the connection
    AddressWithType remote_address(address, peer_address_type);
    AddressWithType local_address = le_initiator_address_;
    on_common_le_connection_complete(remote_address);
    if (status != ErrorCode::SUCCESS) {
      le_client_handler_->Post(common::BindOnce(&LeConnectionCallbacks::OnLeConnectFail,
                                                common::Unretained(le_client_callbacks_), remote_address, status));
      return;
    }
    // TODO: Check and save other connection parameters
    auto role = connection_complete.GetRole();
    uint16_t handle = connection_complete.GetConnectionHandle();
    ASSERT(le_acl_connections_.count(handle) == 0);
    auto queue = std::make_shared<AclConnection::Queue>(10);
    le_acl_connections_.emplace(std::piecewise_construct, std::forward_as_tuple(handle),
                                std::forward_as_tuple(remote_address, queue->GetDownEnd(), handler_));
    auto& connection_proxy = check_and_get_le_connection(handle);
    auto do_disconnect =
        common::BindOnce(&DisconnectorForLe::handle_disconnect, common::Unretained(disconnector_), handle);
    round_robin_scheduler_->Register(RoundRobinScheduler::ConnectionType::LE, handle, queue);
    std::unique_ptr<LeAclConnection> connection(new LeAclConnection(std::move(queue), le_acl_connection_interface_,
                                                                    std::move(do_disconnect), handle, local_address,
                                                                    remote_address, role));
    connection_proxy.le_connection_management_callbacks_ = connection->GetEventCallbacks();
    le_client_handler_->Post(common::BindOnce(&LeConnectionCallbacks::OnLeConnectSuccess,
                                              common::Unretained(le_client_callbacks_), remote_address,
                                              std::move(connection)));
  }

  void on_le_enhanced_connection_complete(LeMetaEventView packet) {
    LeEnhancedConnectionCompleteView connection_complete = LeEnhancedConnectionCompleteView::Create(packet);
    ASSERT(connection_complete.IsValid());
    auto status = connection_complete.GetStatus();
    auto address = connection_complete.GetPeerAddress();
    auto peer_address_type = connection_complete.GetPeerAddressType();
    auto peer_resolvable_address = connection_complete.GetPeerResolvablePrivateAddress();
    AddressWithType remote_address(address, peer_address_type);
    AddressWithType local_address = le_initiator_address_;
    if (!peer_resolvable_address.IsEmpty()) {
      remote_address = AddressWithType(peer_resolvable_address, AddressType::RANDOM_DEVICE_ADDRESS);
    }
    on_common_le_connection_complete(remote_address);
    if (status != ErrorCode::SUCCESS) {
      le_client_handler_->Post(common::BindOnce(&LeConnectionCallbacks::OnLeConnectFail,
                                                common::Unretained(le_client_callbacks_), remote_address, status));
      return;
    }
    // TODO: Check and save other connection parameters
    uint16_t handle = connection_complete.GetConnectionHandle();
    ASSERT(le_acl_connections_.count(handle) == 0);
    auto queue = std::make_shared<AclConnection::Queue>(10);
    le_acl_connections_.emplace(std::piecewise_construct, std::forward_as_tuple(handle),
                                std::forward_as_tuple(remote_address, queue->GetDownEnd(), handler_));
    auto& connection_proxy = check_and_get_le_connection(handle);
    round_robin_scheduler_->Register(RoundRobinScheduler::ConnectionType::LE, handle, queue);
    auto role = connection_complete.GetRole();
    auto do_disconnect =
        common::BindOnce(&DisconnectorForLe::handle_disconnect, common::Unretained(disconnector_), handle);
    std::unique_ptr<LeAclConnection> connection(new LeAclConnection(std::move(queue), le_acl_connection_interface_,
                                                                    std::move(do_disconnect), handle, local_address,
                                                                    remote_address, role));
    connection_proxy.le_connection_management_callbacks_ = connection->GetEventCallbacks();
    le_client_handler_->Post(common::BindOnce(&LeConnectionCallbacks::OnLeConnectSuccess,
                                              common::Unretained(le_client_callbacks_), remote_address,
                                              std::move(connection)));
  }

  void on_le_connection_update_complete(LeMetaEventView view) {
    auto complete_view = LeConnectionUpdateCompleteView::Create(view);
    if (!complete_view.IsValid()) {
      LOG_ERROR("Received on_le_connection_update_complete with invalid packet");
      return;
    } else if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      auto status = complete_view.GetStatus();
      std::string error_code = ErrorCodeText(status);
      LOG_ERROR("Received on_le_connection_update_complete with error code %s", error_code.c_str());
      return;
    }
    auto handle = complete_view.GetConnectionHandle();
    if (le_acl_connections_.find(handle) == le_acl_connections_.end()) {
      LOG_WARN("Can't find connection %hd", handle);
      return;
    }
    auto& connection = le_acl_connections_.find(handle)->second;
    connection.le_connection_management_callbacks_->OnConnectionUpdate(
        complete_view.GetConnInterval(), complete_view.GetConnLatency(), complete_view.GetSupervisionTimeout());
  }

  std::chrono::milliseconds GetNextPrivateAddrressIntervalMs() {
    /* 7 minutes minimum, 15 minutes maximum for random address refreshing */
    const uint64_t interval_min_ms = (7 * 60 * 1000);
    const uint64_t interval_random_part_max_ms = (8 * 60 * 1000);

    return std::chrono::milliseconds(interval_min_ms + os::GenerateRandom() % interval_random_part_max_ms);
  }

  /* This function generates Resolvable Private Address (RPA) from Identity
   * Resolving Key |irk| and |prand|*/
  hci::Address GenerateRpa(const Octet16& irk, std::array<uint8_t, 8> prand) {
    /* most significant bit, bit7, bit6 is 01 to be resolvable random */
    constexpr uint8_t BLE_RESOLVE_ADDR_MSB = 0x40;
    constexpr uint8_t BLE_RESOLVE_ADDR_MASK = 0xc0;
    prand[2] &= (~BLE_RESOLVE_ADDR_MASK);
    prand[2] |= BLE_RESOLVE_ADDR_MSB;

    hci::Address address;
    address.address[3] = prand[0];
    address.address[4] = prand[1];
    address.address[5] = prand[2];

    /* encrypt with IRK */
    Octet16 p = crypto_toolbox::aes_128(irk, prand.data(), 3);

    /* set hash to be LSB of rpAddress */
    address.address[0] = p[0];
    address.address[1] = p[1];
    address.address[2] = p[2];
    return address;
  }

  void RotateRandomAddress() {
    // TODO: we must stop advertising, conection initiation, and scanning before calling SetRandomAddress.
    // TODO: ensure this is called before first connection initiation.
    // TODO: obtain proper IRK
    Octet16 irk = {} /* TODO: = BTM_GetDeviceIDRoot() */;
    std::array<uint8_t, 8> random = os::GenerateRandom<8>();
    hci::Address address = GenerateRpa(irk, random);

    hci_layer_->EnqueueCommand(hci::LeSetRandomAddressBuilder::Create(address),
                               handler_->BindOnce(check_command_complete<LeSetRandomAddressCompleteView>));

    le_initiator_address_ = AddressWithType(address, AddressType::RANDOM_DEVICE_ADDRESS);
    address_rotation_alarm_->Schedule(BindOnce(&le_impl::RotateRandomAddress, common::Unretained(this)),
                                      GetNextPrivateAddrressIntervalMs());
  }

  void create_le_connection(AddressWithType address_with_type) {
    // TODO: Add white list handling.
    // TODO: Configure default LE connection parameters?
    uint16_t le_scan_interval = 0x0060;
    uint16_t le_scan_window = 0x0030;
    InitiatorFilterPolicy initiator_filter_policy = InitiatorFilterPolicy::USE_PEER_ADDRESS;
    OwnAddressType own_address_type = static_cast<OwnAddressType>(le_initiator_address_.GetAddressType());
    uint16_t conn_interval_min = 0x0018;
    uint16_t conn_interval_max = 0x0028;
    uint16_t conn_latency = 0x0000;
    uint16_t supervision_timeout = 0x001f4;
    ASSERT(le_client_callbacks_ != nullptr);

    connecting_le_.insert(address_with_type);

    // TODO: make features check nicer, like HCI_LE_EXTENDED_ADVERTISING_SUPPORTED
    if (controller_->GetControllerLeLocalSupportedFeatures() & 0x0010) {
      LeCreateConnPhyScanParameters tmp;
      tmp.scan_interval_ = le_scan_interval;
      tmp.scan_window_ = le_scan_window;
      tmp.conn_interval_min_ = conn_interval_min;
      tmp.conn_interval_max_ = conn_interval_max;
      tmp.conn_latency_ = conn_latency;
      tmp.supervision_timeout_ = supervision_timeout;
      tmp.min_ce_length_ = 0x00;
      tmp.max_ce_length_ = 0x00;

      le_acl_connection_interface_->EnqueueCommand(
          LeExtendedCreateConnectionBuilder::Create(initiator_filter_policy, own_address_type,
                                                    address_with_type.GetAddressType(), address_with_type.GetAddress(),
                                                    0x01 /* 1M PHY ONLY */, {tmp}),
          handler_->BindOnce([](CommandStatusView status) {
            ASSERT(status.IsValid());
            ASSERT(status.GetCommandOpCode() == OpCode::LE_EXTENDED_CREATE_CONNECTION);
          }));
    } else {
      le_acl_connection_interface_->EnqueueCommand(
          LeCreateConnectionBuilder::Create(le_scan_interval, le_scan_window, initiator_filter_policy,
                                            address_with_type.GetAddressType(), address_with_type.GetAddress(),
                                            own_address_type, conn_interval_min, conn_interval_max, conn_latency,
                                            supervision_timeout, kMinimumCeLength, kMaximumCeLength),
          handler_->BindOnce([](CommandStatusView status) {
            ASSERT(status.IsValid());
            ASSERT(status.GetCommandOpCode() == OpCode::LE_CREATE_CONNECTION);
          }));
    }
  }

  void set_le_initiator_address(AddressWithType le_initiator_address) {
    le_initiator_address_ = le_initiator_address;

    if (le_initiator_address_.GetAddressType() != AddressType::RANDOM_DEVICE_ADDRESS) {
      // Usually controllers provide vendor-specific way to override public address. Implement it if it's ever needed.
      LOG_ALWAYS_FATAL("Don't know how to use this type of address");
    }

    if (address_rotation_alarm_) {
      address_rotation_alarm_->Cancel();
      address_rotation_alarm_.reset();
    }

    // TODO: we must stop advertising, conection initiation, and scanning before calling SetRandomAddress.
    hci_layer_->EnqueueCommand(hci::LeSetRandomAddressBuilder::Create(le_initiator_address_.GetAddress()),
                               handler_->BindOnce([](CommandCompleteView status) {}));
  }

  void handle_register_le_callbacks(LeConnectionCallbacks* callbacks, os::Handler* handler) {
    ASSERT(le_client_callbacks_ == nullptr);
    ASSERT(le_client_handler_ == nullptr);
    le_client_callbacks_ = callbacks;
    le_client_handler_ = handler;
  }

  le_acl_connection& check_and_get_le_connection(uint16_t handle) {
    auto connection = le_acl_connections_.find(handle);
    ASSERT(connection != le_acl_connections_.end());
    return connection->second;
  }

  static constexpr uint16_t kMinimumCeLength = 0x0002;
  static constexpr uint16_t kMaximumCeLength = 0x0C00;
  HciLayer* hci_layer_ = nullptr;
  Controller* controller_ = nullptr;
  os::Handler* handler_ = nullptr;
  RoundRobinScheduler* round_robin_scheduler_ = nullptr;
  LeAclConnectionInterface* le_acl_connection_interface_ = nullptr;
  LeConnectionCallbacks* le_client_callbacks_ = nullptr;
  os::Handler* le_client_handler_ = nullptr;
  std::map<uint16_t, le_acl_connection> le_acl_connections_;
  std::set<AddressWithType> connecting_le_;
  AddressWithType le_initiator_address_{Address{}, AddressType::RANDOM_DEVICE_ADDRESS};
  std::unique_ptr<os::Alarm> address_rotation_alarm_;
  DisconnectorForLe* disconnector_;
};

}  // namespace acl_manager
}  // namespace hci
}  // namespace bluetooth
