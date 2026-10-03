// SPDX-License-Identifier: AGPL-3.0-only
// Modified by simplyyjessie, 2026-10-03 (Linux port). See NOTICE.md

#pragma once

#ifdef _WIN32
#include <Windows.h>
#endif

#include "Protocol.h"

class IPCClient
{
public:
	~IPCClient();

	void Connect();
	bool TryConnect();
#ifdef _WIN32
	bool IsConnected() const { return pipe && pipe != INVALID_HANDLE_VALUE; }
#else
	bool IsConnected() const { return sock >= 0; }
#endif
	protocol::Response SendBlocking(const protocol::Request &request);

	void Send(const protocol::Request &request);
	protocol::Response Receive();

private:
	void Disconnect();

#ifdef _WIN32
	void ConnectInternal(DWORD waitMs = 1000);
	HANDLE pipe = INVALID_HANDLE_VALUE;
#else
	void ConnectInternal(unsigned waitMs = 1000);
	int sock = -1;
#endif
};