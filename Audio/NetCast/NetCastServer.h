/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef NETCAST_SERVER_H
#define NETCAST_SERVER_H

#include <Socket.h>
#include <NetworkAddress.h>
#include <String.h>
#include <List.h>
#include <Locker.h>
#include <SupportDefs.h>

static const int32 kServerMaxClients = 10;
static const bigtime_t kServerClientTimeout = 5000000;
static const bigtime_t kServerAcceptTimeout = 1000000;
static const int32 kServerHTTPBufferSize = 4096;
static const float kSendBufferSeconds = 0.5f;
static const int32 kMaxFailedSends = 10;

class NetCastServer {
public:
	class Listener {
	public:
		virtual ~Listener() {}
		virtual void OnClientConnected(const char* address, const char* userAgent) {}
		virtual void OnClientDisconnected(const char* address) {}
		virtual void OnServerStarted(const char* url) {}
		virtual void OnServerStopped() {}
		virtual void OnServerError(const char* error) {}
	};

	struct ClientInfo {
		BAbstractSocket*	socket;
		BString				address;
		BString				userAgent;
		bool				headerSent;
		bigtime_t			connectedTime;
		int32				failedSendCount;
		bigtime_t			lastSuccessfulSend;
	};

							NetCastServer();
	virtual					~NetCastServer();

	status_t				Start(int32 port);
	void					Stop();
	bool					IsRunning() const { return fServerRunning; }

	void					BroadcastData(const uint8* data, size_t size);
	void					SetStreamInfo(const char* mimeType, int32 bitrate,
								float sampleRate, int32 channels);
	void					SendHeaderToNewClients(const uint8* header, size_t headerSize);

	BString					GetStreamURL() const { return fStreamURL; }
	int32					GetClientCount();
	int32					GetPort() const { return fServerPort; }

	void					SetListener(Listener* listener) { fListener = listener; }

private:
	static int32			_ServerThread(void* data);
	void					ServerLoop();
	void					HandleClient(BAbstractSocket* clientSocket);
	bool					ParseHTTPRequest(const char* request, BString& path, BString& userAgent);
	void					SendHTTPResponse(BAbstractSocket* socket);
	void					UpdateStreamURL();
	void					CleanupClients();
	int32					CalculateOptimalSendBuffer() const;

	BSocket*				fServerSocket;
	thread_id				fServerThread;
	volatile bool			fServerRunning;
	int32					fServerPort;

	BList					fClients;
	BLocker					fClientsLock;

	BString					fStreamURL;
	BString					fMimeType;
	int32					fBitrate;
	float					fSampleRate;
	int32					fChannels;

	uint8*					fStreamHeader;
	size_t					fStreamHeaderSize;
	BLocker					fHeaderLock;

	Listener*				fListener;
};

#endif
