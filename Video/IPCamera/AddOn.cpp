/*
 * Copyright 2021, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Autolock.h>
#include <MediaFormats.h>

#include "AddOn.h"
#include "Producer.h"

MediaAddOn::MediaAddOn(image_id id)
	: BMediaAddOn(id)
{
	fFlavorInfo.name = (char *)"IP Camera";
	fFlavorInfo.info = (char *)"IP Camera";
	fFlavorInfo.kinds = B_BUFFER_PRODUCER | B_CONTROLLABLE | B_PHYSICAL_INPUT;
	fFlavorInfo.flavor_flags = 0;
	fFlavorInfo.internal_id = 0;
	fFlavorInfo.possible_count = 1;
	fFlavorInfo.in_format_count = 0;
	fFlavorInfo.in_format_flags = 0;
	fFlavorInfo.in_formats = NULL;
	fFlavorInfo.out_format_count = 1;
	fFlavorInfo.out_format_flags = 0;
	fMediaFormat.type = B_MEDIA_RAW_VIDEO;
	fMediaFormat.u.raw_video = media_raw_video_format::wildcard;
	fMediaFormat.u.raw_video.interlace = 1;
	fMediaFormat.u.raw_video.display.format = B_RGB32;
	fFlavorInfo.out_formats = &fMediaFormat;
	fInitStatus = B_OK;
}

status_t 
MediaAddOn::InitCheck(const char **out_failure_text)
{
	if (fInitStatus < B_OK) {
		*out_failure_text = "Unknown error";
		return fInitStatus;
	}
	return B_OK;
}

int32 
MediaAddOn::CountFlavors()
{
	if (fInitStatus < B_OK)
		return fInitStatus;
	return 1;
}

status_t 
MediaAddOn::GetFlavorAt(int32 n, const flavor_info **out_info)
{
	if (fInitStatus < B_OK)
		return fInitStatus;

	if (n != 0)
		return B_BAD_INDEX;

	*out_info = &fFlavorInfo;
	return B_OK;
}

BMediaNode *
MediaAddOn::InstantiateNodeFor(
		const flavor_info *info, BMessage *config, status_t *out_error)
{
	VideoProducer *node;

	if (fInitStatus < B_OK)
		return NULL;

	if (info->internal_id != fFlavorInfo.internal_id)
		return NULL;

	node = new VideoProducer(this, fFlavorInfo.name, fFlavorInfo.internal_id);
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
