/************************************************************************************
 *
 * D++, A Lightweight C++ library for Discord
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2021 Craig Edwards and D++ contributors
 * (https://github.com/brainboxdotcc/DPP/graphs/contributors)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This folder is a modified fork of libdave, https://github.com/discord/libdave
 * Copyright (c) 2024 Discord, Licensed under MIT
 *
 ************************************************************************************/
#pragma once

#include <mls/core_types.h>
#include <mls/crypto.h>
#include <mls/messages.h>

#include "version.h"

namespace dpp::dave::mls {

::mlspp::CipherSuite::ID CiphersuiteIDForProtocolVersion(ProtocolVersion version) noexcept;
::mlspp::CipherSuite CiphersuiteForProtocolVersion(ProtocolVersion version) noexcept;
::mlspp::CipherSuite::ID CiphersuiteIDForSignatureVersion(SignatureVersion version) noexcept;
::mlspp::CipherSuite CiphersuiteForSignatureVersion(SignatureVersion version) noexcept;
::mlspp::Capabilities LeafNodeCapabilitiesForProtocolVersion(ProtocolVersion version) noexcept;
::mlspp::ExtensionList LeafNodeExtensionsForProtocolVersion(ProtocolVersion version) noexcept;
::mlspp::ExtensionList GroupExtensionsForProtocolVersion(
  ProtocolVersion version,
  const ::mlspp::ExternalSender& externalSender) noexcept;

} // namespace dpp::dave::mls

