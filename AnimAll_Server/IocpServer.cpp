// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (C) Microsoft Corporation.  All Rights Reserved.
//
// Module:
//      iocpserver.cpp
//
// Abstract:
//      This program is a Winsock echo server program that uses I/O Completion Ports 
//      (IOCP) to receive data from and echo data back to a sending client. The server 
//      program supports multiple clients connecting via TCP/IP and sending arbitrary 
//      sized data buffers which the server then echoes back to the client.  For 
//      convenience a simple client program, iocpclient was developed to connect 
//      and continually send data to the server to stress it.
//
//      Direct IOCP support was added to Winsock 2 and is fully implemented on the NT 
//      platform.  IOCPs provide a model for developing very high performance and very 
//      scalable server programs.
//
//      The basic idea is that this server continuously accepts connection requests from 
//      a client program.  When this happens, the accepted socket descriptor is added to 
//      the existing IOCP and an initial receive (WSARecv) is posted on that socket.  When 
//      the client then sends data on that socket, a completion packet will be delivered 
//      and handled by one of the server's worker threads.  The worker thread echoes the 
//      data back to the sender by posting a send (WSASend) containing all the data just 
//      received.  When sending the data back to the client completes, another completion
//      packet will be delivered and again handled by one of the server's worker threads.  
//      Assuming all the data that needed to be sent was actually sent, another receive 
//      (WSARecv) is once again posted and the scenario repeats itself until the client 
//      stops sending data.
//
//      When using IOCPs it is important to remember that the worker threads must be able
//      to distinguish between I/O that occurs on multiple handles in the IOCP as well as 
//      multiple I/O requests initiated on a single handle.  The per handle data 
//      (PER_SOCKET_CONTEXT) is associated with the handle as the CompletionKey when the 
//      handle is added to the IOCP using CreateIoCompletionPort.  The per IO operation 
//      data (PER_IO_CONTEXT) is associated with a specific handle during an I/O 
//      operation as part of the overlapped structure passed to either WSARecv or 
//      WSASend.  Please notice that the first member of the PER_IO_CONTEXT structure is 
//      a WSAOVERLAPPED structure (compatible with the Win32 OVERLAPPED structure).  
//
//      When the worker thread unblocks from GetQueuedCompletionStatus, the key 
//      associated with the handle when the handle was added to the IOCP is returned as 
//      well as the overlapped structure associated when this particular I/O operation 
//      was initiated.
//      
//      This program cleans up all resources and shuts down when CTRL-C is pressed.  
//      This will cause the main thread to break out of the accept loop and close all open 
//      sockets and free all context data.  The worker threads get unblocked by posting  
//      special I/O packets with a NULL CompletionKey to the IOCP.  The worker 
//      threads check for a NULL CompletionKey and exits if it encounters one. If CTRL-BRK 
//      is pressed instead, cleanup process is same as above but instead of exit the process, 
//      the program loops back to restart the server.

//      Another point worth noting is that the Win32 API CreateThread() does not 
//      initialize the C Runtime and therefore, C runtime functions such as 
//      printf() have been avoid or rewritten (see myprintf()) to use just Win32 APIs.
//
//  Usage:
//      Start the server and wait for connections on port 6001
//          iocpserver -e:6001
//
//  Build:
//      Use the headers and libs from the April98 Platform SDK or later.
//      Link with ws2_32.lib
//      
//
//

#pragma warning (disable:4127)

#ifdef _IA64_
#pragma warning(disable:4267)
#endif 

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#define xmalloc(s) HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,(s))
#define xfree(p)   HeapFree(GetProcessHeap(),0,(p))

#include <exception>
#include <process.h>
#include <stdexcept>
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>

#include "iocpserver.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")

const char* g_Port = DEFAULT_PORT;
BOOL g_bEndServer = FALSE;			// set to TRUE on CTRL-C
BOOL g_bRestart = TRUE;				// set to TRUE to CTRL-BRK
BOOL g_bVerbose = FALSE;
DWORD g_dwThreadCount = 0;		//worker thread count
HANDLE g_hIOCP = INVALID_HANDLE_VALUE;
SOCKET g_sdListen = INVALID_SOCKET;
HANDLE g_ThreadHandles[MAX_WORKER_THREAD];
PPER_SOCKET_CONTEXT g_pCtxtList = NULL;		// linked list of context info structures
// maintained to allow the the cleanup 
// handler to cleanly close all sockets and 
// free resources.

CRITICAL_SECTION g_CriticalSection;		// guard access to the global context list

int myprintf(const char* lpFormat, ...);

