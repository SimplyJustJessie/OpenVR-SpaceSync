// SPDX-License-Identifier: AGPL-3.0-only
// Added by simplyyjessie, 2026-10-03 (Linux port). Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md

// Linux transport for IPCServer: an abstract-namespace SOCK_SEQPACKET Unix
// socket. SEQPACKET keeps message boundaries like the Windows message-mode
// pipe, so each recv() is exactly one protocol::Request. Requests are
// handled on the IPC thread, same as the Windows completion routines.

#include "IPCServer.h"
#include "Logging.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <vector>

void IPCServer::Stop()
{
	TRACE("IPCServer::Stop()");
	if (!mainThread.joinable())
		return;

	stop = true;
	if (wakeFd >= 0)
	{
		uint64_t one = 1;
		ssize_t ignored = write(wakeFd, &one, sizeof one);
		(void)ignored;
	}
	mainThread.join();
	running = false;
	TRACE("IPCServer::Stop() finished");
}

static int CreateListenSocket()
{
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
	{
		LOG("IPC socket() failed: %s", strerror(errno));
		return -1;
	}

	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	// Abstract namespace: sun_path starts with a NUL byte.
	const size_t nameLen = strlen(SPACESYNC_SOCKET_NAME);
	memcpy(addr.sun_path + 1, SPACESYNC_SOCKET_NAME, nameLen);
	socklen_t addrLen = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + nameLen);

	if (bind(fd, (sockaddr *)&addr, addrLen) != 0)
	{
		LOG("IPC bind failed (is another SteamVR instance running?): %s", strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 4) != 0)
	{
		LOG("IPC listen failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

// Abstract sockets have no file permissions, so any local user could
// connect. Only accept the user vrserver runs as.
static bool PeerIsSameUser(int client)
{
	ucred cred = {};
	socklen_t len = sizeof cred;
	if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
		return false;
	return cred.uid == getuid();
}

bool IPCServer::ServeClient(int client)
{
	protocol::Request request;
	ssize_t got = recv(client, &request, sizeof request, 0);
	if (got == 0)
	{
		LOG("%s", "IPC client disconnecting normally");
		return false;
	}
	if (got < 0)
	{
		if (errno == EINTR || errno == EAGAIN)
			return true;
		LOG("IPC client disconnecting due to read error: %s", strerror(errno));
		return false;
	}
	if ((size_t)got != sizeof request)
	{
		LOG("IPC client sent %zd bytes, expected %zu; disconnecting", got, sizeof request);
		return false;
	}

	protocol::Response response(protocol::ResponseInvalid);
	HandleRequest(request, response);

	ssize_t sent = send(client, &response, sizeof response, MSG_NOSIGNAL);
	if (sent != (ssize_t)sizeof response)
	{
		LOG("IPC client disconnecting due to write error: %s", sent < 0 ? strerror(errno) : "short write");
		return false;
	}
	return true;
}

void IPCServer::RunThread(IPCServer *_this)
{
	_this->running = true;

	_this->wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (_this->wakeFd < 0)
	{
		LOG("IPC eventfd failed: %s", strerror(errno));
		return;
	}

	_this->listenSocket = CreateListenSocket();
	if (_this->listenSocket < 0)
	{
		close(_this->wakeFd);
		_this->wakeFd = -1;
		return;
	}

	std::vector<pollfd> fds;
	while (!_this->stop)
	{
		fds.clear();
		fds.push_back({ _this->wakeFd, POLLIN, 0 });
		fds.push_back({ _this->listenSocket, POLLIN, 0 });
		for (int client : _this->clients)
			fds.push_back({ client, POLLIN, 0 });

		if (poll(fds.data(), fds.size(), -1) < 0)
		{
			if (errno == EINTR)
				continue;
			LOG("IPC poll failed: %s", strerror(errno));
			break;
		}

		if (_this->stop)
			break;

		if (fds[1].revents & POLLIN)
		{
			int client = accept4(_this->listenSocket, nullptr, nullptr, SOCK_CLOEXEC);
			if (client >= 0)
			{
				if (PeerIsSameUser(client))
				{
					LOG("%s", "IPC client connected");
					_this->clients.insert(client);
				}
				else
				{
					LOG("%s", "IPC client from another user rejected");
					close(client);
				}
			}
		}

		for (size_t i = 2; i < fds.size(); i++)
		{
			if (!fds[i].revents)
				continue;
			bool keep = (fds[i].revents & POLLIN) && _this->ServeClient(fds[i].fd);
			if (!keep)
			{
				close(fds[i].fd);
				_this->clients.erase(fds[i].fd);
			}
		}
	}

	for (int client : _this->clients)
		close(client);
	_this->clients.clear();
	close(_this->listenSocket);
	_this->listenSocket = -1;
	close(_this->wakeFd);
	_this->wakeFd = -1;
}
