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

#ifdef _WIN32
	#include <WinSock2.h>
	#include <WS2tcpip.h>
	#include <io.h>
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
#endif
#include <string_view>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <dpp/exception.h>
#include <dpp/isa_detection.h>
#include <dpp/discordvoiceclient.h>
#include <dpp/json.h>

#ifdef HAVE_VOICE
	#include "voice/enabled/enabled.h"
#else
	#include "voice/stub/stub.h"
#endif

namespace dpp {

moving_averager::moving_averager(uint64_t collection_count_new) {
	collectionCount = collection_count_new;
}

moving_averager moving_averager::operator+=(int64_t value) {
	values.emplace_front(value);
	if (values.size() >= collectionCount) {
		values.pop_back();
	}
	return *this;
}

moving_averager::operator float() {
	float returnData{};
	if (values.size() > 0) {
		for (auto& value : values) {
			returnData += static_cast<float>(value);
		}
		return returnData / static_cast<float>(values.size());
	}
	else {
		return 0.0f;
	}
}

bool discord_voice_client::sodium_initialised = false;
std::string discord_voice_client::external_ip;

discord_voice_client::~discord_voice_client()
{
	cleanup();
}

bool discord_voice_client::is_ready() {
	return has_secret_key;
}

bool discord_voice_client::is_playing() {
	std::lock_guard<std::mutex> lock(this->stream_mutex);
	return (!this->outbuf.empty());
}

uint16_t dave_binary_header_t::get_welcome_transition_id() const {
	uint16_t transition{0};
	std::memcpy(&transition, package, sizeof(uint16_t));
	return ntohs(transition);
}

std::vector<uint8_t> dave_binary_header_t::get_data(size_t length) const {
	return std::vector<uint8_t>(package, package + length - sizeof(dave_binary_header_t));
}

std::vector<uint8_t> dave_binary_header_t::get_welcome_data(size_t length) const {
	return std::vector<uint8_t>(package + sizeof(uint16_t), package + length - sizeof(dave_binary_header_t));
}

std::string discord_voice_client::get_privacy_code() const {
#ifdef HAVE_VOICE
	return is_end_to_end_encrypted() ? mls_state->privacy_code : "";
#else
	return "";
#endif
}

void discord_voice_client::get_user_privacy_code(const dpp::snowflake user, privacy_code_callback_t callback) const {
#ifdef HAVE_VOICE
	if (!is_end_to_end_encrypted()) {
		callback("");
		return;
	}
	mls_state->dave_session->GetPairwiseFingerprint(0x0000, user.str(), [callback](const std::vector<uint8_t>& data) {
		std::cout << dpp::utility::debug_dump((uint8_t*)data.data(), data.size());
		callback(data.size() == 64 ? generate_displayable_code(data, 45) : "");
	});
#else
	callback("");
#endif
}

bool discord_voice_client::is_end_to_end_encrypted() const {
#ifdef HAVE_VOICE
	return mls_state && !mls_state->privacy_code.empty();
#else
	return false;
#endif
}

discord_voice_client& discord_voice_client::pause_audio(bool pause) {
	this->paused = pause;
	return *this;
}

bool discord_voice_client::is_paused() {
	return this->paused;
}

float discord_voice_client::get_secs_remaining() {
	std::lock_guard<std::mutex> lock(this->stream_mutex);
	float ret = 0;

	for (const auto& packet : outbuf) {
		ret += packet.duration * (timescale / 1000000000.0f);
	}

	return ret;
}

dpp::utility::uptime discord_voice_client::get_remaining() {
	float fp_secs = get_secs_remaining();
	return dpp::utility::uptime((time_t)ceil(fp_secs));
}

discord_voice_client& discord_voice_client::stop_audio() {
	std::lock_guard<std::mutex> lock(this->stream_mutex);
	outbuf.clear();
	track_meta.clear();
	tracks = 0;
	return *this;
}

dpp::utility::uptime discord_voice_client::get_uptime()
{
	return dpp::utility::uptime(time(nullptr) - connect_time);
}

bool discord_voice_client::is_connected()
{
	return (this->get_state() == CONNECTED);
}

void discord_voice_client::error(uint32_t errorcode)
{
	const static std::map<uint32_t, std::string> errortext = {
		{ 1000, "Socket shutdown" },
		{ 1001, "Client is leaving" },
		{ 1002, "Endpoint received a malformed frame" },
		{ 1003, "Endpoint received an unsupported frame" },
		{ 1004, "Reserved code" },
		{ 1005, "Expected close status, received none" },
		{ 1006, "No close code frame has been received" },
		{ 1007, "Endpoint received inconsistent message (e.g. malformed UTF-8)" },
		{ 1008, "Generic error" },
		{ 1009, "Endpoint won't process large frame" },
		{ 1010, "Client wanted an extension which server did not negotiate" },
		{ 1011, "Internal server error while operating" },
		{ 1012, "Server/service is restarting" },
		{ 1013, "Temporary server condition forced blocking client's request" },
		{ 1014, "Server acting as gateway received an invalid response" },
		{ 1015, "Transport Layer Security handshake failure" },
		{ 4001, "Unknown opcode" },
		{ 4002, "Failed to decode payload" },
		{ 4003, "Not authenticated" },
		{ 4004, "Authentication failed" },
		{ 4005, "Already authenticated" },
		{ 4006, "Session no longer valid" },
		{ 4009, "Session timeout" },
		{ 4011, "Server not found" },
		{ 4012, "Unknown protocol" },
		{ 4014, "Disconnected" },
		{ 4015, "Voice server crashed" },
		{ 4016, "Unknown encryption mode" }
	};
	std::string error = "Unknown error";
	auto i = errortext.find(errorcode);
	if (i != errortext.end()) {
		error = i->second;
	}
	log(dpp::ll_warning, "Voice session error: " + std::to_string(errorcode) + " on channel " + std::to_string(channel_id) + ": " + error);

	/* Errors 4004...4016 except 4014 are fatal and cause termination of the voice session */
	if (errorcode >= 4003) {
		stop_audio();
		this->terminating = true;
		log(dpp::ll_error, "This is a non-recoverable error, giving up on voice connection");
	}
}

void discord_voice_client::set_user_gain(snowflake user_id, float factor)
{
#ifdef HAVE_VOICE
	int16_t gain;

	if (factor < 0.0f) {
		/* Invalid factor; must be nonnegative. */
		return;
	} else if (factor == 0.0f) {
		/*
		 * Client probably wants to mute the user,
		 * but log10(0) is undefined, so let's
		 * hardcode the gain to the Opus minimum
		 * for clients.
		 */
		gain = -32768;
	} else {
		/*
		 * OPUS_SET_GAIN takes a value (x) in Q8 dB units.
		 * factor = 10^(x / (20 * 256))
		 * x = log_10(factor) * 20 * 256
		 */
		gain = static_cast<int16_t>(std::log10(factor) * 20.0f * 256.0f);
	}

	std::lock_guard lk(voice_courier_shared_state.mtx);

	voice_courier_shared_state
		/*
		 * Use of the modifying operator[] is intentional;
		 * this is so that we can set ctls on the decoder
		 * even before the user speaks. The decoder doesn't
		 * even have to be ready now, and the setting doesn't
		 * actually take place until we receive some voice
		 * from that speaker.
		 */
		.parked_voice_payloads[user_id]
		.pending_decoder_ctls.push_back([gain](OpusDecoder& decoder) {
			opus_decoder_ctl(&decoder, OPUS_SET_GAIN(gain));
		});
#endif
}

void discord_voice_client::log(dpp::loglevel severity, const std::string &msg) const
{
	creator->log(severity, msg);
}

void discord_voice_client::queue_message(const std::string &j, bool to_front)
{
	std::unique_lock locker(queue_mutex);
	if (to_front) {
		message_queue.emplace_front(j);
	} else {
		message_queue.emplace_back(j);
	}
}

void discord_voice_client::clear_queue()
{
	std::unique_lock locker(queue_mutex);
	message_queue.clear();
}

size_t discord_voice_client::get_queue_size()
{
	std::shared_lock locker(queue_mutex);
	return message_queue.size();
}

const std::vector<std::string> discord_voice_client::get_marker_metadata() {
	std::shared_lock locker(queue_mutex);
	return track_meta;
}

void discord_voice_client::one_second_timer()
{
	if (terminating) {
		throw dpp::connection_exception(err_voice_terminating, "Terminating voice connection");
	}
	/* Rate limit outbound messages, 1 every odd second, 2 every even second */
	if (this->get_state() == CONNECTED) {
		for (int x = 0; x < (time(nullptr) % 2) + 1; ++x) {
			std::unique_lock locker(queue_mutex);
			if (!message_queue.empty()) {
				std::string message = message_queue.front();
				message_queue.pop_front();
				this->write(message, OP_TEXT);
			}
		}

		if (this->heartbeat_interval) {
			/* Check if we're due to emit a heartbeat */
			if (time(nullptr) > last_heartbeat + ((heartbeat_interval / 1000.0) * 0.75)) {
				queue_message(json({
					{"op", voice_opcode_connection_heartbeat},
					{
						"d", {
							{"t", rand()},
							{"seq_ack", receive_sequence},
						}
					},
				}).dump(-1, ' ', false, json::error_handler_t::replace), true);
				last_heartbeat = time(nullptr);
			}
		}
	}
}

discord_voice_client& discord_voice_client::insert_marker(const std::string& metadata) {
	/* Insert a track marker. A track marker is a single 16 bit value of 0xFFFF.
	 * This is too small to be a valid RTP packet so the send function knows not
	 * to actually send it, and instead to skip it
	 */
	uint16_t tm = AUDIO_TRACK_MARKER;
	this->send((const char*)&tm, sizeof(uint16_t), 0);
	{
		std::lock_guard<std::mutex> lock(this->stream_mutex);
		track_meta.push_back(metadata);
		tracks++;
	}
	return *this;
}

uint32_t discord_voice_client::get_tracks_remaining() {
	std::lock_guard<std::mutex> lock(this->stream_mutex);
	if (outbuf.empty()) {
		return 0;
	} else {
		return tracks + 1;
	}
}

discord_voice_client& discord_voice_client::skip_to_next_marker() {
	std::lock_guard<std::mutex> lock(this->stream_mutex);
	if (!outbuf.empty()) {
		/* Find the first marker to skip to */
		auto i = std::find_if(outbuf.begin(), outbuf.end(), [](const voice_out_packet &v){
			return v.packet.size() == sizeof(uint16_t) && (*((uint16_t*)(v.packet.data()))) == AUDIO_TRACK_MARKER;
		});

		if (i != outbuf.end()) {
			/* Skip queued packets until including found marker */
			outbuf.erase(outbuf.begin(), i+1);
		} else {
			/* No market found, skip the whole queue */
			outbuf.clear();
		}
	}

	if (tracks > 0) {
		tracks--;
	}

	if (!track_meta.empty()) {
		track_meta.erase(track_meta.begin());
	}

	return *this;
}

discord_voice_client& discord_voice_client::send_silence(const uint64_t duration) {
	uint8_t silence_packet[3] = { 0xf8, 0xff, 0xfe };
	send_audio_opus(silence_packet, 3, duration);
	return *this;
}

discord_voice_client& discord_voice_client::set_send_audio_type(send_audio_type_t type)
{
	std::lock_guard<std::mutex> lock(this->stream_mutex);
	send_audio_type = type;
	return *this;
}

discord_voice_client& discord_voice_client::speak() {
	if (!this->sending) {
		this->queue_message(json({
		{"op", voice_opcode_client_speaking},
		{"d", {
			{"speaking", 1},
			{"delay", 0},
			{"ssrc", ssrc}
		}}
		}).dump(-1, ' ', false, json::error_handler_t::replace), true);
		sending = true;
	}
	return *this;
}

discord_voice_client& discord_voice_client::set_timescale(uint64_t new_timescale) {
	timescale = new_timescale;
	return *this;
}

uint64_t discord_voice_client::get_timescale() {
	return timescale;
}

std::string discord_voice_client::discover_ip() {
	dpp::socket newfd = SOCKET_ERROR;
	unsigned char packet[74] = { 0 };
	(*(uint16_t*)(packet)) = htons(0x01);
	(*(uint16_t*)(packet + 2)) = htons(70);
	(*(uint32_t*)(packet + 4)) = htonl((uint32_t)this->ssrc);
	if ((newfd = ::socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
		sockaddr_in servaddr{};
		memset(&servaddr, 0, sizeof(sockaddr_in));
		servaddr.sin_family = AF_INET;
		servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
		servaddr.sin_port = htons(0);
		if (bind(newfd, (sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
			log(ll_warning, "Could not bind socket for IP discovery");
			return "";
		}
		memset(&servaddr, 0, sizeof(servaddr));
		servaddr.sin_family = AF_INET;
		servaddr.sin_port = htons(this->port);
		servaddr.sin_addr.s_addr = inet_addr(this->ip.c_str());
		if (::connect(newfd, (const sockaddr*)&servaddr, sizeof(sockaddr_in)) < 0) {
			log(ll_warning, "Could not connect socket for IP discovery");
			return "";
		}
		if (::send(newfd, (const char*)packet, 74, 0) == -1) {
			log(ll_warning, "Could not send packet for IP discovery");
			return "";
		}
		if (recv(newfd, (char*)packet, 74, 0) == -1) {
			log(ll_warning, "Could not receive packet for IP discovery");
			return "";
		}

		close_socket(newfd);

		//utility::debug_dump(packet, 74);
		return std::string((const char*)(packet + 8));
	}
	return "";
}

discord_voice_client& discord_voice_client::set_iteration_interval(uint16_t interval) {
	this->iteration_interval = interval;
	return *this;
}

uint16_t discord_voice_client::get_iteration_interval() {
	return this->iteration_interval;
}

} // namespace dpp
