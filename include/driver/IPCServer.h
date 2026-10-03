// SPDX-License-Identifier: AGPL-3.0-only
// Modified by simplyyjessie, 2026-10-03 (Linux port). See NOTICE.md

#pragma once

#include "Protocol.h"

#include <thread>
#include <set>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

class ServerTrackedDeviceProvider;

class IPCServer
{
public:
	IPCServer(ServerTrackedDeviceProvider *driver) : driver(driver) { }
	~IPCServer();

	void Run();
	void Stop();

private:
	void HandleRequest(const protocol::Request &request, protocol::Response &response);

#ifdef _WIN32
	struct PipeInstance
	{
		OVERLAPPED overlap; // Used by the API
		HANDLE pipe;
		IPCServer *server;

		protocol::Request request;
		protocol::Response response;
	};

	PipeInstance *CreatePipeInstance(HANDLE pipe);
	void ClosePipeInstance(PipeInstance *pipeInst);

	static void RunThread(IPCServer *_this);
	static BOOL CreateAndConnectInstance(LPOVERLAPPED overlap, HANDLE &pipe);
	static void WINAPI CompletedReadCallback(DWORD err, DWORD bytesRead, LPOVERLAPPED overlap);
	static void WINAPI CompletedWriteCallback(DWORD err, DWORD bytesWritten, LPOVERLAPPED overlap);
#else
	static void RunThread(IPCServer *_this);
	// Answers one request from the client; false when it should be dropped.
	bool ServeClient(int client);
#endif

	std::thread mainThread;

	bool running = false;
	bool stop = false;

#ifdef _WIN32
	std::set<PipeInstance *> pipes;
	HANDLE connectEvent;
#else
	std::set<int> clients;
	int listenSocket = -1;
	int wakeFd = -1;  // eventfd that interrupts poll() on Stop()
#endif

	ServerTrackedDeviceProvider *driver;
};
