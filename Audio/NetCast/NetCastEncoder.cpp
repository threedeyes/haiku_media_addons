/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>
#include <ByteOrder.h>

#include <new>

#include "NetCastEncoder.h"
#include "NetCastDebug.h"

void
NetCastEncoder::ConvertToFloat(const void* input, int32 frames,
	const media_raw_audio_format& format, float* output)
{
	int32 channels = format.channel_count;

	switch (format.format) {
		case media_raw_audio_format::B_AUDIO_FLOAT: {
			const float* in = static_cast<const float*>(input);
			for (int32 i = 0; i < frames * channels; i++) {
				output[i] = in[i];
			}
			break;
		}
		case media_raw_audio_format::B_AUDIO_INT: {
			const int32* in = static_cast<const int32*>(input);
			for (int32 i = 0; i < frames * channels; i++) {
				output[i] = in[i] / 2147483648.0f;
			}
			break;
		}
		case media_raw_audio_format::B_AUDIO_SHORT: {
			const int16* in = static_cast<const int16*>(input);
			for (int32 i = 0; i < frames * channels; i++) {
				output[i] = in[i] / 32768.0f;
			}
			break;
		}
		case media_raw_audio_format::B_AUDIO_CHAR: {
			const int8* in = static_cast<const int8*>(input);
			for (int32 i = 0; i < frames * channels; i++) {
				output[i] = in[i] / 128.0f;
			}
			break;
		}
		case media_raw_audio_format::B_AUDIO_UCHAR: {
			const uint8* in = static_cast<const uint8*>(input);
			for (int32 i = 0; i < frames * channels; i++) {
				output[i] = (in[i] - 128) / 128.0f;
			}
			break;
		}
		default:
			memset(output, 0, frames * channels * sizeof(float));
			break;
	}
}

void
NetCastEncoder::ResampleAndMix(const float* input, int32 inputFrames,
	int32 inputChannels, float inputRate, int16* output, int32* outputFrames,
	float outputRate, int32 outputChannels)
{
	float ratio = outputRate / inputRate;
	*outputFrames = static_cast<int32>(inputFrames * ratio);

	for (int32 i = 0; i < *outputFrames; i++) {
		float srcPos = i / ratio;
		int32 srcIndex = static_cast<int32>(srcPos);
		float frac = srcPos - srcIndex;

		if (srcIndex >= inputFrames - 1) {
			srcIndex = inputFrames - 1;
			frac = 0.0f;
		}

		for (int32 ch = 0; ch < outputChannels; ch++) {
			float sample = 0.0f;

			if (inputChannels == 1 && outputChannels == 2) {
				float s1 = input[srcIndex];
				float s2 = (srcIndex + 1 < inputFrames) ? input[srcIndex + 1] : s1;
				sample = s1 + frac * (s2 - s1);
			} else if (inputChannels == 2 && outputChannels == 1) {
				float left1 = input[srcIndex * 2];
				float right1 = input[srcIndex * 2 + 1];
				float left2 = (srcIndex + 1 < inputFrames) ? input[(srcIndex + 1) * 2] : left1;
				float right2 = (srcIndex + 1 < inputFrames) ? input[(srcIndex + 1) * 2 + 1] : right1;
				float mono1 = (left1 + right1) * 0.5f;
				float mono2 = (left2 + right2) * 0.5f;
				sample = mono1 + frac * (mono2 - mono1);
			} else if (inputChannels == outputChannels) {
				int32 srcCh = (ch < inputChannels) ? ch : 0;
				float s1 = input[srcIndex * inputChannels + srcCh];
				float s2 = (srcIndex + 1 < inputFrames) ? input[(srcIndex + 1) * inputChannels + srcCh] : s1;
				sample = s1 + frac * (s2 - s1);
			} else {
				int32 srcCh = ch % inputChannels;
				float s1 = input[srcIndex * inputChannels + srcCh];
				float s2 = (srcIndex + 1 < inputFrames) ? input[(srcIndex + 1) * inputChannels + srcCh] : s1;
				sample = s1 + frac * (s2 - s1);
			}

			if (sample > 1.0f) sample = 1.0f;
			if (sample < -1.0f) sample = -1.0f;

			output[i * outputChannels + ch] = static_cast<int16>(sample * 32767.0f);
		}
	}
}

PCMEncoder::PCMEncoder()
	: fOutputSampleRate(44100),
	  fOutputChannels(2),
	  fDataSize(0),
	  fTempFloatBuffer(NULL),
	  fTempFloatBufferSize(0),
	  fTempPCMBuffer(NULL),
	  fTempPCMBufferSize(0)
{
	TRACE_CALL("");
}

