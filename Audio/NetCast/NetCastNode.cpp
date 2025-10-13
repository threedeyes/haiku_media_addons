/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <Buffer.h>
#include <TimeSource.h>
#include <ParameterWeb.h>
#include <FindDirectory.h>
#include <Directory.h>
#include <Path.h>
#include <File.h>
#include <Message.h>
#include <ByteOrder.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <new>

#include "NetCastNode.h"
#include "NetCastDebug.h"

NetCastNode::NetCastNode(BMediaAddOn* addon, BMessage* config)
	: BMediaNode("NetCast"),
	  BBufferConsumer(B_MEDIA_RAW_AUDIO),
	  BMediaEventLooper(),
	  BControllable(),
	  fAddOn(addon),
	  fConnected(false),
	  fEncoder(NULL),
	  fCodecType(EncoderFactory::CODEC_PCM),
	  fPCMBuffer(NULL),
	  fPCMBufferSize(0),
	  fOutputBuffer(NULL),
	  fOutputBufferSize(0),
	  fWAVHeader(NULL),
	  fServerEnabled(false),
	  fServerPort(kDefaultPort),
	  fBitrate(kDefaultBitrate),
	  fBufferSize(kDefaultBufferSize),
	  fPreferredSampleRate(kDefaultSampleRate),
	  fPreferredChannels(kDefaultChannels),
	  fLastPortChange(0),
	  fLastCodecChange(0),
	  fLastBitrateChange(0),
	  fLastBufferSizeChange(0),
	  fLastSampleRateChange(0),
	  fLastChannelsChange(0),
	  fLastServerEnableChange(0),
	  fStarted(false),
	  fTSRunning(false),
	  fTSThread(-1)
{
	TRACE_CALL("addon=%p", addon);

	AddNodeKind(B_PHYSICAL_OUTPUT);

	InitDefaults();

	if (config) {
		TRACE_INFO("Loading configuration from BMessage");
		int32 value;

		if (config->FindInt32("port", &value) == B_OK) {
			fServerPort = value;
			TRACE_VERBOSE("Config: port=%ld", value);
		}

		if (config->FindInt32("bitrate", &value) == B_OK) {
			fBitrate = value;
			TRACE_VERBOSE("Config: bitrate=%ld", value);
		}

		if (config->FindInt32("buffer_size", &value) == B_OK) {
			fBufferSize = value;
			TRACE_VERBOSE("Config: buffer_size=%ld", value);
		}

		float floatValue;
		if (config->FindFloat("sample_rate", &floatValue) == B_OK) {
			fPreferredSampleRate = floatValue;
			TRACE_VERBOSE("Config: sample_rate=%.0f", floatValue);
		}

		if (config->FindInt32("channels", &value) == B_OK) {
			fPreferredChannels = value;
			TRACE_VERBOSE("Config: channels=%ld", value);
		}

		bool boolValue;
		if (config->FindBool("server_enabled", &boolValue) == B_OK) {
			fServerEnabled = boolValue;
			TRACE_VERBOSE("Config: server_enabled=%d", boolValue);
		}

		if (config->FindInt32("codec", &value) == B_OK) {
			if (value >= 0 && value < EncoderFactory::GetCodecCount()) {
				fCodecType = static_cast<EncoderFactory::CodecType>(value);
				TRACE_VERBOSE("Config: codec=%ld", value);
			}
		}
	}

	SetEventLatency(10000);

	fEncoder = EncoderFactory::CreateEncoder(fCodecType);
	if (!fEncoder) {
		TRACE_WARNING("Failed to create encoder type %d, falling back to PCM", fCodecType);
		fEncoder = EncoderFactory::CreateEncoder(EncoderFactory::CODEC_PCM);
	}

	fOutputBufferSize = fBufferSize;
	fOutputBuffer = new(std::nothrow) uint8[fOutputBufferSize];
	if (!fOutputBuffer) {
		TRACE_ERROR("Failed to allocate output buffer of size %lu", fOutputBufferSize);
	}

	fWAVHeader = new(std::nothrow) uint8[kWAVHeaderSize];
	if (!fWAVHeader) {
		TRACE_ERROR("Failed to allocate WAV header buffer");
	}

	fServer.SetListener(this);

	TRACE_INFO("NetCastNode created: port=%ld, codec=%d, bitrate=%ld",
		fServerPort, fCodecType, fBitrate);
}

NetCastNode::~NetCastNode()
{
	TRACE_CALL("");

	fServer.Stop();

	BMediaEventLooper::Quit();

	SaveSettings();

	fEncoderLock.Lock();
	if (fEncoder) {
		fEncoder->Uninit();
		delete fEncoder;
		fEncoder = NULL;
	}
	fEncoderLock.Unlock();

	delete[] fPCMBuffer;
	delete[] fOutputBuffer;
	delete[] fWAVHeader;

	TRACE_INFO("NetCastNode destroyed");
}

void
NetCastNode::InitDefaults()
{
	TRACE_CALL("");

	fServerEnabled = false;
	fServerPort = kDefaultPort;
	fBitrate = kDefaultBitrate;
	fBufferSize = kDefaultBufferSize;
	fPreferredSampleRate = kDefaultSampleRate;
	fPreferredChannels = kDefaultChannels;
	fCodecType = EncoderFactory::CODEC_PCM;

	fLastPortChange = 0;
	fLastCodecChange = 0;
	fLastBitrateChange = 0;
	fLastBufferSizeChange = 0;
	fLastSampleRateChange = 0;
	fLastChannelsChange = 0;
	fLastServerEnableChange = 0;
}

