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
	// 스레드는 코어의 두 배로 설정
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
			// 1. 완료 포트 생성
			g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
			if (g_hIOCP == NULL) {
				myprintf("CreateIoCompletionPort() failed to create I/O completion port: %d\n",
					GetLastError());
				__leave;
			}

			// 2. 스레드 생성, 핸들 저장
			for (DWORD dwCPU = 0; dwCPU < g_dwThreadCount; dwCPU++) {

				//
				// 오버랩된 I/O 요청을 처리하기 위해 워커 스레드를 생성한다.
				// 시스템의 CPU당 2개의 워커 스레드를 생성하기로 한 결정은 휴리스틱(경험적)이다.
				// 또한, 워커 스레드가 계속 실행될 것이기 때문에 핸들이 더 이상 필요하지 않으므로 스레드 핸들은 바로 닫힌다.
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

			// 3. 리슨 소켓 생성
			if (!CreateListenSocket())
				__leave;

			while (TRUE) {

				//
				// 콘솔이 종료될 때까지 클라이언트들의 연결을 지속적으로 수락한다.
				//
				sdAccept = WSAAccept(g_sdListen, NULL, NULL, NULL, 0);
				if (sdAccept == SOCKET_ERROR) {

					//
					// 사용자가 Ctrl+C, Ctrl+Brk를 누르거나, 콘솔 윈도우를 닫으면 컨트롤 핸들러가 리슨 소켓을 닫는다.
					// 위의 WSAAccept또한 실패하고, 루프에서 나간다.
					//
					myprintf("WSAAccept() failed: %d\n", WSAGetLastError());
					__leave;
				}

				//
				// 우리는 방금 반환된 소켓 디스크립터를 그와 관련된 키 데이터와 함께 IOCP에 추가합니다.
				// 또한, 전역 컨텍스트 구조체 목록(키 데이터)도 전역 목록에 추가됩니다.
				//
				lpPerSocketContext = UpdateCompletionPort(sdAccept, ClientIoRead, TRUE);
				if (lpPerSocketContext == NULL)
					__leave;

				//
				// WSAAccept가 반환된 "후에" CTRL-C가 눌리면, CTRL-C 핸들러가 이 플래그를 설정할 것이고
				// 우리는 다른 읽기 작업을 게시하기 전에(그러나 소켓을 닫기 위한 목록에 추가한 후에) 여기서 루프를 빠져나갈 수 있습니다.
				//
				if (g_bEndServer)
					break;

				//
				// 초기 수신 작업 게시
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
			// 워커 스레드 종료
			//
			if (g_hIOCP) {
				for (DWORD i = 0; i < g_dwThreadCount; i++)
					PostQueuedCompletionStatus(g_hIOCP, 0, 0, NULL);
			}

			//
			// 확실하게 워커 스레드를 종료한다.
			//
			if (WAIT_OBJECT_0 != WaitForMultipleObjects(g_dwThreadCount, g_ThreadHandles, TRUE, 1000))
				myprintf("WaitForMultipleObjects() failed: %d\n", GetLastError());
			else
				// 모든 스레드 핸들이 신호를 보냈다면
				for (DWORD i = 0; i < g_dwThreadCount; i++) {
					if (g_ThreadHandles[i] != INVALID_HANDLE_VALUE) 
						CloseHandle(g_ThreadHandles[i]);
					g_ThreadHandles[i] = INVALID_HANDLE_VALUE;
				}

			// 컨텍스트 리스트를 정리하여 모든 소켓 컨텍스트, io 컨텍스트를 해제한다.
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

		// 리슨 소켓을 닫으면 WSAAccept가 대기 상태에서 반환된다.
		// 서버가 종료될 때 메인 스레드가 WSAAccept의 블로킹에서 빠져나와 종료 절차를 진행하게 만든다.
		// 리슨 소켓을 닫을 때 임시 변수를 사용해야 하는 이유
		// 1. 리슨 소켓에 여러 스레드가 접근할 수 있으므로 데이터 경합을 방지하기 위해
		// 2. 리슨 소켓을 닫는 동안 즉시 INVALID_SOCKET으로 설정하면 다른 스레드가 그 즉시 소켓이 닫혔음을 알 수 있어 잘못된 소켓 핸들을 참조하는 것을 방지할 수 있다.
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

	// 로컬 머신의 네트워크 인터페이스 통해 지정된 포트를 사용하는 소켓 설정
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
	// 소켓의 전송 버퍼링을 비활성화합니다. SO_SNDBUF를 0으로 설정하면 winsock이 전송 버퍼링을 중지하고, 우리 버퍼에서 직접 전송을 수행하여 CPU 사용량을 줄일 수 있습니다.
	// 하지만, 이것은 소켓이 전송 파이프라인을 채우지 못하게 합니다.이로 인해 가득 차지 않은 패킷이 전송될 수 있으며, 이는 IP 및 TCP 헤더의 오버헤드가 전달되는 데이터 양에 비해 크게 되는 문제를 초래할 수 있습니다.
	//	전송 버퍼를 비활성화하는 것은 수신 버퍼를 비활성화하는 것보다 덜 심각한 결과를 가져옵니다.
	//
	nZero = 0;
	nRet = setsockopt(g_sdListen, SOL_SOCKET, SO_SNDBUF, (char*)&nZero, sizeof(nZero));
	if (nRet == SOCKET_ERROR) {
		myprintf("setsockopt(SNDBUF) failed: %d\n", WSAGetLastError());
		return(FALSE);
	}

	//
	// 수신 버퍼링을 비활성화하지 마십시오. 수신 버퍼링을 비활성화하면 네트워크 성능이 저하됩니다.
	// 수신이 게시되지 않고 수신 버퍼가 없으면 TCP 스택이 창 크기를 0으로 설정하고 상대방은 더 이상 데이터를 보낼 수 없게 됩니다.
	// 지속 시간 값을 설정하지 마십시오.특히 중단적 닫기로 설정하지 마십시오.
	// 중단적 닫기로 설정하고 전송될 데이터가 조금 남아 있거나 상대방에게 확인되지 않은 데이터가 있는 경우
	// 연결이 강제로 재설정되어 데이터 손실이 발생할 수 있습니다(즉, 상대방이 마지막 데이터를 받지 못할 수 있습니다).
	// 이는 나쁜 상황입니다.악의적인 클라이언트가 연결한 후 데이터를 보내거나 받지 않는 상황이 걱정된다면
	// 서버는 각 연결에 대해 타이머를 유지해야 합니다.
	// 일정 시간이 지나 서버가 연결을 "정체" 상태로 간주하면 그때 지속 시간을 중단적으로 설정하고 연결을 닫을 수 있습니다.
	//
	// 정체 상태로 간주되는 클라이언트 소켓의 옵션을 변경
	//LINGER lingerStruct;

	//lingerStruct.l_onoff = 1;	// LINGER 옵션 활성화
	//lingerStruct.l_linger = 0;	// 중단적 종료 : 소켓이 닫힐 때 즉시 연결을 종료하고, 남은 데이터를 전송하지 않는다.

	//nRet = setsockopt(g_sdListen, SOL_SOCKET, SO_LINGER,
	//				  (char *)&lingerStruct, sizeof(lingerStruct) );
	//if( nRet == SOCKET_ERROR ) {
	//	myprintf("setsockopt(SO_LINGER) failed: %d\n", WSAGetLastError());
	//	return(FALSE);
	//}
	

	freeaddrinfo(addrlocal);

	return(TRUE);
}


// IOCP에 추가된 모든 소켓 핸들에 대한 IO 요청을 다루는 워커 스레드
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

			// CTRL-C 핸들러는 NULL CompletionKey로 I/O 패킷을 게시한다.
			// 이를 받는 경우 스레드를 종료한다.
			return(0);
		}

		if (g_bEndServer) 
		{
			// 메인 스레드가 서버 종료에 대해 필요한 정리를 수행할 것이다.
			return(0);
		}

		if (!bSuccess || (bSuccess && (dwIoSize == 0))) {

			// 클라이언트 연결 종료 혹은 네트워크 오류. 해당 클라이언트와의 연결을 정리한다.
			CloseClient(lpPerSocketContext, FALSE);
			continue;
		}

		// 이 소켓과 연관된 PER_IO_CONTEXT를 확인하여 어떤 유형의 IO 패킷이 완료되었는지 확인, 처리한다.
		lpIOContext = (PPER_IO_CONTEXT)lpOverlapped;
		switch (lpIOContext->IOOperation) {
		case ClientIoRead:

			// 읽기 작업이 완료되면, 동일한 데이터 버퍼를 통해
			// 클라이언트에게 데이터를 에코하기 위해 쓰기 작업을 게시합니다.
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

			// 쓰기 작업이 완료되면, 전송하려는 모든 데이터가 실제로 전송되었는지 확인합니다.
			lpIOContext->IOOperation = ClientIoWrite;
			lpIOContext->nSentBytes += dwIoSize;
			dwFlags = 0;
			if (lpIOContext->nSentBytes < lpIOContext->nTotalBytes) {

				// 이전 쓰기 작업이 모든 데이터를 보내지 못한 경우,
				// 해당 작업을 완료하기 위해 쓰기 작업을 게시한다.
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

				// 해당 소켓에 대한 이전 쓰기 작업이 완료된 경우, 읽기 작업을 게시한다.
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
//  소켓에 대한 컨텍스트 구조체를 할당하고 소켓을 IOCP에 추가합니다.
//  또한, 컨텍스트 구조체를 글로벌 컨텍스트 구조체 목록에 추가합니다.
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
// 클라이언트와의 연결을 종료한다.
// 소켓을 닫는다.(CTRL-C로 인해 시작된 경우 소켓 종료는 우아하지 않습니다).
// 또한, 해당 소켓과 관련된 모든 컨텍스트 데이터도 해제된다.
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
			// 이어서 수행되는 closesocket이 강제 종료(abortive)되도록 한다.
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

	// IOCP 키 초기화
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

// 컨텍스트 리스트에 클라이언트 연결 컨텍스트를 추가한다.
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
// 컨텍스트 리스트로부터 특정 컨텍스트를 삭제한다.
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
			// 해당 노드가 삭제할 유일한 노드인 경우
			g_pCtxtList = NULL;
		}
		else if ((pBack == NULL) && (pForward != NULL)) 
		{
			// 해당 노드가 리스트의 첫 번째 노드인 경우
			pForward->pCtxtBack = NULL;
			g_pCtxtList = pForward;
		}
		else if ((pBack != NULL) && (pForward == NULL)) 
		{
			// 해당 노드가 리스트의 마지막 노드인 경우
			pBack->pCtxtForward = NULL;
		}
		else if (pBack && pForward) 
		{
			// 해당 노드가 리스트의 중간에 있는 경우
			pBack->pCtxtForward = pForward;
			pForward->pCtxtBack = pBack;
		}

		// 소켓의 모든 io 컨텍스트 해제
		pTempIO = (PPER_IO_CONTEXT)(lpPerSocketContext->pIOContext);
		while(pTempIO)
		{
			pNextIO = (PPER_IO_CONTEXT)(pTempIO->pIOContextForward);

			if (g_bEndServer)
			{
				// 종료 상황에서의 컨텍스트 해제
				// GetQueuedCompletionStatus에 의해 해제되지 않은 컨텍스트에 대해서만 확인한다.
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