void __cdecl main(int argc, char* argv[]) {

	SYSTEM_INFO systemInfo;
	WSADATA wsaData;
	SOCKET sdAccept = INVALID_SOCKET;
	PPER_SOCKET_CONTEXT lpPerSocketContext = NULL;
	DWORD dwRecvNumBytes = 0;
	DWORD dwFlags = 0;
	int nRet = 0;

	for (int i = 0; i < MAX_WORKER_THREAD; i++) {
		g_ThreadHandles[i] = INVALID_HANDLE_VALUE;
	}

	if (!ValidOptions(argc, argv))
		return;

	if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
		myprintf("SetConsoleCtrlHandler() failed to install console handler: %d\n",
			GetLastError());
		return;
	}

	GetSystemInfo(&systemInfo);
	// ������� �ھ��� �� ��� ����
	g_dwThreadCount = systemInfo.dwNumberOfProcessors * 2;

	if ((nRet = WSAStartup(MAKEWORD(2, 2), &wsaData)) != 0) {
		myprintf("WSAStartup() failed: %d\n", nRet);
		SetConsoleCtrlHandler(CtrlHandler, FALSE);
		return;
	}

	InitializeCriticalSection(&g_CriticalSection);

	while (g_bRestart) 
	{
		g_bRestart = FALSE;
		g_bEndServer = FALSE;

		__try 
		{
			// 1. �Ϸ� ��Ʈ ����
			g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
			if (g_hIOCP == NULL) {
				myprintf("CreateIoCompletionPort() failed to create I/O completion port: %d\n",
					GetLastError());
				__leave;
			}

			// 2. ������ ����, �ڵ� ����
			for (DWORD dwCPU = 0; dwCPU < g_dwThreadCount; dwCPU++) {

				//
				// �������� I/O ��û�� ó���ϱ� ���� ��Ŀ �����带 �����Ѵ�.
				// �ý����� CPU�� 2���� ��Ŀ �����带 �����ϱ�� �� ������ �޸���ƽ(������)�̴�.
				// ����, ��Ŀ �����尡 ��� ����� ���̱� ������ �ڵ��� �� �̻� �ʿ����� �����Ƿ� ������ �ڵ��� �ٷ� ������.
				//
				HANDLE hThread = INVALID_HANDLE_VALUE;
				UINT dwThreadId = 0;

				//void* argList = (g_hIOCP, lpPerSocketContext);
				hThread = (HANDLE)_beginthreadex(NULL, 0, WorkerThread, g_hIOCP, 0, &dwThreadId);
				if (hThread == NULL) {
					myprintf("CreateThread() failed to create worker thread: %d\n",
						GetLastError());
					__leave;
				}
				g_ThreadHandles[dwCPU] = hThread;
				hThread = INVALID_HANDLE_VALUE;
			}

			// 3. ���� ���� ����
			if (!CreateListenSocket())
				__leave;

			while (TRUE) {

				//
				// �ܼ��� ����� ������ Ŭ���̾�Ʈ���� ������ ���������� �����Ѵ�.
				//
				sdAccept = WSAAccept(g_sdListen, NULL, NULL, NULL, 0);
				if (sdAccept == SOCKET_ERROR) {

					//
					// ����ڰ� Ctrl+C, Ctrl+Brk�� �����ų�, �ܼ� �����츦 ������ ��Ʈ�� �ڵ鷯�� ���� ������ �ݴ´�.
					// ���� WSAAccept���� �����ϰ�, �������� ������.
					//
					myprintf("WSAAccept() failed: %d\n", WSAGetLastError());
					__leave;
				}

				//
				// �츮�� ��� ��ȯ�� ���� ��ũ���͸� �׿� ���õ� Ű �����Ϳ� �Բ� IOCP�� �߰��մϴ�.
				// ����, ���� ���ؽ�Ʈ ����ü ���(Ű ������)�� ���� ��Ͽ� �߰��˴ϴ�.
				//
				lpPerSocketContext = UpdateCompletionPort(sdAccept, ClientIoRead, TRUE);
				if (lpPerSocketContext == NULL)
					__leave;

				//
				// WSAAccept�� ��ȯ�� "�Ŀ�" CTRL-C�� ������, CTRL-C �ڵ鷯�� �� �÷��׸� ������ ���̰�
				// �츮�� �ٸ� �б� �۾��� �Խ��ϱ� ����(�׷��� ������ �ݱ� ���� ��Ͽ� �߰��� �Ŀ�) ���⼭ ������ �������� �� �ֽ��ϴ�.
				//
				if (g_bEndServer)
					break;

				//
				// �ʱ� ���� �۾� �Խ�
				//
				//nRet = WSARecv(sdAccept, &(lpPerSocketContext->pIOContext->wsabuf),
				//	1, &dwRecvNumBytes, &dwFlags,
				//	&(lpPerSocketContext->pIOContext->Overlapped), NULL);
				nRet = WSARecv(sdAccept, &(lpPerSocketContext->pIOContext->wsabuf),
					1, nullptr, &dwFlags,
					&(lpPerSocketContext->pIOContext->Overlapped), NULL);
				if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
					myprintf("WSARecv() Failed: %d\n", WSAGetLastError());
					CloseClient(lpPerSocketContext, FALSE);
				}
			} //while
		}

		__finally {

			g_bEndServer = TRUE;

			//
			// ��Ŀ ������ ����
			//
			if (g_hIOCP) {
				for (DWORD i = 0; i < g_dwThreadCount; i++)
					PostQueuedCompletionStatus(g_hIOCP, 0, 0, NULL);
			}

			//
			// Ȯ���ϰ� ��Ŀ �����带 �����Ѵ�.
			//
			if (WAIT_OBJECT_0 != WaitForMultipleObjects(g_dwThreadCount, g_ThreadHandles, TRUE, 1000))
				myprintf("WaitForMultipleObjects() failed: %d\n", GetLastError());
			else
				// ��� ������ �ڵ��� ��ȣ�� ���´ٸ�
				for (DWORD i = 0; i < g_dwThreadCount; i++) {
					if (g_ThreadHandles[i] != INVALID_HANDLE_VALUE) 
						CloseHandle(g_ThreadHandles[i]);
					g_ThreadHandles[i] = INVALID_HANDLE_VALUE;
				}

			// ���ؽ�Ʈ ����Ʈ�� �����Ͽ� ��� ���� ���ؽ�Ʈ, io ���ؽ�Ʈ�� �����Ѵ�.
			CtxtListFree();

			if (g_hIOCP) {
				CloseHandle(g_hIOCP);
				g_hIOCP = NULL;
			}

			if (g_sdListen != INVALID_SOCKET) {
				closesocket(g_sdListen);
				g_sdListen = INVALID_SOCKET;
			}

			if (sdAccept != INVALID_SOCKET) {
				closesocket(sdAccept);
				sdAccept = INVALID_SOCKET;
			}

		} //finally

		if (g_bRestart) {
			myprintf("\niocpserver is restarting...\n");
		}
		else
			myprintf("\niocpserver is exiting...\n");

	} //while (g_bRestart)

	DeleteCriticalSection(&g_CriticalSection);
	WSACleanup();
	SetConsoleCtrlHandler(CtrlHandler, FALSE);
} //main      

