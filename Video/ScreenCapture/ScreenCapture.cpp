/*
 * Copyright 2021, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "ScreenCapture.h"

ScreenCapture::ScreenCapture(BScreen *screen)
	: BDirectWindow(BRect(-1, -1, 0, 0), "FakeDirectWindow",
		B_NO_BORDER_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
		B_AVOID_FRONT | B_AVOID_FOCUS | B_NO_WORKSPACE_ACTIVATION,
		B_CURRENT_WORKSPACE)
	,fDirectAvailable(false)
	,fScreen(screen)
{
}

void
ScreenCapture::DirectConnected(direct_buffer_info *info)
{
	switch (info->buffer_state & B_DIRECT_MODE_MASK) {
		case B_DIRECT_START:
		case B_DIRECT_MODIFY:
			fDirectInfo = *info;
			fDirectAvailable = true;
			break;
		case B_DIRECT_STOP:
			fDirectAvailable = false;
			break;
		default:
			break;
	}
}

status_t
ScreenCapture::ReadBitmap(BBitmap *bitmap, bool direct)
{
	if (direct && fDirectAvailable) {
		memcpy(bitmap->Bits(), fDirectInfo.bits, bitmap->BitsLength());
		return B_OK;
	}
	return fScreen->ReadBitmap(bitmap);
}
