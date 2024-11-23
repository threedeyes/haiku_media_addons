/*
 * Copyright 2024, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Buffer.h>
#include <BufferGroup.h>
#include <ParameterWeb.h>
#include <TimeSource.h>
#include <Autolock.h>
#include <FindDirectory.h>
#include <String.h>
#include <Debug.h>

#include "Producer.h"

#include <jpeglib.h>
#include <setjmp.h>

struct JpegErrorManager {
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

void JpegErrorExit(j_common_ptr cinfo) {
	JpegErrorManager* myerr = (JpegErrorManager*)cinfo->err;
	longjmp(myerr->setjmp_buffer, 1);
}

UVCProducer::UVCProducer(
		BMediaAddOn *addon, const char *name, int32 internal_id, uvc_device_t* device)
	: BMediaNode(name)
	, BMediaEventLooper()
	, BBufferProducer(B_MEDIA_RAW_VIDEO)
	, BControllable()
	, fInitStatus(B_NO_INIT)
	, fInternalID(internal_id)
	, fAddOn(addon)
	, fBufferGroup(NULL)
	, fThread(-1)
	, fFrame(0)
	, fFrameBase(0)
	, fFrameSync(-1)
	, fPerformanceTimeBase(0)
	, fProcessingLatency(0LL)
	, fRunning(false)
	, fConnected(false)
	, fEnabled(false)
	, fDevice(device)
	, fDeviceHandle(NULL)
	, fCurrentFormatIndex(1)
	, fCurrentResolutionIndex(1)
	, fCurrentFrameRateIndex(1)
	, fFrameBuffer(NULL)
	, fFrameBufferSize(0)
	, fLastFormatChange(0)
	, fLastResolutionChange(0)
	, fLastFrameRateChange(0)
	, fLastPresetChange(0)
{
	fOutput.destination = media_destination::null;
	fOutput.format.type = B_MEDIA_RAW_VIDEO;
	fOutput.format.u.raw_video = media_raw_video_format::wildcard;
	fOutput.format.u.raw_video.display.format = B_RGB32;

	fInitStatus = SetupDevice();
}

UVCProducer::~UVCProducer()
{
	SaveAddonSettings();

	CleanupDevice();

	while (!fFormats.IsEmpty())
		delete (FormatDesc*)fFormats.RemoveItem((int32)0);

	while (!fResolutions.IsEmpty())
		delete (ResolutionDesc*)fResolutions.RemoveItem((int32)0);

	while (!fFrameRates.IsEmpty())
		delete (FrameRateDesc*)fFrameRates.RemoveItem((int32)0);

	while (!fControls.IsEmpty())
		delete (ControlDesc*)fControls.RemoveItem((int32)0);

	delete[] fFrameBuffer;
}

status_t
UVCProducer::SetupDevice()
{
	uvc_error_t res = uvc_open(fDevice, &fDeviceHandle);
	if (res < 0)
		return B_ERROR;

    
    res = uvc_get_device_descriptor(fDevice, &fDeviceDescriptor);
    if (res < 0)	        
        return B_ERROR;

	return B_OK;
}

void
UVCProducer::CleanupDevice()
{	
	if (fInitStatus == B_OK) {
		if (fConnected)
			Disconnect(fOutput.source, fOutput.destination);

		if (fRunning)
			HandleStop();
	}
	
	if (fDeviceDescriptor) {
		uvc_free_device_descriptor(fDeviceDescriptor);
		fDeviceDescriptor = NULL;
	}

	if (fDeviceHandle) {
		uvc_close(fDeviceHandle);
		fDeviceHandle = NULL;
	}
}

status_t
UVCProducer::CollectFormats()
{
	const uvc_format_desc_t *format_desc = uvc_get_format_descs(fDeviceHandle);
	
	while (format_desc) {
		if (format_desc->bDescriptorSubtype == UVC_VS_FORMAT_MJPEG) {
			FormatDesc* desc = new FormatDesc;
			desc->index = format_desc->bFormatIndex;
			desc->format = UVC_FRAME_FORMAT_MJPEG;
			strcpy(desc->name, "MJPEG");
			fFormats.AddItem(desc);
		}
		else if (format_desc->bDescriptorSubtype == UVC_VS_FORMAT_UNCOMPRESSED) {
			FormatDesc* desc = new FormatDesc;
			desc->index = format_desc->bFormatIndex;
			desc->format = UVC_FRAME_FORMAT_YUYV;
			strcpy(desc->name, "YUYV");
			fFormats.AddItem(desc);
		}
		format_desc = format_desc->next;
	}

	if (!fFormats.IsEmpty())
		return CollectResolutions(fCurrentFormatIndex);

	return B_ERROR;
}

status_t
UVCProducer::CollectResolutions(uint8_t formatIndex)
{
	while (!fResolutions.IsEmpty())
		delete (ResolutionDesc*)fResolutions.RemoveItem((int32)0);

	const uvc_format_desc_t *format_desc = uvc_get_format_descs(fDeviceHandle);
	while (format_desc && format_desc->bFormatIndex != formatIndex)
		format_desc = format_desc->next;

	if (!format_desc)
		return B_ERROR;

	const uvc_frame_desc_t *frame_desc = format_desc->frame_descs;
	while (frame_desc) {
		ResolutionDesc* desc = new ResolutionDesc;
		desc->width = frame_desc->wWidth;
		desc->height = frame_desc->wHeight;
		desc->index = frame_desc->bFrameIndex;

		fResolutions.AddItem(desc);
		frame_desc = frame_desc->next;
	}

	if (!fResolutions.IsEmpty())
		return CollectFrameRates(formatIndex, fCurrentResolutionIndex);

	return B_ERROR;
}

status_t
UVCProducer::CollectFrameRates(uint8_t formatIndex, uint8_t frameIndex)
{
	while (!fFrameRates.IsEmpty())
		delete (FrameRateDesc*)fFrameRates.RemoveItem((int32)0);

	const uvc_format_desc_t *format_desc = uvc_get_format_descs(fDeviceHandle);
	while (format_desc && format_desc->bFormatIndex != formatIndex)
		format_desc = format_desc->next;

	if (!format_desc)
		return B_ERROR;
	
	const uvc_frame_desc_t *frame_desc = format_desc->frame_descs;
	while (frame_desc && frame_desc->bFrameIndex != frameIndex)
		frame_desc = frame_desc->next;

	if (!frame_desc)
		return B_ERROR;

	int32 index = 1;
	const uint32_t *interval = frame_desc->intervals;
	while (interval && *interval) {
		uint32_t fps = 10000000 / *interval;
		if (fps > 0) {
			FrameRateDesc* desc = new FrameRateDesc;
			desc->fps = fps;
			desc->index = index;
			fFrameRates.AddItem(desc);
		}
		interval++;
		index++;
	}

	return fFrameRates.IsEmpty() ? B_ERROR : B_OK;
}

status_t
UVCProducer::InitControls()
{
	while (!fControls.IsEmpty())
		delete (ControlDesc*)fControls.RemoveItem((int32)0);
	
	int16_t reqInt16;
	uint16_t reqUint16;
	
	const uvc_processing_unit_t *processing_unit = uvc_get_processing_units(fDeviceHandle);
	if (processing_unit) {
		uint32_t bmControls = processing_unit->bmControls;
		if (bmControls & (1 << 0)) {
			ControlDesc *ctrl = new ControlDesc;
			ctrl->param_id = P_BRIGHTNESS;
			strcpy(ctrl->name, "Brightness");
			uvc_get_brightness(fDeviceHandle, &reqInt16, UVC_GET_MIN); ctrl->min = (float)reqInt16;
			uvc_get_brightness(fDeviceHandle, &reqInt16, UVC_GET_MAX); ctrl->max = (float)reqInt16;
			uvc_get_brightness(fDeviceHandle, &reqInt16, UVC_GET_DEF); ctrl->def = (float)reqInt16;
			ctrl->value = ctrl->def;
			ctrl->prev_value = ctrl->def;
			ctrl->changed = 0;
			fControls.AddItem(ctrl);	
		}
		if (bmControls & (1 << 1)) {
			ControlDesc *ctrl = new ControlDesc;
			ctrl->param_id = P_CONTRAST;
			strcpy(ctrl->name, "Contrast");
			uvc_get_contrast(fDeviceHandle, &reqUint16, UVC_GET_MIN); ctrl->min = (float)reqUint16;
			uvc_get_contrast(fDeviceHandle, &reqUint16, UVC_GET_MAX); ctrl->max = (float)reqUint16;
			uvc_get_contrast(fDeviceHandle, &reqUint16, UVC_GET_DEF); ctrl->def = (float)reqUint16;
			ctrl->value = ctrl->def;
			ctrl->prev_value = ctrl->def;
			ctrl->changed = 0;
			fControls.AddItem(ctrl);
		}
		if (bmControls & (1 << 2)) {
			ControlDesc *ctrl = new ControlDesc;
			ctrl->param_id = P_HUE;
			strcpy(ctrl->name, "Hue");
			uvc_get_hue(fDeviceHandle, &reqInt16, UVC_GET_MIN); ctrl->min = (float)reqInt16;
			uvc_get_hue(fDeviceHandle, &reqInt16, UVC_GET_MAX); ctrl->max = (float)reqInt16;
			uvc_get_hue(fDeviceHandle, &reqInt16, UVC_GET_DEF); ctrl->def = (float)reqInt16;
			ctrl->value = ctrl->def;
			ctrl->prev_value = ctrl->def;
			ctrl->changed = 0;
			fControls.AddItem(ctrl);
		}
		if (bmControls & (1 << 3)) {
			ControlDesc *ctrl = new ControlDesc;
			ctrl->param_id = P_SATURATION;
			strcpy(ctrl->name, "Saturation");
			uvc_get_saturation(fDeviceHandle, &reqUint16, UVC_GET_MIN); ctrl->min = (float)reqUint16;
			uvc_get_saturation(fDeviceHandle, &reqUint16, UVC_GET_MAX); ctrl->max = (float)reqUint16;
			uvc_get_saturation(fDeviceHandle, &reqUint16, UVC_GET_DEF); ctrl->def = (float)reqUint16;
			ctrl->value = ctrl->def;
			ctrl->prev_value = ctrl->def;
			ctrl->changed = 0;
			fControls.AddItem(ctrl);
		}
		return B_OK;
	}

	return B_ERROR;
}

void
UVCProducer::NodeRegistered()
{
	if (fInitStatus != B_OK) {
		ReportError(B_NODE_IN_DISTRESS);
		return;
	}
		
	InitControls();
	LoadAddonSettings();
	CollectFormats();
	MakeParameterWeb();	

	fOutput.node = Node();
	fOutput.source.port = ControlPort();
	fOutput.source.id = 0;
	fOutput.destination = media_destination::null;
	strcpy(fOutput.name, Name());
	//SetPriority(B_REAL_TIME_PRIORITY);
	Run();
}

void
UVCProducer::MakeParameterWeb()
{
	BParameterWeb* web = new BParameterWeb();
	BParameterGroup* uvc_param_group = web->MakeGroup("UVC");
	BParameterGroup *format_param_group = uvc_param_group->MakeGroup("Format");

	if (!fConnected ) {
		BDiscreteParameter* formatParam = format_param_group->MakeDiscreteParameter(
				P_FORMAT, B_MEDIA_RAW_VIDEO, "Format", B_GENERIC);
		for (int32 i = 0; i < fFormats.CountItems(); i++) {
			FormatDesc* desc = (FormatDesc*)fFormats.ItemAt(i);
			formatParam->AddItem(desc->index, desc->name);
		}

		BDiscreteParameter* resolutionParam = format_param_group->MakeDiscreteParameter(
				P_RESOLUTION, B_MEDIA_RAW_VIDEO, "Resolution", B_GENERIC);
		for (int32 i = 0; i < fResolutions.CountItems(); i++) {
			ResolutionDesc* desc = (ResolutionDesc*)fResolutions.ItemAt(i);
			char name[32];
			snprintf(name, sizeof(name), "%dx%d", desc->width, desc->height);
			resolutionParam->AddItem(desc->index, name);
		}

		BDiscreteParameter* fpsParam = format_param_group->MakeDiscreteParameter(
				P_FRAMERATE, B_MEDIA_RAW_VIDEO, "Frame Rate", B_GENERIC);
		for (int32 i = 0; i < fFrameRates.CountItems(); i++) {
			FrameRateDesc* desc = (FrameRateDesc*)fFrameRates.ItemAt(i);
			char name[32];
			snprintf(name, sizeof(name), "%d fps", desc->fps);
			fpsParam->AddItem(desc->index, name);
		}
	}

	if (!fControls.IsEmpty()) {
		BDiscreteParameter* presetParam = format_param_group->MakeDiscreteParameter(
				P_PRESET, B_MEDIA_RAW_VIDEO, "Preset", B_GENERIC);
		presetParam->AddItem(0, "Default");
		presetParam->AddItem(1, "Custom");

		for (int32 i = 0; i < fControls.CountItems(); i++) {
			ControlDesc* ctrl = (ControlDesc*)fControls.ItemAt(i);
			BParameterGroup *control_param_group = uvc_param_group->MakeGroup(ctrl->name);
			control_param_group->MakeContinuousParameter(ctrl->param_id, B_MEDIA_RAW_VIDEO,
			ctrl->name, B_GAIN, "", ctrl->min, ctrl->max, 1);
		}
	}

	SetParameterWeb(web);
}

void
UVCProducer::HandleStart(bigtime_t performance_time)
{
	if(fRunning)
		return;

	fFrame = 0;
	fFrameBase = 0;
	fPerformanceTimeBase = performance_time;

	fFrameSync = create_sem(0, "frame synchronization");
	if (fFrameSync < B_OK)
		return;

	StartStreaming();

	fThread = spawn_thread(_frame_generator, "frame generator",
			B_NORMAL_PRIORITY, this);
	if (fThread < B_OK) {
		delete_sem(fFrameSync);
		return;
	}	

	resume_thread(fThread);

	fRunning = true;	
}

void
UVCProducer::HandleStop()
{
	if (!fRunning)
		return;

	StopStreaming();

	delete_sem(fFrameSync);
	wait_for_thread(fThread, &fThread);

	fRunning = false;	
}

void
UVCProducer::HandleTimeWarp(bigtime_t performance_time)
{
	fPerformanceTimeBase = performance_time;
	fFrameBase = fFrame;
	release_sem(fFrameSync);
}

void
UVCProducer::HandleSeek(bigtime_t performance_time)
{
	fPerformanceTimeBase = performance_time;
	fFrameBase = fFrame;
	release_sem(fFrameSync);
}

void
UVCProducer::HandleParameter(uint32 parameter)
{
	if (parameter == P_FORMAT || parameter == P_RESOLUTION || parameter == P_PRESET)
		MakeParameterWeb();

	SaveAddonSettings();
}



/* BMediaNode */
port_id
UVCProducer::ControlPort() const
{
	return BMediaNode::ControlPort();
}

