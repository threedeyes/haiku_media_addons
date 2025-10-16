/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <Resources.h>
#include <File.h>
#include <image.h>

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>

#include <new>

#include "NetCastServer.h"
#include "NetCastDebug.h"

NetCastServer::NetCastServer()
	: fServerSocket(NULL),
	  fServerThread(-1),
	  fServerRunning(false),
	  fServerPort(0),
	  fStreamName("Live Audio Stream"),
	  fMimeType("audio/wav"),
	  fBitrate(128),
	  fSampleRate(44100.0f),
	  fChannels(2),
	  fBufferMultiplier(1.0f),
	  fStreamHeader(NULL),
	  fStreamHeaderSize(0),
	  fListener(NULL),
	  fAddOnImage(-1)
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
NetCastServer::ClearClientBuffers()
{
	TRACE_CALL("");

	if (!fClientsLock.Lock())
		return;

	int32 count = fClients.CountItems();
	for (int32 i = 0; i < count; i++) {
		ClientInfo* client = static_cast<ClientInfo*>(fClients.ItemAt(i));
		if (client && client->bufferLock.Lock()) {
			client->writePos = 0;
			client->readPos = 0;
			client->dataInBuffer = 0;
			client->failedSendCount = 0;
			client->bufferLock.Unlock();
			TRACE_VERBOSE("Cleared buffer for client %s", client->address.String());
		}
	}

	fClientsLock.Unlock();
	TRACE_INFO("Cleared buffers for %ld clients", count);
}

bool
NetCastServer::AddToClientBuffer(ClientInfo* client, const uint8* data, size_t size)
{
	if (!client->bufferLock.Lock())
		return false;

	if (client->dataInBuffer + size > client->bufferSize) {
		client->bufferLock.Unlock();
		return false;
	}

	for (size_t i = 0; i < size; i++) {
		client->dataBuffer[client->writePos] = data[i];
		client->writePos = (client->writePos + 1) % client->bufferSize;
	}
	client->dataInBuffer += size;

	client->bufferLock.Unlock();
	return true;
}

void
NetCastServer::FlushClientBuffer(ClientInfo* client, bool& shouldDisconnect)
{
	if (!client->bufferLock.Lock())
		return;

	while (client->dataInBuffer > 0) {
		size_t contiguous = client->bufferSize - client->readPos;
		size_t toSend = (contiguous < client->dataInBuffer) ? contiguous : client->dataInBuffer;

		ssize_t sent = client->socket->Write(
			client->dataBuffer + client->readPos, toSend);

		if (sent > 0) {
			client->readPos = (client->readPos + sent) % client->bufferSize;
			client->dataInBuffer -= sent;
			client->lastSuccessfulSend = system_time();
			client->failedSendCount = 0;

			if (sent < static_cast<ssize_t>(toSend)) {
				break;
			}
		} else if (sent < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				break;
			} else {
				shouldDisconnect = true;
				break;
			}
		} else {
			shouldDisconnect = true;
			break;
		}
	}

	if (client->dataInBuffer > client->bufferSize * 0.9) {
		client->failedSendCount++;
		if (client->failedSendCount >= kMaxFailedSends) {
			shouldDisconnect = true;
		}
	}

	client->bufferLock.Unlock();
}

void
NetCastServer::DisconnectClient(int32 index)
{
	ClientInfo* client = static_cast<ClientInfo*>(fClients.ItemAt(index));
	if (!client)
		return;

	TRACE_INFO("Disconnecting client %s", client->address.String());

	if (fListener)
		fListener->OnClientDisconnected(client->address.String());

	delete client->socket;
	delete[] client->dataBuffer;
	fClients.RemoveItem(index);
	delete client;
}

