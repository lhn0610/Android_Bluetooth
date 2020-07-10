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

#include "hci/acl_manager.h"

#include <atomic>
#include <set>

#include "common/bidi_queue.h"
#include "hci/acl_manager/classic_impl.h"
#include "hci/acl_manager/connection_management_callbacks.h"
#include "hci/acl_manager/le_acl_connection.h"
#include "hci/acl_manager/le_impl.h"
#include "hci/acl_manager/round_robin_scheduler.h"
#include "hci/controller.h"
#include "hci/hci_layer.h"
#include "security/security_module.h"

namespace bluetooth {
namespace hci {

constexpr uint16_t kQualcommDebugHandle = 0xedc;

using acl_manager::AclConnection;
using common::Bind;
using common::BindOnce;

using acl_manager::classic_impl;
using acl_manager::ClassicAclConnection;
using acl_manager::ConnectionCallbacks;

using acl_manager::le_impl;
using acl_manager::LeAclConnection;
using acl_manager::LeConnectionCallbacks;

using acl_manager::RoundRobinScheduler;

struct AclManager::impl {
  impl(const AclManager& acl_manager) : acl_manager_(acl_manager) {}

  void Start() {
    hci_layer_ = acl_manager_.GetDependency<HciLayer>();
    handler_ = acl_manager_.GetHandler();
    controller_ = acl_manager_.GetDependency<Controller>();
    round_robin_scheduler_ = new RoundRobinScheduler(handler_, controller_, hci_layer_->GetAclQueueEnd());

    hci_queue_end_ = hci_layer_->GetAclQueueEnd();
    hci_queue_end_->RegisterDequeue(
        handler_, common::Bind(&impl::dequeue_and_route_acl_packet_to_connection, common::Unretained(this)));
    classic_impl_ = new classic_impl(hci_layer_, controller_, handler_, round_robin_scheduler_);
    le_impl_ = new le_impl(hci_layer_, controller_, handler_, round_robin_scheduler_, classic_impl_);
  }

  void Stop() {
    delete le_impl_;
    delete classic_impl_;
    hci_queue_end_->UnregisterDequeue();
    delete round_robin_scheduler_;
    if (enqueue_registered_.exchange(false)) {
      hci_queue_end_->UnregisterEnqueue();
    }
    hci_queue_end_ = nullptr;
    handler_ = nullptr;
    hci_layer_ = nullptr;
  }

  // Invoked from some external Queue Reactable context 2
  void dequeue_and_route_acl_packet_to_connection() {
    auto packet = hci_queue_end_->TryDequeue();
    ASSERT(packet != nullptr);
    if (!packet->IsValid()) {
      LOG_INFO("Dropping invalid packet of size %zu", packet->size());
      return;
    }
    uint16_t handle = packet->GetHandle();
    if (handle == kQualcommDebugHandle) {
      return;
    }
    auto connection_pair = classic_impl_->acl_connections_.find(handle);
    if (connection_pair != classic_impl_->acl_connections_.end()) {
      connection_pair->second.assembler_.on_incoming_packet(*packet);
    } else {
      auto le_connection_pair = le_impl_->le_acl_connections_.find(handle);
      if (le_connection_pair == le_impl_->le_acl_connections_.end()) {
        LOG_INFO("Dropping packet of size %zu to unknown connection 0x%0hx", packet->size(), handle);
        return;
      }
      le_connection_pair->second.assembler_.on_incoming_packet(*packet);
    }
  }

  const AclManager& acl_manager_;

  classic_impl* classic_impl_ = nullptr;
  le_impl* le_impl_ = nullptr;
  os::Handler* handler_ = nullptr;
  Controller* controller_ = nullptr;
  HciLayer* hci_layer_ = nullptr;
  RoundRobinScheduler* round_robin_scheduler_ = nullptr;
  common::BidiQueueEnd<AclPacketBuilder, AclPacketView>* hci_queue_end_ = nullptr;
  std::atomic_bool enqueue_registered_ = false;
  uint16_t default_link_policy_settings_ = 0xffff;
};

AclManager::AclManager() : pimpl_(std::make_unique<impl>(*this)) {}

void AclManager::RegisterCallbacks(ConnectionCallbacks* callbacks, os::Handler* handler) {
  ASSERT(callbacks != nullptr && handler != nullptr);
  GetHandler()->Post(common::BindOnce(&classic_impl::handle_register_callbacks,
                                      common::Unretained(pimpl_->classic_impl_), common::Unretained(callbacks),
                                      common::Unretained(handler)));
}

void AclManager::RegisterLeCallbacks(LeConnectionCallbacks* callbacks, os::Handler* handler) {
  ASSERT(callbacks != nullptr && handler != nullptr);
  GetHandler()->Post(common::BindOnce(&le_impl::handle_register_le_callbacks, common::Unretained(pimpl_->le_impl_),
                                      common::Unretained(callbacks), common::Unretained(handler)));
}

void AclManager::CreateConnection(Address address) {
  GetHandler()->Post(
      common::BindOnce(&classic_impl::create_connection, common::Unretained(pimpl_->classic_impl_), address));
}

void AclManager::CreateLeConnection(AddressWithType address_with_type) {
  GetHandler()->Post(
      common::BindOnce(&le_impl::create_le_connection, common::Unretained(pimpl_->le_impl_), address_with_type));
}

void AclManager::SetLeInitiatorAddress(AddressWithType initiator_address) {
  GetHandler()->Post(
      common::BindOnce(&le_impl::set_le_initiator_address, common::Unretained(pimpl_->le_impl_), initiator_address));
}

void AclManager::CancelConnect(Address address) {
  GetHandler()->Post(BindOnce(&classic_impl::cancel_connect, common::Unretained(pimpl_->classic_impl_), address));
}

void AclManager::MasterLinkKey(KeyFlag key_flag) {
  GetHandler()->Post(BindOnce(&classic_impl::master_link_key, common::Unretained(pimpl_->classic_impl_), key_flag));
}

void AclManager::SwitchRole(Address address, Role role) {
  GetHandler()->Post(BindOnce(&classic_impl::switch_role, common::Unretained(pimpl_->classic_impl_), address, role));
}

uint16_t AclManager::ReadDefaultLinkPolicySettings() {
  ASSERT_LOG(pimpl_->default_link_policy_settings_ != 0xffff, "Settings were never written");
  return pimpl_->default_link_policy_settings_;
}

void AclManager::WriteDefaultLinkPolicySettings(uint16_t default_link_policy_settings) {
  pimpl_->default_link_policy_settings_ = default_link_policy_settings;
  GetHandler()->Post(BindOnce(&classic_impl::write_default_link_policy_settings,
                              common::Unretained(pimpl_->classic_impl_), default_link_policy_settings));
}

void AclManager::SetSecurityModule(security::SecurityModule* security_module) {
  GetHandler()->Post(
      BindOnce(&classic_impl::set_security_module, common::Unretained(pimpl_->classic_impl_), security_module));
}

void AclManager::ListDependencies(ModuleList* list) {
  list->add<HciLayer>();
  list->add<Controller>();
}

void AclManager::Start() {
  pimpl_->Start();
}

void AclManager::Stop() {
  pimpl_->Stop();
}

std::string AclManager::ToString() const {
  return "Acl Manager";
}

const ModuleFactory AclManager::Factory = ModuleFactory([]() { return new AclManager(); });

AclManager::~AclManager() = default;

}  // namespace hci
}  // namespace bluetooth
