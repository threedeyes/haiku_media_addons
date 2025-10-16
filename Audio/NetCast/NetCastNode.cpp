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

NetCastNode::NetCastNode(BMediaAddOn* addon, BMessage* config, image_id addonImage)
	: BMediaNode("NetCast"),
	  BBufferConsumer(B_MEDIA_RAW_AUDIO),
	  BMediaEventLooper(),
	  BControllable(),
	  fAddOn(addon),
	  fAddOnImage(addonImage),
	  fConnected(false),
	  fEncoder(NULL),
	  fCodecType(EncoderFactory::CODEC_PCM),
	  fOutputBuffer(NULL),
	  fOutputBufferSize(0),
	  fWAVHeader(NULL),
	  fServerEnabled(false),
	  fServerPort(kDefaultPort),
	  fBitrate(kDefaultBitrate),
	  fOutputSampleRate(kDefaultOutputSampleRate),
	  fOutputChannels(kDefaultOutputChannels),
	  fMP3Quality(kDefaultMP3Quality),
	  fEncoderSettingsChanged(false),
	  fLastPortChange(0),
	  fLastCodecChange(0),
	  fLastBitrateChange(0),
	  fLastOutputSampleRateChange(0),
	  fLastOutputChannelsChange(0),
	  fLastMP3QualityChange(0),
	  fLastServerEnableChange(0),
	  fStarted(false),
	  fTSRunning(false),
	  fTSThread(-1)
{
	TRACE_CALL("addon=%p, image_id=%ld", addon, addonImage);

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

		float floatValue;
		if (config->FindFloat("output_sample_rate", &floatValue) == B_OK) {
			fOutputSampleRate = floatValue;
			TRACE_VERBOSE("Config: output_sample_rate=%.0f", floatValue);
		}

		if (config->FindInt32("output_channels", &value) == B_OK) {
			fOutputChannels = value;
			TRACE_VERBOSE("Config: output_channels=%ld", value);
		}

		if (config->FindInt32("mp3_quality", &value) == B_OK) {
			fMP3Quality = value;
			TRACE_VERBOSE("Config: mp3_quality=%ld", value);
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

	SetEventLatency(5000);

	fEncoder = EncoderFactory::CreateEncoder(fCodecType);
	if (!fEncoder) {
		TRACE_WARNING("Failed to create encoder type %d, falling back to PCM", fCodecType);
		fEncoder = EncoderFactory::CreateEncoder(EncoderFactory::CODEC_PCM);
	}

#ifdef HAVE_LAME
	if (fCodecType == EncoderFactory::CODEC_MP3) {
		MP3Encoder* mp3Encoder = dynamic_cast<MP3Encoder*>(fEncoder);
		if (mp3Encoder) {
			mp3Encoder->SetQuality(fMP3Quality);
		}
	}
#endif

	fOutputBufferSize = 16384;
	fOutputBuffer = new(std::nothrow) uint8[fOutputBufferSize];
	if (!fOutputBuffer) {
		TRACE_ERROR("Failed to allocate output buffer of size %lu", fOutputBufferSize);
	}

	fWAVHeader = new(std::nothrow) uint8[kWAVHeaderSize];
	if (!fWAVHeader) {
		TRACE_ERROR("Failed to allocate WAV header buffer");
	}

	fServer.SetListener(this);
	fServer.SetAddOnImage(fAddOnImage);

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
		if (fOutputBuffer && fOutputBufferSize > 0) {
			int32 flushedSize = fEncoder->Flush(fOutputBuffer, fOutputBufferSize);
			if (flushedSize > 0) {
				TRACE_INFO("Flushed %ld bytes on encoder shutdown", flushedSize);
				fServer.BroadcastData(fOutputBuffer, flushedSize);
			}
		}
		fEncoder->Uninit();
		delete fEncoder;
		fEncoder = NULL;
	}
	fEncoderLock.Unlock();

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
	fStreamName = "Live Audio Stream";
	fBitrate = kDefaultBitrate;
	fOutputSampleRate = kDefaultOutputSampleRate;
	fOutputChannels = kDefaultOutputChannels;
	fMP3Quality = kDefaultMP3Quality;
	fCodecType = EncoderFactory::CODEC_MP3;

	fLastPortChange = 0;
	fLastStreamNameChange = 0;
	fLastCodecChange = 0;
	fLastBitrateChange = 0;
	fLastOutputSampleRateChange = 0;
	fLastOutputChannelsChange = 0;
	fLastMP3QualityChange = 0;
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
	strcpy(fInput.name, "audio input");

	fStarted = false;

	SetPriority(B_URGENT_PRIORITY);

	status_t loadStatus = LoadSettings();
	if (loadStatus != B_OK) {
		TRACE_WARNING("Failed to load settings: 0x%lx, using defaults", loadStatus);
		SaveSettings();
	}

	fEncoderLock.Lock();

	NetCastEncoder* newEncoder = EncoderFactory::CreateEncoder(fCodecType);
	if (newEncoder) {
		if (fEncoder) {
			fEncoder->Uninit();
			delete fEncoder;
		}
		fEncoder = newEncoder;

#ifdef HAVE_LAME
		if (fCodecType == EncoderFactory::CODEC_MP3) {
			MP3Encoder* mp3Encoder = dynamic_cast<MP3Encoder*>(fEncoder);
			if (mp3Encoder) {
				mp3Encoder->SetQuality(fMP3Quality);
			}
		}
#endif

		status_t result = fEncoder->SetOutputFormat(fOutputSampleRate,
			fOutputChannels, fBitrate);

		if (result == B_OK) {
			int32 recommendedSize = fEncoder->RecommendedBufferSize(
				static_cast<int32>(fOutputSampleRate / 10));

			if (recommendedSize > static_cast<int32>(fOutputBufferSize)) {
				delete[] fOutputBuffer;
				fOutputBufferSize = recommendedSize;
				fOutputBuffer = new(std::nothrow) uint8[fOutputBufferSize];
				if (!fOutputBuffer) {
					TRACE_ERROR("Failed to allocate output buffer: %lu bytes", fOutputBufferSize);
					fOutputBufferSize = 0;
				}
			}

			const char* mimeType = fEncoder->MimeType();
			int32 actualBitrate = GetActualBitrate();
			fServer.SetStreamInfo(mimeType, actualBitrate, fOutputSampleRate, fOutputChannels);

			float bufferMultiplier = fEncoder->GetBufferMultiplier();
			fServer.SetBufferMultiplier(bufferMultiplier);

			if (fCodecType == EncoderFactory::CODEC_PCM && fWAVHeader) {
				PrepareWAVHeader();
				fServer.SendHeaderToNewClients(fWAVHeader, kWAVHeaderSize);
			} else {
				fServer.SendHeaderToNewClients(NULL, 0);
			}

			TRACE_INFO("Encoder initialized: %s @ %ld kbps, %.0f Hz, %ld ch",
				mimeType, actualBitrate, fOutputSampleRate, fOutputChannels);
		}

		TRACE_INFO("Encoder created from settings: type=%d", fCodecType);
	} else {
		TRACE_ERROR("Failed to create encoder from settings, keeping default");
	}

	fEncoderLock.Unlock();

	BParameterWeb* web = MakeParameterWeb();
	SetParameterWeb(web);

	Run();

	fServer.SetStreamName(fStreamName.String());

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

bool
NetCastNode::IsSampleRateSupported(float rate) const
{
	for (size_t i = 0; i < sizeof(kSupportedSampleRates) / sizeof(kSupportedSampleRates[0]); i++) {
		if (rate == kSupportedSampleRates[i])
			return true;
	}
	return false;
}

int32
NetCastNode::GetActualBitrate() const
{
	if (fCodecType == EncoderFactory::CODEC_PCM) {
		return static_cast<int32>(fOutputSampleRate * fOutputChannels * 16 / 1000);
	}
#ifdef HAVE_LAME
	else if (fCodecType == EncoderFactory::CODEC_MP3) {
		return fBitrate;
	}
#endif
	return 0;
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

	TRACE_INFO("Accepted format: %.0f Hz, %ld ch, format=%ld",
		format->u.raw_audio.frame_rate,
		format->u.raw_audio.channel_count,
		format->u.raw_audio.format);
	
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

	fServer.ClearClientBuffers();

	fConnected = true;

	if (RunState() == B_STARTED) {
		fStarted = true;
		TRACE_INFO("Graph already running, activating buffer processing");
	}

	UpdateEncoder();

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
	fStarted = false;

	TRACE_INFO("Disconnected from producer");

	fEncoderLock.Lock();

	if (fEncoder && fOutputBuffer && fOutputBufferSize > 0) {
		int32 flushedSize = fEncoder->Flush(fOutputBuffer, fOutputBufferSize);
		if (flushedSize > 0) {
			TRACE_INFO("Flushed %ld bytes on disconnect", flushedSize);
			fServer.BroadcastData(fOutputBuffer, flushedSize);
		}
	}

	fEncoderLock.Unlock();

	fServer.ClearClientBuffers();
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

	fServer.ClearClientBuffers();

	UpdateEncoder();

	return B_OK;
}

void
NetCastNode::HandleParameter(uint32 parameter)
{
	TRACE_CALL("parameter=%lu", parameter);

	SaveSettings();
}

void
NetCastNode::HandleEvent(const media_timed_event* event,
	bigtime_t lateness, bool realTimeEvent)
{
	TRACE_VERBOSE("type=%d, lateness=%lld", event->type, lateness);

	switch (event->type) {
		case BTimedEventQueue::B_HANDLE_BUFFER:
			if (fConnected && !fStarted) {
				fStarted = true;
				TRACE_INFO("Auto-started from incoming buffer");
			}
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

	int32 frameSize;
	switch (fmt.format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
			frameSize = fmt.channel_count * 4;
			break;
		case media_raw_audio_format::B_AUDIO_INT:
			frameSize = fmt.channel_count * 4;
			break;
		case media_raw_audio_format::B_AUDIO_SHORT:
			frameSize = fmt.channel_count * 2;
			break;
		case media_raw_audio_format::B_AUDIO_CHAR:
		case media_raw_audio_format::B_AUDIO_UCHAR:
			frameSize = fmt.channel_count;
			break;
		default:
			TRACE_ERROR("Unsupported audio format: %ld", fmt.format);
			return;
	}

	int32 frames = buffer->SizeUsed() / frameSize;

	TRACE_VERBOSE("Processing buffer: %ld frames, %.0f Hz, %ld ch",
		frames, fmt.frame_rate, fmt.channel_count);

	EncodeAndStream(buffer->Data(), frames, fmt);
}

void
NetCastNode::EncodeAndStream(const void* data, int32 frames,
	const media_raw_audio_format& format)
{
	if (!fEncoderLock.Lock())
		return;

	if (!fEncoder || !fOutputBuffer) {
		TRACE_WARNING("Encoder or output buffer not available");
		fEncoderLock.Unlock();
		return;
	}

	int32 encodedSize = fEncoder->EncodeBuffer(data, frames, format,
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

	if (!fEncoderLock.Lock())
		return;

	if (fEncoder && fOutputBuffer && fOutputBufferSize > 0) {
		int32 flushedSize = fEncoder->Flush(fOutputBuffer, fOutputBufferSize);
		if (flushedSize > 0) {
			TRACE_INFO("Flushed %ld bytes before encoder update", flushedSize);
			fServer.BroadcastData(fOutputBuffer, flushedSize);
		}
		fEncoder->Uninit();
	}
	
	status_t result = fEncoder->SetOutputFormat(fOutputSampleRate,
									 fOutputChannels,
									 fBitrate);

	if (result != B_OK) {
		TRACE_ERROR("Failed to initialize encoder: 0x%lx", result);
		fEncoderLock.Unlock();
		return;
	}

	TRACE_INFO("Encoder initialized: %.0f Hz, %ld ch, %ld kbps",
		fOutputSampleRate, fOutputChannels, fBitrate);

	int32 recommendedSize = fEncoder->RecommendedBufferSize(
		static_cast<int32>(fOutputSampleRate / 10));

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
	int32 actualBitrate = GetActualBitrate();
	fServer.SetStreamInfo(mimeType, actualBitrate, fOutputSampleRate, fOutputChannels);

	float bufferMultiplier = fEncoder ? fEncoder->GetBufferMultiplier() : 1.0f;
	fServer.SetBufferMultiplier(bufferMultiplier);

	TRACE_INFO("Stream info updated: %s @ %ld kbps, %.0f Hz, %ld ch (buffer mult: %.1f)",
		mimeType, actualBitrate, fOutputSampleRate, fOutputChannels, bufferMultiplier);

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

	uint32 sampleRate = static_cast<uint32>(fOutputSampleRate);
	uint16 channels = static_cast<uint16>(fOutputChannels);
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

	BString str;
	if (settings.FindString("stream_name", &str) == B_OK) {
		fStreamName = str;
		TRACE_VERBOSE("Loaded stream_name: %s", str.String());
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

	if (settings.FindInt32("output_sample_rate", &value) == B_OK) {
		if (IsSampleRateSupported(static_cast<float>(value))) {
			fOutputSampleRate = static_cast<float>(value);
			TRACE_VERBOSE("Loaded output_sample_rate: %ld", value);
		}
	}

	if (settings.FindInt32("output_channels", &value) == B_OK) {
		if (value >= 1 && value <= 2) {
			fOutputChannels = value;
			TRACE_VERBOSE("Loaded output_channels: %ld", value);
		}
	}

	if (settings.FindInt32("mp3_quality", &value) == B_OK) {
		if (value >= 0 && value <= 9) {
			fMP3Quality = value;
			TRACE_VERBOSE("Loaded mp3_quality: %ld", value);
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
	settings.AddString("stream_name", fStreamName);
	settings.AddInt32("codec", static_cast<int32>(fCodecType));
	settings.AddInt32("bitrate", fBitrate);
	settings.AddInt32("output_sample_rate", static_cast<int32>(fOutputSampleRate));
	settings.AddInt32("output_channels", fOutputChannels);
	settings.AddInt32("mp3_quality", fMP3Quality);
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

	BParameterGroup* formatGroup = mainGroup->MakeGroup("Output Format");

	BDiscreteParameter* rateParam = formatGroup->MakeDiscreteParameter(
		P_OUTPUT_SAMPLE_RATE, B_MEDIA_NO_TYPE, "Sample Rate", B_GENERIC);
	for (size_t i = 0; i < sizeof(kSupportedSampleRates) / sizeof(kSupportedSampleRates[0]); i++) {
		int32 rate = static_cast<int32>(kSupportedSampleRates[i]);
		BString label;
		label << rate << " Hz";
		rateParam->AddItem(rate, label.String());
	}

	BDiscreteParameter* channelsParam = formatGroup->MakeDiscreteParameter(
		P_OUTPUT_CHANNELS, B_MEDIA_NO_TYPE, "Channels", B_GENERIC);
	channelsParam->AddItem(1, "Mono");
	channelsParam->AddItem(2, "Stereo");

	formatGroup->MakeNullParameter(0, B_MEDIA_NO_TYPE, "", B_GENERIC);

	BDiscreteParameter* codecParam = formatGroup->MakeDiscreteParameter(
		P_CODEC_TYPE, B_MEDIA_NO_TYPE, "Codec", B_GENERIC);
	for (int32 i = 0; i < EncoderFactory::GetCodecCount(); i++) {
		codecParam->AddItem(i, EncoderFactory::GetCodecName(
			static_cast<EncoderFactory::CodecType>(i)));
	}

#ifdef HAVE_LAME
	if (fCodecType == EncoderFactory::CODEC_MP3) {
		BDiscreteParameter* bitrateParam = formatGroup->MakeDiscreteParameter(
			P_BITRATE, B_MEDIA_NO_TYPE, "Bitrate", B_GENERIC);
		bitrateParam->AddItem(64, "64 kbps");
		bitrateParam->AddItem(96, "96 kbps");
		bitrateParam->AddItem(128, "128 kbps");
		bitrateParam->AddItem(192, "192 kbps");
		bitrateParam->AddItem(256, "256 kbps");
		bitrateParam->AddItem(320, "320 kbps");

		BDiscreteParameter* qualityParam = formatGroup->MakeDiscreteParameter(
			P_MP3_QUALITY, B_MEDIA_NO_TYPE, "Quality", B_GENERIC);
		qualityParam->AddItem(0, "Best (0)");
		qualityParam->AddItem(2, "High (2)");
		qualityParam->AddItem(5, "Medium (5)");
		qualityParam->AddItem(7, "Low (7)");
		qualityParam->AddItem(9, "Fast (9)");
	}
#endif

	if (fEncoderSettingsChanged) {
		formatGroup->MakeNullParameter(0, B_MEDIA_NO_TYPE, "", B_GENERIC);
		formatGroup->MakeNullParameter(0, B_MEDIA_NO_TYPE,
			"Restart Media Services to apply changes", B_GENERIC);
	}

	BParameterGroup* serverGroup = mainGroup->MakeGroup("Server Control");

	BDiscreteParameter* enableParam = serverGroup->MakeDiscreteParameter(
		P_SERVER_ENABLE, B_MEDIA_NO_TYPE, "Enable Server", B_ENABLE);
	enableParam->AddItem(0, "Disabled");
	enableParam->AddItem(1, "Enabled");

	serverGroup->MakeTextParameter(
		P_SERVER_PORT, B_MEDIA_NO_TYPE, "Port: ", B_GENERIC, 16);

	serverGroup->MakeTextParameter(
		P_STREAM_NAME, B_MEDIA_NO_TYPE, "Stream Name: ", B_GENERIC, 128);

	serverGroup->MakeNullParameter(0, B_MEDIA_NO_TYPE,
		"\n________________________________________________________", B_GENERIC);

	serverGroup->MakeTextParameter(
		P_SERVER_URL, B_MEDIA_NO_TYPE, "Web Player: ", B_GENERIC, 256);
	serverGroup->MakeTextParameter(
		P_STREAM_URL, B_MEDIA_NO_TYPE, "Stream URL: ", B_GENERIC, 256);

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

		case P_STREAM_NAME: {
			if (*size < static_cast<size_t>(fStreamName.Length() + 1))
				return B_NO_MEMORY;
			strcpy(static_cast<char*>(value), fStreamName.String());
			*size = fStreamName.Length() + 1;
			*last_change = fLastStreamNameChange;
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

		case P_OUTPUT_SAMPLE_RATE:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = static_cast<int32>(fOutputSampleRate);
			*size = sizeof(int32);
			*last_change = fLastOutputSampleRateChange;
			return B_OK;

		case P_OUTPUT_CHANNELS:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = fOutputChannels;
			*size = sizeof(int32);
			*last_change = fLastOutputChannelsChange;
			return B_OK;

		case P_MP3_QUALITY:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = fMP3Quality;
			*size = sizeof(int32);
			*last_change = fLastMP3QualityChange;
			return B_OK;

		case P_SERVER_ENABLE:
			if (*size < sizeof(int32))
				return B_NO_MEMORY;
			*static_cast<int32*>(value) = fServerEnabled ? 1 : 0;
			*size = sizeof(int32);
			*last_change = fLastServerEnableChange;
			return B_OK;

		case P_SERVER_URL: {
			BString url;
			if (fServer.IsRunning()) {
				url = fServer.GetServerURL();
			} else {
				url = "";
			}
			if (*size < static_cast<size_t>(url.Length() + 1))
				return B_NO_MEMORY;
			strcpy(static_cast<char*>(value), url.String());
			*size = url.Length() + 1;
			*last_change = fLastServerEnableChange;
			return B_OK;
		}
		case P_STREAM_URL: {
			BString url;
			if (fServer.IsRunning()) {
				url = fServer.GetStreamURL();
			} else {
				url = "";
			}
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

	bool needsWebUpdate = false;

	switch (id) {
		case P_SERVER_PORT: {
			const char* portStr = static_cast<const char*>(value);
			int32 newPort = atoi(portStr);
			if (newPort >= 1024 && newPort <= 65535 && newPort != fServerPort) {
				TRACE_INFO("Port changed: %ld -> %ld", fServerPort, newPort);

				bool wasRunning = fServer.IsRunning();

				if (wasRunning) {
					TRACE_INFO("Stopping server to change port");
					fServer.Stop();
				}

				fServerPort = newPort;
				fLastPortChange = when;

				if (wasRunning) {
					TRACE_INFO("Restarting server on new port %ld", newPort);
					fServer.Start(fServerPort);
				}

				needsWebUpdate = true;

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_STREAM_NAME: {
			const char* newName = static_cast<const char*>(value);
			if (fStreamName != newName) {
				TRACE_INFO("Stream name changed: '%s' -> '%s'",
					fStreamName.String(), newName);
				fStreamName = newName;
				fLastStreamNameChange = when;

				fServer.SetStreamName(fStreamName.String());

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_CODEC_TYPE: {
			int32 newCodec = *static_cast<const int32*>(value);
			if (newCodec >= 0 && newCodec < EncoderFactory::GetCodecCount() &&
				newCodec != static_cast<int32>(fCodecType)) {

				TRACE_INFO("Codec type changed in settings: %d -> %ld (will apply on reconnect)",
					fCodecType, newCodec);

				fCodecType = static_cast<EncoderFactory::CodecType>(newCodec);
				fLastCodecChange = when;
				fEncoderSettingsChanged = true;
				needsWebUpdate = true;

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_BITRATE: {
			int32 newBitrate = *static_cast<const int32*>(value);
			if (newBitrate >= 32 && newBitrate <= 320 && newBitrate != fBitrate) {
				TRACE_INFO("Bitrate changed in settings: %ld -> %ld (will apply on reconnect)",
					fBitrate, newBitrate);
				fBitrate = newBitrate;
				fLastBitrateChange = when;
				fEncoderSettingsChanged = true;
				needsWebUpdate = true;

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_OUTPUT_SAMPLE_RATE: {
			float newRate = static_cast<float>(*static_cast<const int32*>(value));
			if (newRate != fOutputSampleRate) {
				TRACE_INFO("Output sample rate changed in settings: %.0f -> %.0f (will apply on reconnect)",
					fOutputSampleRate, newRate);
				fOutputSampleRate = newRate;
				fLastOutputSampleRateChange = when;
				fEncoderSettingsChanged = true;
				needsWebUpdate = true;

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_OUTPUT_CHANNELS: {
			int32 newChannels = *static_cast<const int32*>(value);
			if (newChannels >= 1 && newChannels <= 2 && newChannels != fOutputChannels) {
				TRACE_INFO("Output channels changed in settings: %ld -> %ld (will apply on reconnect)",
					fOutputChannels, newChannels);
				fOutputChannels = newChannels;
				fLastOutputChannelsChange = when;
				fEncoderSettingsChanged = true;
				needsWebUpdate = true;

				EventQueue()->AddEvent(media_timed_event(when,
					BTimedEventQueue::B_PARAMETER, NULL,
					BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL));
			}
			break;
		}

		case P_MP3_QUALITY: {
			int32 newQuality = *static_cast<const int32*>(value);
			if (newQuality >= 0 && newQuality <= 9 && newQuality != fMP3Quality) {
				TRACE_INFO("MP3 quality changed in settings: %ld -> %ld (will apply on reconnect)",
					fMP3Quality, newQuality);
				fMP3Quality = newQuality;
				fLastMP3QualityChange = when;
				fEncoderSettingsChanged = true;
				needsWebUpdate = true;

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

	if (needsWebUpdate) {
		BParameterWeb* web = MakeParameterWeb();
		SetParameterWeb(web);
	}

	SaveSettings();
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