BMediaAddOn *
UVCProducer::AddOn(int32 *internal_id) const
{
	if (internal_id)
		*internal_id = fInternalID;

	return fAddOn;
}

status_t
UVCProducer::HandleMessage(int32 message, const void *data, size_t size)
{
	return B_ERROR;
}

void
UVCProducer::SetTimeSource(BTimeSource *time_source)
{
	release_sem(fFrameSync);
}

status_t
UVCProducer::RequestCompleted(const media_request_info &info)
{
	return BMediaNode::RequestCompleted(info);
}

/* BBufferProducer */
status_t
UVCProducer::FormatSuggestionRequested(media_type type, int32 quality,
	media_format *format)
{
	if (type != B_MEDIA_RAW_VIDEO)
		return B_MEDIA_BAD_FORMAT;

	*format = fOutput.format;
	return B_OK;
}

status_t
UVCProducer::FormatProposal(const media_source &output, media_format *format)
{
	if (!format)
		return B_BAD_VALUE;

	if (output != fOutput.source)
		return B_MEDIA_BAD_SOURCE;

	if (format->type != B_MEDIA_RAW_VIDEO)
		return B_MEDIA_BAD_FORMAT;

	*format = fOutput.format;
	return B_OK;
}

status_t
UVCProducer::FormatChangeRequested(const media_source &source,
		const media_destination &destination, media_format *io_format,
		int32 *_deprecated_)
{
	return B_ERROR;
}

