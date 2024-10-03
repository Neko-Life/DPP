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
#include <fstream>
#include <algorithm>
#include <cmath>
#include <dpp/exception.h>
#include <dpp/isa_detection.h>
#include <dpp/discordvoiceclient.h>
#include <dpp/json.h>

#ifdef HAVE_VOICE
	#include <sodium.h>
	#include <opus/opus.h>
	#include "dave/session.h"
	#include "dave/decryptor.h"
	#include "dave/encryptor.h"
#else
	struct OpusDecoder {};
	struct OpusEncoder {};
	struct OpusRepacketizer {};
	namespace dpp::dave {
		struct Session {};
		struct Encryptor {};
		struct Decryptor {};
	};
#endif

namespace dpp {

/**
 * @brief Sample rate for OPUS (48khz)
 */
[[maybe_unused]] constexpr int32_t opus_sample_rate_hz = 48000;

/**
 * @brief Channel count for OPUS (stereo)
 */
[[maybe_unused]] constexpr int32_t opus_channel_count = 2;

/**
 * @brief Discord voice protocol version
 */
constexpr uint8_t voice_protocol_version = 8;

/**
 * @brief Our public IP address
 */
static std::string external_ip;

struct dave_state {
	std::unique_ptr<dave::mls::Session> dave_session{};
	std::shared_ptr<::mlspp::SignaturePrivateKey> mls_key;
	std::vector<uint8_t> cached_commit;
	uint64_t transition_id{0};
	std::map<dpp::snowflake, std::unique_ptr<dave::Decryptor>> decryptors;
	std::unique_ptr<dave::Encryptor> encryptor;
	std::string privacy_code;
};

/**
 * @brief Transport encryption type (libsodium)
 */
constexpr std::string_view transport_encryption_protocol = "aead_xchacha20_poly1305_rtpsize";


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

/**
 * @brief Represents an RTP packet. Size should always be exactly 12.
 */
struct rtp_header {
	uint16_t constant;
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc;

