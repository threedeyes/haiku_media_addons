/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef NETCAST_ADDON_H
#define NETCAST_ADDON_H

#include <MediaAddOn.h>

class NetCastAddOn : public BMediaAddOn {
public:
							NetCastAddOn(image_id image);
	virtual					~NetCastAddOn();
	
	virtual status_t		InitCheck(const char** out_failure_text);
	virtual int32			CountFlavors();
	virtual status_t		GetFlavorAt(int32 n, const flavor_info** out_info);
	virtual BMediaNode*		InstantiateNodeFor(const flavor_info* info,
								BMessage* config, status_t* out_error);
								
private:
	media_format			fFormat;
	flavor_info				fInfo;
	image_id				fAddOnImage;
};

extern "C" _EXPORT BMediaAddOn* make_media_addon(image_id image);

#endif
