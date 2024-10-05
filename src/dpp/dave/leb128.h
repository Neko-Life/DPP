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
 *  https://webrtc.googlesource.com/src/+/refs/heads/main/modules/rtp_rtcp/source/leb128.cc
 *  Copyright (c) 2023 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 ************************************************************************************/
#pragma once

#include <cstddef>
#include <cstdint>

namespace dpp::dave {

constexpr size_t Leb128MaxSize = 10;

// Returns number of bytes needed to store `value` in leb128 format.
size_t Leb128Size(uint64_t value);

// Reads leb128 encoded value and advance read_at by number of bytes consumed.
// Sets read_at to nullptr on error.
uint64_t ReadLeb128(const uint8_t*& readAt, const uint8_t* end);

// Encodes `value` in leb128 format. Assumes buffer has size of at least
// Leb128Size(value). Returns number of bytes consumed.
size_t WriteLeb128(uint64_t value, uint8_t* buffer);

} // namespace dpp::dave