PCMEncoder::~PCMEncoder()
{
	TRACE_CALL("");
	delete[] fTempFloatBuffer;
	delete[] fTempPCMBuffer;
}

status_t
PCMEncoder::SetOutputFormat(float sampleRate, int32 channels, int32 bitrate)
{
	TRACE_CALL("sampleRate=%.0f, channels=%ld, bitrate=%ld", sampleRate, channels, bitrate);

	fOutputSampleRate = sampleRate;
	fOutputChannels = channels;
	fDataSize = 0;

	TRACE_INFO("PCM encoder initialized: %.0f Hz, %ld channels", sampleRate, channels);

	return B_OK;
}

void
PCMEncoder::Uninit()
{
	TRACE_CALL("");
	TRACE_INFO("PCM encoder uninitialized, total data: %lu bytes", fDataSize);
}

int32
PCMEncoder::EncodeBuffer(const void* inputData, int32 inputFrames,
	const media_raw_audio_format& inputFormat, uint8* outBuffer, int32 outBufferSize)
{
	TRACE_VERBOSE("inputFrames=%ld, inputRate=%.0f, inputChannels=%ld",
		inputFrames, inputFormat.frame_rate, inputFormat.channel_count);

	size_t floatBufferSize = inputFrames * inputFormat.channel_count * sizeof(float);
	if (floatBufferSize > fTempFloatBufferSize) {
		delete[] fTempFloatBuffer;
		fTempFloatBuffer = new(std::nothrow) float[inputFrames * inputFormat.channel_count];
		if (!fTempFloatBuffer) {
			TRACE_ERROR("Failed to allocate float buffer");
			return 0;
		}
		fTempFloatBufferSize = floatBufferSize;
	}

	ConvertToFloat(inputData, inputFrames, inputFormat, fTempFloatBuffer);

	int32 maxOutputFrames = static_cast<int32>(inputFrames * (fOutputSampleRate / inputFormat.frame_rate)) + 2;
	size_t pcmBufferSize = maxOutputFrames * fOutputChannels * sizeof(int16);
	if (pcmBufferSize > fTempPCMBufferSize) {
		delete[] fTempPCMBuffer;
		fTempPCMBuffer = new(std::nothrow) int16[maxOutputFrames * fOutputChannels];
		if (!fTempPCMBuffer) {
			TRACE_ERROR("Failed to allocate PCM buffer");
			return 0;
		}
		fTempPCMBufferSize = pcmBufferSize;
	}

	int32 outputFrames;
	ResampleAndMix(fTempFloatBuffer, inputFrames, inputFormat.channel_count,
		inputFormat.frame_rate, fTempPCMBuffer, &outputFrames,
		fOutputSampleRate, fOutputChannels);

	int32 dataSize = outputFrames * fOutputChannels * 2;
	if (dataSize > outBufferSize) {
		TRACE_ERROR("Output buffer overflow: %ld > %ld", dataSize, outBufferSize);
		return 0;
	}

	int16* outData = reinterpret_cast<int16*>(outBuffer);
	for (int32 i = 0; i < outputFrames * fOutputChannels; i++) {
		outData[i] = B_HOST_TO_LENDIAN_INT16(fTempPCMBuffer[i]);
	}

	fDataSize += dataSize;
	TRACE_VERBOSE("Encoded %ld bytes PCM data (%ld frames)", dataSize, outputFrames);

	return dataSize;
}

int32
PCMEncoder::Flush(uint8* outBuffer, int32 outBufferSize)
{
	TRACE_VERBOSE("");
	return 0;
}

int32
PCMEncoder::RecommendedBufferSize(int32 pcmSamples) const
{
	return pcmSamples * fOutputChannels * 2;
}

#ifdef HAVE_LAME

MP3Encoder::MP3Encoder()
	: fLame(NULL),
	  fOutputChannels(2),
	  fOutputSampleRate(44100.0f),
	  fBitrate(128),
	  fQuality(7),
	  fTempFloatBuffer(NULL),
	  fTempFloatBufferSize(0),
	  fTempPCMBuffer(NULL),
	  fTempPCMBufferSize(0),
	  fInternalBuffer(NULL),
	  fInternalBufferSize(0),
	  fInternalBufferUsed(0),
	  fMinChunkSize(0)
{
	TRACE_CALL("");
}

MP3Encoder::~MP3Encoder()
{
	TRACE_CALL("");
	Uninit();
	delete[] fTempFloatBuffer;
	delete[] fTempPCMBuffer;
	delete[] fInternalBuffer;
}

