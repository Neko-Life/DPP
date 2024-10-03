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
 ************************************************************************************/

#include <fstream>
#include <dpp/exception.h>
#include <dpp/isa_detection.h>
#include <dpp/discordvoiceclient.h>

#include "stub.h"

namespace dpp {

	void discord_voice_client::voice_courier_loop(discord_voice_client& client, courier_shared_state_t& shared_state) {
	}

	void discord_voice_client::cleanup(){
	}

	void discord_voice_client::run() {
	}

	void discord_voice_client::thread_run() {
	}

	bool discord_voice_client::voice_payload::operator<(const voice_payload& other) const {
		return false;
	}

	bool discord_voice_client::handle_frame(const std::string &data, ws_opcode opcode) {
		return false;
	}

	void discord_voice_client::read_ready() {
	}

	void discord_voice_client::write_ready() {
	}

	discord_voice_client& discord_voice_client::send_audio_raw(uint16_t* audio_data, const size_t length)  {
		return *this;
	}

	discord_voice_client& discord_voice_client::send_audio_opus(uint8_t* opus_packet, const size_t length) {
		return *this;
	}

	discord_voice_client& discord_voice_client::send_audio_opus(uint8_t* opus_packet, const size_t length, uint64_t duration) {
		return *this;
	}

}