//
//  Just validate the command line options.
//
BOOL ValidOptions(int argc, char* argv[]) {

	BOOL bRet = TRUE;

	for (int i = 1; i < argc; i++) {
		if ((argv[i][0] == '-') || (argv[i][0] == '/')) {
			switch (tolower(argv[i][1])) {
			case 'e':
				if (strlen(argv[i]) > 3)
					g_Port = &argv[i][3];
				break;

			case 'v':
				g_bVerbose = TRUE;
				break;

			case '?':
				myprintf("Usage:\n  iocpserver [-p:port] [-v] [-?]\n");
				myprintf("  -e:port\tSpecify echoing port number\n");
				myprintf("  -v\t\tVerbose\n");
				myprintf("  -?\t\tDisplay this help\n");
				bRet = FALSE;
				break;

			default:
				myprintf("Unknown options flag %s\n", argv[i]);
				bRet = FALSE;
				break;
			}
		}
	}

	return(bRet);
}

//
//  Intercept CTRL-C or CTRL-BRK events and cause the server to initiate shutdown.
//  CTRL-BRK resets the restart flag, and after cleanup the server restarts.
//
BOOL WINAPI CtrlHandler(DWORD dwEvent) {

	SOCKET sockTemp = INVALID_SOCKET;

	switch (dwEvent) {
	case CTRL_BREAK_EVENT:
		g_bRestart = TRUE;
	case CTRL_C_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
	case CTRL_CLOSE_EVENT:
		if (g_bVerbose)
			myprintf("CtrlHandler: closing listening socket\n");

		//
		// cause the accept in the main thread loop to fail
		//

		// ���� ������ ������ WSAAccept�� ��� ���¿��� ��ȯ�ȴ�.
		// ������ ����� �� ���� �����尡 WSAAccept�� ���ŷ���� �������� ���� ������ �����ϰ� �����.
		// ���� ������ ���� �� �ӽ� ������ ����ؾ� �ϴ� ����
		// 1. ���� ���Ͽ� ���� �����尡 ������ �� �����Ƿ� ������ ������ �����ϱ� ����
		// 2. ���� ������ �ݴ� ���� ��� INVALID_SOCKET���� �����ϸ� �ٸ� �����尡 �� ��� ������ �������� �� �� �־� �߸��� ���� �ڵ��� �����ϴ� ���� ������ �� �ִ�.
		sockTemp = g_sdListen;
		g_sdListen = INVALID_SOCKET;
		g_bEndServer = TRUE;
		closesocket(sockTemp);
		sockTemp = INVALID_SOCKET;
		break;

	default:
		// unknown type--better pass it on.
		return(FALSE);
	}
	return(TRUE);
}