status_t
UVCProducer::GetNextOutput(int32 *cookie, media_output *out_output)
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
UVCProducer::GetLatency(bigtime_t *out_latency)
{
	*out_latency = EventLatency() + SchedulingLatency();
	return B_OK;
}

status_t
UVCProducer::PrepareToConnect(const media_source &source,
		const media_destination &destination, media_format *format,
		media_source *out_source, char *out_name)
{
	if (fConnected)
		return EALREADY;

	if (source != fOutput.source)
		return B_MEDIA_BAD_SOURCE;

	if (fOutput.destination != media_destination::null)
		return B_MEDIA_ALREADY_CONNECTED;

	ResolutionDesc* resolution = NULL;
	for (int32 i = 0; i < fResolutions.CountItems(); i++) {
		ResolutionDesc* desc = (ResolutionDesc*)fResolutions.ItemAt(i);
		if (desc->index == fCurrentResolutionIndex) {
			resolution = desc;
			break;
		}
	}

	if (!resolution)
		return B_ERROR;

	FrameRateDesc* frameRate = NULL;
	for (int32 i = 0; i < fFrameRates.CountItems(); i++) {
		FrameRateDesc* desc = (FrameRateDesc*)fFrameRates.ItemAt(i);
		if (desc->index == fCurrentFrameRateIndex) {
			frameRate = desc;
			break;
		}
	}

	if (!frameRate)
		return B_ERROR;

	format->u.raw_video.display.line_width = resolution->width;
	format->u.raw_video.display.line_count = resolution->height;
	format->u.raw_video.field_rate = frameRate->fps;
	format->u.raw_video.display.format = B_RGB32;

	*out_source = fOutput.source;
	strcpy(out_name, fOutput.name);

	return B_OK;
}