BMediaAddOn*
NetCastNode::AddOn(int32* internal_id) const
{
	TRACE_VERBOSE("");

	if (internal_id)
		*internal_id = 0;

	return fAddOn;
}

void
NetCastNode::NodeRegistered()
{
	TRACE_CALL("");

	AddNodeKind(B_TIME_SOURCE);

	fInput.node = Node();
	fInput.source = media_source::null;
	fInput.destination.port = ControlPort();
	fInput.destination.id = 0;
	fInput.format.type = B_MEDIA_RAW_AUDIO;
	fInput.format.u.raw_audio = media_raw_audio_format::wildcard;
	fInput.format.u.raw_audio.channel_count = fPreferredChannels;
	fInput.format.u.raw_audio.frame_rate = fPreferredSampleRate;
	fInput.format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_SHORT;
	fInput.format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
	strcpy(fInput.name, "audio input");

	fStarted = false;

	SetPriority(B_URGENT_PRIORITY);

	status_t loadStatus = LoadSettings();
	if (loadStatus != B_OK) {
		TRACE_WARNING("Failed to load settings: 0x%lx, using defaults", loadStatus);
		SaveSettings();
	}

	fEncoderLock.Lock();

	NetCastEncoder* savedEncoder = EncoderFactory::CreateEncoder(fCodecType);
	if (savedEncoder) {
		if (fEncoder) {
			fEncoder->Uninit();
			delete fEncoder;
		}
		fEncoder = savedEncoder;
		TRACE_INFO("Encoder recreated from settings: type=%d", fCodecType);
	} else {
		TRACE_ERROR("Failed to create encoder from settings, keeping default");
	}

	fEncoderLock.Unlock();

	BParameterWeb* web = MakeParameterWeb();
	SetParameterWeb(web);

	Run();

	if (fServerEnabled) {
		TRACE_INFO("Auto-starting server on port %ld", fServerPort);
		fServer.Start(fServerPort);
	}

	TRACE_INFO("Node registered and running");
}

void
NetCastNode::SetRunMode(run_mode mode)
{
	TRACE_CALL("mode=%d", mode);
	BMediaEventLooper::SetRunMode(mode);
}

status_t
NetCastNode::HandleMessage(int32 message, const void* data, size_t size)
{
	TRACE_VERBOSE("message=%ld, size=%lu", message, size);
	return B_ERROR;
}

status_t
NetCastNode::AcceptFormat(const media_destination& dest, media_format* format)
{
	TRACE_CALL("dest.port=%ld, dest.id=%ld", dest.port, dest.id);

	if (dest.port != ControlPort()) {
		TRACE_ERROR("Bad destination port: %ld != %ld", dest.port, ControlPort());
		return B_MEDIA_BAD_DESTINATION;
	}

	if (format->type == B_MEDIA_UNKNOWN_TYPE)
		format->type = B_MEDIA_RAW_AUDIO;

	if (format->type != B_MEDIA_RAW_AUDIO) {
		TRACE_ERROR("Bad format type: %ld", format->type);
		return B_MEDIA_BAD_FORMAT;
	}

	media_raw_audio_format& raw = format->u.raw_audio;

	if (raw.frame_rate == media_raw_audio_format::wildcard.frame_rate)
		raw.frame_rate = fPreferredSampleRate;

	if (raw.channel_count == media_raw_audio_format::wildcard.channel_count)
		raw.channel_count = fPreferredChannels;

	if (raw.frame_rate != 8000.0f && raw.frame_rate != 11025.0f &&
		raw.frame_rate != 16000.0f && raw.frame_rate != 22050.0f &&
		raw.frame_rate != 24000.0f && raw.frame_rate != 32000.0f &&
		raw.frame_rate != 44100.0f && raw.frame_rate != 48000.0f &&
		raw.frame_rate != 88200.0f && raw.frame_rate != 96000.0f) {
		TRACE_WARNING("Unsupported sample rate %.0f, using %.0f",
			raw.frame_rate, fPreferredSampleRate);
		raw.frame_rate = fPreferredSampleRate;
	}

	if (raw.channel_count < 1 || raw.channel_count > 8) {
		TRACE_WARNING("Invalid channel count %ld, using %ld",
			raw.channel_count, fPreferredChannels);
		raw.channel_count = fPreferredChannels;
	}

	if (raw.format == media_raw_audio_format::wildcard.format)
		raw.format = media_raw_audio_format::B_AUDIO_SHORT;
	
	switch (raw.format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
		case media_raw_audio_format::B_AUDIO_INT:
		case media_raw_audio_format::B_AUDIO_SHORT:
		case media_raw_audio_format::B_AUDIO_CHAR:
		case media_raw_audio_format::B_AUDIO_UCHAR:
			break;
		default:
			TRACE_WARNING("Unsupported audio format %ld, using B_AUDIO_SHORT", raw.format);
			raw.format = media_raw_audio_format::B_AUDIO_SHORT;
			break;
	}

	if (raw.byte_order == media_raw_audio_format::wildcard.byte_order)
		raw.byte_order = B_MEDIA_HOST_ENDIAN;
	
	if (raw.buffer_size == media_raw_audio_format::wildcard.buffer_size) {
		raw.buffer_size = static_cast<int32>(raw.frame_rate / 20.0f) *
						 raw.channel_count *
						 (raw.format == media_raw_audio_format::B_AUDIO_FLOAT ? 4 : 2);
	}

	TRACE_INFO("Accepted format: %.0f Hz, %ld ch, format=%ld",
		raw.frame_rate, raw.channel_count, raw.format);
	
	return B_OK;
}