//
//  Create a listening socket.
//
BOOL CreateListenSocket(void) {

	int nRet = 0;
	int nZero = 0;
	struct addrinfo hints = { 0 };
	struct addrinfo* addrlocal = NULL;

	// ���� �ӽ��� ��Ʈ��ũ �������̽� ���� ������ ��Ʈ�� ����ϴ� ���� ����
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_IP;

	if (getaddrinfo(NULL, g_Port, &hints, &addrlocal) != 0) {
		myprintf("getaddrinfo() failed with error %d\n", WSAGetLastError());
		return(FALSE);
	}

	if (addrlocal == NULL) {
		myprintf("getaddrinfo() failed to resolve/convert the interface\n");
		return(FALSE);
	}

	g_sdListen = WSASocket(addrlocal->ai_family, addrlocal->ai_socktype, addrlocal->ai_protocol,
		NULL, 0, WSA_FLAG_OVERLAPPED);
	if (g_sdListen == INVALID_SOCKET) {
		myprintf("WSASocket(g_sdListen) failed: %d\n", WSAGetLastError());
		return(FALSE);
	}

	nRet = bind(g_sdListen, addrlocal->ai_addr, (int)addrlocal->ai_addrlen);
	if (nRet == SOCKET_ERROR) {
		myprintf("bind() failed: %d\n", WSAGetLastError());
		return(FALSE);
	}

	nRet = listen(g_sdListen, 5);
	if (nRet == SOCKET_ERROR) {
		myprintf("listen() failed: %d\n", WSAGetLastError());
		return(FALSE);
	}

	//
	// ������ ���� ���۸��� ��Ȱ��ȭ�մϴ�. SO_SNDBUF�� 0���� �����ϸ� winsock�� ���� ���۸��� �����ϰ�, �츮 ���ۿ��� ���� ������ �����Ͽ� CPU ��뷮�� ���� �� �ֽ��ϴ�.
	// ������, �̰��� ������ ���� ������������ ä���� ���ϰ� �մϴ�.�̷� ���� ���� ���� ���� ��Ŷ�� ���۵� �� ������, �̴� IP �� TCP ����� ������尡 ���޵Ǵ� ������ �翡 ���� ũ�� �Ǵ� ������ �ʷ��� �� �ֽ��ϴ�.
	//	���� ���۸� ��Ȱ��ȭ�ϴ� ���� ���� ���۸� ��Ȱ��ȭ�ϴ� �ͺ��� �� �ɰ��� ����� �����ɴϴ�.
	//
	nZero = 0;
	nRet = setsockopt(g_sdListen, SOL_SOCKET, SO_SNDBUF, (char*)&nZero, sizeof(nZero));
	if (nRet == SOCKET_ERROR) {
		myprintf("setsockopt(SNDBUF) failed: %d\n", WSAGetLastError());
		return(FALSE);
	}

	//
	// ���� ���۸��� ��Ȱ��ȭ���� ���ʽÿ�. ���� ���۸��� ��Ȱ��ȭ�ϸ� ��Ʈ��ũ ������ ���ϵ˴ϴ�.
	// ������ �Խõ��� �ʰ� ���� ���۰� ������ TCP ������ â ũ�⸦ 0���� �����ϰ� ������ �� �̻� �����͸� ���� �� ���� �˴ϴ�.
	// ���� �ð� ���� �������� ���ʽÿ�.Ư�� �ߴ��� �ݱ�� �������� ���ʽÿ�.
	// �ߴ��� �ݱ�� �����ϰ� ���۵� �����Ͱ� ���� ���� �ְų� ���濡�� Ȯ�ε��� ���� �����Ͱ� �ִ� ���
	// ������ ������ �缳���Ǿ� ������ �ս��� �߻��� �� �ֽ��ϴ�(��, ������ ������ �����͸� ���� ���� �� �ֽ��ϴ�).
	// �̴� ���� ��Ȳ�Դϴ�.�������� Ŭ���̾�Ʈ�� ������ �� �����͸� �����ų� ���� �ʴ� ��Ȳ�� �����ȴٸ�
	// ������ �� ���ῡ ���� Ÿ�̸Ӹ� �����ؾ� �մϴ�.
	// ���� �ð��� ���� ������ ������ "��ü" ���·� �����ϸ� �׶� ���� �ð��� �ߴ������� �����ϰ� ������ ���� �� �ֽ��ϴ�.
	//
	// ��ü ���·� ���ֵǴ� Ŭ���̾�Ʈ ������ �ɼ��� ����
	//LINGER lingerStruct;

	//lingerStruct.l_onoff = 1;	// LINGER �ɼ� Ȱ��ȭ
	//lingerStruct.l_linger = 0;	// �ߴ��� ���� : ������ ���� �� ��� ������ �����ϰ�, ���� �����͸� �������� �ʴ´�.

	//nRet = setsockopt(g_sdListen, SOL_SOCKET, SO_LINGER,
	//				  (char *)&lingerStruct, sizeof(lingerStruct) );
	//if( nRet == SOCKET_ERROR ) {
	//	myprintf("setsockopt(SO_LINGER) failed: %d\n", WSAGetLastError());
	//	return(FALSE);
	//}
	

	freeaddrinfo(addrlocal);

	return(TRUE);
}