void
UVCProducer::Connect(status_t error, const media_source &source,
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
	#define NODE_LATENCY 2000
	SetEventLatency(latency + NODE_LATENCY);

	fFrameBufferSize = fConnectedFormat.display.line_width * fConnectedFormat.display.line_count * 4;
	delete[] fFrameBuffer;
	fFrameBuffer = new uint8_t[fFrameBufferSize];

	bigtime_t now = system_time();
	memset(fFrameBuffer, 0, fFrameBufferSize);
	fProcessingLatency = system_time() - now;

	fBufferGroup = new BBufferGroup(fFrameBufferSize, 16);

	if (fBufferGroup->InitCheck() < B_OK) {
		delete fBufferGroup;
		fBufferGroup = NULL;
		return;
	}

	fConnected = true;
	fEnabled = true;

	MakeParameterWeb();

	release_sem(fFrameSync);
}

void
UVCProducer::Disconnect(const media_source &source,
		const media_destination &destination)
{
	if (!fConnected)
		return;

	if (source != fOutput.source || destination != fOutput.destination)
		return;

	HandleStop();

	fEnabled = false;
	fOutput.destination = media_destination::null;

	fLock.Lock();
		delete fBufferGroup;
		fBufferGroup = NULL;
	fLock.Unlock();

	fConnected = false;

	MakeParameterWeb();
}

