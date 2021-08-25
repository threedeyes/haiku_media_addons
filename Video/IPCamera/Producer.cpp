/*
 * Copyright 2021, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Buffer.h>
#include <BufferGroup.h>
#include <ParameterWeb.h>
#include <TimeSource.h>

#include <Autolock.h>

#include <iostream>

#include "Producer.h"
#include "Icons.h"

VideoProducer::VideoProducer(
		BMediaAddOn *addon, const char *name, int32 internal_id)
  :	BMediaNode(name),
	BMediaEventLooper(),
	BBufferProducer(B_MEDIA_RAW_VIDEO),
	BControllable()
	,fInitStatus(B_NO_INIT)
	,fInternalID(internal_id)
	,fAddOn(addon)
	,fBufferGroup(NULL)
	,fFrameGeneratorThread(-1)
	,fFFMEGReaderThread(-1)
	,fFrameSync(-1)
	,fProcessingLatency(0LL)
	,fRunning(false)
	,fConnected(false)
	,fEnabled(false)
	,fStreamConnected(false)
	,fURL("rtsp://")
	,fReconnectTime(0)
	,fKeepAspect(1)
	,fFlipVertical(0)
	,fFlipHorizontal(0)
	,fBrightness(0)
	,fContrast(0)
	,fSaturation(0)
	,pFrameRGB(NULL)
{
	fOutput.destination = media_destination::null;
	LoadAddonSettings();
	fCameraIcon = new BBitmap(BRect(0, 0, 255, 255), B_RGB32);
	BIconUtils::GetVectorIcon(kWebCameraIcon, sizeof(kWebCameraIcon), fCameraIcon);
	fLEDIcon = new BBitmap(BRect(0, 0, 64, 64), B_RGB32);
	BIconUtils::GetVectorIcon(kLEDIcon, sizeof(kLEDIcon), fLEDIcon);
	fInitStatus = B_OK;
	return;
}

VideoProducer::~VideoProducer()
{
	SaveAddonSettings();

	if (fInitStatus == B_OK) {
		if (fConnected)
			Disconnect(fOutput.source, fOutput.destination);
		if (fRunning)
			HandleStop();
	}

	delete fCameraIcon;
	delete fLEDIcon;
}

/* BMediaNode */

port_id
VideoProducer::ControlPort() const
{
	return BMediaNode::ControlPort();
}

BMediaAddOn *
VideoProducer::AddOn(int32 *internal_id) const
{
	if (internal_id)
		*internal_id = fInternalID;
	return fAddOn;
}

status_t 
VideoProducer::HandleMessage(int32 message, const void *data, size_t size)
{
	return B_ERROR;
}

void
VideoProducer::SetTimeSource(BTimeSource *time_source)
{
	release_sem(fFrameSync);
}

status_t
VideoProducer::RequestCompleted(const media_request_info &info)
{
	return BMediaNode::RequestCompleted(info);
}

/* BMediaEventLooper */

