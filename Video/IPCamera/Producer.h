/*
 * Copyright 2021, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _VIDEO_PRODUCER_H
#define _VIDEO_PRODUCER_H

#include <kernel/OS.h>
#include <media/BufferProducer.h>
#include <media/Controllable.h>
#include <media/MediaDefs.h>
#include <media/MediaEventLooper.h>
#include <media/MediaNode.h>
#include <ParameterWeb.h>

#include <String.h>
#include <StorageKit.h>
#include <support/Locker.h>

extern "C"
{
	#include "libavcodec/avcodec.h"
	#include "libavformat/avformat.h"
	#include "libswscale/swscale.h"
};


class VideoProducer :
	public virtual BMediaEventLooper,
	public virtual BBufferProducer,
	public virtual BControllable
{
public:
						VideoProducer(BMediaAddOn *addon,
							const char *name, int32 internal_id);
	virtual				~VideoProducer();

	virtual	status_t	InitCheck() const { return fInitStatus; }

/* BMediaNode */
	public:
	virtual port_id		ControlPort() const;
	virtual	BMediaAddOn	*AddOn(int32 * internal_id) const;
	virtual	status_t 	HandleMessage(int32 message, const void *data, size_t size);
protected:	
	virtual	void 		Preroll() {};
	virtual void		SetTimeSource(BTimeSource * time_source);
	virtual status_t	RequestCompleted(const media_request_info & info);

/* BMediaEventLooper */
protected:
	virtual	void 		NodeRegistered();
	virtual void		HandleEvent(const media_timed_event *event,
							bigtime_t lateness, bool realTimeEvent = false);
	virtual void		CleanUpEvent(const media_timed_event *event);
	virtual bigtime_t	OfflineTime();
	virtual void		ControlLoop();
	virtual status_t	DeleteHook(BMediaNode * node);

/* BBufferProducer */									
protected:
	virtual	status_t	FormatSuggestionRequested(media_type type, int32 quality,
							media_format * format);
	virtual	status_t 	FormatProposal(const media_source &output,
							media_format *format);
	virtual	status_t	FormatChangeRequested(const media_source &source,
							const media_destination &destination,
							media_format *io_format, int32 *_deprecated_);
	virtual	status_t 	GetNextOutput(int32 * cookie, media_output * out_output);
	virtual	status_t	DisposeOutputCookie(int32 cookie) { return B_OK; }
	virtual	status_t	SetBufferGroup(const media_source &for_source,
							BBufferGroup * group) { return B_ERROR; }
	virtual	status_t 	VideoClippingChanged(const media_source &for_source,
							int16 num_shorts, int16 *clip_data,
							const media_video_display_info &display,
							int32 * _deprecated_) { return B_ERROR; }
	virtual	status_t	GetLatency(bigtime_t * out_latency);
	virtual	status_t	PrepareToConnect(const media_source &what,
							const media_destination &where,
							media_format *format,
							media_source *out_source, char *out_name);
	virtual	void		Connect(status_t error, const media_source &source,
							const media_destination &destination,
							const media_format & format, char *io_name);
	virtual	void 		Disconnect(const media_source & what,
							const media_destination & where);
	virtual	void 		LateNoticeReceived(const media_source & what,
							bigtime_t how_much, bigtime_t performance_time) {};
	virtual	void 		EnableOutput(const media_source & what, bool enabled,
							int32 * _deprecated_);
	virtual	status_t	SetPlayRate(int32 numer,int32 denom) { return B_ERROR; }
	virtual	void 		AdditionalBufferRequested(const media_source & source,
							media_buffer_id prev_buffer, bigtime_t prev_time,
							const media_seek_tag * prev_tag) {};
	virtual	void		LatencyChanged(const media_source & source,
							const media_destination & destination,
							bigtime_t new_latency, uint32 flags) {};

/* BControllable */									
protected:
	virtual status_t	GetParameterValue(int32 id, bigtime_t *last_change,
							void *value, size_t *size);
	virtual void		SetParameterValue(int32 id, bigtime_t when,
							const void *value, size_t size);
	virtual status_t	StartControlPanel(BMessenger *out_messenger);

/* state */
private:
	void				HandleStart(bigtime_t performance_time);
	void				HandleStop();
	void				HandleTimeWarp(bigtime_t performance_time);
	void				HandleSeek(bigtime_t performance_time);

	status_t			fInitStatus;

	int32				fInternalID;
	BMediaAddOn			*fAddOn;

	BLocker				fLock;
	BBufferGroup		*fBufferGroup;

	uint32				fFrame;
	uint32				fFrameBase;
	bigtime_t			fPerformanceTimeBase;
	bigtime_t			fProcessingLatency;
	media_output		fOutput;
	media_raw_video_format	fConnectedFormat;
	bool				fRunning;
	bool				fConnected;
	bool				fEnabled;

/* thread */
	thread_id			fThread;
	thread_id			fThreadReader;
	sem_id				fFrameSync;

	int32				FrameGenerator();
	static int32		_frame_generator_(void *data);

	int32				StreamReader();
	static int32		_stream_reader_(void *data);

/* settings */
	status_t			OpenAddonSettings(BFile& file, uint32 mode);
	status_t			LoadAddonSettings();
	status_t			SaveAddonSettings();

/* params */
	enum				{
							P_URL,
							P_RECONNECT,
							P_ASPECT,
							P_FLIP_VERTICAL,
							P_FLIP_HORIZONTAL,
							P_BRIGHTNESS,
							P_CONTRAST,
							P_SATURATION
						};

	BString				fURL;
	int32				fReconnectTime;

	int32				fKeepAspect;
	int32				fFlipHorizontal;
	int32				fFlipVertical;

	float				fBrightness;
	float				fContrast;
	float				fSaturation;
		
	bigtime_t			fLastKeepAspectChange;
	bigtime_t			fLastFlipHChange;
	bigtime_t			fLastFlipVChange;
	bigtime_t			fLastURLChange;
	bigtime_t			fLastReconnectChange;
	bigtime_t			fLastBrightnessChange;
	bigtime_t			fLastContrastChange;
	bigtime_t			fLastSaturationChange;

/* ffmeg */
	AVFrame				*pFrameRGB;
	bool				fStreamConnected;
};

#endif