void
UVCProducer::EnableOutput(const media_source &source, bool enabled,
		int32 *_deprecated_)
{
	if (source != fOutput.source)
		return;

	fEnabled = enabled;
}

/* BControllable */                                    
status_t
UVCProducer::GetParameterValue(int32 id, bigtime_t *last_change,
		void *value, size_t *size)
{
	if (!value || !size)
		return B_BAD_VALUE;

	switch (id) {
		case P_FORMAT:
		{
			*last_change = fLastFormatChange;
			*size = sizeof(uint32);
			*(uint32 *)value = fCurrentFormatIndex;
			break;
		}
		case P_RESOLUTION:
		{
			*last_change = fLastResolutionChange;
			*size = sizeof(uint32);
			*(uint32 *)value = fCurrentResolutionIndex;
			break;
		}
		case P_FRAMERATE:
		{
			*last_change = fLastFrameRateChange;
			*size = sizeof(uint32);
			*(uint32 *)value = fCurrentFrameRateIndex;
			break;
		}
		case P_PRESET:
		{
			bool state = 0;
			for (int32 i = 0; i < fControls.CountItems(); i++) {
				ControlDesc* ctrl = (ControlDesc*)fControls.ItemAt(i);
				if (ctrl->value != ctrl->def) {
					state = 1;
					break;
				}
			}
			*last_change = fLastPresetChange;
			*size = sizeof(uint32);
			*(uint32 *)value = state;
			break;
		}
		case P_BRIGHTNESS:
		case P_CONTRAST:
		case P_HUE:
		case P_SATURATION:
		{
			for (int32 i = 0; i < fControls.CountItems(); i++) {
				ControlDesc* ctrl = (ControlDesc*)fControls.ItemAt(i);
				if (ctrl->param_id == id) {
					*last_change = ctrl->changed;
					*size = sizeof(float);
					*(float *)value = ctrl->value;
					break;
				}
			}
			break;
		}
		default:
			return B_BAD_VALUE;
	}

	return B_OK;
}