void 
VideoProducer::NodeRegistered()
{
	if (fInitStatus != B_OK) {
		ReportError(B_NODE_IN_DISTRESS);
		return;
	}

	BParameterWeb* web = new BParameterWeb;

	BParameterGroup *network_group = web->MakeGroup("Network");
	BTextParameter *url = network_group->MakeTextParameter(
		P_URL, B_MEDIA_RAW_VIDEO, "URL", B_GENERIC, B_PATH_NAME_LENGTH);
	BDiscreteParameter *reconnect = network_group->MakeDiscreteParameter(
		P_RECONNECT, B_MEDIA_RAW_VIDEO, "Auto reconnect to network stream:", B_GENERIC);
	reconnect->AddItem(0, "Disabled");
	reconnect->AddItem(1, "1 sec.");
	reconnect->AddItem(5, "5 sec.");
	reconnect->AddItem(15, "15 sec.");
	reconnect->AddItem(60, "1 min.");

	BParameterGroup *video_group = web->MakeGroup("Camera");
	BParameterGroup *param_video_group = video_group->MakeGroup("Parameters");
	BDiscreteParameter *aspect = param_video_group->MakeDiscreteParameter(
		P_ASPECT, B_MEDIA_RAW_VIDEO, "Keep aspect ratio", B_ENABLE);
	BDiscreteParameter *flip_h = param_video_group->MakeDiscreteParameter(
		P_FLIP_HORIZONTAL, B_MEDIA_RAW_VIDEO, "Flip horizontal", B_ENABLE);
	BDiscreteParameter *flip_v = param_video_group->MakeDiscreteParameter(
		P_FLIP_VERTICAL, B_MEDIA_RAW_VIDEO, "Flip vertical", B_ENABLE);

	BParameterGroup *image_param_group = param_video_group->MakeGroup("Brightness");
	image_param_group->MakeContinuousParameter(
			       P_BRIGHTNESS, B_MEDIA_RAW_VIDEO, "Brightness", B_GAIN,
			         "", -100.0, 100.0, 1);
	image_param_group = param_video_group->MakeGroup("Contrast");
	image_param_group->MakeContinuousParameter(
			       P_CONTRAST, B_MEDIA_RAW_VIDEO, "Contrast", B_GAIN,
			         "", -100.0, 100.0, 1);
	image_param_group = param_video_group->MakeGroup("Saturation");
	image_param_group->MakeContinuousParameter(
			       P_SATURATION, B_MEDIA_RAW_VIDEO, "Saturation", B_GAIN,
			         "", -100.0, 100.0, 1);

	BParameterGroup *about_group = web->MakeGroup("About");
	about_group->MakeNullParameter(0, B_MEDIA_NO_TYPE,
		"URL examples:\n", B_GENERIC);
	about_group->MakeNullParameter(0, B_MEDIA_NO_TYPE,
		"http://192.168.1.123:4747/video", B_GENERIC);
	about_group->MakeNullParameter(0, B_MEDIA_NO_TYPE,
		"rtsp://user:password@ipcam.myhome.net:8080/h264_pcm.sdp", B_GENERIC);
	about_group->MakeNullParameter(0, B_MEDIA_NO_TYPE,
		"\n\n\n\n\n\n\n\n\n\n\n", B_GENERIC);

	SetParameterWeb(web);

	fOutput.node = Node();
	fOutput.source.port = ControlPort();
	fOutput.source.id = 0;
	fOutput.destination = media_destination::null;
	strcpy(fOutput.name, Name());

	fOutput.format.type = B_MEDIA_RAW_VIDEO;
	fOutput.format.u.raw_video = media_raw_video_format::wildcard;
	fOutput.format.u.raw_video.interlace = 1;
	fOutput.format.u.raw_video.display.format = B_RGB32;

//	fOutput.format.u.raw_video.display.line_width = 640;
//	fOutput.format.u.raw_video.display.line_count = 480;

	Run();
}

void 
VideoProducer::HandleEvent(const media_timed_event *event,
		bigtime_t lateness, bool realTimeEvent)
{
	switch(event->type)
	{
		case BTimedEventQueue::B_START:
			HandleStart(event->event_time);
			break;
		case BTimedEventQueue::B_STOP:
			HandleStop();
			break;
		case BTimedEventQueue::B_WARP:
			HandleTimeWarp(event->bigdata);
			break;
		case BTimedEventQueue::B_SEEK:
			HandleSeek(event->bigdata);
			break;
		case BTimedEventQueue::B_HANDLE_BUFFER:
		case BTimedEventQueue::B_DATA_STATUS:
		case BTimedEventQueue::B_PARAMETER:
		default:
			break;
	}
}

void 
VideoProducer::CleanUpEvent(const media_timed_event *event)
{
	BMediaEventLooper::CleanUpEvent(event);
}

bigtime_t
VideoProducer::OfflineTime()
{
	return BMediaEventLooper::OfflineTime();
}

void
VideoProducer::ControlLoop()
{
	BMediaEventLooper::ControlLoop();
}

status_t
VideoProducer::DeleteHook(BMediaNode * node)
{
	return BMediaEventLooper::DeleteHook(node);
}

/* BBufferProducer */

status_t 
VideoProducer::FormatSuggestionRequested(
		media_type type, int32 quality, media_format *format)
{
	if (type != B_MEDIA_ENCODED_VIDEO)
		return B_MEDIA_BAD_FORMAT;

	*format = fOutput.format;
	return B_OK;
}

