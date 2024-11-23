/*
 * Copyright 2024, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Autolock.h>
#include <MediaFormats.h>
#include <media/MediaNode.h>

#include "AddOn.h"
#include "Producer.h"

MediaAddOn::MediaAddOn(image_id imid)
	: BMediaAddOn(imid)
	, fContext(NULL)
	, fDeviceList(NULL)
	, fDeviceCount(0)
	, fFlavorInfos(NULL)
	, fMediaFormats(NULL)
{
	fInitStatus = uvc_init(&fContext, NULL);
	if (fInitStatus < B_OK)
		return;
	
	uvc_device_t **list;
	fInitStatus = uvc_get_device_list(fContext, &list);
	if (fInitStatus < B_OK) {
		uvc_exit(fContext);
		fContext = NULL;
		return;
	}

	uvc_device_t *device;
	int i = 0;
	while ((device = list[i++]) != NULL) {
		fDeviceCount++;
	}

	if (fDeviceCount == 0) {
		uvc_free_device_list(list, 1);
		uvc_exit(fContext);
		fContext = NULL;
		fInitStatus = B_ERROR;
		return;
	}

	fFlavorInfos = new flavor_info[fDeviceCount];
	fMediaFormats = new media_format[fDeviceCount];
	fDeviceList = new uvc_device_t*[fDeviceCount];

	for (int32 i = 0; i < fDeviceCount; i++) {
		uvc_device_t *device = list[i];
		fDeviceList[i] = device;

		uvc_device_descriptor_t *desc = NULL;
		uvc_get_device_descriptor(device, &desc);
	
		fFlavorInfos[i].name = strdup(desc ? desc->product : "UVC Camera");
		fFlavorInfos[i].info = strdup(desc ? desc->manufacturer : "Unknown manufacturer");
		fFlavorInfos[i].kinds = B_BUFFER_PRODUCER | B_CONTROLLABLE | B_PHYSICAL_INPUT;
		fFlavorInfos[i].flavor_flags = 0;
		fFlavorInfos[i].internal_id = i;
		fFlavorInfos[i].possible_count = 1;
		fFlavorInfos[i].in_format_count = 0;
		fFlavorInfos[i].in_formats = NULL;
		fFlavorInfos[i].out_format_count = 1;
		fFlavorInfos[i].out_format_flags = 0;

		fMediaFormats[i].type = B_MEDIA_RAW_VIDEO;
		fMediaFormats[i].u.raw_video = media_raw_video_format::wildcard;
		fMediaFormats[i].u.raw_video.display.format = B_RGB32;
		fFlavorInfos[i].out_formats = &fMediaFormats[i];

		if (desc)
			uvc_free_device_descriptor(desc);
	}

	uvc_free_device_list(list, 0);
}

MediaAddOn::~MediaAddOn()
{
	if (fFlavorInfos) {
		for (int32 i = 0; i < fDeviceCount; i++) {
			free((void*)fFlavorInfos[i].name);
			free((void*)fFlavorInfos[i].info);
		}
		delete[] fFlavorInfos;
	}

	if (fMediaFormats)
		delete[] fMediaFormats;

	if (fDeviceList) {
		for (int32 i = 0; i < fDeviceCount; i++) {
			uvc_unref_device(fDeviceList[i]);
		}
		delete[] fDeviceList;
	}

	if (fContext)
		uvc_exit(fContext);
}

status_t
MediaAddOn::InitCheck(const char **out_failure_text)
{
	if (fInitStatus < B_OK) {
		*out_failure_text = "Failed to initialize UVC context";
		return fInitStatus;
	}

	return B_OK;
}

int32
MediaAddOn::CountFlavors()
{
	return fDeviceCount;
}

status_t
MediaAddOn::GetFlavorAt(int32 n, const flavor_info **out_info)
{
	if (n < 0 || n >= fDeviceCount)
		return B_BAD_INDEX;

	*out_info = &fFlavorInfos[n];
	return B_OK;
}

BMediaNode *
MediaAddOn::InstantiateNodeFor(
        const flavor_info *info, BMessage *config, status_t *out_error)
{
	if (info->internal_id < 0 || info->internal_id >= fDeviceCount) {
		*out_error = B_BAD_INDEX;
		return NULL;
	}

	UVCProducer *node = new UVCProducer(this, 
			fFlavorInfos[info->internal_id].name,
			info->internal_id,
			fDeviceList[info->internal_id]);

	if (node && (node->InitCheck() < B_OK)) {
		delete node;
		node = NULL;
	}

	return node;
}

BMediaAddOn *
make_media_addon(image_id id)
{
	return new MediaAddOn(id);
}