status_t
NetCastNode::GetNextInput(int32* cookie, media_input* out_input)
{
	TRACE_VERBOSE("cookie=%ld", *cookie);

	if (*cookie != 0)
		return B_BAD_INDEX;

	*out_input = fInput;
	*cookie = 1;
	return B_OK;
}

void
NetCastNode::DisposeInputCookie(int32 cookie)
{
	TRACE_VERBOSE("cookie=%ld", cookie);
}

void
NetCastNode::BufferReceived(BBuffer* buffer)
{
	TRACE_VERBOSE("buffer=%p, start_time=%lld, size=%lu",
		buffer, buffer->Header()->start_time, buffer->SizeUsed());

	if (buffer->Header()->destination != fInput.destination.id) {
		TRACE_WARNING("Buffer destination mismatch: %ld != %ld",
			buffer->Header()->destination, fInput.destination.id);
		buffer->Recycle();
		return;
	}

	media_timed_event event(buffer->Header()->start_time,
		BTimedEventQueue::B_HANDLE_BUFFER, buffer,
		BTimedEventQueue::B_RECYCLE_BUFFER);
	EventQueue()->AddEvent(event);
}

void
NetCastNode::ProducerDataStatus(const media_destination& for_whom,
	int32 status, bigtime_t at_performance_time)
{
	TRACE_INFO("status=%ld, time=%lld", status, at_performance_time);
}

status_t
NetCastNode::GetLatencyFor(const media_destination& for_whom,
	bigtime_t* out_latency, media_node_id* out_timesource)
{
	TRACE_VERBOSE("");

	if (for_whom.port != ControlPort()) {
		TRACE_ERROR("Bad destination in GetLatencyFor");
		return B_MEDIA_BAD_DESTINATION;
	}

	*out_latency = EventLatency();
	*out_timesource = TimeSource()->ID();
	return B_OK;
}

status_t
NetCastNode::Connected(const media_source& producer,
	const media_destination& where, const media_format& with_format,
	media_input* out_input)
{
	TRACE_CALL("producer.port=%ld, producer.id=%ld", producer.port, producer.id);

	if (where.port != ControlPort()) {
		TRACE_ERROR("Bad destination in Connected");
		return B_MEDIA_BAD_DESTINATION;
	}

	fInput.source = producer;
	fInput.format = with_format;
	fInput.destination = where;
	*out_input = fInput;

	TRACE_INFO("Connected: %.0f Hz, %ld channels, format=%ld",
		with_format.u.raw_audio.frame_rate,
		with_format.u.raw_audio.channel_count,
		with_format.u.raw_audio.format);

	UpdateEncoder();

	fConnected = true;

	BParameterWeb* web = MakeParameterWeb();
	SetParameterWeb(web);

	return B_OK;
}

void
NetCastNode::Disconnected(const media_source& producer,
	const media_destination& where)
{
	TRACE_CALL("producer.port=%ld, producer.id=%ld", producer.port, producer.id);

	if (where.port != ControlPort() || where.id != fInput.destination.id) {
		TRACE_WARNING("Disconnection destination mismatch");
		return;
	}

	fInput.source = media_source::null;
	fConnected = false;

	TRACE_INFO("Disconnected from producer");

	fEncoderLock.Lock();

	if (fEncoder)
		fEncoder->Uninit();

	fEncoderLock.Unlock();
	
	BParameterWeb* web = MakeParameterWeb();
	SetParameterWeb(web);
}

status_t
NetCastNode::FormatChanged(const media_source& producer,
	const media_destination& consumer, int32 change_tag,
	const media_format& format)
{
	TRACE_CALL("change_tag=%ld", change_tag);

	if (consumer.port != ControlPort()) {
		TRACE_ERROR("Bad destination in FormatChanged");
		return B_MEDIA_BAD_DESTINATION;
	}

	fInput.format = format;

	TRACE_INFO("Format changed: %.0f Hz, %ld channels",
		format.u.raw_audio.frame_rate,
		format.u.raw_audio.channel_count);

	UpdateEncoder();

	return B_OK;
}

void
NetCastNode::HandleParameter(uint32 parameter)
{
	TRACE_CALL("parameter=%lu", parameter);

	switch (parameter) {
		case P_SERVER_ENABLE:
		case P_SERVER_PORT:
		case P_CODEC_TYPE:
		case P_BITRATE:
		case P_BUFFER_SIZE:
		case P_SAMPLE_RATE:
		case P_CHANNELS:
		{
			BParameterWeb* web = MakeParameterWeb();
			SetParameterWeb(web);
			break;
		}
	}
	
	SaveSettings();
}

void
NetCastNode::HandleEvent(const media_timed_event* event,
	bigtime_t lateness, bool realTimeEvent)
{
	TRACE_VERBOSE("type=%d, lateness=%lld", event->type, lateness);

	switch (event->type) {
		case BTimedEventQueue::B_HANDLE_BUFFER:
			if (fStarted)
				ProcessBuffer(static_cast<BBuffer*>(event->pointer));
			static_cast<BBuffer*>(event->pointer)->Recycle();
			break;

		case BTimedEventQueue::B_START:
			fStarted = true;
			TRACE_INFO("Node started");
			break;

		case BTimedEventQueue::B_STOP:
			fStarted = false;
			TRACE_INFO("Node stopped");
			EventQueue()->FlushEvents(0, BTimedEventQueue::B_ALWAYS, true,
				BTimedEventQueue::B_HANDLE_BUFFER);
			break;

		case BTimedEventQueue::B_PARAMETER:
			HandleParameter(event->data);
			break;
	}
}