void
UVCProducer::SetParameterValue(int32 id, bigtime_t when,
        const void *value, size_t size)
{
	if (value == nullptr || size == 0)
		return;

	bool needRestart = fRunning;

	if (needRestart)
		StopStreaming();

	switch (id) {
		case P_FORMAT:
		{
			uint32 newValue = *(uint32 *)value;
			if (newValue != fCurrentFormatIndex) {
				fCurrentFormatIndex = newValue;
				fLastFormatChange = when;
			
				CollectResolutions(fCurrentFormatIndex);
				if (!fResolutions.IsEmpty()) {
					ResolutionDesc* resolution = (ResolutionDesc*)fResolutions.ItemAt(0);
					fCurrentResolutionIndex = resolution->index;
					fLastResolutionChange = when;

					CollectFrameRates(fCurrentFormatIndex, fCurrentResolutionIndex);
					if (!fFrameRates.IsEmpty()) {
						FrameRateDesc* frameRate = (FrameRateDesc*)fFrameRates.ItemAt(0);
						fCurrentFrameRateIndex = frameRate->index;
						fLastFrameRateChange = when;
						BroadcastNewParameterValue(fLastFrameRateChange, P_FRAMERATE, &fCurrentFrameRateIndex, sizeof(fCurrentFrameRateIndex));
					}
					BroadcastNewParameterValue(fCurrentResolutionIndex, P_RESOLUTION, &fCurrentResolutionIndex, sizeof(fCurrentResolutionIndex));
				}
				BroadcastNewParameterValue(fLastFormatChange, P_FORMAT, &fCurrentFormatIndex, sizeof(fCurrentFormatIndex));
			}
			break;
		}
		case P_RESOLUTION:
		{
			uint32 newValue = *(uint32 *)value;
			if (newValue != fCurrentResolutionIndex) {
				fCurrentResolutionIndex = newValue;
				fLastResolutionChange = when;
	
				CollectFrameRates(fCurrentFormatIndex, fCurrentResolutionIndex);
				if (!fFrameRates.IsEmpty()) {
					FrameRateDesc* frameRate = (FrameRateDesc*)fFrameRates.ItemAt(0);
					fCurrentFrameRateIndex = frameRate->index;
					fLastFrameRateChange = when;
					BroadcastNewParameterValue(fLastFrameRateChange, P_FRAMERATE, &fCurrentFrameRateIndex, sizeof(fCurrentFrameRateIndex));
				}
				BroadcastNewParameterValue(fCurrentResolutionIndex, P_RESOLUTION, &fCurrentResolutionIndex, sizeof(fCurrentResolutionIndex));
			}
			break;
		}
		case P_FRAMERATE:
		{
			uint32 newValue = *(uint32 *)value;
			if (newValue != fCurrentFrameRateIndex) {
				fCurrentFrameRateIndex = newValue;
				fLastFrameRateChange = when;
				BroadcastNewParameterValue(fLastFrameRateChange, P_FRAMERATE, &fCurrentFrameRateIndex, sizeof(fCurrentFrameRateIndex));
			}
			break;
		}
		case P_PRESET:
		{
			uint32 newValue = *(uint32 *)value;
			if (newValue == 0) {
				for (int32 i = 0; i < fControls.CountItems(); i++) {
					ControlDesc* ctrl = (ControlDesc*)fControls.ItemAt(i);
					if (ctrl->value != ctrl->def) {
						float newValue = *(float *)value;
						if (newValue != ctrl->value) {
							ctrl->value = ctrl->def;
							ctrl->changed = when;
							ctrl->Apply(fDeviceHandle);
							BroadcastNewParameterValue(ctrl->changed, id, &ctrl->value, sizeof(ctrl->value));						
						}
					}
				}
				fLastPresetChange = when;
				BroadcastNewParameterValue(fLastPresetChange, P_PRESET, &newValue, sizeof(uint32));
			}
			break;
		}
		case P_BRIGHTNESS:
		case P_CONTRAST:
		case P_HUE:
		case P_SATURATION:
		{
			uint32 state = 0;
			for (int32 i = 0; i < fControls.CountItems(); i++) {
				ControlDesc* ctrl = (ControlDesc*)fControls.ItemAt(i);
				if (ctrl->param_id == id) {
					float newValue = *(float *)value;
					if (newValue != ctrl->value) {
						ctrl->value = newValue;
						ctrl->changed = when;
						ctrl->Apply(fDeviceHandle);
						BroadcastNewParameterValue(ctrl->changed, id, &ctrl->value, sizeof(ctrl->value));
					}
				}
				if (ctrl->value != ctrl->def)
					state = 1;
			}
			if (state == 1) {
				fLastPresetChange = when;
				BroadcastNewParameterValue(fLastPresetChange, P_PRESET, &state, sizeof(uint32));
			}
			break;
		}
	}

	if (needRestart)
		StartStreaming();

	EventQueue()->AddEvent(media_timed_event(when,
			BTimedEventQueue::B_PARAMETER, NULL,
			BTimedEventQueue::B_NO_CLEANUP, id, 0, NULL, 0));
}

