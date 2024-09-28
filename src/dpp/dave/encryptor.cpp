#include "encryptor.h"
#include <algorithm>
#include <cstring>
#include <bytes/bytes.h>
#include "common.h"
#include "cryptor_manager.h"
#include "logger.h"
#include "codec_utils.h"
#include "array_view.h"
#include "leb128.h"
#include "scope_exit.h"

using namespace std::chrono_literals;

namespace dpp::dave {

constexpr auto kStatsInterval = 10s;

void Encryptor::SetKeyRatchet(std::unique_ptr<IKeyRatchet> keyRatchet)
{
    std::lock_guard<std::mutex> lock(keyGenMutex_);
    keyRatchet_ = std::move(keyRatchet);
    cryptor_ = nullptr;
    currentKeyGeneration_ = 0;
    truncatedNonce_ = 0;
}

void Encryptor::SetPassthroughMode(bool passthroughMode)
{
    passthroughMode_ = passthroughMode;
    UpdateCurrentProtocolVersion(passthroughMode ? 0 : MaxSupportedProtocolVersion());
}

int Encryptor::Encrypt(MediaType mediaType,
		       uint32_t ssrc,
		       array_view<const uint8_t> frame,
		       array_view<uint8_t> encryptedFrame,
		       size_t* bytesWritten)
{
    if (mediaType != Audio && mediaType != Video) {
        DISCORD_LOG(LS_WARNING) << "encrypt failed, invalid media type: "
                                << static_cast<int>(mediaType);
        return 0;
    }

    if (passthroughMode_) {
        // Pass frame through without encrypting
	std::memcpy(encryptedFrame.data(), frame.data(), frame.size());
        *bytesWritten = frame.size();
        stats_[mediaType].passthroughCount++;
        return ResultCode::Success;
    }

    {
        std::lock_guard<std::mutex> lock(keyGenMutex_);
        if (!keyRatchet_) {
            stats_[mediaType].encryptFailureCount++;
            return ResultCode::EncryptionFailure;
        }
    }

    auto start = std::chrono::steady_clock::now();
    auto result = ResultCode::Success;

    // write the codec identifier
    auto codec = CodecForSsrc(ssrc);

    auto frameProcessor = GetOrCreateFrameProcessor();
    ScopeExit cleanup([&] { ReturnFrameProcessor(std::move(frameProcessor)); });

    frameProcessor->ProcessFrame(frame, codec);

    const auto& unencryptedBytes = frameProcessor->GetUnencryptedBytes();
    const auto& encryptedBytes = frameProcessor->GetEncryptedBytes();
    auto& ciphertextBytes = frameProcessor->GetCiphertextBytes();

    const auto& unencryptedRanges = frameProcessor->GetUnencryptedRanges();
    auto unencryptedRangesSize = UnencryptedRangesSize(unencryptedRanges);

    auto additionalData = make_array_view(unencryptedBytes.data(), unencryptedBytes.size());
    auto plaintextBuffer = make_array_view(encryptedBytes.data(), encryptedBytes.size());
    auto ciphertextBuffer = make_array_view(ciphertextBytes.data(), ciphertextBytes.size());

    auto frameSize = encryptedBytes.size() + unencryptedBytes.size();
    auto tagBuffer = make_array_view(encryptedFrame.data() + frameSize, kAesGcm128TruncatedTagBytes);

    auto nonceBuffer = std::array<uint8_t, kAesGcm128NonceBytes>();
    auto nonceBufferView = make_array_view<const uint8_t>(nonceBuffer.data(), nonceBuffer.size());

    constexpr auto MAX_CIPHERTEXT_VALIDATION_RETRIES = 10;

    // some codecs (e.g. H26X) have packetizers that cannot handle specific byte sequences
    // so we attempt up to MAX_CIPHERTEXT_VALIDATION_RETRIES to encrypt the frame
    // calling into codec utils to validate the ciphertext + supplemental section
    // and re-rolling the truncated nonce if it fails

    // the nonce increment will definitely change the ciphertext and the tag
    // incrementing the nonce will also change the appropriate bytes
    // in the tail end of the nonce
    // which can remove start codes from the last 1 or 2 bytes of the nonce
    // and the two bytes of the unencrypted header bytes
    for (auto attempt = 1; attempt <= MAX_CIPHERTEXT_VALIDATION_RETRIES; ++attempt) {
        auto [cryptor, truncatedNonce] = GetNextCryptorAndNonce();

        if (!cryptor) {
            result = ResultCode::EncryptionFailure;
            break;
        }

        // write the truncated nonce to our temporary full nonce array
        // (since the encryption call expects a full size nonce)
        std::memcpy(nonceBuffer.data() + kAesGcm128TruncatedSyncNonceOffset,
               &truncatedNonce,
               kAesGcm128TruncatedSyncNonceBytes);

        // encrypt the plaintext, adding the unencrypted header to the tag
        bool success = cryptor->encrypt(
		ciphertextBuffer, plaintextBuffer, nonceBufferView, additionalData, tagBuffer);

        stats_[mediaType].encryptAttempts++;
        stats_[mediaType].encryptMaxAttempts =
          std::max(stats_[mediaType].encryptMaxAttempts, (uint64_t)attempt);

        if (!success) {
            assert(false && "Failed to encrypt frame");
            result = ResultCode::EncryptionFailure;
            break;
        }

        auto reconstructedFrameSize = frameProcessor->ReconstructFrame(encryptedFrame);
        assert(reconstructedFrameSize == frameSize && "Failed to reconstruct frame");

        auto nonceSize = Leb128Size(truncatedNonce);

        auto truncatedNonceBuffer = make_array_view(tagBuffer.end(), nonceSize);
        auto unencryptedRangesBuffer =
		make_array_view(truncatedNonceBuffer.end(), unencryptedRangesSize);
        auto supplementalBytesBuffer =
		make_array_view(unencryptedRangesBuffer.end(), sizeof(SupplementalBytesSize));
        auto markerBytesBuffer = make_array_view(supplementalBytesBuffer.end(), sizeof(MagicMarker));

        // write the nonce
        auto res = WriteLeb128(truncatedNonce, truncatedNonceBuffer.begin());
        if (res != nonceSize) {
            assert(false && "Failed to write truncated nonce");
            result = ResultCode::EncryptionFailure;
            break;
        }

        // write the unencrypted ranges
        res = SerializeUnencryptedRanges(
          unencryptedRanges, unencryptedRangesBuffer.begin(), unencryptedRangesBuffer.size());
        if (res != unencryptedRangesSize) {
            assert(false && "Failed to write unencrypted ranges");
            result = ResultCode::EncryptionFailure;
            break;
        }

        // write the supplemental bytes size
        SupplementalBytesSize supplementalBytes =
          kSupplementalBytes + nonceSize + unencryptedRangesSize;
	std::memcpy(supplementalBytesBuffer.data(), &supplementalBytes, sizeof(SupplementalBytesSize));

        // write the marker bytes, ends the frame
	std::memcpy(markerBytesBuffer.data(), &kMarkerBytes, sizeof(MagicMarker));

        auto encryptedFrameBytes = reconstructedFrameSize + kAesGcm128TruncatedTagBytes +
          nonceSize + unencryptedRangesSize + sizeof(SupplementalBytesSize) + sizeof(MagicMarker);

        if (codec_utils::validate_encrypted_frame(
		*frameProcessor, make_array_view(encryptedFrame.data(), encryptedFrameBytes))) {
            *bytesWritten = encryptedFrameBytes;
            break;
        }
        else if (attempt >= MAX_CIPHERTEXT_VALIDATION_RETRIES) {
            assert(false && "Failed to validate encrypted section for codec");
            result = ResultCode::EncryptionFailure;
            break;
        }
    }

    auto now = std::chrono::steady_clock::now();
    stats_[mediaType].encryptDuration +=
      std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
    if (result == ResultCode::Success) {
        stats_[mediaType].encryptSuccessCount++;
    }
    else {
        stats_[mediaType].encryptFailureCount++;
    }

    if (now > lastStatsTime_ + kStatsInterval) {
        lastStatsTime_ = now;
        DISCORD_LOG(LS_INFO) << "Encrypted audio: " << stats_[Audio].encryptSuccessCount
                             << ", video: " << stats_[Video].encryptSuccessCount
                             << ". Failed audio: " << stats_[Audio].encryptFailureCount
                             << ", video: " << stats_[Video].encryptFailureCount;
        DISCORD_LOG(LS_INFO) << "Last encrypted frame, type: "
                             << (mediaType == Audio ? "audio" : "video") << ", ssrc: " << ssrc
                             << ", size: " << frame.size();
    }

    return result;
}

size_t Encryptor::GetMaxCiphertextByteSize(MediaType mediaType, size_t frameSize)
{
    return frameSize + kSupplementalBytes + kTransformPaddingBytes;
}

void Encryptor::AssignSsrcToCodec(uint32_t ssrc, Codec codecType)
{
    auto existingCodecIt = std::find_if(
      ssrcCodecPairs_.begin(), ssrcCodecPairs_.end(), [ssrc](const SsrcCodecPair& pair) {
          return pair.first == ssrc;
      });

    if (existingCodecIt == ssrcCodecPairs_.end()) {
        ssrcCodecPairs_.emplace_back(ssrc, codecType);
    }
    else {
        existingCodecIt->second = codecType;
    }
}

Codec Encryptor::CodecForSsrc(uint32_t ssrc)
{
    auto existingCodecIt = std::find_if(
      ssrcCodecPairs_.begin(), ssrcCodecPairs_.end(), [ssrc](const SsrcCodecPair& pair) {
          return pair.first == ssrc;
      });

    if (existingCodecIt != ssrcCodecPairs_.end()) {
        return existingCodecIt->second;
    }
    else {
        return Codec::Unknown;
    }
}

std::unique_ptr<OutboundFrameProcessor> Encryptor::GetOrCreateFrameProcessor()
{
    std::lock_guard<std::mutex> lock(frameProcessorsMutex_);
    if (frameProcessors_.empty()) {
        return std::make_unique<OutboundFrameProcessor>();
    }
    auto frameProcessor = std::move(frameProcessors_.back());
    frameProcessors_.pop_back();
    return frameProcessor;
}

void Encryptor::ReturnFrameProcessor(std::unique_ptr<OutboundFrameProcessor> frameProcessor)
{
    std::lock_guard<std::mutex> lock(frameProcessorsMutex_);
    frameProcessors_.push_back(std::move(frameProcessor));
}

Encryptor::CryptorAndNonce Encryptor::GetNextCryptorAndNonce()
{
    std::lock_guard<std::mutex> lock(keyGenMutex_);
    if (!keyRatchet_) {
        return {nullptr, 0};
    }

    auto generation = compute_wrapped_generation(currentKeyGeneration_,
						 ++truncatedNonce_ >> kRatchetGenerationShiftBits);

    if (generation != currentKeyGeneration_ || !cryptor_) {
        currentKeyGeneration_ = generation;

        auto encryptionKey = keyRatchet_->GetKey(currentKeyGeneration_);
        cryptor_ = create_cipher(encryptionKey);
    }

    return {cryptor_, truncatedNonce_};
}

void Encryptor::UpdateCurrentProtocolVersion(ProtocolVersion version)
{
    if (version == currentProtocolVersion_) {
        return;
    }

    currentProtocolVersion_ = version;
    if (protocolVersionChangedCallback_) {
        protocolVersionChangedCallback_();
    }
}

} // namespace dpp::dave