void
NetCastNode::ProcessBuffer(BBuffer* buffer)
{
	if (!fConnected)
		return;

	const media_raw_audio_format& fmt = fInput.format.u.raw_audio;
	TRACE_VERBOSE("Processing buffer: %lu bytes, %.0f Hz, %ld ch",
		buffer->SizeUsed(), fmt.frame_rate, fmt.channel_count);

	ConvertToStereo16(buffer->Data(), buffer->SizeUsed(), fmt);
}

void
NetCastNode::ConvertToStereo16(const void* inData, size_t inSize,
	const media_raw_audio_format& fmt)
{
	int32 inChannels = fmt.channel_count;
	int32 inFrames;

	switch (fmt.format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
			inFrames = inSize / (inChannels * 4);
			break;
		case media_raw_audio_format::B_AUDIO_INT:
			inFrames = inSize / (inChannels * 4);
			break;
		case media_raw_audio_format::B_AUDIO_SHORT:
			inFrames = inSize / (inChannels * 2);
			break;
		case media_raw_audio_format::B_AUDIO_CHAR:
		case media_raw_audio_format::B_AUDIO_UCHAR:
			inFrames = inSize / inChannels;
			break;
		default:
			TRACE_ERROR("Unsupported audio format in ConvertToStereo16: %ld", fmt.format);
			return;
	}

	if (inFrames == 0) {
		TRACE_WARNING("Zero frames in buffer");
		return;
	}

	int32 outChannels = inChannels < 2 ? 2 : inChannels;
	size_t stereoSize = inFrames * outChannels * sizeof(int16);

	if (stereoSize > fPCMBufferSize) {
		delete[] fPCMBuffer;
		fPCMBuffer = new(std::nothrow) int16[inFrames * outChannels];
		if (!fPCMBuffer) {
			TRACE_ERROR("Failed to allocate PCM buffer of size %lu", stereoSize);
			fPCMBufferSize = 0;
			return;
		}
		fPCMBufferSize = stereoSize;
		TRACE_VERBOSE("Reallocated PCM buffer: %lu bytes", stereoSize);
	}

	int16* out = fPCMBuffer;

	if (fmt.format == media_raw_audio_format::B_AUDIO_FLOAT) {
		const float* in = static_cast<const float*>(inData);
		for (int32 i = 0; i < inFrames; i++) {
			for (int32 ch = 0; ch < outChannels; ch++) {
				float sample;
				if (inChannels == 1) {
					sample = in[i];
				} else if (ch < inChannels) {
					sample = in[i * inChannels + ch];
				} else {
					sample = 0.0f;
				}

				if (sample > 1.0f) sample = 1.0f;
				if (sample < -1.0f) sample = -1.0f;
				
				out[i * outChannels + ch] = static_cast<int16>(sample * 32767.0f);
			}
		}
	} else if (fmt.format == media_raw_audio_format::B_AUDIO_SHORT) {
		const int16* in = static_cast<const int16*>(inData);
		if (inChannels == outChannels) {
			memcpy(out, in, inFrames * outChannels * sizeof(int16));
		} else {
			for (int32 i = 0; i < inFrames; i++) {
				for (int32 ch = 0; ch < outChannels; ch++) {
					if (inChannels == 1) {
						out[i * outChannels + ch] = in[i];
					} else if (ch < inChannels) {
						out[i * outChannels + ch] = in[i * inChannels + ch];
					} else {
						out[i * outChannels + ch] = 0;
					}
				}
			}
		}
	} else if (fmt.format == media_raw_audio_format::B_AUDIO_INT) {
		const int32* in = static_cast<const int32*>(inData);
		for (int32 i = 0; i < inFrames; i++) {
			for (int32 ch = 0; ch < outChannels; ch++) {
				int32 sample;
				if (inChannels == 1) {
					sample = in[i];
				} else if (ch < inChannels) {
					sample = in[i * inChannels + ch];
				} else {
					sample = 0;
				}
				out[i * outChannels + ch] = static_cast<int16>(sample >> 16);
			}
		}
	} else if (fmt.format == media_raw_audio_format::B_AUDIO_CHAR) {
		const int8* in = static_cast<const int8*>(inData);
		for (int32 i = 0; i < inFrames; i++) {
			for (int32 ch = 0; ch < outChannels; ch++) {
				int8 sample;
				if (inChannels == 1) {
					sample = in[i];
				} else if (ch < inChannels) {
					sample = in[i * inChannels + ch];
				} else {
					sample = 0;
				}
				out[i * outChannels + ch] = static_cast<int16>(sample << 8);
			}
		}
	} else if (fmt.format == media_raw_audio_format::B_AUDIO_UCHAR) {
		const uint8* in = static_cast<const uint8*>(inData);
		for (int32 i = 0; i < inFrames; i++) {
			for (int32 ch = 0; ch < outChannels; ch++) {
				uint8 sample;
				if (inChannels == 1) {
					sample = in[i];
				} else if (ch < inChannels) {
					sample = in[i * inChannels + ch];
				} else {
					sample = 128;
				}
				out[i * outChannels + ch] = static_cast<int16>((int(sample) - 128) << 8);
			}
		}
	}

	TRACE_VERBOSE("Converted %ld frames to stereo16", inFrames);

	EncodeAndStream(fPCMBuffer, inFrames);
}

