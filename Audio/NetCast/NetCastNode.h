/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef NETCAST_NODE_H
#define NETCAST_NODE_H

#include <BufferConsumer.h>
#include <MediaEventLooper.h>
#include <MediaNode.h>
#include <Controllable.h>
#include <ParameterWeb.h>
#include <Locker.h>
#include <String.h>
#include <File.h>
#include <TimeSource.h>

#include "NetCastEncoder.h"
#include "NetCastServer.h"

static const int32 kDefaultPort = 8000;
static const int32 kDefaultBitrate = 128;
static const int32 kDefaultChunkDivider = 20;
static const float kDefaultSampleRate = 44100.0f;
static const int32 kDefaultChannels = 2;
static const int32 kWAVHeaderSize = 44;

static const float kSupportedSampleRates[] = {
	11025.0f,
	22050.0f,
	44100.0f,
	48000.0f
};

class NetCastNode : public BBufferConsumer,
					 public BMediaEventLooper,
					 public BControllable,
					 public NetCastServer::Listener,
					 public BTimeSource {
public:
							NetCastNode(BMediaAddOn* addon, BMessage* config,
								image_id addonImage);
	virtual					~NetCastNode();

	virtual BMediaAddOn*	AddOn(int32* internal_id) const;
	virtual void			NodeRegistered();
	virtual void			SetRunMode(run_mode mode);

	virtual status_t		HandleMessage(int32 message, const void* data,
								size_t size);
	virtual status_t		AcceptFormat(const media_destination& dest,
								media_format* format);
	virtual status_t		GetNextInput(int32* cookie, media_input* out_input);
	virtual void			DisposeInputCookie(int32 cookie);
	virtual void			BufferReceived(BBuffer* buffer);
	virtual void			ProducerDataStatus(const media_destination& for_whom,
								int32 status, bigtime_t at_performance_time);
	virtual status_t		GetLatencyFor(const media_destination& for_whom,
								bigtime_t* out_latency,
								media_node_id* out_timesource);
	virtual status_t		Connected(const media_source& producer,
								const media_destination& where,
								const media_format& with_format,
								media_input* out_input);
	virtual void			Disconnected(const media_source& producer,
								const media_destination& where);
	virtual status_t		FormatChanged(const media_source& producer,
								const media_destination& consumer,
								int32 change_tag, const media_format& format);

	virtual void			HandleEvent(const media_timed_event* event,
								bigtime_t lateness, bool realTimeEvent);

	virtual status_t		GetParameterValue(int32 id, bigtime_t* last_change,
								void* value, size_t* size);
	virtual void			SetParameterValue(int32 id, bigtime_t when,
								const void* value, size_t size);
	virtual status_t		StartControlPanel(BMessenger* out_messenger);

	virtual void			OnClientConnected(const char* address, const char* userAgent);
	virtual void			OnClientDisconnected(const char* address);
	virtual void			OnServerStarted(const char* url);
	virtual void			OnServerStopped();
	virtual void			OnServerError(const char* error);

	virtual status_t		TimeSourceOp(const time_source_op_info& op, void* _reserved);

protected:
	virtual BParameterWeb*	MakeParameterWeb();

private:
	enum {
		P_SERVER_ENABLE = 1000,
		P_SERVER_PORT,
		P_CODEC_TYPE,
		P_BITRATE,
		P_CHUNK_SIZE,
		P_SAMPLE_RATE,
		P_CHANNELS,
		P_STREAM_URL,
		P_SERVER_URL
	};

	void					InitDefaults();
	void					ProcessBuffer(BBuffer* buffer);
	void					ConvertToPCM16(const void* inData, size_t inSize,
								const media_raw_audio_format& fmt);
	void					EncodeAndStream(const int16* pcmData, int32 samples);
	void					UpdateEncoder();
	void					PrepareWAVHeader();
	void					HandleParameter(uint32 parameter);
	bool					IsSampleRateSupported(float rate) const;

	status_t				LoadSettings();
	status_t				SaveSettings();
	status_t				OpenSettingsFile(BFile& file, uint32 mode);

	static int32			_ClockThread(void* data);
	void					_ClockLoop();

	BMediaAddOn*			fAddOn;
	image_id				fAddOnImage;
	media_input				fInput;
	bool					fConnected;

	NetCastEncoder*			fEncoder;
	BLocker					fEncoderLock;
	EncoderFactory::CodecType fCodecType;

	int16*					fPCMBuffer;
	size_t					fPCMBufferSize;

	uint8*					fOutputBuffer;
	size_t					fOutputBufferSize;

	uint8*					fWAVHeader;

	NetCastServer			fServer;
	bool					fServerEnabled;

	int32					fServerPort;
	int32					fBitrate;
	int32					fChunkDivider;
	float					fPreferredSampleRate;
	int32					fPreferredChannels;

	bool					fParametersChanged;

	bigtime_t				fLastPortChange;
	bigtime_t				fLastCodecChange;
	bigtime_t				fLastBitrateChange;
	bigtime_t				fLastChunkSizeChange;
	bigtime_t				fLastSampleRateChange;
	bigtime_t				fLastChannelsChange;
	bigtime_t				fLastServerEnableChange;

	volatile bool			fStarted;
	volatile bool			fTSRunning;
	thread_id				fTSThread;
};

#endif