// IOCP�� �߰��� ��� ���� �ڵ鿡 ���� IO ��û�� �ٷ�� ��Ŀ ������
UINT WINAPI WorkerThread(LPVOID WorkThreadContext)
{
	HANDLE hIOCP = (HANDLE)WorkThreadContext;
	BOOL bSuccess = FALSE;
	int nRet = 0;
	LPWSAOVERLAPPED lpOverlapped = NULL;
	PPER_SOCKET_CONTEXT lpPerSocketContext = NULL;
	PPER_IO_CONTEXT lpIOContext = NULL;
	WSABUF buffRecv;
	WSABUF buffSend;
	DWORD dwRecvNumBytes = 0;
	DWORD dwSendNumBytes = 0;
	DWORD dwFlags = 0;
	DWORD dwIoSize = 0;

	while (TRUE) {

		//
		// continually loop to service io completion packets
		//
		bSuccess = GetQueuedCompletionStatus(hIOCP, &dwIoSize,
			(PDWORD_PTR)&lpPerSocketContext,
			(LPOVERLAPPED*)&lpOverlapped,
			INFINITE);
		if (!bSuccess)
			myprintf("GetQueuedCompletionStatus() failed: %d\n", GetLastError());

		if (lpPerSocketContext == NULL) {

			// CTRL-C �ڵ鷯�� NULL CompletionKey�� I/O ��Ŷ�� �Խ��Ѵ�.
			// �̸� �޴� ��� �����带 �����Ѵ�.
			return(0);
		}

		if (g_bEndServer) 
		{
			// ���� �����尡 ���� ���ῡ ���� �ʿ��� ������ ������ ���̴�.
			return(0);
		}

		if (!bSuccess || (bSuccess && (dwIoSize == 0))) {

			// Ŭ���̾�Ʈ ���� ���� Ȥ�� ��Ʈ��ũ ����. �ش� Ŭ���̾�Ʈ���� ������ �����Ѵ�.
			CloseClient(lpPerSocketContext, FALSE);
			continue;
		}

		// �� ���ϰ� ������ PER_IO_CONTEXT�� Ȯ���Ͽ� � ������ IO ��Ŷ�� �Ϸ�Ǿ����� Ȯ��, ó���Ѵ�.
		lpIOContext = (PPER_IO_CONTEXT)lpOverlapped;
		switch (lpIOContext->IOOperation) {
		case ClientIoRead:

			// �б� �۾��� �Ϸ�Ǹ�, ������ ������ ���۸� ����
			// Ŭ���̾�Ʈ���� �����͸� �����ϱ� ���� ���� �۾��� �Խ��մϴ�.
			lpIOContext->IOOperation = ClientIoWrite;
			lpIOContext->nTotalBytes = dwIoSize;
			lpIOContext->nSentBytes = 0;
			lpIOContext->wsabuf.len = dwIoSize;
			dwFlags = 0;
			nRet = WSASend(lpPerSocketContext->Socket, &lpIOContext->wsabuf, 1,
				&dwSendNumBytes, dwFlags, &(lpIOContext->Overlapped), NULL);
			if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
				myprintf("WSASend() failed: %d\n", WSAGetLastError());
				CloseClient(lpPerSocketContext, FALSE);
			}
			else if (g_bVerbose) {
				myprintf("WorkerThread %d: Socket(%d) Recv completed (%d bytes), Send posted\n",
					GetCurrentThreadId(), lpPerSocketContext->Socket, dwIoSize);
			}
			break;

		case ClientIoWrite:

			// ���� �۾��� �Ϸ�Ǹ�, �����Ϸ��� ��� �����Ͱ� ������ ���۵Ǿ����� Ȯ���մϴ�.
			lpIOContext->IOOperation = ClientIoWrite;
			lpIOContext->nSentBytes += dwIoSize;
			dwFlags = 0;
			if (lpIOContext->nSentBytes < lpIOContext->nTotalBytes) {

				// ���� ���� �۾��� ��� �����͸� ������ ���� ���,
				// �ش� �۾��� �Ϸ��ϱ� ���� ���� �۾��� �Խ��Ѵ�.
				buffSend.buf = lpIOContext->Buffer + lpIOContext->nSentBytes;
				buffSend.len = lpIOContext->nTotalBytes - lpIOContext->nSentBytes;
				nRet = WSASend(lpPerSocketContext->Socket, &buffSend, 1,
					&dwSendNumBytes, dwFlags, &(lpIOContext->Overlapped), NULL);
				if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
					myprintf("WSASend() failed: %d\n", WSAGetLastError());
					CloseClient(lpPerSocketContext, FALSE);
				}
				else if (g_bVerbose) {
					myprintf("WorkerThread %d: Socket(%d) Send partially completed (%d bytes), Recv posted\n",
						GetCurrentThreadId(), lpPerSocketContext->Socket, dwIoSize);
				}
			}
			else {

				// �ش� ���Ͽ� ���� ���� ���� �۾��� �Ϸ�� ���, �б� �۾��� �Խ��Ѵ�.
				lpIOContext->IOOperation = ClientIoRead;
				dwRecvNumBytes = 0;
				dwFlags = 0;
				buffRecv.buf = lpIOContext->Buffer,
					buffRecv.len = MAX_BUFF_SIZE;
				nRet = WSARecv(lpPerSocketContext->Socket, &buffRecv, 1,
					&dwRecvNumBytes, &dwFlags, &lpIOContext->Overlapped, NULL);
				if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
					myprintf("WSARecv() failed: %d\n", WSAGetLastError());
					CloseClient(lpPerSocketContext, FALSE);
				}
				else if (g_bVerbose) {
					myprintf("WorkerThread %d: Socket(%d) Send completed (%d bytes), Recv posted\n",
						GetCurrentThreadId(), lpPerSocketContext->Socket, dwIoSize);
				}
			}
			break;

		} //switch
	} //while
	return(0);
}