void
NetCastNode::EncodeAndStream(const int16* pcmData, int32 samples)
{
	if (!fEncoderLock.Lock())
		return;

	if (!fEncoder || !fOutputBuffer) {
		TRACE_WARNING("Encoder or output buffer not available");
		fEncoderLock.Unlock();
		return;
	}

	int32 encodedSize = fEncoder->Encode(pcmData, samples, 
										 fOutputBuffer, fOutputBufferSize);

	if (encodedSize > 0) {
		TRACE_VERBOSE("Broadcasting %ld encoded bytes to %ld clients",
			encodedSize, fServer.GetClientCount());
		fServer.BroadcastData(fOutputBuffer, encodedSize);
	} else if (encodedSize < 0) {
		TRACE_ERROR("Encoding failed with error: %ld", encodedSize);
	}

	fEncoderLock.Unlock();
}

void
NetCastNode::UpdateEncoder()
{
	TRACE_CALL("");

	const media_raw_audio_format& fmt = fInput.format.u.raw_audio;

	if (!fEncoderLock.Lock())
		return;

	if (fEncoder)
		fEncoder->Uninit();
	
	status_t result = fEncoder->Init(fmt.frame_rate,
									 fmt.channel_count >= 2 ? 2 : 1,
									 fBitrate);

	if (result != B_OK) {
		TRACE_ERROR("Failed to initialize encoder: 0x%lx", result);
		fEncoderLock.Unlock();
		return;
	}

	TRACE_INFO("Encoder initialized: %.0f Hz, %d ch, %ld kbps",
		fmt.frame_rate, fmt.channel_count >= 2 ? 2 : 1, fBitrate);

	int32 recommendedSize = fEncoder->RecommendedBufferSize(
		static_cast<int32>(fmt.frame_rate / 20.0f));

	if (recommendedSize > static_cast<int32>(fOutputBufferSize)) {
		delete[] fOutputBuffer;
		fOutputBufferSize = recommendedSize;
		fOutputBuffer = new(std::nothrow) uint8[fOutputBufferSize];
		if (!fOutputBuffer) {
			TRACE_ERROR("Failed to reallocate output buffer: %lu bytes", fOutputBufferSize);
			fOutputBufferSize = 0;
			fEncoderLock.Unlock();
			return;
		}
		TRACE_INFO("Reallocated output buffer: %lu bytes", fOutputBufferSize);
	}
	
	const char* mimeType = fEncoder ? fEncoder->MimeType() : "audio/wav";
	fServer.SetStreamInfo(mimeType, fBitrate);

	TRACE_INFO("Stream MIME type: %s", mimeType);

	if (fCodecType == EncoderFactory::CODEC_PCM && fWAVHeader) {
		PrepareWAVHeader();
		fServer.SendHeaderToNewClients(fWAVHeader, kWAVHeaderSize);
		TRACE_INFO("WAV header prepared for new clients");
	} else {
		fServer.SendHeaderToNewClients(NULL, 0);
		TRACE_INFO("No pre-stream header for codec");
	}

	fEncoderLock.Unlock();
}

void
NetCastNode::PrepareWAVHeader()
{
	TRACE_CALL("");

	if (!fWAVHeader)
		return;

	const media_raw_audio_format& fmt = fInput.format.u.raw_audio;
	uint32 sampleRate = static_cast<uint32>(fmt.frame_rate);
	uint16 channels = fmt.channel_count >= 2 ? 2 : 1;
	uint32 maxSize = 0xFFFFFFFF - 8;

	memcpy(fWAVHeader, "RIFF", 4);
	*reinterpret_cast<uint32*>(fWAVHeader + 4) = B_HOST_TO_LENDIAN_INT32(maxSize);
	memcpy(fWAVHeader + 8, "WAVE", 4);

	memcpy(fWAVHeader + 12, "fmt ", 4);
	*reinterpret_cast<uint32*>(fWAVHeader + 16) = B_HOST_TO_LENDIAN_INT32(16);
	*reinterpret_cast<uint16*>(fWAVHeader + 20) = B_HOST_TO_LENDIAN_INT16(1);
	*reinterpret_cast<uint16*>(fWAVHeader + 22) = B_HOST_TO_LENDIAN_INT16(channels);
	*reinterpret_cast<uint32*>(fWAVHeader + 24) = B_HOST_TO_LENDIAN_INT32(sampleRate);
	*reinterpret_cast<uint32*>(fWAVHeader + 28) = B_HOST_TO_LENDIAN_INT32(sampleRate * channels * 2);
	*reinterpret_cast<uint16*>(fWAVHeader + 32) = B_HOST_TO_LENDIAN_INT16(channels * 2);
	*reinterpret_cast<uint16*>(fWAVHeader + 34) = B_HOST_TO_LENDIAN_INT16(16);

	memcpy(fWAVHeader + 36, "data", 4);
	*reinterpret_cast<uint32*>(fWAVHeader + 40) = B_HOST_TO_LENDIAN_INT32(maxSize - 36);

	TRACE_VERBOSE("WAV header: %lu Hz, %d channels", sampleRate, channels);
}

