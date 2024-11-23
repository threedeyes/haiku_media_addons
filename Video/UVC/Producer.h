/*
 * Copyright 2024, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * Copyright 2010-2012 Ken Tossell
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _UVC_PRODUCER_H
#define _UVC_PRODUCER_H

#include <OS.h>
#include <BufferProducer.h>
#include <Controllable.h>
#include <MediaDefs.h>
#include <MediaEventLooper.h>
#include <MediaNode.h>
#include <Locker.h>
#include <File.h>
#include <Path.h>

#include <libuvc/libuvc.h>

#define IYUYV2BGR_2(pyuv, pbgr) { \
		int r = (22987 * ((pyuv)[3] - 128)) >> 14; \
		int g = (-5636 * ((pyuv)[1] - 128) - 11698 * ((pyuv)[3] - 128)) >> 14; \
		int b = (29049 * ((pyuv)[1] - 128)) >> 14; \
		(pbgr)[0] = sat(*(pyuv) + b); \
		(pbgr)[1] = sat(*(pyuv) + g); \
		(pbgr)[2] = sat(*(pyuv) + r); \
		(pbgr)[3] = 255; \
		(pbgr)[4] = sat((pyuv)[2] + b); \
		(pbgr)[5] = sat((pyuv)[2] + g); \
		(pbgr)[6] = sat((pyuv)[2] + r); \
		(pbgr)[7] = 255; \
	}
#define IYUYV2BGR_8(pyuv, pbgr) IYUYV2BGR_4(pyuv, pbgr); IYUYV2BGR_4(pyuv + 8, pbgr + 16);
#define IYUYV2BGR_4(pyuv, pbgr) IYUYV2BGR_2(pyuv, pbgr); IYUYV2BGR_2(pyuv + 4, pbgr + 8);

static inline unsigned char sat(int i) {
	return (unsigned char)( i >= 255 ? 255 : (i < 0 ? 0 : i));
}

class UVCProducer :
	public virtual BMediaNode,
	public BMediaEventLooper,
	public BBufferProducer,
	public BControllable
{
public:
							UVCProducer(BMediaAddOn *addon,
								const char *name, 
								int32 internal_id,
								uvc_device_t* device);
	virtual					~UVCProducer();

	virtual status_t		InitCheck() const { return fInitStatus; }

/* BMediaNode */
public:
	virtual port_id			ControlPort() const;
	virtual BMediaAddOn*	AddOn(int32 * internal_id) const;
	virtual status_t		HandleMessage(int32 message, const void *data,
	size_t size);
protected:    
	virtual void			SetTimeSource(BTimeSource * time_source);
	virtual status_t		RequestCompleted(const media_request_info & info);

/* BMediaEventLooper */
protected:
	virtual void			NodeRegistered();
	virtual void			HandleEvent(const media_timed_event *event,
								bigtime_t lateness, bool realTimeEvent = false);
	virtual void			CleanUpEvent(const media_timed_event *event);
	virtual bigtime_t		OfflineTime();
	virtual void			ControlLoop();
	virtual status_t		DeleteHook(BMediaNode * node);

/* BBufferProducer */                                    
protected:
	virtual status_t		FormatSuggestionRequested(media_type type, int32 quality,
								media_format * format);
	virtual status_t		FormatProposal(const media_source &output,
								media_format *format);
	virtual status_t		FormatChangeRequested(const media_source &source,
								const media_destination &destination,
								media_format *io_format, int32 *_deprecated_);
	virtual status_t		GetNextOutput(int32 * cookie, media_output * out_output);
	virtual status_t		DisposeOutputCookie(int32 cookie) { return B_OK; }
	virtual status_t		SetBufferGroup(const media_source &for_source,
								BBufferGroup *group) { return B_ERROR; }
	virtual status_t		GetLatency(bigtime_t * out_latency);
	virtual status_t		PrepareToConnect(const media_source &what,
								const media_destination &where,
								media_format *format,
								media_source *out_source, char *out_name);
	virtual void 			Connect(status_t error, const media_source &source,
								const media_destination &destination,
								const media_format & format, char *io_name);
	virtual void			Disconnect(const media_source & what,
								const media_destination & where);
	virtual void			LateNoticeReceived(const media_source &what,
								bigtime_t how_much, bigtime_t performance_time) {}
	virtual void			EnableOutput(const media_source & what, bool enabled,
								int32 * _deprecated_);
	virtual void			AdditionalBufferRequested(const media_source &source,
								media_buffer_id prev_buffer, bigtime_t prev_time,
								const media_seek_tag *prev_tag) {}
	virtual void			LatencyChanged(const media_source &source,
								const media_destination &destination,
								bigtime_t new_latency, uint32 flags) {}