//
//  ���Ͽ� ���� ���ؽ�Ʈ ����ü�� �Ҵ��ϰ� ������ IOCP�� �߰��մϴ�.
//  ����, ���ؽ�Ʈ ����ü�� �۷ι� ���ؽ�Ʈ ����ü ��Ͽ� �߰��մϴ�.
//
PPER_SOCKET_CONTEXT UpdateCompletionPort(SOCKET sd, IO_OPERATION ClientIo,
	BOOL bAddToList) {

	PPER_SOCKET_CONTEXT lpPerSocketContext;

	lpPerSocketContext = CtxtAllocate(sd, ClientIo);
	if (lpPerSocketContext == NULL)
		return(NULL);

	g_hIOCP = CreateIoCompletionPort((HANDLE)sd, g_hIOCP, (DWORD_PTR)lpPerSocketContext, 0);
	if (g_hIOCP == NULL) {
		myprintf("CreateIoCompletionPort() failed: %d\n", GetLastError());
		if (lpPerSocketContext->pIOContext)
			xfree(lpPerSocketContext->pIOContext);
		xfree(lpPerSocketContext);
		return(NULL);
	}

	//
	//The listening socket context (bAddToList is FALSE) is not added to the list.
	//All other socket contexts are added to the list.
	//
	if (bAddToList) CtxtListAddTo(lpPerSocketContext);

	if (g_bVerbose)
		myprintf("UpdateCompletionPort: Socket(%d) added to IOCP\n", lpPerSocketContext->Socket);

	return(lpPerSocketContext);
}

