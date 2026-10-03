// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Linux port). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

// Linux counterpart of IPCClient.cpp: talks to the driver over the abstract
// SOCK_SEQPACKET socket served by IPCServerPosix.cpp. Behaviour (retries,
// errors thrown, handshake) mirrors the named-pipe client.

#include "IPCClient.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

IPCClient::~IPCClient()
{
	Disconnect();
}

void IPCClient::Connect()
{
	const int maxAttempts = 3;
	unsigned delayMs = 2000;

	for (int attempt = 1; attempt <= maxAttempts; ++attempt)
	{
		try
		{
			ConnectInternal();
			return;
		}
		catch (const std::runtime_error& e)
		{
			Disconnect();

			if (attempt >= maxAttempts)
				throw;

			fprintf(stderr, "IPC connect failed (attempt %d/3), retrying in %ums: %s\n", attempt, delayMs, e.what());
			std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
			delayMs *= 2;
		}
	}
}

bool IPCClient::TryConnect()
{
	try
	{
		ConnectInternal(100);
		return true;
	}
	catch (const std::runtime_error&)
	{
		Disconnect();
		return false;
	}
}

void IPCClient::Disconnect()
{
	if (sock >= 0)
		close(sock);
	sock = -1;
}

protocol::Response IPCClient::SendBlocking(const protocol::Request &request)
{
	if (!IsConnected())
		return protocol::Response(protocol::ResponseInvalid);
	try
	{
		Send(request);
		return Receive();
	}
	catch (const std::runtime_error&)
	{
		Disconnect();
		return protocol::Response(protocol::ResponseInvalid);
	}
}

void IPCClient::Send(const protocol::Request &request)
{
	ssize_t sent = send(sock, &request, sizeof request, MSG_NOSIGNAL);
	if (sent != (ssize_t)sizeof request)
	{
		throw std::runtime_error(std::string("Error writing IPC request. Error: ") + (sent < 0 ? strerror(errno) : "short write"));
	}
}

protocol::Response IPCClient::Receive()
{
	protocol::Response response(protocol::ResponseInvalid);

	ssize_t got;
	do
	{
		got = recv(sock, &response, sizeof response, 0);
	} while (got < 0 && errno == EINTR);

	if (got < 0)
	{
		throw std::runtime_error(std::string("Error reading IPC response. Error: ") + strerror(errno));
	}
	if (got != (ssize_t)sizeof response)
	{
		throw std::runtime_error("Invalid IPC response with size " + std::to_string(got));
	}

	return response;
}

static int TryOpenSocket()
{
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	const size_t nameLen = strlen(SPACESYNC_SOCKET_NAME);
	memcpy(addr.sun_path + 1, SPACESYNC_SOCKET_NAME, nameLen);
	socklen_t addrLen = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + nameLen);

	if (connect(fd, (sockaddr *)&addr, addrLen) != 0)
	{
		close(fd);
		return -1;
	}
	return fd;
}

void IPCClient::ConnectInternal(unsigned waitMs)
{
	// Like WaitNamedPipe: keep trying for up to waitMs while the driver
	// is not listening yet.
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
	sock = TryOpenSocket();
	while (sock < 0 && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		sock = TryOpenSocket();
	}
	if (sock < 0)
	{
		throw std::runtime_error("SpaceSync driver unavailable. Make sure SteamVR is running and the SpaceSync add-on is enabled in SteamVR settings.");
	}

	Send(protocol::Request(protocol::RequestHandshake));
	auto response = Receive();

	if (response.type != protocol::ResponseHandshake || response.protocol.version != protocol::Version)
	{
		throw std::runtime_error(
			"Incorrect driver version installed, try reinstalling SpaceSync. (Client: " +
			std::to_string(protocol::Version) + ", Driver: " +
			std::to_string(response.protocol.version) + ")"
		);
	}
}