	rtp_header(uint16_t _seq, uint32_t _ts, uint32_t _ssrc) : constant(htons(0x8078)), sequence(htons(_seq)), timestamp(htonl(_ts)), ssrc(htonl(_ssrc)) {
	}
};

bool discord_voice_client::sodium_initialised = false;

bool discord_voice_client::voice_payload::operator<(const voice_payload& other) const {
	if (timestamp != other.timestamp) {
		return timestamp > other.timestamp;
	}

	constexpr rtp_seq_t wrap_around_test_boundary = 5000;
	if ((seq < wrap_around_test_boundary && other.seq >= wrap_around_test_boundary)
    	    || (seq >= wrap_around_test_boundary && other.seq < wrap_around_test_boundary)) {
    		/* Match the cases where exactly one of the sequence numbers "may have"
		 * wrapped around.
		 *
		 * Examples:
		 * 1. this->seq = 65530, other.seq = 10  // Did wrap around
		 * 2. this->seq = 5002, other.seq = 4990 // Not wrapped around
		 *
		 * Add 5000 to both sequence numbers to force wrap around so they can be
		 * compared. This should be fine to do to case 2 as well, as long as the
		 * addend (5000) is not too large to cause one of them to wrap around.
		 *
		 * In practice, we should be unlikely to hit the case where
		 *
		 *           this->seq = 65530, other.seq = 5001
		 *
		 * because we shouldn't receive more than 5000 payloads in one batch, unless
		 * the voice courier thread is super slow. Also remember that the timestamp
		 * is compared first, and payloads this far apart shouldn't have the same
		 * timestamp.
		 */

		/* Casts here ensure the sum wraps around and not implicitly converted to
		 * wider types.
		 */
		return   static_cast<rtp_seq_t>(seq + wrap_around_test_boundary)
		       > static_cast<rtp_seq_t>(other.seq + wrap_around_test_boundary);
	} else {
		return seq > other.seq;
	}
}

#ifdef HAVE_VOICE
size_t audio_mix(discord_voice_client& client, audio_mixer& mixer, opus_int32* pcm_mix, const opus_int16* pcm, size_t park_count, int samples, int& max_samples) {
	/* Mix the combined stream if combined audio is bound */
	if (client.creator->on_voice_receive_combined.empty()) {
		return 0;
	}

	/* We must upsample the data to 32 bits wide, otherwise we could overflow */
	for (opus_int32 v = 0; v < (samples * opus_channel_count) / mixer.byte_blocks_per_register; ++v) {
		mixer.combine_samples(pcm_mix, pcm);
		pcm += mixer.byte_blocks_per_register;
		pcm_mix += mixer.byte_blocks_per_register;
	}
	client.moving_average += park_count;
	max_samples = (std::max)(samples, max_samples);
	return park_count + 1;
}

std::string generate_displayable_code(const std::vector<uint8_t>& data, size_t desired_length = 30, size_t group_size = 5) {

	const size_t group_modulus = std::pow(10, group_size);
	std::stringstream result;

	for (size_t i = 0; i < desired_length; i += group_size) {
		size_t group_value{0};

		for (size_t j = group_size; j > 0; --j) {
			const size_t next_byte = data.at(i + (group_size - j));
			group_value = (group_value << 8) | next_byte;
		}
		group_value %= group_modulus;
		result << std::setw(group_size) << std::setfill('0')  << std::to_string(group_value) << " ";
	}

	return result.str();
}
#endif

void discord_voice_client::voice_courier_loop(discord_voice_client& client, courier_shared_state_t& shared_state) {
#ifdef HAVE_VOICE
	utility::set_thread_name(std::string("vcourier/") + std::to_string(client.server_id));
	while (true) {
		std::this_thread::sleep_for(std::chrono::milliseconds{client.iteration_interval});
		
		struct flush_data_t {
			snowflake user_id;
			rtp_seq_t min_seq;
			std::priority_queue<voice_payload> parked_payloads;
			std::vector<std::function<void(OpusDecoder&)>> pending_decoder_ctls;
			std::shared_ptr<OpusDecoder> decoder;
		};
		std::vector<flush_data_t> flush_data;

		/*
		 * Transport the payloads onto this thread, and
		 * release the lock as soon as possible.
		 */
		{
			std::unique_lock lk(shared_state.mtx);

			/* mitigates vector resizing while holding the mutex */
			flush_data.reserve(shared_state.parked_voice_payloads.size());

			bool has_payload_to_deliver = false;
			for (auto& [user_id, parking_lot] : shared_state.parked_voice_payloads) {
				has_payload_to_deliver = has_payload_to_deliver || !parking_lot.parked_payloads.empty();
				flush_data.push_back(flush_data_t{user_id,
				                                  parking_lot.range.min_seq,
				                                  std::move(parking_lot.parked_payloads),
				                                  /* Quickly check if we already have a decoder and only take the pending ctls if so. */
				                                  parking_lot.decoder ? std::move(parking_lot.pending_decoder_ctls)
				                                                      : decltype(parking_lot.pending_decoder_ctls){},
				                                  parking_lot.decoder});
				parking_lot.range.min_seq = parking_lot.range.max_seq + 1;
				parking_lot.range.min_timestamp = parking_lot.range.max_timestamp + 1;
			}
            
			if (!has_payload_to_deliver) {
				if (shared_state.terminating) {
					/* We have delivered all data to handlers. Terminate now. */
					break;
				}

				shared_state.signal_iteration.wait(lk);
				/*
				 * More data came or about to terminate, or just a spurious wake.
				 * We need to collect the payloads again to determine what to do next.
				 */
				continue;
			}
		}

		if (client.creator->on_voice_receive.empty() && client.creator->on_voice_receive_combined.empty()) {
			/*
			 * We do this check late, to ensure this thread drains the data
			 * and prevents accumulating them even when there are no handlers.
			 */
			continue;
		}

		/* This 32 bit PCM audio buffer is an upmixed version of the streams
		 * combined for all users. This is a wider width audio buffer so that
	 	 * there is no clipping when there are many loud audio sources at once.
		 */
		opus_int32 pcm_mix[23040] = { 0 };
		size_t park_count = 0;
		int max_samples = 0;
		int samples = 0;

		for (auto& d : flush_data) {
			if (!d.decoder) {
				continue;
			}
			for (const auto& decoder_ctl : d.pending_decoder_ctls) {
				decoder_ctl(*d.decoder);
			}
			for (rtp_seq_t seq = d.min_seq; !d.parked_payloads.empty(); ++seq) {
				opus_int16 pcm[23040];
				if (d.parked_payloads.top().seq != seq) {
					/*
					 * Lost a packet with sequence number "seq",
					 * But Opus decoder might be able to guess something.
					 */
					if (int samples = opus_decode(d.decoder.get(), nullptr, 0, pcm, 5760, 0);
					    samples >= 0) {
						/*
						 * Since this sample comes from a lost packet,
						 * we can only pretend there is an event, without any raw payload byte.
						 */
						voice_receive_t vr(nullptr, "", &client, d.user_id, reinterpret_cast<uint8_t*>(pcm),
							samples * opus_channel_count * sizeof(opus_int16));

						park_count = audio_mix(client, *client.mixer, pcm_mix, pcm, park_count, samples, max_samples);
						client.creator->on_voice_receive.call(vr);
					}
				} else {
					voice_receive_t& vr = *d.parked_payloads.top().vr;
					if (vr.audio_data.size() > 0x7FFFFFFF) {
						throw dpp::length_exception(err_massive_audio, "audio_data > 2GB! This should never happen!");
					}
					if (samples = opus_decode(d.decoder.get(), vr.audio_data.data(),
						static_cast<opus_int32>(vr.audio_data.size() & 0x7FFFFFFF), pcm, 5760, 0);
					    samples >= 0) {
						vr.reassign(&client, d.user_id, reinterpret_cast<uint8_t*>(pcm),
							samples * opus_channel_count * sizeof(opus_int16));
						client.end_gain = 1.0f / client.moving_average;
						park_count = audio_mix(client, *client.mixer, pcm_mix, pcm, park_count, samples, max_samples);
						client.creator->on_voice_receive.call(vr);
					}

					d.parked_payloads.pop();
				}
			}
		}

		/* If combined receive is bound, dispatch it */
		if (park_count) {
			
			/* Downsample the 32 bit samples back to 16 bit */
			opus_int16 pcm_downsample[23040] = { 0 };
			opus_int16* pcm_downsample_ptr = pcm_downsample;
			opus_int32* pcm_mix_ptr = pcm_mix;
			client.increment = (client.end_gain - client.current_gain) / static_cast<float>(samples);
			for (int64_t x = 0; x < (samples * opus_channel_count) / client.mixer->byte_blocks_per_register; ++x) {
				client.mixer->collect_single_register(pcm_mix_ptr, pcm_downsample_ptr, client.current_gain, client.increment);
				client.current_gain += client.increment * static_cast<float>(client.mixer->byte_blocks_per_register);
				pcm_mix_ptr += client.mixer->byte_blocks_per_register;
				pcm_downsample_ptr += client.mixer->byte_blocks_per_register;
			}

			voice_receive_t vr(nullptr, "", &client, 0, reinterpret_cast<uint8_t*>(pcm_downsample),
				max_samples * opus_channel_count * sizeof(opus_int16));

			client.creator->on_voice_receive_combined.call(vr);
		}
	}
#endif
}

discord_voice_client::discord_voice_client(dpp::cluster* _cluster, snowflake _channel_id, snowflake _server_id, const std::string &_token, const std::string &_session_id, const std::string &_host, bool enable_dave)
	: websocket_client(_host.substr(0, _host.find(':')), _host.substr(_host.find(':') + 1, _host.length()), "/?v=" + std::to_string(voice_protocol_version), OP_TEXT),
	runner(nullptr),
	connect_time(0),
	mixer(std::make_unique<audio_mixer>()),
	port(0),
	ssrc(0),
	timescale(1000000),
	paused(false),
	encoder(nullptr),
	repacketizer(nullptr),
	fd(INVALID_SOCKET),
	sequence(0),
	receive_sequence(-1),
	timestamp(0),
	packet_nonce(1),
	last_timestamp(std::chrono::high_resolution_clock::now()),
	sending(false),
	tracks(0),
	dave_version(enable_dave ? dave_version_1 : dave_version_none),
	creator(_cluster),
	terminating(false),
	heartbeat_interval(0),
	last_heartbeat(time(nullptr)),
	token(_token),
	sessionid(_session_id),
	server_id(_server_id),
	channel_id(_channel_id)
{
#if HAVE_VOICE
	if (!discord_voice_client::sodium_initialised) {
		if (sodium_init() < 0) {
			throw dpp::voice_exception(err_sodium, "discord_voice_client::discord_voice_client; sodium_init() failed");
		}
		discord_voice_client::sodium_initialised = true;
	}
	int opusError = 0;
	encoder = opus_encoder_create(opus_sample_rate_hz, opus_channel_count, OPUS_APPLICATION_VOIP, &opusError);
	if (opusError) {
		throw dpp::voice_exception(err_opus, "discord_voice_client::discord_voice_client; opus_encoder_create() failed");
	}
	repacketizer = opus_repacketizer_create();
	if (!repacketizer) {
		throw dpp::voice_exception(err_opus, "discord_voice_client::discord_voice_client; opus_repacketizer_create() failed");
	}
	try {
		this->connect();
	}
	catch (std::exception&) {
		cleanup();
		throw;
	}
#else
	throw dpp::voice_exception(err_no_voice_support, "Voice support not enabled in this build of D++");
#endif
}

discord_voice_client::~discord_voice_client()
{
	cleanup();
}

void discord_voice_client::cleanup()
{
	if (runner) {
		this->terminating = true;
		runner->join();
		delete runner;
		runner = nullptr;
	}
#if HAVE_VOICE
	if (encoder) {
		opus_encoder_destroy(encoder);
		encoder = nullptr;
	}
	if (repacketizer) {
		opus_repacketizer_destroy(repacketizer);
		repacketizer = nullptr;
	}
	if (voice_courier.joinable()) {
		{
			std::lock_guard lk(voice_courier_shared_state.mtx);
			voice_courier_shared_state.terminating = true;
		}
		voice_courier_shared_state.signal_iteration.notify_one();
		voice_courier.join();
	}
#endif
}

bool discord_voice_client::is_ready() {
	return has_secret_key;
}

bool discord_voice_client::is_playing() {
	std::lock_guard<std::mutex> lock(this->stream_mutex);
	return (!this->outbuf.empty());
}

void discord_voice_client::thread_run()
{
	utility::set_thread_name(std::string("vc/") + std::to_string(server_id));

	size_t times_looped = 0;
	time_t last_loop_time = time(nullptr);

	do {
		bool error = false;
		ssl_client::read_loop();
		ssl_client::close();

		time_t current_time = time(nullptr);
		/* Here, we check if it's been longer than 3 seconds since the previous loop,
		 * this gives us time to see if it's an actual disconnect, or an error.
		 * This will prevent us from looping too much, meaning error codes do not cause an infinite loop.
		 */
		if (current_time - last_loop_time >= 3)
			times_looped = 0;

		/* This does mean we'll always have times_looped at a minimum of 1, this is intended. */
		times_looped++;
		/* If we've looped 5 or more times, abort the loop. */
		if (times_looped >= 5) {
			log(dpp::ll_warning, "Reached max loops whilst attempting to read from the websocket. Aborting websocket.");
			break;
		}

		last_loop_time = current_time;

		if (!terminating) {
			log(dpp::ll_debug, "Attempting to reconnect the websocket...");
			do {
				try {
					ssl_client::connect();
					websocket_client::connect();
				}
				catch (const std::exception &e) {
					log(dpp::ll_error, std::string("Error establishing voice websocket connection, retry in 5 seconds: ") + e.what());
					ssl_client::close();
					std::this_thread::sleep_for(std::chrono::seconds(5));
					error = true;
				}
			} while (error && !terminating);
		}
	} while(!terminating);
}

void discord_voice_client::run()
{
	this->runner = new std::thread(&discord_voice_client::thread_run, this);
	this->thread_id = runner->native_handle();
}

int discord_voice_client::udp_send(const char* data, size_t length)
{
	sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(this->port);
	servaddr.sin_addr.s_addr = inet_addr(this->ip.c_str());
	return (int) sendto(this->fd, data, (int)length, 0, (const sockaddr*)&servaddr, (int)sizeof(sockaddr_in));
}

int discord_voice_client::udp_recv(char* data, size_t max_length)
{
	return (int) recv(this->fd, data, (int)max_length, 0);
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
	return is_end_to_end_encrypted() ? mls_state->privacy_code : "";
}

void discord_voice_client::get_user_privacy_code(const dpp::snowflake user, privacy_code_callback_t callback) const {
	if (!is_end_to_end_encrypted()) {
		callback("");
		return;
	}
	mls_state->dave_session->GetPairwiseFingerprint(0x0000, user.str(), [callback](const std::vector<uint8_t>& data) {
		std::cout << dpp::utility::debug_dump((uint8_t*)data.data(), data.size());
		callback(data.size() == 64 ? generate_displayable_code(data, 45) : "");
	});
}

bool discord_voice_client::is_end_to_end_encrypted() const {
	return mls_state && !mls_state->privacy_code.empty();
}

bool discord_voice_client::handle_frame(const std::string &data, ws_opcode opcode) {
	json j;

	/**
	 * MLS frames come in as type OP_BINARY, we can also reply to them as type OP_BINARY.
	 */
	if (opcode == OP_BINARY && data.size() >= sizeof(dave_binary_header_t)) {

		auto* dave_header = reinterpret_cast<const dave_binary_header_t*>(data.data());

		switch (dave_header->opcode) {
			case voice_client_dave_mls_external_sender: {
				log(ll_debug, "voice_client_dave_mls_external_sender");

				mls_state->dave_session->SetExternalSender(dave_header->get_data(data.length()));

				mls_state->encryptor = std::make_unique<dave::Encryptor>();
				mls_state->decryptors.clear();
			}
			break;
			case voice_client_dave_mls_proposals: {
				log(ll_debug, "voice_client_dave_mls_proposals");

				std::optional<std::vector<uint8_t>> response = mls_state->dave_session->ProcessProposals(dave_header->get_data(data.length()), dave_mls_user_list);
				if (response.has_value()) {
					auto r = response.value();
					mls_state->cached_commit = r;
					r.insert(r.begin(), voice_client_dave_mls_commit_message);
					this->write(std::string_view(reinterpret_cast<const char*>(r.data()), r.size()), OP_BINARY);
				}
			}
			break;
			case voice_client_dave_announce_commit_transaction: {
				log(ll_debug, "voice_client_dave_announce_commit_transaction");
				auto r = mls_state->dave_session->ProcessCommit(mls_state->cached_commit);
				for (const auto& user : dave_mls_user_list) {
					log(ll_debug, "Setting decryptor key ratchet for user: " + user + ", protocol version: " + std::to_string(mls_state->dave_session->GetProtocolVersion()));
					dpp::snowflake u{user};
					mls_state->decryptors.emplace(u, std::make_unique<dpp::dave::Decryptor>());
					mls_state->decryptors.find(u)->second->TransitionToKeyRatchet(mls_state->dave_session->GetKeyRatchet(user));
				}
				mls_state->encryptor->SetKeyRatchet(mls_state->dave_session->GetKeyRatchet(creator->me.id.str()));

				/**
				 * https://www.ietf.org/archive/id/draft-ietf-mls-protocol-14.html#name-epoch-authenticators
				 * 9.7. Epoch Authenticators
				 * The main MLS key schedule provides a per-epoch epoch_authenticator. If one member of the group is being impersonated by an active attacker,
				 * the epoch_authenticator computed by their client will differ from those computed by the other group members.
				 */
				mls_state->privacy_code = generate_displayable_code(mls_state->dave_session->GetLastEpochAuthenticator());
				log(ll_debug, "E2EE Privacy Code: " + mls_state->privacy_code);
			}
			break;
			case voice_client_dave_mls_welcome: {
				this->mls_state->transition_id = dave_header->get_welcome_transition_id();
				log(ll_debug, "voice_client_dave_mls_welcome with transition id " + std::to_string(this->mls_state->transition_id));
				auto r = mls_state->dave_session->ProcessWelcome(dave_header->get_welcome_data(data.length()), dave_mls_user_list);
				if (r.has_value()) {
					for (const auto& user : dave_mls_user_list) {
						log(ll_debug, "Setting decryptor key ratchet for user: " + user + ", protocol version: " + std::to_string(mls_state->dave_session->GetProtocolVersion()));
						dpp::snowflake u{user};
						mls_state->decryptors.emplace(u, std::make_unique<dpp::dave::Decryptor>());
						mls_state->decryptors.find(u)->second->TransitionToKeyRatchet(mls_state->dave_session->GetKeyRatchet(user));
					}
					mls_state->encryptor->SetKeyRatchet(mls_state->dave_session->GetKeyRatchet(creator->me.id.str()));
				}
				mls_state->privacy_code = generate_displayable_code(mls_state->dave_session->GetLastEpochAuthenticator());
				log(ll_debug, "E2EE Privacy Code: " + mls_state->privacy_code);
			}
			break;
			default:
				log(ll_debug, "Unexpected DAVE frame opcode");
				log(dpp::ll_trace, "R: " + dpp::utility::debug_dump((uint8_t*)(data.data()), data.length()));
			break;
		}

		return true;
	}
	
	try {
		log(dpp::ll_trace, std::string("R: ") + data);
		j = json::parse(data);
	}
	catch (const std::exception &e) {
		log(dpp::ll_error, std::string("discord_voice_client::handle_frame ") + e.what() + ": " + data);
		return true;
	}

	if (j.find("seq") != j.end() && j["seq"].is_number()) {
		/**
		  * Save the sequence number needed for heartbeat and resume payload.
		  *
		  * NOTE: Contrary to the documentation, discord does not seem to send messages with sequence number
		  * in order, should we only save the sequence if it's larger number?
		  */
		receive_sequence = j["seq"].get<int32_t>();
	}

	if (j.find("op") != j.end()) {
		uint32_t op = j["op"];

		switch (op) {
			/* Ping acknowledgement */
			case voice_opcode_connection_heartbeat_ack:
				/* These opcodes do not require a response or further action */
			break;
			case voice_opcode_media_sink:
			case voice_client_flags: {
			}
			break;
			case voice_client_platform: {
				voice_client_platform_t vcp(nullptr, data);
				vcp.voice_client = this;
				vcp.user_id = snowflake_not_null(&j["d"], "user_id");
				vcp.platform = static_cast<client_platform_t>(int8_not_null(&j["d"], "platform"));
				creator->on_voice_client_platform.call(vcp);

			}
			break;
			case voice_opcode_multiple_clients_connect: {
				dave_mls_user_list = j["d"]["user_ids"];
				log(ll_debug, "Number of clients in voice channel: " + std::to_string(dave_mls_user_list.size()));
			}
			break;
			case voice_client_dave_mls_invalid_commit_welcome: {
				this->mls_state->transition_id = j["d"]["transition_id"];
				log(ll_debug, "voice_client_dave_mls_invalid_commit_welcome transition id " + std::to_string(this->mls_state->transition_id));
			}
			break;
			case voice_client_dave_execute_transition: {
				log(ll_debug, "voice_client_dave_execute_transition");
				this->mls_state->transition_id = j["d"]["transition_id"];
				json obj = {
					{ "op", voice_client_dave_transition_ready },
					{
					  "d",
						{
							{ "transition_id", this->mls_state->transition_id },
						}
					}
				};
				this->write(obj.dump(-1, ' ', false, json::error_handler_t::replace), OP_TEXT);
			}
			break;
			/* "The protocol only uses this opcode to indicate when a downgrade to protocol version 0 is upcoming." */
			case voice_client_dave_prepare_transition: {
				uint64_t transition_id = j["d"]["transition_id"];
				uint64_t protocol_version = j["d"]["protocol_version"];
				log(ll_debug, "voice_client_dave_prepare_transition version=" + std::to_string(protocol_version) + " for transition " + std::to_string(transition_id));
			}
			break;
			case voice_client_dave_prepare_epoch: {
				uint64_t protocol_version = j["d"]["protocol_version"];
				uint64_t epoch = j["d"]["epoch"];
				log(ll_debug, "voice_client_dave_prepare_epoch version=" + std::to_string(protocol_version) + " for epoch " + std::to_string(epoch));
				if (epoch == 1) {
					mls_state->dave_session->Reset();
					mls_state->dave_session->Init(dave::MaxSupportedProtocolVersion(), channel_id, creator->me.id.str(), mls_state->mls_key);
				}
			}
			break;
			/* Client Disconnect */
			case voice_opcode_client_disconnect: {
				if (j.find("d") != j.end() && j["d"].find("user_id") != j["d"].end() && !j["d"]["user_id"].is_null()) {
					snowflake u_id = snowflake_not_null(&j["d"], "user_id");
					auto it = std::find_if(ssrc_map.begin(), ssrc_map.end(),
					   [&u_id](const auto & p) { return p.second == u_id; });

					if (it != ssrc_map.end()) {
						ssrc_map.erase(it);
					}

					if (!creator->on_voice_client_disconnect.empty()) {
						voice_client_disconnect_t vcd(nullptr, data);
						vcd.voice_client = this;
						vcd.user_id = u_id;
						creator->on_voice_client_disconnect.call(vcd);
					}
				}
			}
			break;
			/* Speaking */ 
			case voice_opcode_client_speaking:
			/* Client Connect (doesn't seem to work) */
			case voice_opcode_client_connect: {
				if (j.find("d") != j.end() 
					&& j["d"].find("user_id") != j["d"].end() && !j["d"]["user_id"].is_null()
					&& j["d"].find("ssrc") != j["d"].end() && !j["d"]["ssrc"].is_null() && j["d"]["ssrc"].is_number_integer()) {
					uint32_t u_ssrc = j["d"]["ssrc"].get<uint32_t>();
					snowflake u_id = snowflake_not_null(&j["d"], "user_id");
					ssrc_map[u_ssrc] = u_id;

					if (!creator->on_voice_client_speaking.empty()) {
						voice_client_speaking_t vcs(nullptr, data);
						vcs.voice_client = this;
						vcs.user_id = u_id;
						vcs.ssrc = u_ssrc;
						creator->on_voice_client_speaking.call(vcs);
					}
				}
			}
			break;
			/* Voice resume */
			case voice_opcode_connection_resumed:
				log(ll_debug, "Voice connection resumed");
			break;
			/* Voice HELLO */
			case voice_opcode_connection_hello: {
				if (j.find("d") != j.end() && j["d"].find("heartbeat_interval") != j["d"].end() && !j["d"]["heartbeat_interval"].is_null()) {
					this->heartbeat_interval = j["d"]["heartbeat_interval"].get<uint32_t>();
				}

				/* Reset receive_sequence on HELLO */
				receive_sequence = -1;

				if (!modes.empty()) {
					log(dpp::ll_debug, "Resuming voice session " + this->sessionid + "...");
					json obj = {
						{ "op", voice_opcode_connection_resume },
						{
							"d",
							{
								{ "server_id", std::to_string(this->server_id) },
								{ "session_id", this->sessionid },
								{ "token", this->token },
								{ "seq_ack", this->receive_sequence },
							}
						}
					};
					this->write(obj.dump(-1, ' ', false, json::error_handler_t::replace), OP_TEXT);
				} else {
					log(dpp::ll_debug, "Connecting new voice session (DAVE: " + std::string(dave_version == dave_version_1 ? "Enabled" : "Disabled") + ")...");
						json obj = {
						{ "op", voice_opcode_connection_identify },
						{
							"d",
							{
								{ "user_id", creator->me.id },
								{ "server_id", std::to_string(this->server_id) },
								{ "session_id", this->sessionid },
								{ "token", this->token },
								{ "max_dave_protocol_version", dave_version },
							}
						}
					};
					this->write(obj.dump(-1, ' ', false, json::error_handler_t::replace), OP_TEXT);
				}
				this->connect_time = time(nullptr);
			}
			break;
			/* Session description */
			case voice_opcode_connection_description: {
				json &d = j["d"];
				size_t ofs = 0;
				for (auto & c : d["secret_key"]) {
					secret_key[ofs++] = (uint8_t)c;
					if (ofs > secret_key.size() - 1) {
						break;
					}
				}
				has_secret_key = true;

				if (dave_version != dave_version_none) {
					if (j["d"]["dave_protocol_version"] != static_cast<uint32_t>(dave_version)) {
						log(ll_error, "We requested DAVE E2EE but didn't receive it from the server, downgrading...");
						dave_version = dave_version_none;
						send_silence(20);
					}

					mls_state = std::make_unique<dave_state>();
					mls_state->dave_session = std::make_unique<dave::mls::Session>(
						nullptr, "" /* sessionid */, [this](std::string const& s1, std::string const& s2) {
							log(ll_debug, "Dave session constructor callback: " + s1 + ", " + s2);
						});
					mls_state->dave_session->Init(dave::MaxSupportedProtocolVersion(), channel_id, creator->me.id.str(), mls_state->mls_key);
					auto key_response = mls_state->dave_session->GetMarshalledKeyPackage();
					key_response.insert(key_response.begin(), voice_client_dave_mls_key_package);
					this->write(std::string_view(reinterpret_cast<const char*>(key_response.data()), key_response.size()), OP_BINARY);

				} else {
					/* This is needed to start voice receiving and make sure that the start of sending isn't cut off */
					send_silence(20);
				}

				/* Fire on_voice_ready */
				if (!creator->on_voice_ready.empty()) {
					voice_ready_t rdy(nullptr, data);
					rdy.voice_client = this;
					rdy.voice_channel_id = this->channel_id;
					creator->on_voice_ready.call(rdy);
				}

				/* Reset packet_nonce */
				packet_nonce = 1;
			}
			break;
			/* Voice ready */
			case voice_opcode_connection_ready: {
				/* Video stream stuff comes in this frame too, but we can't use it (YET!) */
				json &d = j["d"];
				this->ip = d["ip"].get<std::string>();
				this->port = d["port"].get<uint16_t>();
				this->ssrc = d["ssrc"].get<uint64_t>();
				// Modes
				for (auto & m : d["modes"]) {
					this->modes.push_back(m.get<std::string>());
				}
				log(ll_debug, "Voice websocket established; UDP endpoint: " + ip + ":" + std::to_string(port) + " [ssrc=" + std::to_string(ssrc) + "] with " + std::to_string(modes.size()) + " modes");

				external_ip = discover_ip();

				dpp::socket newfd;
				if ((newfd = ::socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {

					sockaddr_in servaddr{};
					memset(&servaddr, 0, sizeof(sockaddr_in));
					servaddr.sin_family = AF_INET;
					servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
					servaddr.sin_port = htons(0);

					if (bind(newfd, (sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
						throw dpp::connection_exception(err_bind_failure, "Can't bind() client UDP socket");
					}
					
					if (!set_nonblocking(newfd, true)) {
						throw dpp::connection_exception(err_nonblocking_failure, "Can't switch voice UDP socket to non-blocking mode!");
					}

					/* Hook poll() in the ssl_client to add a new file descriptor */
					this->fd = newfd;
					this->custom_writeable_fd = [this] { return want_write(); };
					this->custom_readable_fd = [this] { return want_read(); };
					this->custom_writeable_ready = [this] { write_ready(); };
					this->custom_readable_ready = [this] { read_ready(); };

					int bound_port = 0;
					sockaddr_in sin{};
					socklen_t len = sizeof(sin);
					if (getsockname(this->fd, (sockaddr *)&sin, &len) > -1) {
						bound_port = ntohs(sin.sin_port);
					}

					log(ll_debug, "External IP address: " + external_ip);

					this->write(json({
						{ "op", voice_opcode_connection_select_protocol },
							{ "d", {
								{ "protocol", "udp" },
								{ "data", {
										{ "address", external_ip },
										{ "port", bound_port },
										{ "mode", transport_encryption_protocol }
									}
								}
							}
						}
					}).dump(-1, ' ', false, json::error_handler_t::replace), OP_TEXT);
				}
			}
			break;
			default: {
				log(ll_debug, "Unknown voice opcode " + std::to_string(op) + ": " + data);
			}
			break;
		}
	}
	return true;
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

void discord_voice_client::send(const char* packet, size_t len, uint64_t duration) {
	std::lock_guard<std::mutex> lock(this->stream_mutex);
	voice_out_packet frame;
	frame.packet = std::string(packet, len);
	frame.duration = duration;
	outbuf.emplace_back(frame);
}

void discord_voice_client::read_ready()
{
#ifdef HAVE_VOICE
	uint8_t buffer[65535];
	int packet_size = this->udp_recv((char*)buffer, sizeof(buffer));

	bool receive_handler_is_empty = creator->on_voice_receive.empty() && creator->on_voice_receive_combined.empty();
	if (packet_size <= 0 || receive_handler_is_empty) {
		/* Nothing to do */
		return;
	}

	constexpr size_t header_size = 12;
	if (static_cast<size_t>(packet_size) < header_size) {
		/* Invalid RTP payload */
		return;
	}

	/* It's a "silence packet" - throw it away. */
	if (packet_size < 44) {
		return;
	}

	if (uint8_t payload_type = buffer[1] & 0b0111'1111;
		72 <= payload_type && payload_type <= 76) {
		/*
		 * This is an RTCP payload. Discord is known to send
		 * RTCP Receiver Reports.
		 *
		 * See https://datatracker.ietf.org/doc/html/rfc3551#section-6
		 */
		return;
	}

	voice_payload vp{0, // seq, populate later
		0, // timestamp, populate later
		std::make_unique<voice_receive_t>(nullptr, std::string((char*)buffer, packet_size))};

	vp.vr->voice_client = this;

	uint32_t speaker_ssrc;
	{	/* Get the User ID of the speaker */
		std::memcpy(&speaker_ssrc, &buffer[8], sizeof(uint32_t));
		speaker_ssrc = ntohl(speaker_ssrc);
		vp.vr->user_id = ssrc_map[speaker_ssrc];
	}

	/* Get the sequence number of the voice UDP packet */
	std::memcpy(&vp.seq, &buffer[2], sizeof(rtp_seq_t));
	vp.seq = ntohs(vp.seq);

	/* Get the timestamp of the voice UDP packet */
	std::memcpy(&vp.timestamp, &buffer[4], sizeof(rtp_timestamp_t));
	vp.timestamp = ntohl(vp.timestamp);

	constexpr size_t nonce_size = sizeof(uint32_t);
	/* Nonce is 4 byte at the end of payload with zero padding */
	uint8_t nonce[24] = { 0 };
	std::memcpy(nonce, buffer + packet_size - nonce_size, nonce_size);

	/* Get the number of CSRC in header */
	const size_t csrc_count = buffer[0] & 0b0000'1111;
	/* Skip to the encrypted voice data */
	const ptrdiff_t offset_to_data = header_size + sizeof(uint32_t) * csrc_count;
	size_t total_header_len = offset_to_data;

	uint8_t* ciphertext = buffer + offset_to_data;
	size_t ciphertext_len = packet_size - offset_to_data - nonce_size;

	size_t ext_len = 0;
	if ([[maybe_unused]] const bool uses_extension = (buffer[0] >> 4) & 0b0001) {
		/**
		 * Get the RTP Extensions size, we only get the size here because
		 * the extension itself is encrypted along with the opus packet
		 */
		{
			uint16_t ext_len_in_words;
			memcpy(&ext_len_in_words, &ciphertext[2], sizeof(uint16_t));
			ext_len_in_words = ntohs(ext_len_in_words);
			ext_len = sizeof(uint32_t) * ext_len_in_words;
		}
		constexpr size_t ext_header_len = sizeof(uint16_t) * 2;
		ciphertext += ext_header_len;
		ciphertext_len -= ext_header_len;
		total_header_len += ext_header_len;
	}

	uint8_t decrypted[65535] = { 0 };
	unsigned long long opus_packet_len  = 0;
	if (crypto_aead_xchacha20poly1305_ietf_decrypt(
		decrypted, &opus_packet_len,
		nullptr,
		ciphertext, ciphertext_len,
		buffer,
		/**
		 * Additional Data:
		 * The whole header (including csrc list) +
		 * 4 byte extension header (magic 0xBEDE + 16-bit denoting extension length)
		 */
		total_header_len,
		nonce, secret_key.data()) != 0) {
		/* Invalid Discord RTP payload. */
		return;
	}

	uint8_t *opus_packet = decrypted;
	if (ext_len > 0) {
		/* Skip previously encrypted RTP Header Extension */
		opus_packet += ext_len;
		opus_packet_len -= ext_len;
	}

	/*
	 * We're left with the decrypted, opus-encoded data.
	 * Park the payload and decode on the voice courier thread.
	 */
	vp.vr->audio_data.assign(opus_packet, opus_packet + opus_packet_len);

	{
		std::lock_guard lk(voice_courier_shared_state.mtx);
		auto& [range, payload_queue, pending_decoder_ctls, decoder] = voice_courier_shared_state.parked_voice_payloads[vp.vr->user_id];

		if (!decoder) {
			/*
			 * Most likely this is the first time we encounter this speaker.
			 * Do some initialization for not only the decoder but also the range.
			 */
			range.min_seq = vp.seq;
			range.min_timestamp = vp.timestamp;

			int opus_error = 0;
			decoder.reset(opus_decoder_create(opus_sample_rate_hz, opus_channel_count, &opus_error),
				 &opus_decoder_destroy);
			if (opus_error) {
				/**
				 * NOTE: The -10 here makes the opus_error match up with values of exception_error_code,
				 * which would otherwise conflict as every C library loves to use values from -1 downwards.
				 */
				throw dpp::voice_exception((exception_error_code)(opus_error - 10), "discord_voice_client::discord_voice_client; opus_decoder_create() failed");
			}
		}

		if (vp.seq < range.min_seq && vp.timestamp < range.min_timestamp) {
			/* This packet arrived too late. We can only discard it. */
			return;
		}
		range.max_seq = vp.seq;
		range.max_timestamp = vp.timestamp;
		payload_queue.push(std::move(vp));
	}

	voice_courier_shared_state.signal_iteration.notify_one();

	if (!voice_courier.joinable()) {
		/* Courier thread is not running, start it */
		voice_courier = std::thread(&voice_courier_loop,
							  std::ref(*this),
							  std::ref(voice_courier_shared_state));
	}
#else
	throw dpp::voice_exception(err_no_voice_support, "Voice support not enabled in this build of D++");
#endif
}

void discord_voice_client::write_ready()
{
	uint64_t duration = 0;
	bool track_marker_found = false;
	uint64_t bufsize = 0;
	send_audio_type_t type = satype_recorded_audio; 
	{
		std::lock_guard<std::mutex> lock(this->stream_mutex);
		if (!this->paused && outbuf.size()) {
			type = send_audio_type;
			if (outbuf[0].packet.size() == sizeof(uint16_t) && (*((uint16_t*)(outbuf[0].packet.data()))) == AUDIO_TRACK_MARKER) {
				outbuf.erase(outbuf.begin());
				track_marker_found = true;
				if (tracks > 0) {
					tracks--;
				}
			}
			if (outbuf.size()) {
				if (this->udp_send(outbuf[0].packet.data(), outbuf[0].packet.length()) == (int)outbuf[0].packet.length()) {
					duration = outbuf[0].duration * timescale;
					bufsize = outbuf[0].packet.length();
					outbuf.erase(outbuf.begin());
				}
			}
		}
	}
	if (duration) {
		if (type == satype_recorded_audio) {
			std::chrono::nanoseconds latency = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - last_timestamp);
			std::chrono::nanoseconds sleep_time = std::chrono::nanoseconds(duration) - latency;
			if (sleep_time.count() > 0) {
				std::this_thread::sleep_for(sleep_time);
			}
		}
		else if (type == satype_overlap_audio) {
			std::chrono::nanoseconds latency = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - last_timestamp);
			std::chrono::nanoseconds sleep_time = std::chrono::nanoseconds(duration) + last_sleep_remainder - latency;
			std::chrono::nanoseconds sleep_increment = (std::chrono::nanoseconds(duration) - latency) / AUDIO_OVERLAP_SLEEP_SAMPLES;
			if (sleep_time.count() > 0) {
				uint16_t samples_count = 0;
				std::chrono::nanoseconds overshoot_accumulator{};

				do {
					std::chrono::high_resolution_clock::time_point start_sleep = std::chrono::high_resolution_clock::now();
					std::this_thread::sleep_for(sleep_increment);
					std::chrono::high_resolution_clock::time_point end_sleep = std::chrono::high_resolution_clock::now();

					samples_count++;
					overshoot_accumulator += std::chrono::duration_cast<std::chrono::nanoseconds>(end_sleep - start_sleep) - sleep_increment;
					sleep_time -= std::chrono::duration_cast<std::chrono::nanoseconds>(end_sleep - start_sleep);
				} while (std::chrono::nanoseconds(overshoot_accumulator.count() / samples_count) + sleep_increment < sleep_time);
				last_sleep_remainder = sleep_time;
			} else {
				last_sleep_remainder = std::chrono::nanoseconds(0);
			}
		}

		last_timestamp = std::chrono::high_resolution_clock::now();
		if (!creator->on_voice_buffer_send.empty()) {
			voice_buffer_send_t snd(nullptr, "");
			snd.buffer_size = bufsize;
			snd.packets_left = outbuf.size();
			snd.voice_client = this;
			creator->on_voice_buffer_send.call(snd);
		}
	}
	if (track_marker_found) {
		if (!creator->on_voice_track_marker.empty()) {
			voice_track_marker_t vtm(nullptr, "");
			vtm.voice_client = this;
			{
				std::lock_guard<std::mutex> lock(this->stream_mutex);
				if (!track_meta.empty()) {
					vtm.track_meta = track_meta[0];
					track_meta.erase(track_meta.begin());
				}
			}
			creator->on_voice_track_marker.call(vtm);
		}
	}
}

dpp::utility::uptime discord_voice_client::get_uptime()
{
	return dpp::utility::uptime(time(nullptr) - connect_time);
}

bool discord_voice_client::is_connected()
{
	return (this->get_state() == CONNECTED);
}

dpp::socket discord_voice_client::want_write() {
	std::lock_guard<std::mutex> lock(this->stream_mutex);
	if (!this->paused && !outbuf.empty()) {
		return fd;
	} else {
		return INVALID_SOCKET;
	}
}

dpp::socket discord_voice_client::want_read() {
	return fd;
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

size_t discord_voice_client::encode(uint8_t *input, size_t inDataSize, uint8_t *output, size_t &outDataSize)
{
#if HAVE_VOICE
	outDataSize = 0;
	int mEncFrameBytes = 11520;
	int mEncFrameSize = 2880;
	if (0 == (inDataSize % mEncFrameBytes)) {
		bool isOk = true;
		uint8_t *out = encode_buffer;

		memset(out, 0, sizeof(encode_buffer));
		repacketizer = opus_repacketizer_init(repacketizer);
		if (!repacketizer) {
			log(ll_warning, "opus_repacketizer_init(): failure");
			return outDataSize;
		}
		for (size_t i = 0; i < (inDataSize / mEncFrameBytes); ++ i) {
			const opus_int16* pcm = (opus_int16*)(input + i * mEncFrameBytes);
			int ret = opus_encode(encoder, pcm, mEncFrameSize, out, 65536);
			if (ret > 0) {
				int retval = opus_repacketizer_cat(repacketizer, out, ret);
				if (retval != OPUS_OK) {
					isOk = false;
					log(ll_warning, "opus_repacketizer_cat(): " + std::string(opus_strerror(retval)));
					break;
				}
				out += ret;
			} else {
				isOk = false;
					log(ll_warning, "opus_encode(): " + std::string(opus_strerror(ret)));
				break;
			}
		}
		if (isOk) {
			int ret = opus_repacketizer_out(repacketizer, output, 65536);
			if (ret > 0) {
				outDataSize = ret;
			} else {
				log(ll_warning, "opus_repacketizer_out(): " + std::string(opus_strerror(ret)));
			}
		}
	} else {
		throw dpp::voice_exception(err_invalid_voice_packet_length, "Invalid input data length: " + std::to_string(inDataSize) + ", must be n times of " + std::to_string(mEncFrameBytes));
	}
#else
	throw dpp::voice_exception(err_no_voice_support, "Voice support not enabled in this build of D++");
#endif
	return outDataSize;
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
	{
		std::lock_guard<std::mutex> lock(this->stream_mutex);
		send_audio_type = type;
	}
	return *this;
}

discord_voice_client& discord_voice_client::send_audio_raw(uint16_t* audio_data, const size_t length)  {
#if HAVE_VOICE
	if (length < 4) {
		throw dpp::voice_exception(err_invalid_voice_packet_length, "Raw audio packet size can't be less than 4");
	}

	if ((length % 4) != 0) {
		throw dpp::voice_exception(err_invalid_voice_packet_length, "Raw audio packet size should be divisible by 4");
	}

	if (length > send_audio_raw_max_length) {
		std::string s_audio_data((const char*)audio_data, length);

		while (s_audio_data.length() > send_audio_raw_max_length) {
			std::string packet(s_audio_data.substr(0, send_audio_raw_max_length));
			const auto packet_size = static_cast<ptrdiff_t>(packet.size());

			s_audio_data.erase(s_audio_data.begin(), s_audio_data.begin() + packet_size);

			send_audio_raw((uint16_t*)packet.data(), packet_size);
		}

		return *this;
	}

	if (length < send_audio_raw_max_length) {
		std::string packet((const char*)audio_data, length);
		packet.resize(send_audio_raw_max_length, 0);

		return send_audio_raw((uint16_t*)packet.data(), packet.size());
	}

	opus_int32 encoded_audio_max_length = (opus_int32)length;
	std::vector<uint8_t> encoded_audio(encoded_audio_max_length);
	size_t encoded_audio_length = encoded_audio_max_length;

	encoded_audio_length = this->encode((uint8_t*)audio_data, length, encoded_audio.data(), encoded_audio_length);

	send_audio_opus(encoded_audio.data(), encoded_audio_length);
#else
	throw dpp::voice_exception(err_no_voice_support, "Voice support not enabled in this build of D++");
#endif
	return *this;
}

discord_voice_client& discord_voice_client::send_audio_opus(uint8_t* opus_packet, const size_t length) {
#if HAVE_VOICE
	int samples = opus_packet_get_nb_samples(opus_packet, (opus_int32)length, opus_sample_rate_hz);
	uint64_t duration = (samples / 48) / (timescale / 1000000);
	send_audio_opus(opus_packet, length, duration);
#else
	throw dpp::voice_exception(err_no_voice_support, "Voice support not enabled in this build of D++");
#endif
	return *this;
}

discord_voice_client& discord_voice_client::send_audio_opus(uint8_t* opus_packet, const size_t length, uint64_t duration) {
#if HAVE_VOICE
	int frame_size = (int)(48 * duration * (timescale / 1000000));
	opus_int32 encoded_audio_max_length = (opus_int32)length;
	std::vector<uint8_t> encoded_audio(encoded_audio_max_length);
	size_t encoded_audio_length = encoded_audio_max_length;

	encoded_audio_length = length;
	encoded_audio.reserve(length);
	memcpy(encoded_audio.data(), opus_packet, length);

	++sequence;
	rtp_header header(sequence, timestamp, (uint32_t)ssrc);

	/* Expected payload size is unencrypted header + encrypted opus packet + unencrypted 32 bit nonce */
	size_t packet_siz = sizeof(header) + (encoded_audio_length + crypto_aead_xchacha20poly1305_IETF_ABYTES) + sizeof(packet_nonce);

	std::vector<uint8_t> payload(packet_siz);

	/* Set RTP header */
	std::memcpy(payload.data(), &header, sizeof(header));

	/* Convert nonce to big-endian */
	uint32_t noncel = htonl(packet_nonce);

	/* 24 byte is needed for encrypting, discord just want 4 byte so just fill up the rest with null */
	unsigned char encrypt_nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = { '\0' };
	memcpy(encrypt_nonce, &noncel, sizeof(noncel));

	/* Execute */
	crypto_aead_xchacha20poly1305_ietf_encrypt(
		payload.data() + sizeof(header),
		nullptr,
		encoded_audio.data(),
		encoded_audio_length,
		/* The RTP Header as Additional Data */
		reinterpret_cast<const unsigned char *>(&header),
		sizeof(header),
		nullptr,
		static_cast<const unsigned char*>(encrypt_nonce),
		secret_key.data()
	);

	/* Append the 4 byte nonce to the resulting payload */
	std::memcpy(payload.data() + payload.size() - sizeof(noncel), &noncel, sizeof(noncel));

	this->send(reinterpret_cast<const char*>(payload.data()), payload.size(), duration);
	timestamp += frame_size;

	/* Increment for next packet */
	packet_nonce++;

	speak();
#else
	throw dpp::voice_exception(err_no_voice_support, "Voice support not enabled in this build of D++");
#endif
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
