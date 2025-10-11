/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <NetworkInterface.h>
#include <NetworkRoster.h>

#include <string.h>
#include <stdio.h>

#include <new>

#include "NetCastServer.h"

NetCastServer::NetCastServer()
	: fServerSocket(NULL),
	  fServerThread(-1),
	  fServerRunning(false),
	  fServerPort(0),
	  fMimeType("audio/wav"),
	  fBitrate(128),
	  fStreamHeader(NULL),
	  fStreamHeaderSize(0),
	  fListener(NULL)
{
}

NetCastServer::~NetCastServer()
{
	Stop();
	delete[] fStreamHeader;
}

status_t
NetCastServer::Start(int32 port)
{
	if (fServerRunning) {
		if (fListener)
			fListener->OnServerError("Server already running");
		return B_ERROR;
	}

	fServerPort = port;

	fServerSocket = new(std::nothrow) BSocket();
	if (!fServerSocket) {
		if (fListener)
			fListener->OnServerError("Failed to create socket");
		return B_NO_MEMORY;
	}

	BNetworkAddress address(INADDR_ANY, fServerPort);

	status_t status = fServerSocket->Bind(address, true);
	if (status != B_OK) {
		if (fListener) {
			BString error;
			error << "Bind failed: " << strerror(status);
			fListener->OnServerError(error.String());
		}
		delete fServerSocket;
		fServerSocket = NULL;
		return status;
	}

	status = fServerSocket->Listen(kServerMaxClients);
	if (status != B_OK) {
		if (fListener) {
			BString error;
			error << "Listen failed: " << strerror(status);
			fListener->OnServerError(error.String());
		}
		delete fServerSocket;
		fServerSocket = NULL;
		return status;
	}

	fServerSocket->SetTimeout(kServerAcceptTimeout);

	fServerRunning = true;
	fServerThread = spawn_thread(_ServerThread, "NetCast HTTP Server",
								 B_LOW_PRIORITY, this);

	if (fServerThread < 0) {
		if (fListener)
			fListener->OnServerError("Failed to spawn server thread");
		fServerRunning = false;
		delete fServerSocket;
		fServerSocket = NULL;
		return B_ERROR;
	}

	resume_thread(fServerThread);

	UpdateStreamURL();

	if (fListener)
		fListener->OnServerStarted(fStreamURL.String());

	return B_OK;
}

void
NetCastServer::Stop()
{
	if (!fServerRunning)
		return;

	fServerRunning = false;
	
	if (fServerSocket) {
		delete fServerSocket;
		fServerSocket = NULL;
	}

	if (fServerThread >= 0) {
		status_t result;
		wait_for_thread(fServerThread, &result);
		fServerThread = -1;
	}

	CleanupClients();

	if (fListener)
		fListener->OnServerStopped();
}

void
NetCastServer::BroadcastData(const uint8* data, size_t size)
{
	if (!fClientsLock.Lock())
		return;

	for (int32 i = fClients.CountItems() - 1; i >= 0; i--) {
		ClientInfo* client = static_cast<ClientInfo*>(fClients.ItemAt(i));
		if (!client || !client->socket)
			continue;

		if (!client->headerSent && fStreamHeader && fStreamHeaderSize > 0) {
			fHeaderLock.Lock();
			ssize_t sent = client->socket->Write(fStreamHeader, fStreamHeaderSize);
			fHeaderLock.Unlock();
			
			if (sent == static_cast<ssize_t>(fStreamHeaderSize)) {
				client->headerSent = true;
			}
		}

		ssize_t sent = client->socket->Write(data, size);
		if (sent <= 0) {
			if (fListener)
				fListener->OnClientDisconnected(client->address.String());
			
			delete client->socket;
			fClients.RemoveItem(i);
			delete client;
		}
	}

	fClientsLock.Unlock();
}

void
NetCastServer::SetStreamInfo(const char* mimeType, int32 bitrate)
{
	fMimeType = mimeType;
	fBitrate = bitrate;
}

void
NetCastServer::SendHeaderToNewClients(const uint8* header, size_t headerSize)
{
	fHeaderLock.Lock();

	delete[] fStreamHeader;
	fStreamHeader = NULL;
	fStreamHeaderSize = 0;

	if (header && headerSize > 0) {
		fStreamHeader = new(std::nothrow) uint8[headerSize];
		if (fStreamHeader) {
			memcpy(fStreamHeader, header, headerSize);
			fStreamHeaderSize = headerSize;
		}
	}

	fHeaderLock.Unlock();
}

int32
NetCastServer::GetClientCount()
{
	fClientsLock.Lock();
	int32 count = fClients.CountItems();
	fClientsLock.Unlock();
	return count;
}

int32
NetCastServer::_ServerThread(void* data)
{
	NetCastServer* server = static_cast<NetCastServer*>(data);
	server->ServerLoop();
	return 0;
}

