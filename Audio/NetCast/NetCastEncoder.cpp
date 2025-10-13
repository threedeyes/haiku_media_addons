/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>
#include <ByteOrder.h>

#include "NetCastEncoder.h"
#include "NetCastDebug.h"

PCMEncoder::PCMEncoder()
	: fHeaderSent(false),
	  fSampleRate(44100),
	  fChannels(2),
	  fDataSize(0)
{
	TRACE_CALL("");
}

PCMEncoder::~PCMEncoder()
{
	TRACE_CALL("");
}

status_t
PCMEncoder::Init(float sampleRate, int32 channels, int32 bitrate)
{
	TRACE_CALL("sampleRate=%.0f, channels=%ld, bitrate=%ld", sampleRate, channels, bitrate);

	fSampleRate = sampleRate;
	fChannels = channels;
	fHeaderSent = false;
	fDataSize = 0;

	TRACE_INFO("PCM encoder initialized: %.0f Hz, %ld channels", sampleRate, channels);

	return B_OK;
}

void
PCMEncoder::Uninit()
{
	TRACE_CALL("");
	fHeaderSent = false;
	TRACE_INFO("PCM encoder uninitialized, total data: %lu bytes", fDataSize);
}

void
PCMEncoder::_WriteWAVHeader(uint8* buffer)
{
	TRACE_VERBOSE("Writing WAV header");

	uint32 maxSize = 0xFFFFFFFF - 8;

	memcpy(buffer, "RIFF", 4);
	*(uint32*)(buffer + 4) = B_HOST_TO_LENDIAN_INT32(maxSize);
	memcpy(buffer + 8, "WAVE", 4);

	memcpy(buffer + 12, "fmt ", 4);
	*(uint32*)(buffer + 16) = B_HOST_TO_LENDIAN_INT32(16);
	*(uint16*)(buffer + 20) = B_HOST_TO_LENDIAN_INT16(1);
	*(uint16*)(buffer + 22) = B_HOST_TO_LENDIAN_INT16(fChannels);
	*(uint32*)(buffer + 24) = B_HOST_TO_LENDIAN_INT32((uint32)fSampleRate);
	*(uint32*)(buffer + 28) = B_HOST_TO_LENDIAN_INT32((uint32)(fSampleRate * fChannels * 2));
	*(uint16*)(buffer + 32) = B_HOST_TO_LENDIAN_INT16(fChannels * 2);
	*(uint16*)(buffer + 34) = B_HOST_TO_LENDIAN_INT16(16);

	memcpy(buffer + 36, "data", 4);
	*(uint32*)(buffer + 40) = B_HOST_TO_LENDIAN_INT32(maxSize - 36);
}

int32
PCMEncoder::Encode(const int16* pcm, int32 samples, uint8* outBuffer, int32 outBufferSize)
{
	TRACE_VERBOSE("samples=%ld, bufferSize=%ld", samples, outBufferSize);

	int32 offset = 0;

	if (!fHeaderSent) {
		if (outBufferSize < 44) {
			TRACE_ERROR("Output buffer too small for WAV header: %ld < 44", outBufferSize);
			return 0;
		}
		_WriteWAVHeader(outBuffer);
		offset = 44;
		fHeaderSent = true;
		TRACE_INFO("WAV header sent");
	}

	int32 dataSize = samples * fChannels * 2;
	if (offset + dataSize > outBufferSize) {
		TRACE_ERROR("Output buffer overflow: %ld + %ld > %ld", offset, dataSize, outBufferSize);
		return 0;
	}

	memcpy(outBuffer + offset, pcm, dataSize);

	int16* data = (int16*)(outBuffer + offset);
	for (int32 i = 0; i < samples * fChannels; i++) {
		data[i] = B_HOST_TO_LENDIAN_INT16(data[i]);
	}

	fDataSize += dataSize;
	TRACE_VERBOSE("Encoded %ld bytes PCM data", offset + dataSize);

	return offset + dataSize;
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
	return 44 + pcmSamples * fChannels * 2;
}

#ifdef HAVE_LAME

MP3Encoder::MP3Encoder()
	: fLame(NULL)
{
	TRACE_CALL("");
}

MP3Encoder::~MP3Encoder()
{
	TRACE_CALL("");
	Uninit();
}

status_t
MP3Encoder::Init(float sampleRate, int32 channels, int32 bitrate)
{
	TRACE_CALL("sampleRate=%.0f, channels=%ld, bitrate=%ld", sampleRate, channels, bitrate);

	fLame = lame_init();
	if (!fLame) {
		TRACE_ERROR("Failed to initialize LAME encoder");
		return B_NO_MEMORY;
	}

	lame_set_num_channels(fLame, channels);
	lame_set_in_samplerate(fLame, (int)sampleRate);
	lame_set_out_samplerate(fLame, (int)sampleRate);
	lame_set_brate(fLame, bitrate);
	lame_set_quality(fLame, 5);
	lame_set_mode(fLame, channels == 2 ? STEREO : MONO);
	lame_set_bWriteVbrTag(fLame, 0);
	lame_set_error_protection(fLame, 1);
	lame_set_disable_reservoir(fLame, 1);
	lame_set_strict_ISO(fLame, 0);

	if (lame_init_params(fLame) < 0) {
		TRACE_ERROR("Failed to initialize LAME parameters");
		lame_close(fLame);
		fLame = NULL;
		return B_ERROR;
	}

	TRACE_INFO("MP3 encoder initialized: %.0f Hz, %ld channels, %ld kbps",
			   sampleRate, channels, bitrate);

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
}

int32
MP3Encoder::Encode(const int16* pcm, int32 samples, uint8* outBuffer, int32 outBufferSize)
{
	TRACE_VERBOSE("samples=%ld, bufferSize=%ld", samples, outBufferSize);

	if (!fLame) {
		TRACE_ERROR("LAME encoder not initialized");
		return -1;
	}

	int32 encoded = lame_encode_buffer_interleaved(fLame,
		(short int*)pcm, samples, outBuffer, outBufferSize);

	if (encoded < 0) {
		TRACE_ERROR("LAME encoding failed: %ld", encoded);
		return -1;
	}

	TRACE_VERBOSE("Encoded %ld bytes MP3 data", encoded);
	return encoded;
}

int32
MP3Encoder::Flush(uint8* outBuffer, int32 outBufferSize)
{
	TRACE_VERBOSE("");

	if (!fLame) {
		TRACE_WARNING("LAME encoder not initialized on flush");
		return 0;
	}

	int32 flushed = lame_encode_flush(fLame, outBuffer, outBufferSize);
	TRACE_INFO("Flushed %ld bytes from MP3 encoder", flushed);

	return flushed;
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
		case CODEC_PCM: return "PCM (WAV)";
#ifdef HAVE_LAME
		case CODEC_MP3: return "MP3";
#endif
		default: return "Unknown";
	}
}
