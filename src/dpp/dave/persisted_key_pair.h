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

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <functional>
#include <string>
#include <memory>
#include <vector>
#include <bytes/bytes.h>
#include <mls/crypto.h>
#include "parameters.h"
#include "version.h"

namespace mlspp {
struct SignaturePrivateKey;
};

namespace dpp::dave::mls {

using KeyPairContextType = const char *;

std::shared_ptr<::mlspp::SignaturePrivateKey> GetPersistedKeyPair(KeyPairContextType ctx,
                                                                  const std::string& sessionID,
                                                                  ProtocolVersion version);

struct KeyAndSelfSignature {
    std::vector<uint8_t> key;
    std::vector<uint8_t> signature;
};

KeyAndSelfSignature GetPersistedPublicKey(KeyPairContextType ctx,
                                          const std::string& sessionID,
                                          SignatureVersion version);

bool DeletePersistedKeyPair(KeyPairContextType ctx,
                            const std::string& sessionID,
                            SignatureVersion version);

constexpr unsigned KeyVersion = 1;

} // namespace dpp::dave::mls



namespace dpp {
	namespace dave {
		namespace mls {
			namespace detail {
				std::shared_ptr<::mlspp::SignaturePrivateKey> GetGenericPersistedKeyPair(KeyPairContextType ctx, const std::string& id, ::mlspp::CipherSuite suite);
				bool DeleteGenericPersistedKeyPair(KeyPairContextType ctx, const std::string& id);
			}
		}
	}
}