//
// Ŭ���̾�Ʈ���� ������ �����Ѵ�.
// ������ �ݴ´�.(CTRL-C�� ���� ���۵� ��� ���� ����� ������� �ʽ��ϴ�).
// ����, �ش� ���ϰ� ���õ� ��� ���ؽ�Ʈ �����͵� �����ȴ�.
//
VOID CloseClient(PPER_SOCKET_CONTEXT lpPerSocketContext,
	BOOL bGraceful) {

	EnterCriticalSection(&g_CriticalSection);

	if (lpPerSocketContext) {
		if (g_bVerbose)
			myprintf("CloseClient: Socket(%d) connection closing (graceful=%s)\n",
				lpPerSocketContext->Socket, (bGraceful ? "TRUE" : "FALSE"));
		if (!bGraceful) 
		{
			// �̾ ����Ǵ� closesocket�� ���� ����(abortive)�ǵ��� �Ѵ�.
			LINGER  lingerStruct;

			lingerStruct.l_onoff = 1;
			lingerStruct.l_linger = 0;
			setsockopt(lpPerSocketContext->Socket, SOL_SOCKET, SO_LINGER,
				(char*)&lingerStruct, sizeof(lingerStruct));
		}
		closesocket(lpPerSocketContext->Socket);
		lpPerSocketContext->Socket = INVALID_SOCKET;
		CtxtListDeleteFrom(lpPerSocketContext);
		lpPerSocketContext = NULL;
	}
	else {
		myprintf("CloseClient: lpPerSocketContext is NULL\n");
	}

	LeaveCriticalSection(&g_CriticalSection);

	return;
}

//
// Allocate a socket context for the new connection.  
//
PPER_SOCKET_CONTEXT CtxtAllocate(SOCKET sd, IO_OPERATION ClientIO) {

	PPER_SOCKET_CONTEXT lpPerSocketContext;

	EnterCriticalSection(&g_CriticalSection);

	// IOCP Ű �ʱ�ȭ
	lpPerSocketContext = (PPER_SOCKET_CONTEXT)xmalloc(sizeof(PER_SOCKET_CONTEXT));
	if (lpPerSocketContext) {
		lpPerSocketContext->pIOContext = (PPER_IO_CONTEXT)xmalloc(sizeof(PER_IO_CONTEXT));
		if (lpPerSocketContext->pIOContext) {
			lpPerSocketContext->Socket = sd;
			lpPerSocketContext->pCtxtBack = NULL;
			lpPerSocketContext->pCtxtForward = NULL;

			lpPerSocketContext->pIOContext->Overlapped.Internal = 0;
			lpPerSocketContext->pIOContext->Overlapped.InternalHigh = 0;
			lpPerSocketContext->pIOContext->Overlapped.Offset = 0;
			lpPerSocketContext->pIOContext->Overlapped.OffsetHigh = 0;
			lpPerSocketContext->pIOContext->Overlapped.hEvent = NULL;
			lpPerSocketContext->pIOContext->IOOperation = ClientIO;
			lpPerSocketContext->pIOContext->pIOContextForward = NULL;
			lpPerSocketContext->pIOContext->nTotalBytes = 0;
			lpPerSocketContext->pIOContext->nSentBytes = 0;
			lpPerSocketContext->pIOContext->wsabuf.buf = lpPerSocketContext->pIOContext->Buffer;
			lpPerSocketContext->pIOContext->wsabuf.len = sizeof(lpPerSocketContext->pIOContext->Buffer);

			ZeroMemory(lpPerSocketContext->pIOContext->wsabuf.buf, lpPerSocketContext->pIOContext->wsabuf.len);
		}
		else {
			xfree(lpPerSocketContext);
			myprintf("HeapAlloc() PER_IO_CONTEXT failed: %d\n", GetLastError());
		}

	}
	else {
		myprintf("HeapAlloc() PER_SOCKET_CONTEXT failed: %d\n", GetLastError());
	}

	LeaveCriticalSection(&g_CriticalSection);

	return(lpPerSocketContext);
}

