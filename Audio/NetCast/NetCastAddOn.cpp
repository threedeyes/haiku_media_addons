/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>

#include "NetCastAddOn.h"
#include "NetCastNode.h"
#include "NetCastDebug.h"

NetCastAddOn::NetCastAddOn(image_id image)
	: BMediaAddOn(image),
	  fAddOnImage(image)
{
	TRACE_CALL("image_id=%ld", image);

	fFormat.type = B_MEDIA_RAW_AUDIO;
	fFormat.require_flags = 0;
	fFormat.deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;
	fFormat.u.raw_audio = media_raw_audio_format::wildcard;

	fInfo.internal_id = 0;
	fInfo.name = "NetCast";
	fInfo.info = "Streams audio over HTTP";
	fInfo.kinds = B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT | B_CONTROLLABLE;
	fInfo.flavor_flags = 0;
	fInfo.possible_count = 1;
	fInfo.in_format_count = 1;
	fInfo.in_formats = &fFormat;
	fInfo.out_format_count = 0;
	fInfo.out_formats = NULL;

	TRACE_INFO("NetCast addon initialized");
}

NetCastAddOn::~NetCastAddOn()
{
	TRACE_CALL("");
	TRACE_INFO("NetCast addon destroyed");
}

status_t
NetCastAddOn::InitCheck(const char** out_failure_text)
{
	TRACE_CALL("");
	return B_OK;
}

int32
NetCastAddOn::CountFlavors()
{
	TRACE_VERBOSE("");
	return 1;
}

status_t
NetCastAddOn::GetFlavorAt(int32 n, const flavor_info** out_info)
{
	TRACE_VERBOSE("n=%ld", n);

	if (n != 0) {
		TRACE_ERROR("Invalid flavor index: %ld", n);
		return B_ERROR;
	}

	*out_info = &fInfo;
	return B_OK;
}

BMediaNode*
NetCastAddOn::InstantiateNodeFor(const flavor_info* info,
	BMessage* config, status_t* out_error)
{
	TRACE_CALL("flavor_id=%ld", info ? info->internal_id : -1);

	if (out_error)
		*out_error = B_OK;

	BMediaNode* node = new NetCastNode(this, config, fAddOnImage);
	TRACE_INFO("NetCastNode instantiated: %p", node);

	return node;
}

extern "C" _EXPORT BMediaAddOn*
make_media_addon(image_id image)
{
	TRACE_CALL("image_id=%ld", image);
	return new NetCastAddOn(image);
}