void
NetCastServer::BroadcastData(const uint8* data, size_t size)
{
	TRACE_VERBOSE("Broadcasting %lu bytes to clients", size);

	if (!fClientsLock.Lock())
		return;

	bigtime_t now = system_time();

	for (int32 i = fClients.CountItems() - 1; i >= 0; i--) {
		ClientInfo* client = static_cast<ClientInfo*>(fClients.ItemAt(i));
		if (!client || !client->socket)
			continue;

		bool shouldDisconnect = false;

		if (!client->headerSent && fStreamHeader && fStreamHeaderSize > 0) {
			fHeaderLock.Lock();
			ssize_t headerSent = client->socket->Write(fStreamHeader, fStreamHeaderSize);
			fHeaderLock.Unlock();

			if (headerSent == static_cast<ssize_t>(fStreamHeaderSize)) {
				client->headerSent = true;
				client->lastSuccessfulSend = now;
				TRACE_INFO("Sent complete header to %s", client->address.String());
			} else if (headerSent < 0) {
				if (errno == EWOULDBLOCK || errno == EAGAIN) {
					TRACE_WARNING("Header send blocked for %s, disconnecting",
						client->address.String());
				} else {
					TRACE_WARNING("Header send failed for %s: %s",
						client->address.String(), strerror(errno));
				}
				shouldDisconnect = true;
			} else {
				TRACE_ERROR("Partial header send (%ld/%lu) to %s, disconnecting",
					headerSent, fStreamHeaderSize, client->address.String());
				shouldDisconnect = true;
			}

			if (shouldDisconnect) {
				DisconnectClient(i);
				continue;
			}
		}

		if (!AddToClientBuffer(client, data, size)) {
			TRACE_WARNING("Client buffer overflow: %s", client->address.String());
			DisconnectClient(i);
			continue;
		}

		FlushClientBuffer(client, shouldDisconnect);

		if (shouldDisconnect)
			DisconnectClient(i);
	}

	fClientsLock.Unlock();
}

void
NetCastServer::SetStreamInfo(const char* mimeType, int32 bitrate,
	float sampleRate, int32 channels)
{
	TRACE_CALL("mime=%s, bitrate=%ld, sampleRate=%.0f, channels=%ld",
		mimeType, bitrate, sampleRate, channels);

	fMimeType = mimeType;
	fBitrate = bitrate;
	fSampleRate = sampleRate;
	fChannels = channels;

	TRACE_INFO("Stream format: %s, %ld kbps, %.0f Hz, %ld ch",
		mimeType, bitrate, sampleRate, channels);
}

void
NetCastServer::SetStreamName(const char* name)
{
	TRACE_CALL("name=%s", name);

	if (name && strlen(name) > 0) {
		fStreamName = name;
	} else {
		fStreamName = "Live Audio Stream";
	}

	TRACE_INFO("Stream name set to: %s", fStreamName.String());
}

int32
NetCastServer::CalculateOptimalSendBuffer() const
{
	int32 bufferSize;

	if (fMimeType == "audio/wav") {
		bufferSize = static_cast<int32>(fSampleRate * fChannels * 2 * 0.5f);
		TRACE_VERBOSE("PCM buffer: %.0f Hz × %ld ch × 2 × 0.5 sec = %ld bytes",
			fSampleRate, fChannels, bufferSize);
	} else if (fMimeType == "audio/mpeg") {
		bufferSize = (fBitrate * 1024 / 8) * 1;
		TRACE_VERBOSE("MP3 buffer: %ld kbps × 1 sec = %ld bytes",
			fBitrate, bufferSize);
	} else {
		bufferSize = 65536;
		TRACE_WARNING("Unknown format '%s', using default buffer: %ld bytes",
			fMimeType.String(), bufferSize);
	}

	bufferSize = static_cast<int32>(bufferSize * fBufferMultiplier);
	TRACE_VERBOSE("Buffer multiplier %.1f applied: %ld bytes", fBufferMultiplier, bufferSize);

	const int32 MIN_BUFFER = 8192;
	const int32 MAX_BUFFER = 524288;

	if (bufferSize < MIN_BUFFER)
		bufferSize = MIN_BUFFER;

	if (bufferSize > MAX_BUFFER)
		bufferSize = MAX_BUFFER;

	return bufferSize;
}