status_t
UVCProducer::StartControlPanel(BMessenger *out_messenger)
{
	return B_ERROR;
}

/* BMediaEventLooper */
void 
UVCProducer::HandleEvent(const media_timed_event *event,
		bigtime_t lateness, bool realTimeEvent)
{
	switch(event->type) {
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
		case BTimedEventQueue::B_PARAMETER:
			HandleParameter(event->data);
			break;
		case BTimedEventQueue::B_HANDLE_BUFFER:
		case BTimedEventQueue::B_DATA_STATUS:
		default:
			break;
	}
}

void 
UVCProducer::CleanUpEvent(const media_timed_event *event)
{
	BMediaEventLooper::CleanUpEvent(event);
}

bigtime_t
UVCProducer::OfflineTime()
{
	return BMediaEventLooper::OfflineTime();
}

void
UVCProducer::ControlLoop()
{
	BMediaEventLooper::ControlLoop();
}

status_t
UVCProducer::DeleteHook(BMediaNode * node)
{
	return BMediaEventLooper::DeleteHook(node);
}

status_t
UVCProducer::OpenAddonSettings(BFile& file, uint32 mode)
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return B_ERROR;

	path.Append("UVCMediaAddon");
	mkdir(path.Path(), ALLPERMS);
	BString filename;
	filename << fDeviceDescriptor->product << " - " << fDeviceDescriptor->serialNumber;
	path.Append(filename);

	return file.SetTo(path.Path(), mode);
}

status_t
UVCProducer::LoadAddonSettings()
{
	BFile file;
	status_t status = OpenAddonSettings(file, B_READ_ONLY);
	if (status != B_OK)
		return status;

	BMessage settings;
	status = settings.Unflatten(&file);
	if (status != B_OK)
		return status;

	if (settings.FindUInt8("Format", &fCurrentFormatIndex) != B_OK)
		fCurrentFormatIndex = 1;

	if (settings.FindUInt8("Resolution", &fCurrentResolutionIndex) != B_OK)
		fCurrentResolutionIndex = 1;

	if (settings.FindUInt8("FrameRate", &fCurrentFrameRateIndex) != B_OK)
		fCurrentFrameRateIndex = 1;

	for (int32 i = 0; i < fControls.CountItems(); i++) {
		ControlDesc* ctrl = (ControlDesc*)fControls.ItemAt(i);
		if (settings.FindFloat(ctrl->name, &ctrl->value) != B_OK)
			ctrl->value = ctrl->def;
	}

	return B_OK;
}

