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
#include "NetCastDebug.h"

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
	TRACE_CALL("");
}

NetCastServer::~NetCastServer()
{
	TRACE_CALL("");
	Stop();
	delete[] fStreamHeader;
}

status_t
NetCastServer::Start(int32 port)
{
	TRACE_CALL("port=%ld", port);

	if (fServerRunning) {
		TRACE_ERROR("Server already running");
		if (fListener)
			fListener->OnServerError("Server already running");
		return B_ERROR;
	}

	fServerPort = port;

	fServerSocket = new(std::nothrow) BSocket();
	if (!fServerSocket) {
		TRACE_ERROR("Failed to create socket");
		if (fListener)
			fListener->OnServerError("Failed to create socket");
		return B_NO_MEMORY;
	}

	BNetworkAddress address(INADDR_ANY, fServerPort);

	status_t status = fServerSocket->Bind(address, true);
	if (status != B_OK) {
		TRACE_ERROR("Bind failed on port %ld: 0x%lx (%s)",
			fServerPort, status, strerror(status));
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
		TRACE_ERROR("Listen failed: 0x%lx (%s)", status, strerror(status));
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
		TRACE_ERROR("Failed to spawn server thread: %ld", fServerThread);
		if (fListener)
			fListener->OnServerError("Failed to spawn server thread");
		fServerRunning = false;
		delete fServerSocket;
		fServerSocket = NULL;
		return B_ERROR;
	}

	resume_thread(fServerThread);

	UpdateStreamURL();

	TRACE_INFO("Server started on port %ld: %s", fServerPort, fStreamURL.String());

	if (fListener)
		fListener->OnServerStarted(fStreamURL.String());

	return B_OK;
}

void
NetCastServer::Stop()
{
	TRACE_CALL("");

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
		TRACE_INFO("Server thread stopped");
		fServerThread = -1;
	}

	CleanupClients();

	TRACE_INFO("Server stopped");

	if (fListener)
		fListener->OnServerStopped();
}

void
NetCastServer::BroadcastData(const uint8* data, size_t size)
{
	TRACE_VERBOSE("Broadcasting %lu bytes to clients", size);

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
				TRACE_VERBOSE("Sent header to client %s", client->address.String());
			} else {
				TRACE_WARNING("Failed to send header to %s: sent=%ld",
					client->address.String(), sent);
			}
		}

		ssize_t sent = client->socket->Write(data, size);
		if (sent <= 0) {
			TRACE_WARNING("Client %s disconnected (write error)", client->address.String());
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
	TRACE_INFO("Stream info updated: %s, %ld kbps", mimeType, bitrate);
	fMimeType = mimeType;
	fBitrate = bitrate;
}

void
NetCastServer::SendHeaderToNewClients(const uint8* header, size_t headerSize)
{
	TRACE_CALL("headerSize=%lu", headerSize);

	fHeaderLock.Lock();

	delete[] fStreamHeader;
	fStreamHeader = NULL;
	fStreamHeaderSize = 0;

	if (header && headerSize > 0) {
		fStreamHeader = new(std::nothrow) uint8[headerSize];
		if (fStreamHeader) {
			memcpy(fStreamHeader, header, headerSize);
			fStreamHeaderSize = headerSize;
			TRACE_INFO("Stream header set: %lu bytes", headerSize);
		} else {
			TRACE_ERROR("Failed to allocate stream header: %lu bytes", headerSize);
		}
	} else {
		TRACE_INFO("Stream header cleared");
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
	TRACE_CALL("");
	TRACE_INFO("Server loop started");

	while (fServerRunning) {
		BAbstractSocket* clientSocket = NULL;
		status_t status = fServerSocket->Accept(clientSocket);

		if (status == B_TIMED_OUT)
			continue;
		
		if (status != B_OK) {
			TRACE_WARNING("Accept failed: 0x%lx", status);
			continue;
		}
		
		if (!clientSocket) {
			TRACE_WARNING("Accept returned NULL socket");
			continue;
		}
		
		fClientsLock.Lock();
		int32 clientCount = fClients.CountItems();
		fClientsLock.Unlock();

		if (clientCount >= kServerMaxClients) {
			TRACE_WARNING("Rejecting client: maximum %d clients reached", kServerMaxClients);
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

	TRACE_INFO("Server loop ended");
}

bool
NetCastServer::ParseHTTPRequest(const char* request, BString& path, BString& userAgent)
{
	TRACE_VERBOSE("Parsing HTTP request");

	if (strncmp(request, "GET ", 4) != 0) {
		TRACE_WARNING("Not a GET request");
		return false;
	}

	const char* pathStart = request + 4;
	const char* pathEnd = strchr(pathStart, ' ');

	if (!pathEnd) {
		TRACE_WARNING("Invalid HTTP request format");
		return false;
	}

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

	TRACE_VERBOSE("Parsed: path='%s', user-agent='%s'", path.String(), userAgent.String());

	return true;
}

void
NetCastServer::HandleClient(BAbstractSocket* clientSocket)
{
	TRACE_CALL("client=%p", clientSocket);

	char buffer[kServerHTTPBufferSize];
	ssize_t bytesRead = clientSocket->Read(buffer, sizeof(buffer) - 1);

	if (bytesRead <= 0) {
		TRACE_WARNING("Failed to read from client: %ld", bytesRead);
		delete clientSocket;
		return;
	}

	buffer[bytesRead] = '\0';
	TRACE_VERBOSE("Received %ld bytes from client", bytesRead);

	BString path, userAgent;
	if (!ParseHTTPRequest(buffer, path, userAgent)) {
		TRACE_WARNING("Invalid HTTP request");
		BString response = "HTTP/1.1 400 Bad Request\r\n"
						   "Content-Type: text/plain\r\n"
						   "Connection: close\r\n\r\n"
						   "Invalid HTTP request\n";
		clientSocket->Write(response.String(), response.Length());
		delete clientSocket;
		return;
	}

	if (path != "/stream" && path != "/stream.wav" && path != "/stream.mp3") {
		TRACE_WARNING("Invalid path requested: %s", path.String());
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

	TRACE_INFO("Client accepted: %s [%s]", info->address.String(), userAgent.String());

	if (fListener)
		fListener->OnClientConnected(info->address.String(), userAgent.String());
}

void
NetCastServer::SendHTTPResponse(BAbstractSocket* socket)
{
	TRACE_VERBOSE("Sending HTTP response");

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
	TRACE_CALL("");

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
							TRACE_VERBOSE("Found network address: %s", addrString.String());
						}
					}
				}
			}
		}
	}

	if (!foundAddress) {
		fStreamURL << "http://localhost:" << fServerPort << "/stream";
		TRACE_WARNING("No network interface found, using localhost");
	}

	TRACE_INFO("Stream URL: %s", fStreamURL.String());
}

void
NetCastServer::CleanupClients()
{
	TRACE_CALL("");

	fClientsLock.Lock();
	int32 count = fClients.CountItems();
	for (int32 i = 0; i < count; i++) {
		ClientInfo* client = static_cast<ClientInfo*>(fClients.ItemAt(i));
		if (client) {
			delete client->socket;
			delete client;
		}
	}
	fClients.MakeEmpty();
	fClientsLock.Unlock();

	TRACE_INFO("All clients cleaned up");
}