BString
NetCastServer::LoadHTMLTemplate()
{
	BString html;

	if (fAddOnImage < 0) {
		TRACE_ERROR("Invalid add-on image ID");
		return html;
	}

	image_info imageInfo;
	if (get_image_info(fAddOnImage, &imageInfo) != B_OK) {
		TRACE_ERROR("Failed to get image info for image_id %ld", fAddOnImage);
		return html;
	}

	BFile file(imageInfo.name, B_READ_ONLY);
	if (file.InitCheck() != B_OK) {
		TRACE_ERROR("Failed to open add-on file: %s", imageInfo.name);
		return html;
	}

	BResources resources;
	if (resources.SetTo(&file) != B_OK) {
		TRACE_ERROR("Failed to load resources from: %s", imageInfo.name);
		return html;
	}

	size_t size;
	const char* data = (const char*)resources.LoadResource('FILE', "player.html", &size);
	if (!data) {
		TRACE_ERROR("Failed to load HTML template resource");
		return html;
	}

	html.SetTo(data, size);
	TRACE_INFO("Loaded HTML template from add-on: %lu bytes", size);

	return html;
}

const char*
NetCastServer::GetMimeType(const char* filename)
{
	if (strstr(filename, ".svg"))
		return "image/svg+xml";
	if (strstr(filename, ".html"))
		return "text/html";
	if (strstr(filename, ".css"))
		return "text/css";
	if (strstr(filename, ".js"))
		return "application/javascript";
	return "application/octet-stream";
}

void
NetCastServer::SendResourceFile(BAbstractSocket* socket, const char* resourceName)
{
	TRACE_VERBOSE("Sending resource: %s", resourceName);

	if (fAddOnImage < 0) {
		TRACE_ERROR("Invalid add-on image ID");
		BString error = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
		socket->Write(error.String(), error.Length());
		return;
	}

	image_info imageInfo;
	if (get_image_info(fAddOnImage, &imageInfo) != B_OK) {
		TRACE_ERROR("Failed to get image info");
		BString error = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
		socket->Write(error.String(), error.Length());
		return;
	}

	BFile file(imageInfo.name, B_READ_ONLY);
	if (file.InitCheck() != B_OK) {
		TRACE_ERROR("Failed to open add-on file");
		BString error = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
		socket->Write(error.String(), error.Length());
		return;
	}

	BResources resources;
	if (resources.SetTo(&file) != B_OK) {
		TRACE_ERROR("Failed to load resources");
		BString error = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
		socket->Write(error.String(), error.Length());
		return;
	}

	size_t size;
	const char* data = (const char*)resources.LoadResource('FILE', resourceName, &size);
	if (!data) {
		TRACE_ERROR("Failed to load resource %s", resourceName);
		BString error = "HTTP/1.1 404 Not Found\r\n\r\n";
		socket->Write(error.String(), error.Length());
		return;
	}

	BString response;
	response << "HTTP/1.1 200 OK\r\n"
			 << "Content-Type: " << GetMimeType(resourceName) << "\r\n"
			 << "Content-Length: " << size << "\r\n"
			 << "Cache-Control: public, max-age=86400\r\n"
			 << "Connection: close\r\n"
			 << "\r\n";

	socket->Write(response.String(), response.Length());
	socket->Write(data, size);

	TRACE_INFO("Sent resource %s: %lu bytes", resourceName, size);
}