status_t
UVCProducer::SaveAddonSettings()
{
	BFile file;
	status_t status = OpenAddonSettings(file, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (status != B_OK)
		return status;

	BMessage settings('UVC_');
	settings.AddUInt8("Format", fCurrentFormatIndex);
	settings.AddUInt8("Resolution", fCurrentResolutionIndex);
	settings.AddUInt8("FrameRate", fCurrentFrameRateIndex);

	for (int32 i = 0; i < fControls.CountItems(); i++) {
		ControlDesc* ctrl = (ControlDesc*)fControls.ItemAt(i);
		settings.AddFloat(ctrl->name, ctrl->value);
	}

	status = settings.Flatten(&file);

	return status;
}

status_t
UVCProducer::StartStreaming()
{
	if (!fDeviceHandle)
		return B_ERROR;

	FormatDesc* format = NULL;
	ResolutionDesc* resolution = NULL;
	FrameRateDesc* frameRate = NULL;

	for (int32 i = 0; i < fFormats.CountItems(); i++) {
		FormatDesc* desc = (FormatDesc*)fFormats.ItemAt(i);
		if (desc->index == fCurrentFormatIndex) {
			format = desc;
			break;
		}
	}

	for (int32 i = 0; i < fResolutions.CountItems(); i++) {
		ResolutionDesc* desc = (ResolutionDesc*)fResolutions.ItemAt(i);
		if (desc->index == fCurrentResolutionIndex) {
			resolution = desc;
			break;
		}
	}

	for (int32 i = 0; i < fFrameRates.CountItems(); i++) {
		FrameRateDesc* desc = (FrameRateDesc*)fFrameRates.ItemAt(i);
		if (desc->index == fCurrentFrameRateIndex) {
			frameRate = desc;
			break;
		}
	}

	if (!format || !resolution || !frameRate)
		return B_ERROR;

	uvc_error_t res = uvc_get_stream_ctrl_format_size(
		fDeviceHandle,
		&fStreamCtrl,
		format->format,
		resolution->width,
		resolution->height,
		frameRate->fps
	);

	if (res < 0)
		return B_ERROR;

	res = uvc_start_streaming(
		fDeviceHandle,
		&fStreamCtrl,
		_uvc_callback,
		this,
		0
	);

	return res < 0 ? B_ERROR : B_OK;
}

void
UVCProducer::StopStreaming()
{
	if (fDeviceHandle)
		uvc_stop_streaming(fDeviceHandle);
}

void
UVCProducer::_uvc_callback(uvc_frame_t *frame, void *ptr)
{
	UVCProducer *producer = static_cast<UVCProducer*>(ptr);
	producer->HandleFrame(frame);    
}

void
UVCProducer::HandleFrame(uvc_frame_t *frame)
{
	if (fFrameBuffer == NULL || fFrameBufferSize == 0)
		return;
	
	BAutolock frameLocker(fLock);

	// MJPEG frame
	if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
		struct jpeg_decompress_struct cinfo;
		struct JpegErrorManager jerr;

		cinfo.err = jpeg_std_error(&jerr.pub);
		jerr.pub.error_exit = JpegErrorExit;

		if (setjmp(jerr.setjmp_buffer)) {
			jpeg_destroy_decompress(&cinfo);
			return;
		}

		jpeg_create_decompress(&cinfo);
		jpeg_mem_src(&cinfo, (unsigned char*)frame->data, frame->data_bytes);
		jpeg_read_header(&cinfo, TRUE);
		cinfo.out_color_space = JCS_EXT_BGRA;
		jpeg_start_decompress(&cinfo);

		int row_stride = cinfo.output_width * cinfo.output_components;
		JSAMPARRAY buffer_array = (*cinfo.mem->alloc_sarray)(
				(j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

		uint8_t* out_data = fFrameBuffer;
		while (cinfo.output_scanline < cinfo.output_height) {
			jpeg_read_scanlines(&cinfo, buffer_array, 1);
			memcpy(out_data, buffer_array[0], row_stride);
			out_data += row_stride;
		}

		jpeg_finish_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
	// YUYV frame
	} else if (frame->frame_format == UVC_FRAME_FORMAT_YUYV) {
		uint8_t* yuyv_data = (uint8_t*)frame->data;
		uint8_t* rgb32_data = fFrameBuffer;

		uint8_t *prgb = (uint8_t*)fFrameBuffer;
		uint8_t *pyuv = (uint8_t*)frame->data;
		uint8_t *pyuv_end = pyuv + frame->data_bytes;

		while (pyuv < pyuv_end) {
			IYUYV2BGR_8(pyuv, prgb);	
			prgb += 4 * 8;
			pyuv += 2 * 8;
		}
	// Not supported frame
	} else {
		memset(fFrameBuffer, 0, fFrameBufferSize);
		return;
	}
}

int32
UVCProducer::_frame_generator(void *data)
{
	return ((UVCProducer *)data)->FrameGenerator();
}

int32 
UVCProducer::FrameGenerator()
{
	bigtime_t wait_until = system_time();

	while (1) {
		status_t err = acquire_sem_etc(fFrameSync, 1, B_ABSOLUTE_TIMEOUT,
				wait_until);

		if ((err != B_OK) && (err != B_TIMED_OUT))
			break;

		fFrame++;

		if (!fConnected || !fRunning || !fEnabled)
			continue;

		wait_until = TimeSource()->RealTimeFor(fPerformanceTimeBase +
				(bigtime_t)((fFrame - fFrameBase) *
				(1000000 / fConnectedFormat.field_rate)), 0) - fProcessingLatency;

		if (wait_until < system_time())
			continue;

		if (err == B_OK)
			continue;

		BAutolock frameLocker(fLock);

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

		if (fFrameBuffer == NULL || fFrameBufferSize == 0)
			memset((unsigned char*)buffer->Data(), 0, h->size_used);
		else
			memcpy((unsigned char*)buffer->Data(), fFrameBuffer, fFrameBufferSize);

		if (SendBuffer(buffer, fOutput.source, fOutput.destination) < B_OK)
			buffer->Recycle();
	}

	return B_OK;
}
