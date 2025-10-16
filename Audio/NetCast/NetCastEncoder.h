/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef NETCAST_ENCODER_H
#define NETCAST_ENCODER_H

#include <SupportDefs.h>
#include <String.h>
#include <MediaDefs.h>

class NetCastEncoder {
public:
	virtual					~NetCastEncoder() {}
	
	virtual status_t		SetOutputFormat(float sampleRate, int32 channels,
								 int32 bitrate) = 0;
	virtual void			Uninit() = 0;

	virtual int32			EncodeBuffer(const void* inputData, int32 inputFrames,
								const media_raw_audio_format& inputFormat,
								uint8* outBuffer, int32 outBufferSize) = 0;
	virtual int32			Flush(uint8* outBuffer, int32 outBufferSize) = 0;

	virtual const char*		MimeType() const = 0;
	virtual const char*		Name() const = 0;
	virtual int32			RecommendedBufferSize(int32 pcmSamples) const = 0;
	virtual float			GetBufferMultiplier() const { return 1.0f; }

protected:
	void					ConvertToFloat(const void* input, int32 frames,
								const media_raw_audio_format& format, float* output);
	void					ResampleAndMix(const float* input, int32 inputFrames,
								int32 inputChannels, float inputRate,
								int16* output, int32* outputFrames,
								float outputRate, int32 outputChannels);
};

class PCMEncoder : public NetCastEncoder {
public:
							PCMEncoder();
	virtual					~PCMEncoder();

	virtual status_t		SetOutputFormat(float sampleRate, int32 channels, int32 bitrate);
	virtual void			Uninit();
	virtual int32			EncodeBuffer(const void* inputData, int32 inputFrames,
								const media_raw_audio_format& inputFormat,
								uint8* outBuffer, int32 outBufferSize);
	virtual int32			Flush(uint8* outBuffer, int32 outBufferSize);
	virtual const char*		MimeType() const { return "audio/wav"; }
	virtual const char*		Name() const { return "PCM"; }
	virtual int32			RecommendedBufferSize(int32 pcmSamples) const;
	virtual float			GetBufferMultiplier() const { return 1.0f; }

private:
	float					fOutputSampleRate;
	int32					fOutputChannels;
	uint32					fDataSize;

	float*					fTempFloatBuffer;
	size_t					fTempFloatBufferSize;
	int16*					fTempPCMBuffer;
	size_t					fTempPCMBufferSize;
};

#ifdef HAVE_LAME
#include <lame/lame.h>

class MP3Encoder : public NetCastEncoder {
public:
							MP3Encoder();
	virtual					~MP3Encoder();

	virtual status_t		SetOutputFormat(float sampleRate, int32 channels, int32 bitrate);
	virtual void			Uninit();
	virtual int32			EncodeBuffer(const void* inputData, int32 inputFrames,
								const media_raw_audio_format& inputFormat,
								uint8* outBuffer, int32 outBufferSize);
	virtual int32			Flush(uint8* outBuffer, int32 outBufferSize);
	virtual const char*		MimeType() const { return "audio/mpeg"; }
	virtual const char*		Name() const { return "MP3"; }
	virtual int32			RecommendedBufferSize(int32 pcmSamples) const;
	virtual float			GetBufferMultiplier() const { return 1.0f; }

	void					SetQuality(int32 quality);

private:
	lame_global_flags*		fLame;
	int32					fOutputChannels;
	float					fOutputSampleRate;
	int32					fBitrate;
	int32					fQuality;

	float*					fTempFloatBuffer;
	size_t					fTempFloatBufferSize;
	int16*					fTempPCMBuffer;
	size_t					fTempPCMBufferSize;

	uint8*					fInternalBuffer;
	size_t					fInternalBufferSize;
	size_t					fInternalBufferUsed;
	int32					fMinChunkSize;
};
#endif

class EncoderFactory {
public:
	enum CodecType {
		CODEC_PCM = 0,
#ifdef HAVE_LAME
		CODEC_MP3,
#endif
		CODEC_COUNT
	};

	static NetCastEncoder*	CreateEncoder(CodecType type);
	static const char*		GetCodecName(CodecType type);
	static int32			GetCodecCount() { return CODEC_COUNT; }
};

#endif