void
NetCastServer::ServerLoop()
{
	while (fServerRunning) {
		BAbstractSocket* clientSocket = NULL;
		status_t status = fServerSocket->Accept(clientSocket);

		if (status == B_TIMED_OUT)
			continue;
		
		if (status != B_OK)
			continue;
		
		if (!clientSocket)
			continue;
		
		fClientsLock.Lock();
		int32 clientCount = fClients.CountItems();
		fClientsLock.Unlock();

		if (clientCount >= kServerMaxClients) {
			BString response = "HTTP/1.1 503 Service Unavailable\r\n"
							   "Content-Type: text/plain\r\n"
							   "Connection: close\r\n\r\n"
							   "Server busy - maximum clients reached\n";
			clientSocket->Write(response.String(), response.Length());
			delete clientSocket;
			continue;
		}

		clientSocket->SetTimeout(kServerClientTimeout);

		HandleClient(clientSocket);
	}
}

bool
NetCastServer::ParseHTTPRequest(const char* request, BString& path, BString& userAgent)
{
	if (strncmp(request, "GET ", 4) != 0)
		return false;

	const char* pathStart = request + 4;
	const char* pathEnd = strchr(pathStart, ' ');

	if (!pathEnd)
		return false;

	path.SetTo(pathStart, pathEnd - pathStart);

	userAgent = "Unknown";
	const char* uaStart = strstr(request, "User-Agent: ");
	if (uaStart) {
		uaStart += 12;
		const char* uaEnd = strchr(uaStart, '\r');
		if (uaEnd) {
			userAgent.SetTo(uaStart, uaEnd - uaStart);
		}
	}

	return true;
}

void
NetCastServer::HandleClient(BAbstractSocket* clientSocket)
{
	char buffer[kServerHTTPBufferSize];
	ssize_t bytesRead = clientSocket->Read(buffer, sizeof(buffer) - 1);

	if (bytesRead <= 0) {
		delete clientSocket;
		return;
	}

	buffer[bytesRead] = '\0';

	BString path, userAgent;
	if (!ParseHTTPRequest(buffer, path, userAgent)) {
		BString response = "HTTP/1.1 400 Bad Request\r\n"
						   "Content-Type: text/plain\r\n"
						   "Connection: close\r\n\r\n"
						   "Invalid HTTP request\n";
		clientSocket->Write(response.String(), response.Length());
		delete clientSocket;
		return;
	}

	if (path != "/stream" && path != "/stream.wav" && path != "/stream.mp3") {
		BString response = "HTTP/1.1 404 Not Found\r\n"
						   "Content-Type: text/plain\r\n"
						   "Connection: close\r\n\r\n"
						   "Not found. Try /stream\n";
		clientSocket->Write(response.String(), response.Length());
		delete clientSocket;
		return;
	}

	SendHTTPResponse(clientSocket);

	ClientInfo* info = new ClientInfo;
	info->socket = clientSocket;
	info->address = clientSocket->Peer().ToString();

	int32 colonPos = info->address.FindFirst(':');
	if (colonPos > 0)
		info->address.Truncate(colonPos);

	info->userAgent = userAgent;
	info->headerSent = false;
	info->connectedTime = system_time();

	fClientsLock.Lock();
	fClients.AddItem(info);
	fClientsLock.Unlock();

	if (fListener)
		fListener->OnClientConnected(info->address.String(), userAgent.String());
}

void
NetCastServer::SendHTTPResponse(BAbstractSocket* socket)
{
	BString response;
	response << "HTTP/1.1 200 OK\r\n";
	response << "Content-Type: " << fMimeType << "\r\n";
	response << "Connection: close\r\n";
	response << "Cache-Control: no-cache, no-store\r\n";
	response << "Pragma: no-cache\r\n";
	response << "icy-name: NetCast Audio Stream\r\n";
	response << "icy-br: " << fBitrate << "\r\n";
	response << "Server: NetCast/1.0 (Haiku)\r\n";
	response << "\r\n";

	socket->Write(response.String(), response.Length());
}

void
NetCastServer::UpdateStreamURL()
{
	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;

	fStreamURL = "";

	bool foundAddress = false;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		if ((interface.Flags() & IFF_LOOPBACK) == 0 && 
			(interface.Flags() & IFF_UP)) {
			int32 addressCount = interface.CountAddresses();
			for (int32 i = 0; i < addressCount; i++) {
				BNetworkInterfaceAddress addr;
				if (interface.GetAddressAt(i, addr) == B_OK) {
					BNetworkAddress netAddr = addr.Address();
					if (netAddr.Family() == AF_INET) {
						BString addrString = netAddr.ToString();
						int32 colonPos = addrString.FindFirst(':');
						if (colonPos > 0) {
							addrString.Truncate(colonPos);
						}

						if (!foundAddress) {
							fStreamURL << "http://" << addrString << ":" 
									  << fServerPort << "/stream";
							foundAddress = true;
						}
					}
				}
			}
		}
	}

	if (!foundAddress)
		fStreamURL << "http://localhost:" << fServerPort << "/stream";
}

void
NetCastServer::CleanupClients()
{
	fClientsLock.Lock();
	for (int32 i = 0; i < fClients.CountItems(); i++) {
		ClientInfo* client = static_cast<ClientInfo*>(fClients.ItemAt(i));
		if (client) {
			delete client->socket;
			delete client;
		}
	}
	fClients.MakeEmpty();
	fClientsLock.Unlock();
}