void
NetCastNode::OnClientConnected(const char* address, const char* userAgent)
{
	TRACE_INFO("Client connected: %s [%s]", address, userAgent);
}

void
NetCastNode::OnClientDisconnected(const char* address)
{
	TRACE_INFO("Client disconnected: %s", address);
}

void
NetCastNode::OnServerStarted(const char* url)
{
	TRACE_INFO("Server started: %s", url);

	BParameterWeb* web = MakeParameterWeb();
	SetParameterWeb(web);
}

void
NetCastNode::OnServerStopped()
{
	TRACE_INFO("Server stopped");

	BParameterWeb* web = MakeParameterWeb();
	SetParameterWeb(web);
}

void
NetCastNode::OnServerError(const char* error)
{
	TRACE_ERROR("Server error: %s", error);
}

status_t
NetCastNode::OpenSettingsFile(BFile& file, uint32 mode)
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK) {
		TRACE_ERROR("Failed to find user settings directory");
		return B_ERROR;
	}

	path.Append("Media");
	mkdir(path.Path(), 0755);
	path.Append("NetCast");

	TRACE_VERBOSE("Settings file: %s", path.Path());

	return file.SetTo(path.Path(), mode);
}

status_t
NetCastNode::LoadSettings()
{
	TRACE_CALL("");

	BFile file;
	status_t status = OpenSettingsFile(file, B_READ_ONLY);
	if (status != B_OK) {
		TRACE_WARNING("Failed to open settings file: 0x%lx", status);
		return status;
	}

	BMessage settings;
	status = settings.Unflatten(&file);
	if (status != B_OK) {
		TRACE_ERROR("Failed to unflatten settings: 0x%lx", status);
		return status;
	}

	int32 value;

	if (settings.FindInt32("port", &value) == B_OK) {
		if (value >= 1024 && value <= 65535) {
			fServerPort = value;
			TRACE_VERBOSE("Loaded port: %ld", value);
		}
	}

	if (settings.FindInt32("codec", &value) == B_OK) {
		if (value >= 0 && value < EncoderFactory::GetCodecCount()) {
			fCodecType = static_cast<EncoderFactory::CodecType>(value);
			TRACE_VERBOSE("Loaded codec: %ld", value);
		}
	}

	if (settings.FindInt32("bitrate", &value) == B_OK) {
		if (value >= 32 && value <= 320) {
			fBitrate = value;
			TRACE_VERBOSE("Loaded bitrate: %ld", value);
		}
	}

	if (settings.FindInt32("buffer_size", &value) == B_OK) {
		if (value >= 1024 && value <= 65536) {
			fBufferSize = value;
			TRACE_VERBOSE("Loaded buffer_size: %ld", value);
		}
	}

	if (settings.FindInt32("sample_rate", &value) == B_OK) {
		if (value > 0 && value <= 192000) {
			fPreferredSampleRate = static_cast<float>(value);
			TRACE_VERBOSE("Loaded sample_rate: %ld", value);
		}
	}

	if (settings.FindInt32("channels", &value) == B_OK) {
		if (value >= 1 && value <= 8) {
			fPreferredChannels = value;
			TRACE_VERBOSE("Loaded channels: %ld", value);
		}
	}

	bool enabled;
	if (settings.FindBool("server_enabled", &enabled) == B_OK) {
		fServerEnabled = enabled;
		TRACE_VERBOSE("Loaded server_enabled: %d", enabled);
	}

	TRACE_INFO("Settings loaded successfully");

	return B_OK;
}