status_t 
VideoProducer::FormatProposal(const media_source &output, media_format *format)
{
	status_t err;

	if (!format)
		return B_BAD_VALUE;

	if (output != fOutput.source)
		return B_MEDIA_BAD_SOURCE;

	err = format_is_compatible(*format, fOutput.format) ?
			B_OK : B_MEDIA_BAD_FORMAT;
	*format = fOutput.format;

	return err;		
}

status_t 
VideoProducer::FormatChangeRequested(const media_source &source,
		const media_destination &destination, media_format *io_format,
		int32 *_deprecated_)
{
	if (source != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	return B_ERROR;	
}

status_t 
VideoProducer::GetNextOutput(int32 *cookie, media_output *out_output)
{
	if (!out_output)
		return B_BAD_VALUE;

	if ((*cookie) != 0)
		return B_BAD_INDEX;
	
	*out_output = fOutput;
	(*cookie)++;
	return B_OK;
}

status_t 
VideoProducer::GetLatency(bigtime_t *out_latency)
{
	*out_latency = EventLatency() + SchedulingLatency();
	return B_OK;
}

status_t 
VideoProducer::PrepareToConnect(const media_source &source,
		const media_destination &destination, media_format *format,
		media_source *out_source, char *out_name)
{
	if (fConnected)
		return EALREADY;

	if (source != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	
	if (fOutput.destination != media_destination::null)
		return B_MEDIA_ALREADY_CONNECTED;

	if (!format_is_compatible(*format, fOutput.format)) {
		*format = fOutput.format;
		return B_MEDIA_BAD_FORMAT;
	}

	format->u.raw_video.display.line_width = 640;
	format->u.raw_video.display.line_count = 480;

	if (format->u.raw_video.field_rate == 0)
		format->u.raw_video.field_rate = 29.97f;

	*out_source = fOutput.source;
	strcpy(out_name, fOutput.name);

	fOutput.destination = destination;

	return B_OK;
}

void 
VideoProducer::Connect(status_t error, const media_source &source,
		const media_destination &destination, const media_format &format,
		char *io_name)
{
	if (fConnected)
		return;

	if ((source != fOutput.source) || (error < B_OK) ||
		!const_cast<media_format *>(&format)->Matches(&fOutput.format))
		return;

	fOutput.destination = destination;
	strcpy(io_name, fOutput.name);

	if (fOutput.format.u.raw_video.field_rate != 0.0f) {
		fPerformanceTimeBase = fPerformanceTimeBase +
				(bigtime_t)
					((fFrame - fFrameBase) *
					(1000000 / fOutput.format.u.raw_video.field_rate));
		fFrameBase = fFrame;
	}
	
	fConnectedFormat = format.u.raw_video;

	bigtime_t latency = 0;
	media_node_id tsID = 0;
	FindLatencyFor(fOutput.destination, &latency, &tsID);
	#define NODE_LATENCY 1000
	SetEventLatency(latency + NODE_LATENCY);

	uint32 *buffer, *p, f = 3;
	p = buffer = (uint32 *)malloc(4 * fConnectedFormat.display.line_count *
			fConnectedFormat.display.line_width);
	if (!buffer)
		return;

	bigtime_t now = system_time();
	for (int y = 0; y < (int)fConnectedFormat.display.line_count; y++)
		for (int x = 0; x < (int)fConnectedFormat.display.line_width; x++)
			*(p++) = 0;
	fProcessingLatency = system_time() - now;
	free(buffer);

	fBufferGroup = new BBufferGroup(4 * fConnectedFormat.display.line_width *
			fConnectedFormat.display.line_count, 8);
	if (fBufferGroup->InitCheck() < B_OK) {
		delete fBufferGroup;
		fBufferGroup = NULL;
		return;
	}

	fConnected = true;
	fEnabled = true;

	release_sem(fFrameSync);
}

void 
VideoProducer::Disconnect(const media_source &source,
		const media_destination &destination)
{
	if (!fConnected)
		return;

	if ((source != fOutput.source) || (destination != fOutput.destination))
		return;

	fEnabled = false;
	fOutput.destination = media_destination::null;

	fLock.Lock();
		delete fBufferGroup;
		fBufferGroup = NULL;
	fLock.Unlock();

	fConnected = false;
}

void 
VideoProducer::EnableOutput(const media_source &source, bool enabled,
		int32 *_deprecated_)
{
	if (source != fOutput.source)
		return;
	fEnabled = enabled;
}

/* BControllable */									

status_t 
VideoProducer::GetParameterValue(
	int32 id, bigtime_t *last_change, void *value, size_t *size)
{
	switch (id) {
		case P_ASPECT:
		{
			*last_change = fLastKeepAspectChange;
			*size = sizeof(fKeepAspect);
			*((int32 *) value) = fKeepAspect;
			return B_OK;
		}
		case P_FLIP_VERTICAL:
		{
			*last_change = fLastFlipVChange;
			*size = sizeof(fFlipVertical);
			*((int32 *) value) = fFlipVertical;
			return B_OK;
		}
		case P_FLIP_HORIZONTAL:
		{
			*last_change = fLastFlipHChange;
			*size = sizeof(fFlipHorizontal);
			*((int32 *) value) = fFlipHorizontal;
			return B_OK;
		}
		case P_URL:
		{
			if (*size < fURL.Length() + 1)
				return EINVAL;
			*last_change = fLastURLChange;
			*size = fURL.Length() + 1;
			memcpy(value, fURL.String(), *size);
			return B_OK;
		}
		case P_RECONNECT:
		{
			*last_change = fLastReconnectChange;
			*size = sizeof(fReconnectTime);
			*((int32 *) value) = fReconnectTime;
			return B_OK;
		}
		case P_BRIGHTNESS:
		{
			*last_change = fLastBrightnessChange;
			*size = sizeof(float);
			*((float *) value) = fBrightness;
			return B_OK;
		}
		case P_CONTRAST:
		{
			*last_change = fLastContrastChange;
			*size = sizeof(float);
			*((float *) value) = fContrast;
			return B_OK;
		}
		case P_SATURATION:
		{
			*last_change = fLastSaturationChange;
			*size = sizeof(float);
			*((float *) value) = fSaturation;
			return B_OK;
		}
	}
	return B_BAD_VALUE;	
}

void
VideoProducer::SetParameterValue(
	int32 id, bigtime_t when, const void *value, size_t size)
{
	if (value == NULL || size == 0)
		return;
	switch (id) {
		case P_ASPECT:
		{
			fKeepAspect = *((const int32 *) value);
			fLastKeepAspectChange = when;
			break;
		}
		case P_FLIP_VERTICAL:
		{
			fFlipVertical = *((const int32 *) value);
			fLastFlipVChange = when;
			break;
		}
		case P_FLIP_HORIZONTAL:
		{
			fFlipHorizontal = *((const int32 *) value);
			fLastFlipHChange = when;
			break;
		}		
		case P_BRIGHTNESS:
		{
			fBrightness = *((const float *) value);
			fLastBrightnessChange = when;
			break;			
		}
		case P_CONTRAST:
		{
			fContrast = *((const float *) value);
			fLastContrastChange = when;
			break;			
		}
		case P_SATURATION:
		{
			fSaturation = *((const float *) value);
			fLastSaturationChange = when;
			break;
		}
		case P_RECONNECT:
		{
			fReconnectTime = *((const int32 *) value);
			fLastReconnectChange = when;
			break;
		}
		case P_URL:
		{
			fURL.SetTo((const char *)value);
			StreamReaderRestart();
			break;
		}
	}

	SaveAddonSettings();

	BroadcastNewParameterValue(when, id, const_cast<void *>(value), sizeof(int32));
}

status_t
VideoProducer::StartControlPanel(BMessenger *out_messenger)
{
	return BControllable::StartControlPanel(out_messenger);
}

/* VideoProducer */

void
VideoProducer::HandleStart(bigtime_t performance_time)
{
	if (fRunning)
		return;

	fFrame = 0;
	fFrameBase = 0;
	fPerformanceTimeBase = performance_time;

	fFrameSync = create_sem(0, "frame synchronization");
	if (fFrameSync < B_OK)
		goto err1;

	fStreamConnected = false;
	if (!StreamReaderRestart())
		goto err2;

	fFrameGeneratorThread = spawn_thread(_frame_generator_, "frame generator",
			B_NORMAL_PRIORITY, this);
	if (fFrameGeneratorThread < B_OK)
		goto err2;

	resume_thread(fFrameGeneratorThread);

	fRunning = true;
	return;

err2:
	delete_sem(fFrameSync);
err1:
	return;
}

void
VideoProducer::HandleStop(void)
{
	if (!fRunning)
		return;

	status_t retval;

	delete_sem(fFrameSync);
	wait_for_thread(fFrameGeneratorThread, &retval);
	fFrameGeneratorThread = -1;

	fStreamConnected = false;
	wait_for_thread(fFFMEGReaderThread, &retval);
	fFFMEGReaderThread = -1;

	fRunning = false;
}

void
VideoProducer::HandleTimeWarp(bigtime_t performance_time)
{
	fPerformanceTimeBase = performance_time;
	fFrameBase = fFrame;
	release_sem(fFrameSync);
}

void
VideoProducer::HandleSeek(bigtime_t performance_time)
{
	fPerformanceTimeBase = performance_time;
	fFrameBase = fFrame;
	release_sem(fFrameSync);
}

status_t
VideoProducer::OpenAddonSettings(BFile& file, uint32 mode)
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return B_ERROR;

	path.Append("IPCameraAddon");

	return file.SetTo(path.Path(), mode);
}


status_t
VideoProducer::LoadAddonSettings()
{
	BFile file;
	status_t status = OpenAddonSettings(file, B_READ_ONLY);
	if (status != B_OK)
		return status;

	BMessage settings;
	status = settings.Unflatten(&file);
	if (status != B_OK)
		return status;

	if (settings.FindString("URL", &fURL) != B_OK)
		fURL.SetTo("rtsp://");
	if (settings.FindInt32("ReconnectTime", &fReconnectTime) != B_OK)
		fReconnectTime = 0;
	if (settings.FindInt32("KeepAspect", &fKeepAspect) != B_OK)
		fKeepAspect = 1;
	if (settings.FindInt32("FlipHorizontal", &fFlipHorizontal) != B_OK)
		fFlipHorizontal = 0;
	if (settings.FindInt32("FlipVertical", &fFlipVertical) != B_OK)
		fFlipVertical = 0;
	if (settings.FindFloat("Brightness", &fBrightness) != B_OK)
		fBrightness = 0;
	if (settings.FindFloat("Contrast", &fContrast) != B_OK)
		fContrast = 0;
	if (settings.FindFloat("Saturation", &fSaturation) != B_OK)
		fSaturation = 0;
}


status_t
VideoProducer::SaveAddonSettings()
{
	BFile file;
	status_t status = OpenAddonSettings(file, B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	if (status != B_OK)
		return status;

	BMessage settings('IPCA');
	settings.AddString("URL", fURL);
	settings.AddInt32("ReconnectTime", fReconnectTime);
	settings.AddInt32("KeepAspect", fKeepAspect);
	settings.AddInt32("FlipHorizontal", fFlipHorizontal);
	settings.AddInt32("FlipVertical", fFlipVertical);
	settings.AddFloat("Brightness", fBrightness);
	settings.AddFloat("Contrast", fContrast);
	settings.AddFloat("Saturation", fSaturation);

	status = settings.Flatten(&file);

	return status;
}

int32 
VideoProducer::FrameGenerator()
{
	bigtime_t wait_until = system_time();

	while (1) {
		status_t err = acquire_sem_etc(fFrameSync, 1, B_ABSOLUTE_TIMEOUT, wait_until);

		if ((err != B_OK) && (err != B_TIMED_OUT))
			break;

		fFrame++;

		wait_until = TimeSource()->RealTimeFor(fPerformanceTimeBase +
			(bigtime_t)((fFrame - fFrameBase) *
			(1000000 / fConnectedFormat.field_rate)), 0) - fProcessingLatency;

		if (wait_until < system_time())
			continue;

		if (err == B_OK)
			continue;

		if (!fRunning || !fEnabled)
			continue;

		BAutolock _(fLock);

		BBuffer *buffer = fBufferGroup->RequestBuffer(
			4 * fConnectedFormat.display.line_width *
			fConnectedFormat.display.line_count, 0LL);
		if (!buffer)
			continue;

		media_header *h = buffer->Header();
		h->type = B_MEDIA_RAW_VIDEO;
		h->time_source = TimeSource()->ID();
		h->size_used = 4 * fConnectedFormat.display.line_width *
			fConnectedFormat.display.line_count;
		h->start_time = fPerformanceTimeBase +
			(bigtime_t)((fFrame - fFrameBase) *
			(1000000 / fConnectedFormat.field_rate));
		h->file_pos = 0;
		h->orig_size = 0;
		h->data_offset = 0;
		h->u.raw_video.field_gamma = 1.0;
		h->u.raw_video.field_sequence = fFrame;
		h->u.raw_video.field_number = 0;
		h->u.raw_video.pulldown_number = 0;
		h->u.raw_video.first_active_line = 1;
		h->u.raw_video.line_count = fConnectedFormat.display.line_count;

		if (fStreamConnected) {
			if (!fFlipVertical && !fFlipHorizontal) {
				memcpy((unsigned char*)buffer->Data(),
					(unsigned char*)pFrameRGB->data[0], buffer->Size());
			}else {
				uint32 *dst = (uint32 *)buffer->Data();
				uint32 *src = (uint32 *)pFrameRGB->data[0];
				if (fFlipVertical) {
					src+= ((int)fConnectedFormat.display.line_count - 1) *
						(int)fConnectedFormat.display.line_width;
				}

				for (int y = 0; y < (int)fConnectedFormat.display.line_count; y++) {
					if (fFlipHorizontal) {
						for(int x = 0; x < (int)fConnectedFormat.display.line_width; x++)
							dst[x] = src[((int)fConnectedFormat.display.line_width - 1) - x];
					} else
						memcpy((unsigned char*)dst, (unsigned char*)src,
							(int)fConnectedFormat.display.line_width * 4);

					dst += (int)fConnectedFormat.display.line_width;
					src += (fFlipVertical ? -1 : 1) * (int)fConnectedFormat.display.line_width;
				}
			}
		} else {
			int bufferSize = (int)fConnectedFormat.display.line_width *
				(int)fConnectedFormat.display.line_count * sizeof(uint32);

			memset(buffer->Data(), 0, bufferSize);

			if (fCameraIcon != NULL && fLEDIcon != NULL) {
				int inverse = (fFrame / 15) % 2;

				BPoint cameraPosition((fConnectedFormat.display.line_width - fCameraIcon->Bounds().IntegerWidth()) / 2,
					(fConnectedFormat.display.line_count - fCameraIcon->Bounds().IntegerHeight()) / 2);
				BPoint ledPosition = BPoint(0, 0);

				BPrivate::ConvertBits(fCameraIcon->Bits(), buffer->Data(), fCameraIcon->BitsLength(), bufferSize,
					fCameraIcon->BytesPerRow(), (int)fConnectedFormat.display.line_width * sizeof(uint32),
					B_RGBA32, B_RGB32, BPoint(0, 0), cameraPosition,
					fCameraIcon->Bounds().IntegerWidth(), fCameraIcon->Bounds().IntegerHeight());
				if (inverse) {
					BPrivate::ConvertBits(fLEDIcon->Bits(), buffer->Data(), fLEDIcon->BitsLength(), bufferSize,
						fLEDIcon->BytesPerRow(), (int)fConnectedFormat.display.line_width * sizeof(uint32),
						B_RGBA32, B_RGB32, BPoint(0, 0), ledPosition,
						fLEDIcon->Bounds().IntegerWidth(), fLEDIcon->Bounds().IntegerHeight());
				}
			}
		}

		if (SendBuffer(buffer, fOutput.source, fOutput.destination) < B_OK)
			buffer->Recycle();
	}

	return B_OK;
}

int32
VideoProducer::_frame_generator_(void *data)
{
	return ((VideoProducer *)data)->FrameGenerator();
}

int32
VideoProducer::StreamReader()
{
	AVFormatContext *pFormatCtx;
	AVCodecContext *pCodecCtx;
	AVCodec *pCodec;
	AVFrame	*pFrame;
	AVPacket *packet;
	uint8_t *out_buffer;
	int	videoindex;
	int y_size;
	int ret, got_picture;
	struct SwsContext *img_convert_ctx;
	
	av_register_all();
	avformat_network_init();
	pFormatCtx = avformat_alloc_context();

	if (avformat_open_input(&pFormatCtx, fURL.String(), NULL, NULL) != 0) {
		std::cout << "Can't open input stream." << std::endl;
		return -1;
	}

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
		std::cout << "Can't find stream information." << std::endl;
		return -1;
	}

	videoindex = -1;
	for (int i = 0; i < pFormatCtx->nb_streams; i++) {
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoindex = i;
			break;
		}
	}
	if (videoindex == -1) {
		std::cout << "Can't find a video stream." << std::endl;
		avformat_close_input(&pFormatCtx);
		return -1;
	}

	pCodecCtx = pFormatCtx->streams[videoindex]->codec;
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if (pCodec == NULL) {
		std::cout << "Can't find Codec." << std::endl;
		avformat_close_input(&pFormatCtx);
		return -1;
	}

	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)	{
		std::cout << "Can't open the selected Codec." << std::endl;
		return -1;
	}
	
	std::cout << "Time of this video: " << pFormatCtx->duration << " us." << std::endl;
	double num = pFormatCtx->streams[videoindex]->r_frame_rate.num;
	double den = pFormatCtx->streams[videoindex]->r_frame_rate.den;
	double delay = 1000000 / (num / den);

	pFrame = av_frame_alloc();
	pFrameRGB = av_frame_alloc();

	out_buffer = (uint8_t *)av_malloc(avpicture_get_size(AV_PIX_FMT_BGR0,
		fConnectedFormat.display.line_width, (int)fConnectedFormat.display.line_count));
	avpicture_fill((AVPicture *)pFrameRGB, out_buffer, AV_PIX_FMT_BGR0,
		fConnectedFormat.display.line_width, (int)fConnectedFormat.display.line_count);
	packet = (AVPacket *)av_malloc(sizeof(AVPacket));

	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
		fConnectedFormat.display.line_width, (int)fConnectedFormat.display.line_count,
		AV_PIX_FMT_BGR0, SWS_BICUBIC, NULL, NULL, NULL);

	fStreamConnected = true;

	while (av_read_frame(pFormatCtx, packet) >= 0 && fStreamConnected) {
		if (packet->stream_index == videoindex) {
			
			ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
			if (ret < 0) {
				std::cout << "Decode Error." << std::endl;
				return -1;
			}
			
			int *table;
			int *inv_table;
			int brightness, contrast, saturation, srcRange, dstRange;
			sws_getColorspaceDetails(img_convert_ctx, &inv_table, &srcRange, &table,
				&dstRange, &brightness, &contrast, &saturation);
			brightness = ((int(fBrightness) << 16) + 50) / 100;
			contrast = (((int(fContrast) + 100) << 16) + 50) / 100;
			saturation = (((int(fSaturation)+100) << 16) + 50) / 100;
			sws_setColorspaceDetails(img_convert_ctx, inv_table, srcRange, table,
				dstRange, brightness, contrast, saturation);

			if (got_picture) {
				sws_scale(img_convert_ctx, (const uint8_t* const*)pFrame->data,
					pFrame->linesize, 0, pCodecCtx->height,
					pFrameRGB->data, pFrameRGB->linesize);				
				//snooze(delay);
			}
		}
		av_free_packet(packet);
	}
	fStreamConnected = false;

	sws_freeContext(img_convert_ctx);

	av_frame_free(&pFrameRGB);
	av_frame_free(&pFrame);
	avcodec_close(pCodecCtx);
	avformat_close_input(&pFormatCtx);

	return 0;
}

int32
VideoProducer::_stream_reader_(void *data)
{
	return ((VideoProducer *)data)->StreamReader();
}

bool
VideoProducer::StreamReaderRestart()
{
	if (fFFMEGReaderThread > 0) {
		fStreamConnected = false;
		status_t retval;
		wait_for_thread(fFFMEGReaderThread, &retval);
		fFFMEGReaderThread = -1;
	}

	fFFMEGReaderThread = spawn_thread(_stream_reader_, "ffmpeg reader",
		B_NORMAL_PRIORITY, (void*)this);
	if (fFFMEGReaderThread >= B_OK) {
		resume_thread(fFFMEGReaderThread);
		return true;
	}

	return false;
}