/* BControllable */                                    
protected:
	virtual status_t		GetParameterValue(int32 id, bigtime_t *last_change,
								void *value, size_t *size);
	virtual void			SetParameterValue(int32 id, bigtime_t when,
								const void *value, size_t size);
	virtual status_t		StartControlPanel(BMessenger *out_messenger);

private:
	enum {    	
		P_FORMAT = 1,
		P_RESOLUTION,
		P_FRAMERATE,
		P_PRESET,
		P_BRIGHTNESS,
		P_CONTRAST,
		P_HUE,
		P_SATURATION
	};

	struct FormatDesc {
		uint8_t     index;
		uvc_frame_format format;
		char        name[32];
	};

	struct ResolutionDesc {
		uint16_t	width;
		uint16_t	height;
		uint8_t		index;
	};

	struct FrameRateDesc {
		uint32_t	fps;
		uint8_t		index;
	};

	struct ControlDesc {
		int32		param_id;
		char		name[32];
		float		min;
		float		max;
		float		def;
		float		value;
		float		prev_value;
		bigtime_t	changed;
		
		void Apply(uvc_device_handle_t *fDeviceHandle) {
			switch (param_id) {
				case P_BRIGHTNESS:
					uvc_set_brightness(fDeviceHandle, value);
					break;
				case P_CONTRAST:
					uvc_set_contrast(fDeviceHandle, value);
					break;
				case P_HUE:
					uvc_set_hue(fDeviceHandle, value);
					break;
				case P_SATURATION:
					uvc_set_saturation(fDeviceHandle, value);
					break;				
			}
		};
	};

private:
	void					MakeParameterWeb();
	void					HandleStart(bigtime_t performance_time);
	void					HandleStop();
	void					HandleTimeWarp(bigtime_t performance_time);
	void					HandleSeek(bigtime_t performance_time);
	void					HandleParameter(uint32 parameter);

	status_t				SetupDevice();
	void					CleanupDevice();

	status_t				CollectFormats();
	status_t				CollectResolutions(uint8_t formatIndex);
	status_t				CollectFrameRates(uint8_t formatIndex, uint8_t resolutionIndex);
	status_t				InitControls();
	
	status_t				OpenAddonSettings(BFile& file, uint32 mode);
	status_t				LoadAddonSettings();
	status_t				SaveAddonSettings();

	status_t				StartStreaming();
	void					StopStreaming();

	static void				_uvc_callback(uvc_frame_t *frame, void *ptr);
	void					HandleFrame(uvc_frame_t *frame);
	
	static int32			_frame_generator(void *data);
	int32					FrameGenerator();

private:
	status_t				fInitStatus;
	int32					fInternalID;
	BMediaAddOn				*fAddOn;

	BLocker					fLock;
	BBufferGroup			*fBufferGroup;

	thread_id				fThread;
	sem_id					fFrameSync;
	uint32					fFrame;
	uint32					fFrameBase;
	bigtime_t				fPerformanceTimeBase;
	bigtime_t				fProcessingLatency;
	media_output			fOutput;
	media_raw_video_format	fConnectedFormat;
	bool					fRunning;
	bool					fConnected;
	bool					fEnabled;

	// frame buffer
	uint8*					fFrameBuffer;
	size_t					fFrameBufferSize;

	// UVC specific
	uvc_device_t*			fDevice;
	uvc_device_handle_t*	fDeviceHandle;
	uvc_stream_ctrl_t		fStreamCtrl;
	uvc_device_descriptor_t* fDeviceDescriptor;

	// Format parameters
	BList					fFormats;
	BList					fResolutions;
	BList					fFrameRates;
	BList					fControls;

	uint8					fCurrentFormatIndex;
	uint8					fCurrentResolutionIndex;
	uint8					fCurrentFrameRateIndex;

	// Parameter change times
	bigtime_t				fLastFormatChange;
	bigtime_t				fLastResolutionChange;
	bigtime_t				fLastFrameRateChange;
	bigtime_t				fLastPresetChange;
};

#endif // _UVC_PRODUCER_H
