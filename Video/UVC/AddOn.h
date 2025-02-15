/*
 * Copyright 2024, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _UVC_VIDEO_ADDON_H
#define _UVC_VIDEO_ADDON_H

#include <media/MediaAddOn.h>
#include <libuvc/libuvc.h>

class UVCProducer;

extern "C" _EXPORT BMediaAddOn *make_media_addon(image_id you);

class MediaAddOn : public BMediaAddOn
{
public:
						MediaAddOn(image_id imid);
	virtual				~MediaAddOn();

	virtual status_t	InitCheck(const char **out_failure_text);

	virtual int32		CountFlavors();
	virtual status_t	GetFlavorAt(int32 n, const flavor_info ** out_info);
	virtual BMediaNode*	InstantiateNodeFor(
							const flavor_info*	info,
							BMessage * config,
							status_t * out_error);

	virtual status_t	GetConfigurationFor(BMediaNode *node, BMessage *message) { return B_OK; }
	virtual status_t	SaveConfigInfo(BMediaNode *node, BMessage *message)	{ return B_OK; }

	virtual bool		WantsAutoStart() { return false; }
	virtual status_t	AutoStart(int in_count, BMediaNode **out_node,
							int32 *out_internal_id, bool *out_has_more)	{ return B_ERROR; }

private:
	status_t			fInitStatus;
	uvc_context_t*		fContext;
	uvc_device_t**		fDeviceList;
	int32				fDeviceCount;
	flavor_info*		fFlavorInfos;
	media_format*		fMediaFormats;
};

#endif //_UVC_VIDEO_ADDON_H