status_t
MP3Encoder::SetOutputFormat(float sampleRate, int32 channels, int32 bitrate)
{
	TRACE_CALL("sampleRate=%.0f, channels=%ld, bitrate=%ld", sampleRate, channels, bitrate);

	fLame = lame_init();
	if (!fLame) {
		TRACE_ERROR("Failed to initialize LAME encoder");
		return B_NO_MEMORY;
	}

	fOutputChannels = channels;
	fOutputSampleRate = sampleRate;
	fBitrate = bitrate;

	lame_set_num_channels(fLame, channels);
	lame_set_in_samplerate(fLame, (int)sampleRate);
	lame_set_out_samplerate(fLame, (int)sampleRate);
	lame_set_brate(fLame, bitrate);
	lame_set_quality(fLame, fQuality);
	lame_set_mode(fLame, channels == 2 ? JOINT_STEREO : MONO);
	lame_set_VBR(fLame, vbr_off);
	lame_set_bWriteVbrTag(fLame, 0);
	lame_set_error_protection(fLame, 0);
	lame_set_disable_reservoir(fLame, 1);
	lame_set_strict_ISO(fLame, 0);
	lame_set_findReplayGain(fLame, 0);

	if (lame_init_params(fLame) < 0) {
		TRACE_ERROR("Failed to initialize LAME parameters");
		lame_close(fLame);
		fLame = NULL;
		return B_ERROR;
	}

	fMinChunkSize = 834;

	fInternalBufferSize = 8192;
	fInternalBuffer = new(std::nothrow) uint8[fInternalBufferSize];
	if (!fInternalBuffer) {
		TRACE_ERROR("Failed to allocate internal buffer");
		lame_close(fLame);
		fLame = NULL;
		return B_NO_MEMORY;
	}
	fInternalBufferUsed = 0;

	TRACE_INFO("MP3 encoder initialized: %.0f Hz, %ld channels, %ld kbps, quality=%ld, min chunk=%ld bytes",
			   sampleRate, channels, bitrate, fQuality, fMinChunkSize);

	return B_OK;
}

void
MP3Encoder::Uninit()
{
	TRACE_CALL("");
	if (fLame) {
		lame_close(fLame);
		fLame = NULL;
		TRACE_INFO("MP3 encoder uninitialized");
	}

	delete[] fInternalBuffer;
	fInternalBuffer = NULL;
	fInternalBufferSize = 0;
	fInternalBufferUsed = 0;
}

void
MP3Encoder::SetQuality(int32 quality)
{
	if (quality >= 0 && quality <= 9) {
		fQuality = quality;
		TRACE_INFO("MP3 quality set to %ld", quality);
	}
}

