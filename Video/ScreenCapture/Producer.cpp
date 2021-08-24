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
#include <Debug.h>

#include "Producer.h"

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
	,fThread(-1)
	,fFrameSync(-1)
	,fProcessingLatency(0LL)
	,fRunning(false)
	,fConnected(false)
	,fEnabled(false)
	,fDirect(1)
	,fFlipVertical(0)
	,fFlipHorizontal(0)
	,fFPS(15)
{
	fOutput.destination = media_destination::null;

	fScreen = new BScreen(B_MAIN_SCREEN_ID);
	if (fScreen->ColorSpace() != B_RGB32)
		return;

	fScreenCapture = new ScreenCapture(fScreen);
	fScreenCapture->Show();

	fBitmap = new BBitmap(fScreen->Frame(), B_RGB32);

	LoadAddonSettings();

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

		fScreenCapture->Lock();
		fScreenCapture->Quit();

		delete fBitmap;
	}
	delete fScreen;
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

	BParameterGroup *video_group = web->MakeGroup("Parameters");
	BDiscreteParameter *fps = video_group->MakeDiscreteParameter(
		P_FPS, B_MEDIA_RAW_VIDEO, "Frame rate:", B_GENERIC);
	fps->AddItem(1, "1");
	fps->AddItem(5, "5");
	fps->AddItem(10, "10");
	fps->AddItem(15, "15");
	fps->AddItem(20, "20");
	fps->AddItem(25, "25");
	fps->AddItem(30, "30");
	BDiscreteParameter *direct = video_group->MakeDiscreteParameter(
		P_DIRECT, B_MEDIA_RAW_VIDEO, "Use BDirectWindow", B_ENABLE);
	BDiscreteParameter *flip_h = video_group->MakeDiscreteParameter(
		P_FLIP_HORIZONTAL, B_MEDIA_RAW_VIDEO, "Flip horizontal", B_ENABLE);
	BDiscreteParameter *flip_v = video_group->MakeDiscreteParameter(
		P_FLIP_VERTICAL, B_MEDIA_RAW_VIDEO, "Flip vertical", B_ENABLE);

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
	if (fConnected) {
		return EALREADY;
	}

	if (source != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	
	if (fOutput.destination != media_destination::null)
		return B_MEDIA_ALREADY_CONNECTED;

	if (!format_is_compatible(*format, fOutput.format)) {
		*format = fOutput.format;
		return B_MEDIA_BAD_FORMAT;
	}

	format->u.raw_video.display.line_width = fScreen->Frame().Width() + 1;
	format->u.raw_video.display.line_count = fScreen->Frame().Height() + 1;

	if (format->u.raw_video.field_rate == 0)
		format->u.raw_video.field_rate = fFPS;

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
	if (fConnected) {
		return;
	}

	if (	(source != fOutput.source) || (error < B_OK) ||
			!const_cast<media_format *>(&format)->Matches(&fOutput.format)) {
		return;
	}

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

	/* get the latency */
	bigtime_t latency = 0;
	media_node_id tsID = 0;
	FindLatencyFor(fOutput.destination, &latency, &tsID);
	#define NODE_LATENCY 1000
	SetEventLatency(latency + NODE_LATENCY);

	uint32 *buffer, *p, f = 3;
	p = buffer = (uint32 *)malloc(4 * fConnectedFormat.display.line_count *
			fConnectedFormat.display.line_width);
	if (!buffer) {
		return;
	}
	bigtime_t now = system_time();
	for (int y = 0; y < (int)fConnectedFormat.display.line_count; y++)
		for (int x = 0; x < (int)fConnectedFormat.display.line_width; x++)
			*(p++) = 0;
	fProcessingLatency = system_time() - now;
	free(buffer);

	/* Create the buffer group */
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
	if (!fConnected) {
		return;
	}

	if ((source != fOutput.source) || (destination != fOutput.destination)) {
		return;
	}

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
		case P_FPS:
		{
			*last_change = fLastFPSChange;
			*size = sizeof(fFPS);
			*((int32 *) value) = fFPS;
			return B_OK;
		}
		case P_DIRECT:
		{
			*last_change = fLastDirectChange;
			*size = sizeof(fDirect);
			*((int32 *) value) = fDirect;
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
		case P_FPS:
		{
			fFPS = *((const int32 *) value);
			fLastFPSChange = when;
			break;
		}
		case P_DIRECT:
		{
			fDirect = *((const int32 *) value);
			fLastDirectChange = when;
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
	if (fRunning) {
		return;
	}

	fFrame = 0;
	fFrameBase = 0;
	fPerformanceTimeBase = performance_time;

	fFrameSync = create_sem(0, "frame synchronization");
	if (fFrameSync < B_OK)
		goto err1;

	fThread = spawn_thread(_frame_generator_, "frame generator",
			B_NORMAL_PRIORITY, this);
	if (fThread < B_OK)
		goto err2;

	resume_thread(fThread);

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
	if (!fRunning) {
		return;
	}

	delete_sem(fFrameSync);
	wait_for_thread(fThread, &fThread);

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



int32 
VideoProducer::FrameGenerator()
{
	bigtime_t wait_until = system_time();

	while (1) {
		status_t err = acquire_sem_etc(fFrameSync, 1, B_ABSOLUTE_TIMEOUT,
				wait_until);

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

		fScreenCapture->ReadBitmap(fBitmap, fDirect != 0);

		if (!fFlipVertical && !fFlipHorizontal) {
			memcpy((unsigned char*)buffer->Data(), (unsigned char*)fBitmap->Bits(), fBitmap->BitsLength());
		} else {
			uint32 *dst = (uint32 *)buffer->Data();
			uint32 *src = (uint32 *)fBitmap->Bits();
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

		if (SendBuffer(buffer, fOutput.source, fOutput.destination) < B_OK)
			buffer->Recycle();
	}

	return B_OK;
}

status_t
VideoProducer::OpenAddonSettings(BFile& file, uint32 mode)
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return B_ERROR;

	path.Append("ScreenCaptureAddon");

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

	if (settings.FindInt32("FPS", &fFPS) != B_OK)
		fFPS = 15.0;
	if (settings.FindInt32("FlipHorizontal", &fFlipHorizontal) != B_OK)
		fFlipHorizontal = 0;
	if (settings.FindInt32("FlipVertical", &fFlipVertical) != B_OK)
		fFlipVertical = 0;
	if (settings.FindInt32("Direct", &fDirect) != B_OK)
		fDirect = 1;
}


status_t
VideoProducer::SaveAddonSettings()
{
	BFile file;
	status_t status = OpenAddonSettings(file, B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	if (status != B_OK)
		return status;

	BMessage settings('SCRN');
	settings.AddInt32("FPS", fFPS);
	settings.AddInt32("FlipHorizontal", fFlipHorizontal);
	settings.AddInt32("FlipVertical", fFlipVertical);
	settings.AddInt32("Direct", fDirect);
	status = settings.Flatten(&file);

	return status;
}

int32
VideoProducer::_frame_generator_(void *data)
{
	return ((VideoProducer *)data)->FrameGenerator();
}
