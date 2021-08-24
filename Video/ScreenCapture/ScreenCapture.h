/*
 * Copyright 2021, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _H_SCREEN_CAPTURE
#define _H_SCREEN_CAPTURE

#include <Bitmap.h>
#include <Screen.h>
#include <DirectWindow.h>
#include <SupportDefs.h>

class  ScreenCapture: public BDirectWindow {
public:
						ScreenCapture(BScreen *screen);
	virtual	void		DirectConnected(direct_buffer_info* info);
	status_t			ReadBitmap(BBitmap *bitmap, bool direct = true);
private:
	BScreen				*fScreen;
	direct_buffer_info 	fDirectInfo;
	bool				fDirectAvailable;
};

#endif //_H_SCREEN_CAPTURE
