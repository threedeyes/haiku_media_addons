/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef NETCAST_ENCODER_H
#define NETCAST_ENCODER_H

#include <SupportDefs.h>
#include <String.h>

class NetCastEncoder {
public:
	virtual					~NetCastEncoder() {}
	
	virtual status_t		Init(float sampleRate, int32 channels, 
								 int32 bitrate) = 0;
	virtual void			Uninit() = 0;
	
	virtual int32			Encode(const int16* pcm, int32 samples,
								   uint8* outBuffer, int32 outBufferSize) = 0;
	virtual int32			Flush(uint8* outBuffer, int32 outBufferSize) = 0;
	
	virtual const char*		MimeType() const = 0;
	virtual const char*		Name() const = 0;
	virtual int32			RecommendedBufferSize(int32 pcmSamples) const = 0;
};

class PCMEncoder : public NetCastEncoder {
public:
							PCMEncoder();
	virtual					~PCMEncoder();
	
	virtual status_t		Init(float sampleRate, int32 channels, int32 bitrate);
	virtual void			Uninit();
	virtual int32			Encode(const int16* pcm, int32 samples,
								   uint8* outBuffer, int32 outBufferSize);
	virtual int32			Flush(uint8* outBuffer, int32 outBufferSize);
	virtual const char*		MimeType() const { return "audio/wav"; }
	virtual const char*		Name() const { return "PCM"; }
	virtual int32			RecommendedBufferSize(int32 pcmSamples) const;
	
private:
	float					fSampleRate;
	int32					fChannels;
	uint32					fDataSize;
};

#ifdef HAVE_LAME
#include <lame/lame.h>

class MP3Encoder : public NetCastEncoder {
public:
							MP3Encoder();
	virtual					~MP3Encoder();
	
	virtual status_t		Init(float sampleRate, int32 channels, int32 bitrate);
	virtual void			Uninit();
	virtual int32			Encode(const int16* pcm, int32 samples,
								   uint8* outBuffer, int32 outBufferSize);
	virtual int32			Flush(uint8* outBuffer, int32 outBufferSize);
	virtual const char*		MimeType() const { return "audio/mpeg"; }
	virtual const char*		Name() const { return "MP3"; }
	virtual int32			RecommendedBufferSize(int32 pcmSamples) const;
	
private:
	lame_global_flags*		fLame;
	int32					fChannels;
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