void
NetCastServer::SendHTMLPage(BAbstractSocket* socket)
{
	TRACE_VERBOSE("Sending HTML page");

	BString htmlTemplate = LoadHTMLTemplate();
	if (htmlTemplate.Length() == 0) {
		TRACE_ERROR("HTML template is empty, sending error page");
		BString error = "HTTP/1.1 500 Internal Server Error\r\n"
						"Content-Type: text/plain\r\n\r\n"
						"Failed to load HTML template from add-on resources";
		socket->Write(error.String(), error.Length());
		return;
	}

	BString response;
	response << "HTTP/1.1 200 OK\r\n"
			 << "Content-Type: text/html; charset=utf-8\r\n"
			 << "Connection: close\r\n"
			 << "Cache-Control: no-cache\r\n"
			 << "Content-Length: " << htmlTemplate.Length() << "\r\n"
			 << "\r\n"
			 << htmlTemplate;

	socket->Write(response.String(), response.Length());
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

	if (path == "/" || path == "/index.html") {
		SendHTMLPage(clientSocket);
		delete clientSocket;
		return;
	}

	if (path.StartsWith("/resource/")) {
		BString resourceName = path.String() + 10;
		SendResourceFile(clientSocket, resourceName.String());
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

	int sockfd = clientSocket->Socket();

	int sndbuf = CalculateOptimalSendBuffer();
	if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0) {
		TRACE_INFO("Socket send buffer: %d bytes (%.1f KB)", sndbuf, sndbuf / 1024.0f);
	} else {
		TRACE_WARNING("Failed to set SO_SNDBUF: %s", strerror(errno));
	}

	int rcvbuf = 4096;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == 0) {
		TRACE_VERBOSE("Receive buffer reduced to %d bytes", rcvbuf);
	} else {
		TRACE_WARNING("Failed to set SO_RCVBUF: %s", strerror(errno));
	}

	int flags = fcntl(sockfd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
		TRACE_VERBOSE("Set socket to non-blocking mode");
	}

	int nodelay = 1;
	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) == 0) {
		TRACE_VERBOSE("TCP_NODELAY enabled");
	} else {
		TRACE_WARNING("Failed to enable TCP_NODELAY: %s", strerror(errno));
	}

	ClientInfo* info = new ClientInfo;
	info->socket = clientSocket;
	info->address = clientSocket->Peer().ToString();

	int32 colonPos = info->address.FindFirst(':');
	if (colonPos > 0)
		info->address.Truncate(colonPos);

	info->userAgent = userAgent;
	info->headerSent = false;
	info->connectedTime = system_time();
	info->failedSendCount = 0;
	info->lastSuccessfulSend = system_time();

	info->bufferSize = CalculateOptimalSendBuffer();
	info->dataBuffer = new(std::nothrow) uint8[info->bufferSize];
	info->writePos = 0;
	info->readPos = 0;
	info->dataInBuffer = 0;

	if (!info->dataBuffer) {
		TRACE_ERROR("Failed to allocate client buffer");
		delete info;
		delete clientSocket;
		return;
	}

	fClientsLock.Lock();
	fClients.AddItem(info);
	fClientsLock.Unlock();

	TRACE_INFO("Client accepted: %s [%s] (buffer: %lu bytes)",
		info->address.String(), userAgent.String(), info->bufferSize);

	if (fListener)
		fListener->OnClientConnected(info->address.String(), userAgent.String());
}

void
NetCastServer::SendHTTPResponse(BAbstractSocket* socket)
{
	TRACE_VERBOSE("Sending HTTP response");

	BString response;
	response << "HTTP/1.1 200 OK\r\n";

	if (fMimeType == "audio/wav" || fMimeType == "audio/wave") {
		response << "Content-Type: " << fMimeType
				 << "; rate=" << static_cast<int32>(fSampleRate)
				 << "; channels=" << fChannels
				 << "; bits=16\r\n";
	} else {
		response << "Content-Type: " << fMimeType << "\r\n";
	}

	response << "Connection: close\r\n";
	response << "Cache-Control: no-cache, no-store, must-revalidate\r\n";
	response << "Pragma: no-cache\r\n";
	response << "Expires: 0\r\n";
	response << "X-Content-Duration: 0\r\n";

	response << "icy-name: " << fStreamName << "\r\n";

	if (fMimeType == "audio/mpeg")
		response << "icy-br: " << fBitrate << "\r\n";

	response << "icy-pub: 0\r\n";

	response << "X-Audio-Samplerate: " << static_cast<int32>(fSampleRate) << "\r\n";
	response << "X-Audio-Channels: " << fChannels << "\r\n";
	response << "X-Audio-Bitrate: " << fBitrate << "\r\n";

	if (fMimeType == "audio/wav" || fMimeType == "audio/wave")
		response << "X-Audio-Bitdepth: 16\r\n";

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

	fServerURL = "";
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
							fServerURL << "http://" << addrString << ":" << fServerPort;
							fStreamURL << fServerURL << "/stream";
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
			delete[] client->dataBuffer;
			delete client;
		}
	}
	fClients.MakeEmpty();
	fClientsLock.Unlock();

	TRACE_INFO("All clients cleaned up");
}