// ���ؽ�Ʈ ����Ʈ�� Ŭ���̾�Ʈ ���� ���ؽ�Ʈ�� �߰��Ѵ�.
VOID CtxtListAddTo(PPER_SOCKET_CONTEXT lpPerSocketContext) {

	PPER_SOCKET_CONTEXT     pTemp;

	EnterCriticalSection(&g_CriticalSection);

	if (g_pCtxtList == NULL) {

		//
		// add the first node to the linked list
		//
		lpPerSocketContext->pCtxtBack = NULL;
		lpPerSocketContext->pCtxtForward = NULL;
		g_pCtxtList = lpPerSocketContext;
	}
	else {

		//
		// add node to head of list
		//
		pTemp = g_pCtxtList;

		g_pCtxtList = lpPerSocketContext;
		lpPerSocketContext->pCtxtBack = pTemp;
		lpPerSocketContext->pCtxtForward = NULL;

		pTemp->pCtxtForward = lpPerSocketContext;
	}

	LeaveCriticalSection(&g_CriticalSection);
	return;
}

//
// ���ؽ�Ʈ ����Ʈ�κ��� Ư�� ���ؽ�Ʈ�� �����Ѵ�.
//
VOID CtxtListDeleteFrom(PPER_SOCKET_CONTEXT lpPerSocketContext) {

	PPER_SOCKET_CONTEXT pBack;
	PPER_SOCKET_CONTEXT pForward;
	PPER_IO_CONTEXT     pNextIO = NULL;
	PPER_IO_CONTEXT     pTempIO = NULL;


	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		myprintf("EnterCriticalSection raised an exception.\n");
		return;
	}

	if (lpPerSocketContext) {
		pBack = lpPerSocketContext->pCtxtBack;
		pForward = lpPerSocketContext->pCtxtForward;


		if ((pBack == NULL) && (pForward == NULL)) 
		{
			// �ش� ��尡 ������ ������ ����� ���
			g_pCtxtList = NULL;
		}
		else if ((pBack == NULL) && (pForward != NULL)) 
		{
			// �ش� ��尡 ����Ʈ�� ù ��° ����� ���
			pForward->pCtxtBack = NULL;
			g_pCtxtList = pForward;
		}
		else if ((pBack != NULL) && (pForward == NULL)) 
		{
			// �ش� ��尡 ����Ʈ�� ������ ����� ���
			pBack->pCtxtForward = NULL;
		}
		else if (pBack && pForward) 
		{
			// �ش� ��尡 ����Ʈ�� �߰��� �ִ� ���
			pBack->pCtxtForward = pForward;
			pForward->pCtxtBack = pBack;
		}

		// ������ ��� io ���ؽ�Ʈ ����
		pTempIO = (PPER_IO_CONTEXT)(lpPerSocketContext->pIOContext);
		while(pTempIO)
		{
			pNextIO = (PPER_IO_CONTEXT)(pTempIO->pIOContextForward);

			if (g_bEndServer)
			{
				// ���� ��Ȳ������ ���ؽ�Ʈ ����
				// GetQueuedCompletionStatus�� ���� �������� ���� ���ؽ�Ʈ�� ���ؼ��� Ȯ���Ѵ�.
				while (!HasOverlappedIoCompleted((LPOVERLAPPED)pTempIO))
					Sleep(0);
			}
			xfree(pTempIO);

			pTempIO = pNextIO;
		}

		xfree(lpPerSocketContext);
		lpPerSocketContext = NULL;

	}
	else {
		myprintf("CtxtListDeleteFrom: lpPerSocketContext is NULL\n");
	}

	LeaveCriticalSection(&g_CriticalSection);
	return;
}

//
//  Free all context structure in the global list of context structures.
//
VOID CtxtListFree() {

	PPER_SOCKET_CONTEXT     pTemp1, pTemp2;

	EnterCriticalSection(&g_CriticalSection);

	pTemp1 = g_pCtxtList;
	while (pTemp1) {
		pTemp2 = pTemp1->pCtxtBack;
		CloseClient(pTemp1, FALSE);
		pTemp1 = pTemp2;
	}

	LeaveCriticalSection(&g_CriticalSection);
	return;
}

//
// Our own printf. This is done because calling printf from multiple
// threads can AV. The standard out for WriteConsole is buffered...
//
int myprintf(const char* lpFormat, ...) {

	int nLen = 0;
	int nRet = 0;
	char cBuffer[512];
	va_list arglist;
	HANDLE hOut = NULL;
	HRESULT hRet;

	ZeroMemory(cBuffer, sizeof(cBuffer));

	va_start(arglist, lpFormat);

	nLen = lstrlenA(lpFormat);
	hRet = StringCchVPrintfA(cBuffer, 512, lpFormat, arglist);

	if (nRet >= nLen || GetLastError() == 0) {
		hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		if (hOut != INVALID_HANDLE_VALUE)
			WriteConsole(hOut, cBuffer, lstrlenA(cBuffer), (LPDWORD)&nLen, NULL);
	}

	return nLen;
}