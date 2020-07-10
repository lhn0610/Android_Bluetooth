/*
 *
 *  Copyright 2020 The Android Open Source Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#pragma once

#include <memory>
#include <vector>

#include "hci/address_with_type.h"
#include "hci/hci_packets.h"
#include "security/internal/security_manager_impl.h"

namespace bluetooth {
namespace security {

/**
 * Manages the security attributes, pairing, bonding of devices, and the
 * encryption/decryption of communications.
 */
class FacadeConfigurationApi {
 public:
  friend class internal::SecurityManagerImpl;
  friend class SecurityModule;

  void SetIoCapability(hci::IoCapability io_capability);
  void SetAuthenticationRequirements(hci::AuthenticationRequirements authentication_requirement);
  void SetOobData(hci::OobDataPresent oob_present);

 protected:
  FacadeConfigurationApi(os::Handler* security_handler, internal::SecurityManagerImpl* security_manager_impl)
      : security_handler_(security_handler), security_manager_impl_(security_manager_impl) {}

 private:
  os::Handler* security_handler_ = nullptr;
  internal::SecurityManagerImpl* security_manager_impl_;
  DISALLOW_COPY_AND_ASSIGN(FacadeConfigurationApi);
};

}  // namespace security
}  // namespace bluetooth