int32
MP3Encoder::EncodeBuffer(const void* inputData, int32 inputFrames,
	const media_raw_audio_format& inputFormat, uint8* outBuffer, int32 outBufferSize)
{
	TRACE_VERBOSE("inputFrames=%ld, inputRate=%.0f, inputChannels=%ld",
		inputFrames, inputFormat.frame_rate, inputFormat.channel_count);

	if (!fLame) {
		TRACE_ERROR("LAME encoder not initialized");
		return -1;
	}

	size_t floatBufferSize = inputFrames * inputFormat.channel_count * sizeof(float);
	if (floatBufferSize > fTempFloatBufferSize) {
		delete[] fTempFloatBuffer;
		fTempFloatBuffer = new(std::nothrow) float[inputFrames * inputFormat.channel_count];
		if (!fTempFloatBuffer) {
			TRACE_ERROR("Failed to allocate float buffer");
			return -1;
		}
		fTempFloatBufferSize = floatBufferSize;
	}

	ConvertToFloat(inputData, inputFrames, inputFormat, fTempFloatBuffer);

	int32 maxOutputFrames = static_cast<int32>(inputFrames * (fOutputSampleRate / inputFormat.frame_rate)) + 2;
	size_t pcmBufferSize = maxOutputFrames * fOutputChannels * sizeof(int16);
	if (pcmBufferSize > fTempPCMBufferSize) {
		delete[] fTempPCMBuffer;
		fTempPCMBuffer = new(std::nothrow) int16[maxOutputFrames * fOutputChannels];
		if (!fTempPCMBuffer) {
			TRACE_ERROR("Failed to allocate PCM buffer");
			return -1;
		}
		fTempPCMBufferSize = pcmBufferSize;
	}

	int32 outputFrames;
	ResampleAndMix(fTempFloatBuffer, inputFrames, inputFormat.channel_count,
		inputFormat.frame_rate, fTempPCMBuffer, &outputFrames,
		fOutputSampleRate, fOutputChannels);

	uint8 tempMP3Buffer[8192];
	int32 encoded;
	if (fOutputChannels == 1) {
		encoded = lame_encode_buffer(fLame,
			fTempPCMBuffer, fTempPCMBuffer,
			outputFrames, tempMP3Buffer, sizeof(tempMP3Buffer));
	} else {
		encoded = lame_encode_buffer_interleaved(fLame,
			fTempPCMBuffer, outputFrames, tempMP3Buffer, sizeof(tempMP3Buffer));
	}

	if (encoded < 0) {
		TRACE_ERROR("LAME encoding failed: %ld", encoded);
		return -1;
	}

	if (encoded > 0) {
		if (fInternalBufferUsed + encoded > fInternalBufferSize) {
			TRACE_WARNING("Internal buffer overflow, flushing");
			int32 toSend = fInternalBufferUsed;
			if (toSend > outBufferSize) {
				toSend = outBufferSize;
			}
			memcpy(outBuffer, fInternalBuffer, toSend);
			fInternalBufferUsed -= toSend;
			if (fInternalBufferUsed > 0) {
				memmove(fInternalBuffer, fInternalBuffer + toSend, fInternalBufferUsed);
			}
			return toSend;
		}

		memcpy(fInternalBuffer + fInternalBufferUsed, tempMP3Buffer, encoded);
		fInternalBufferUsed += encoded;

		TRACE_VERBOSE("Added %ld bytes to internal buffer (total: %lu)",
			encoded, fInternalBufferUsed);
	}

	if (fInternalBufferUsed > 0) {
		int32 toSend = fInternalBufferUsed;
		if (toSend > outBufferSize) {
			toSend = outBufferSize;
		}

		memcpy(outBuffer, fInternalBuffer, toSend);

		fInternalBufferUsed -= toSend;
		if (fInternalBufferUsed > 0) {
			memmove(fInternalBuffer, fInternalBuffer + toSend, fInternalBufferUsed);
		}

		TRACE_VERBOSE("Sending %ld bytes (remaining in buffer: %lu)", toSend, fInternalBufferUsed);
		return toSend;
	}

	return 0;
}

int32
MP3Encoder::Flush(uint8* outBuffer, int32 outBufferSize)
{
	TRACE_CALL("");

	if (!fLame) {
		TRACE_WARNING("LAME encoder not initialized on flush");
		return 0;
	}

	uint8 tempBuffer[8192];
	int32 flushed = lame_encode_flush_nogap(fLame, tempBuffer, sizeof(tempBuffer));

	if (flushed > 0) {
		if (fInternalBufferUsed + flushed <= fInternalBufferSize) {
			memcpy(fInternalBuffer + fInternalBufferUsed, tempBuffer, flushed);
			fInternalBufferUsed += flushed;
		}
	}

	int32 totalSent = 0;
	while (fInternalBufferUsed > 0 && totalSent < outBufferSize) {
		int32 toSend = fInternalBufferUsed;
		if (toSend > outBufferSize - totalSent) {
			toSend = outBufferSize - totalSent;
		}

		memcpy(outBuffer + totalSent, fInternalBuffer, toSend);
		totalSent += toSend;

		fInternalBufferUsed -= toSend;
		if (fInternalBufferUsed > 0) {
			memmove(fInternalBuffer, fInternalBuffer + toSend, fInternalBufferUsed);
		}
	}

	TRACE_INFO("Flushed %ld bytes from MP3 encoder", totalSent);

	return totalSent;
}

int32
MP3Encoder::RecommendedBufferSize(int32 pcmSamples) const
{
	return (int32)(1.25f * pcmSamples) + 7200;
}

#endif

NetCastEncoder*
EncoderFactory::CreateEncoder(CodecType type)
{
	TRACE_CALL("type=%d", type);

	NetCastEncoder* encoder = NULL;

	switch (type) {
		case CODEC_PCM:
			encoder = new PCMEncoder();
			TRACE_INFO("Created PCM encoder");
			break;
#ifdef HAVE_LAME
		case CODEC_MP3:
			encoder = new MP3Encoder();
			TRACE_INFO("Created MP3 encoder");
			break;
#endif
		default:
			TRACE_ERROR("Unknown codec type: %d", type);
			return NULL;
	}

	return encoder;
}

const char*
EncoderFactory::GetCodecName(CodecType type)
{
	switch (type) {
		case CODEC_PCM: return "PCM";
#ifdef HAVE_LAME
		case CODEC_MP3: return "MP3";
#endif
		default: return "Unknown";
	}
}