status_t
NetCastNode::SaveSettings()
{
	TRACE_CALL("");

	BFile file;
	status_t status = OpenSettingsFile(file, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (status != B_OK) {
		TRACE_ERROR("Failed to open settings file for writing: 0x%lx", status);
		return status;
	}

	BMessage settings('NETC');
	settings.AddInt32("port", fServerPort);
	settings.AddInt32("codec", static_cast<int32>(fCodecType));
	settings.AddInt32("bitrate", fBitrate);
	settings.AddInt32("buffer_size", fBufferSize);
	settings.AddInt32("sample_rate", static_cast<int32>(fPreferredSampleRate));
	settings.AddInt32("channels", fPreferredChannels);
	settings.AddBool("server_enabled", fServerEnabled);

	status = settings.Flatten(&file);
	if (status != B_OK) {
		TRACE_ERROR("Failed to save settings: 0x%lx", status);
		return status;
	}

	TRACE_INFO("Settings saved successfully");

	return B_OK;
}

BParameterWeb*
NetCastNode::MakeParameterWeb()
{
	TRACE_CALL("");

	BParameterWeb* web = new BParameterWeb();

	BParameterGroup* mainGroup = web->MakeGroup("NetCast Settings");

	BParameterGroup* serverGroup = mainGroup->MakeGroup("Server");

	BDiscreteParameter* enableParam = serverGroup->MakeDiscreteParameter(
		P_SERVER_ENABLE, B_MEDIA_RAW_AUDIO, "Enable Server", B_ENABLE);
	enableParam->AddItem(0, "Disabled");
	enableParam->AddItem(1, "Enabled");

	if (!fServer.IsRunning()) {
		serverGroup->MakeTextParameter(
			P_SERVER_PORT, B_MEDIA_RAW_AUDIO, "Port: ", B_GENERIC, 16);
	}

	if (fServer.IsRunning()) {
		serverGroup->MakeNullParameter(0, B_MEDIA_NO_TYPE,
		"____________________________________________________________", B_GENERIC);
		serverGroup->MakeTextParameter(
			P_STREAM_URL, B_MEDIA_RAW_AUDIO, "URL: ", B_GENERIC, 256);
	}

	if (!fServer.IsRunning()) {
		BParameterGroup* encodingGroup = mainGroup->MakeGroup("Encoding");

		BDiscreteParameter* codecParam = encodingGroup->MakeDiscreteParameter(
			P_CODEC_TYPE, B_MEDIA_RAW_AUDIO, "Codec", B_GENERIC);

		for (int32 i = 0; i < EncoderFactory::GetCodecCount(); i++) {
			codecParam->AddItem(i, EncoderFactory::GetCodecName(
				static_cast<EncoderFactory::CodecType>(i)));
		}

#ifdef HAVE_LAME
		if (fCodecType == EncoderFactory::CODEC_MP3) {
			BDiscreteParameter* bitrateParam = encodingGroup->MakeDiscreteParameter(
				P_BITRATE, B_MEDIA_RAW_AUDIO, "Bitrate", B_GENERIC);
			bitrateParam->AddItem(64, "64 kbps");
			bitrateParam->AddItem(96, "96 kbps");
			bitrateParam->AddItem(128, "128 kbps");
			bitrateParam->AddItem(192, "192 kbps");
			bitrateParam->AddItem(256, "256 kbps");
			bitrateParam->AddItem(320, "320 kbps");
		}
#endif

		BDiscreteParameter* bufferParam = encodingGroup->MakeDiscreteParameter(
			P_BUFFER_SIZE, B_MEDIA_RAW_AUDIO, "Buffer Size", B_GENERIC);
		bufferParam->AddItem(1024, "1 KB");
		bufferParam->AddItem(2048, "2 KB");
		bufferParam->AddItem(4096, "4 KB");
		bufferParam->AddItem(8192, "8 KB");
		bufferParam->AddItem(16384, "16 KB");
		bufferParam->AddItem(32768, "32 KB");
	}

	if (!fConnected) {
		BParameterGroup* formatGroup = mainGroup->MakeGroup("Audio Format");

		BDiscreteParameter* rateParam = formatGroup->MakeDiscreteParameter(
			P_SAMPLE_RATE, B_MEDIA_RAW_AUDIO, "Sample Rate", B_GENERIC);
		rateParam->AddItem(11025, "11025 Hz");
		rateParam->AddItem(22050, "22050 Hz");
		rateParam->AddItem(44100, "44100 Hz");
		rateParam->AddItem(48000, "48000 Hz");

		BDiscreteParameter* channelsParam = formatGroup->MakeDiscreteParameter(
			P_CHANNELS, B_MEDIA_RAW_AUDIO, "Channels", B_GENERIC);
		channelsParam->AddItem(1, "Mono");
		channelsParam->AddItem(2, "Stereo");
	}

	return web;
}

status_t
NetCastNode::GetParameterValue(int32 id, bigtime_t* last_change,
	void* value, size_t* size)
{
	TRACE_VERBOSE("id=%ld", id);

	if (!value || !size)
		return B_BAD_VALUE;

	switch (id) {
		case P_SERVER_PORT: {
			char portStr[32];
			snprintf(portStr, sizeof(portStr), "%d", static_cast<int>(fServerPort));
			size_t len = strlen(portStr) + 1;
			if (*size < len)
				return B_NO_MEMORY;
			strcpy(static_cast<char*>(value), portStr);
			*size = len;
			*last_change = fLastPortChange;
			return B_OK;
		}

		case P_CODEC_TYPE:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = static_cast<int32>(fCodecType);
			*size = sizeof(int32);
			*last_change = fLastCodecChange;
			return B_OK;

		case P_BITRATE:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = fBitrate;
			*size = sizeof(int32);
			*last_change = fLastBitrateChange;
			return B_OK;

		case P_BUFFER_SIZE:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = fBufferSize;
			*size = sizeof(int32);
			*last_change = fLastBufferSizeChange;
			return B_OK;

		case P_SAMPLE_RATE:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = static_cast<int32>(fPreferredSampleRate);
			*size = sizeof(int32);
			*last_change = fLastSampleRateChange;
			return B_OK;

		case P_CHANNELS:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = fPreferredChannels;
			*size = sizeof(int32);
			*last_change = fLastChannelsChange;
			return B_OK;

		case P_SERVER_ENABLE:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = fServerEnabled ? 1 : 0;
			*size = sizeof(int32);
			*last_change = fLastServerEnableChange;
			return B_OK;

		case P_STREAM_URL: {
			BString url = fServer.GetStreamURL();
			if (*size < static_cast<size_t>(url.Length() + 1))
				return B_NO_MEMORY;
			strcpy(static_cast<char*>(value), url.String());
			*size = url.Length() + 1;
			*last_change = fLastServerEnableChange;
			return B_OK;
		}

		default:
			return B_BAD_VALUE;
	}
}

void
NetCastNode::SetParameterValue(int32 id, bigtime_t when,
	const void* value, size_t size)
{
	TRACE_CALL("id=%ld", id);

	if (!value || size == 0)
		return;

	switch (id) {
		case P_SERVER_PORT: {
			if (fServer.IsRunning())
				break;

			const char* portStr = static_cast<const char*>(value);
			int32 newPort = atoi(portStr);
			if (newPort >= 1024 && newPort <= 65535 && newPort != fServerPort) {
				TRACE_INFO("Port changed: %ld -> %ld", fServerPort, newPort);
				fServerPort = newPort;
				fLastPortChange = when;

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}
		
		case P_CODEC_TYPE: {
			if (fServer.IsRunning())
				break;

			int32 newCodec = *static_cast<const int32*>(value);
			if (newCodec >= 0 && newCodec < EncoderFactory::GetCodecCount()) {
				NetCastEncoder* newEncoder = EncoderFactory::CreateEncoder(
					static_cast<EncoderFactory::CodecType>(newCodec));

				if (!newEncoder) {
					TRACE_ERROR("Failed to create encoder type %ld", newCodec);
					break;
				}

				TRACE_INFO("Codec changed: %d -> %ld", fCodecType, newCodec);

				fEncoderLock.Lock();

				NetCastEncoder* oldEncoder = fEncoder;
				fEncoder = newEncoder;
				fCodecType = static_cast<EncoderFactory::CodecType>(newCodec);

				if (fConnected) {
					UpdateEncoder();
				}

				fEncoderLock.Unlock();

				if (oldEncoder) {
					oldEncoder->Uninit();
					delete oldEncoder;
				}

				fLastCodecChange = when;

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_BITRATE: {
			if (fServer.IsRunning())
				break;

			int32 newBitrate = *static_cast<const int32*>(value);
			if (newBitrate >= 32 && newBitrate <= 320 && newBitrate != fBitrate) {
				TRACE_INFO("Bitrate changed: %ld -> %ld", fBitrate, newBitrate);
				fBitrate = newBitrate;
				fLastBitrateChange = when;
				if (fConnected) {
					UpdateEncoder();
				}

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_BUFFER_SIZE: {
			if (fServer.IsRunning())
				break;

			int32 newSize = *static_cast<const int32*>(value);
			if (newSize >= 1024 && newSize <= 65536 && newSize != fBufferSize) {
				TRACE_INFO("Buffer size changed: %ld -> %ld", fBufferSize, newSize);
				fBufferSize = newSize;
				fLastBufferSizeChange = when;
				if (newSize > static_cast<int32>(fOutputBufferSize)) {
					delete[] fOutputBuffer;
					fOutputBufferSize = newSize;
					fOutputBuffer = new(std::nothrow) uint8[fOutputBufferSize];
				}

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_SAMPLE_RATE: {
			if (fConnected)
				break;

			float newRate = static_cast<float>(*static_cast<const int32*>(value));
			if (newRate != fPreferredSampleRate) {
				TRACE_INFO("Sample rate changed: %.0f -> %.0f", fPreferredSampleRate, newRate);
				fPreferredSampleRate = newRate;
				fLastSampleRateChange = when;
				fInput.format.u.raw_audio.frame_rate = newRate;

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_CHANNELS: {
			if (fConnected)
				break;

			int32 newChannels = *static_cast<const int32*>(value);
			if (newChannels >= 1 && newChannels <= 2 && newChannels != fPreferredChannels) {
				TRACE_INFO("Channels changed: %ld -> %ld", fPreferredChannels, newChannels);
				fPreferredChannels = newChannels;
				fLastChannelsChange = when;
				fInput.format.u.raw_audio.channel_count = newChannels;

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_SERVER_ENABLE: {
			bool enable = (*static_cast<const int32*>(value)) != 0;
			if (enable != fServerEnabled) {
				TRACE_INFO("Server enable changed: %d -> %d", fServerEnabled, enable);
				fServerEnabled = enable;
				fLastServerEnableChange = when;

				if (enable && !fServer.IsRunning()) {
					fServer.Start(fServerPort);
				} else if (!enable && fServer.IsRunning()) {
					fServer.Stop();
				}

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, P_SERVER_ENABLE, 0, NULL));
			}
			break;
		}
	}
}

status_t
NetCastNode::StartControlPanel(BMessenger* out_messenger)
{
	TRACE_CALL("");
	return B_ERROR;
}

status_t
NetCastNode::TimeSourceOp(const time_source_op_info& op, void* _reserved)
{
	switch (op.op) {
		case B_TIMESOURCE_START:
			if (!fTSRunning) {
				fTSRunning = true;
				fTSThread = spawn_thread(_ClockThread, "NetCast TimeSource",
					B_REAL_TIME_PRIORITY, this);
				if (fTSThread >= 0)
					resume_thread(fTSThread);
			}
			break;
		case B_TIMESOURCE_STOP:
		case B_TIMESOURCE_STOP_IMMEDIATELY:
			if (fTSRunning) {
				fTSRunning = false;
				status_t r;
				if (fTSThread >= 0) {
					wait_for_thread(fTSThread, &r);
					fTSThread = -1;
				}
				PublishTime(0, 0, 1.0f);
			}
			break;
		case B_TIMESOURCE_SEEK:
			BroadcastTimeWarp(op.real_time, op.performance_time);
			break;
		default:
			break;
	}
	return B_OK;
}

int32
NetCastNode::_ClockThread(void* data)
{
	static_cast<NetCastNode*>(data)->_ClockLoop();
	return 0;
}

void
NetCastNode::_ClockLoop()
{
	bigtime_t baseReal = system_time();
	bigtime_t basePerf = 0;
	while (fTSRunning) {
		bigtime_t now = system_time();
		bigtime_t perf = basePerf + (now - baseReal);
		PublishTime(perf, now, 1.0f);
		snooze(5000);
	}
}
