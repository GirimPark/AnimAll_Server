// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (C) Microsoft Corporation.  All Rights Reserved.
//
// Module:
//      iocpclient.cpp
//
// Abstract:
//      Use the -? commandline switch to determine available options.
//
//      This application is a very simple minded program that sends buffers of
//      data to a server and waits for the server to echo the data back and then
//      compares the first and last byte of the buffers.  The destination server
//      can be specified using the (-n) option and the destination port using the 
//      (-e) option.  The size of the buffer to send is specified using the (-b)
//      option which is in 1k increments.  Multiple threads can be spawned to hit
//      the server.
//
//      Please note that spawning multiple threads is not a scalable way
//      to handle multiple socket connections.  This sample was built for the
//      express purpose of providing a simple and easy to understand client to 
//      pound on the iocp socket server.  
//
//      Another point worth noting is that the Win32 API CreateThread() does not 
//      initialize the C Runtime and therefore, C runtime functions such as 
//      printf() have been avoid or rewritten (see printf()) to use just Win32 APIs.
//
// Entry Points:
//      main - this is where it all starts
//
// Build:
//      Use the headers and libs from the Jan98 Platform SDK or later.
//      Link with ws2_32.lib
//      
//
//

#pragma warning (disable:4127)
#pragma warning(disable: 4996)

#ifdef _IA64_
#pragma warning(disable:4706 4267)
#endif 

#include <process.h>
#include <stdlib.h>
#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <strsafe.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")

#define MAXTHREADS 64

#define xmalloc(s) HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,(s))
#define xfree(p)   {HeapFree(GetProcessHeap(),0,(p)); p = NULL;}

typedef struct _OPTIONS {
	char szHostname[64];
	char* port;
	int nTotalThreads;
	int nBufSize;
	BOOL bVerbose;
} OPTIONS;

typedef struct THREADINFO {
	HANDLE hThread[MAXTHREADS];
	SOCKET sd[MAXTHREADS];
} THREADINFO;

static OPTIONS default_options = { "localhost", (char*)"5001", 1, 4096, FALSE };
static OPTIONS g_Options;
static THREADINFO g_ThreadInfo;
static BOOL g_bEndClient = FALSE;
static WSAEVENT g_hCleanupEvent[1];	// 이벤트 개체

static BOOL WINAPI CtrlHandler(DWORD dwEvent);
static BOOL ValidOptions(char* argv[], int argc);
static VOID Usage(char* szProgramname, OPTIONS* pOptions);
static UINT WINAPI EchoThread(LPVOID lpParameter);
static BOOL CreateConnectedSocket(int nThreadNum);
static BOOL SendBuffer(int nThreadNum, char* outbuf);
static BOOL RecvBuffer(int nThreadNum, char* inbuf);

int __cdecl main(int argc, char* argv[]) {

	OSVERSIONINFO verInfo = { 0 };
	WSADATA WSAData;
	UINT dwThreadId = 0;
	DWORD dwRet = 0;
	BOOL bInitError = FALSE;
	int nThreadNum[MAXTHREADS];
	int i = 0;
	int nRet = 0;

	verInfo.dwOSVersionInfoSize = sizeof(verInfo);
	GetVersionEx(&verInfo);
	if (verInfo.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) {

		//
		// Since this application can heavily stress system resources
		// we decided to limit running it on NT.
		//
		printf("Please run %s only on NT, thank you\n", argv[0]);
		return(0);
	}

	for (i = 0; i < MAXTHREADS; i++) {
		g_ThreadInfo.sd[i] = INVALID_SOCKET;
		g_ThreadInfo.hThread[i] = INVALID_HANDLE_VALUE;
		nThreadNum[i] = 0;
	}

	g_hCleanupEvent[0] = WSA_INVALID_EVENT;

	if (!ValidOptions(argv, argc))
		return(1);

	if ((nRet = WSAStartup(MAKEWORD(2, 2), &WSAData)) != 0) {
		printf("WSAStartup() failed: %d", nRet);
		return(1);
	}

	// 이벤트 개체 생성
	if (WSA_INVALID_EVENT == (g_hCleanupEvent[0] = WSACreateEvent()))
	{
		printf("WSACreateEvent() failed: %d\n", WSAGetLastError());
		WSACleanup();
		return(1);
	}

	//
	// be able to gracefully handle CTRL-C and close handles
	//
	if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
		printf("SetConsoleCtrlHandler() failed: %d\n", GetLastError());
		if (g_hCleanupEvent[0] != WSA_INVALID_EVENT) {
			WSACloseEvent(g_hCleanupEvent[0]);
			g_hCleanupEvent[0] = WSA_INVALID_EVENT;
		}
		WSACleanup();
		return(1);
	}

	//
	// spawn the threads 
	//
	for (i = 0; i < g_Options.nTotalThreads && !bInitError; i++) {

		// CTRL-C가 모든 소켓이 연결되기 전에 눌리면,
		// 특히 서버가 다운되어 있고 연결 실패를 기다려야 하는 경우 프로그램 종료에 시간이 좀 걸릴 수 있습니다.
		// 이 전역 플래그를 확인하면 그 과정을 단축할 수 있습니다.
		if (g_bEndClient)
			break;

		else if (CreateConnectedSocket(i)) {
			// 각 스레드에 고유한 메모리 위치를 전달해야 합니다.
			// 그렇지 않으면 모든 스레드가 실행될 때쯤 값이 변경될 수 있습니다.
			nThreadNum[i] = i;
			g_ThreadInfo.hThread[i] = (HANDLE)_beginthreadex(NULL, 0, EchoThread, &nThreadNum[i], 0, &dwThreadId);
			if (g_ThreadInfo.hThread[i] == NULL) {
				printf("CreateThread(%d) failed: %d\n", i, GetLastError());
				bInitError = TRUE;
				break;
			}
		}
	}

	if (!bInitError) {

		//
		// wait for the threads to exit
		//
		dwRet = WaitForMultipleObjects(g_Options.nTotalThreads, g_ThreadInfo.hThread, TRUE, INFINITE);
		if (dwRet == WAIT_FAILED)
			printf("WaitForMultipleObject(): %d\n", GetLastError());
	}

	if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
		printf("GenerateConsoleCtrlEvent() failed: %d\n", GetLastError());
	};

	if (WSAWaitForMultipleEvents(1, g_hCleanupEvent, TRUE, WSA_INFINITE, FALSE) == WSA_WAIT_FAILED) {
		printf("WSAWaitForMultipleEvents() failed: %d\n", WSAGetLastError());
	};

	if (g_hCleanupEvent[0] != WSA_INVALID_EVENT) {
		WSACloseEvent(g_hCleanupEvent[0]);
		g_hCleanupEvent[0] = WSA_INVALID_EVENT;
	}

	WSACleanup();

	//
	// Restores default processing of CTRL signals.
	//
	SetConsoleCtrlHandler(CtrlHandler, FALSE);
	SetConsoleCtrlHandler(NULL, FALSE);

	return(0);
}

//
// Abstract:
//     This is the thread that continually sends and receives a specific size
//     buffer to the server.  Upon receipt of the echo from the server, a
//     simple check is performed to check the integrity of the transfer.
//
static UINT WINAPI EchoThread(LPVOID lpParameter)
{
	char* inbuf = NULL;
	char* outbuf = NULL;
	int* pArg = (int*)lpParameter;
	int nThreadNum = *pArg;

	printf("Starting thread %d\n", nThreadNum);

	inbuf = (char*)xmalloc(g_Options.nBufSize);
	outbuf = (char*)xmalloc(g_Options.nBufSize);

	if ((inbuf) && (outbuf)) {

		//
		// NOTE data possible data loss with INT conversion to BYTE
		//
		FillMemory(outbuf, g_Options.nBufSize, (BYTE)nThreadNum);	// memset
		memcpy(outbuf, "echo", 5);

		while (TRUE) {

			//
			// just continually send and wait for the server to echo the data
			// back.  Just do a simple minded comparison.
			//
			if (SendBuffer(nThreadNum, outbuf) &&
				RecvBuffer(nThreadNum, inbuf)) 
			{
				if ((inbuf[0] == outbuf[0]) &&
					(inbuf[g_Options.nBufSize - 1] == outbuf[g_Options.nBufSize - 1])) 
				{
					if (g_Options.bVerbose)
						printf("ack(%d)\n", nThreadNum);
				}
				else 
				{
					printf("nak(%d) in[0]=%d, out[0]=%d in[%d]=%d out[%d]%d\n",
						nThreadNum,
						inbuf[0], outbuf[0],
						g_Options.nBufSize - 1, inbuf[g_Options.nBufSize - 1],
						g_Options.nBufSize - 1, outbuf[g_Options.nBufSize - 1]);
					break;
				}
			}
			else
				break;
		}
	}

	if (inbuf)
		xfree(inbuf);
	if (outbuf)
		xfree(outbuf);

	return(TRUE);
}

// 소켓을 생성하고 서버와 연결한다.
static BOOL CreateConnectedSocket(int nThreadNum)
{
	BOOL bRet = TRUE;
	int nRet = 0;
	struct addrinfo hints = { 0 };
	struct addrinfo* addr_srv = NULL;

	//
	// Resolve the interface
	//
	hints.ai_flags = 0;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (getaddrinfo(g_Options.szHostname, g_Options.port, &hints, &addr_srv) != 0) {
		printf("getaddrinfo() failed with error %d\n", WSAGetLastError());
		bRet = FALSE;
	}

	if (addr_srv == NULL) {
		printf("getaddrinfo() failed to resolve/convert the interface\n");
		bRet = FALSE;
	}
	else {
		g_ThreadInfo.sd[nThreadNum] = socket(addr_srv->ai_family, addr_srv->ai_socktype, addr_srv->ai_protocol);
		if (g_ThreadInfo.sd[nThreadNum] == INVALID_SOCKET) {
			printf("socket() failed: %d\n", WSAGetLastError());
			bRet = FALSE;
		}
	}

	if (bRet != FALSE) {
		nRet = connect(g_ThreadInfo.sd[nThreadNum], addr_srv->ai_addr, (int)addr_srv->ai_addrlen);
		if (nRet == SOCKET_ERROR) {
			printf("connect(thread %d) failed: %d\n", nThreadNum, WSAGetLastError());
			bRet = FALSE;
		}
		else
			printf("connected(thread %d)\n", nThreadNum);

		freeaddrinfo(addr_srv);
	}

	return(bRet);
}

//
// Abstract:
//     Send a buffer - keep send'ing until the requested amount of
//     data has been sent or the socket has been closed or error.
//
static BOOL SendBuffer(int nThreadNum, char* outbuf) {

	BOOL bRet = TRUE;
	char* bufp = outbuf;
	int nTotalSend = 0;
	int nSend = 0;

	while (nTotalSend < g_Options.nBufSize) {
		nSend = send(g_ThreadInfo.sd[nThreadNum], bufp, g_Options.nBufSize - nTotalSend, 0);
		if (nSend == SOCKET_ERROR) {
			printf("send(thread=%d) failed: %d\n", nThreadNum, WSAGetLastError());
			bRet = FALSE;
			break;
		}
		else if (nSend == 0) {
			printf("connection closed\n");
			bRet = FALSE;
			break;
		}
		else {
			nTotalSend += nSend;
			bufp += nSend;
		}
	}

	if (strlen(outbuf) != 0)
		printf("send : %s\n", outbuf);

	return(bRet);
}

//
// Abstract:
//     Receive a buffer - keep recv'ing until the requested amount of
//     data has been received or the socket has been closed or error.
//
static BOOL RecvBuffer(int nThreadNum, char* inbuf) {

	BOOL bRet = TRUE;
	char* bufp = inbuf;
	int nTotalRecv = 0;
	int nRecv = 0;

	while (nTotalRecv < g_Options.nBufSize) {
		nRecv = recv(g_ThreadInfo.sd[nThreadNum], bufp, g_Options.nBufSize - nTotalRecv, 0);
		if (nRecv == SOCKET_ERROR) {
			printf("recv(thread=%d) failed: %d\n", nThreadNum, WSAGetLastError());
			bRet = FALSE;
			break;
		}
		else if (nRecv == 0) {
			printf("connection closed\n");
			bRet = FALSE;
			break;
		}
		else {
			nTotalRecv += nRecv;
			bufp += nRecv;
		}
	}

	if(strlen(inbuf) != 0)
		printf("recv : %s\n", inbuf);

	return(bRet);
}

//
// Abstract:
//      Verify options passed in and set options structure accordingly.
//
static BOOL ValidOptions(char* argv[], int argc) {

	g_Options = default_options;
	HRESULT hRet;

	for (int i = 1; i < argc; i++) {
		if ((argv[i][0] == '-') || (argv[i][0] == '/')) {
			switch (tolower(argv[i][1])) {
			case 'b':
				if (lstrlen((LPCWSTR)argv[i]) > 3)
					g_Options.nBufSize = 1024 * atoi(&argv[i][3]);
				break;

			case 'e':
				if (lstrlen((LPCWSTR)argv[i]) > 3)
					g_Options.port = &argv[i][3];
				break;

			case 'n':
				if (lstrlen((LPCWSTR)argv[i]) > 3)
				{
					hRet = StringCbCopyN((STRSAFE_LPWSTR)g_Options.szHostname, 64, (STRSAFE_PCNZWCH)&argv[i][3], 64);
				}
				break;

			case 't':
				if (lstrlen((LPCWSTR)argv[i]) > 3)
					g_Options.nTotalThreads = min(MAXTHREADS, atoi(&argv[i][3]));
				break;

			case 'v':
				g_Options.bVerbose = TRUE;
				break;

			case '?':
				Usage(argv[0], &default_options);
				return(FALSE);
				break;

			default:
				printf("  unknown options flag %s\n", argv[i]);
				Usage(argv[0], &default_options);
				return(FALSE);
				break;
			}
		}
		else {
			printf("  unknown option %s\n", argv[i]);
			Usage(argv[0], &default_options);
			return(FALSE);
		}
	}

	return(TRUE);
}

//
// Abstract:
//      Print out usage table for the program
//
static VOID Usage(char* szProgramname, OPTIONS* pOptions) {

	printf("usage:\n%s [-b:#] [-e:#] [-n:host] [-t:#] [-v]\n",
		szProgramname);
	printf("%s -?\n", szProgramname);
	printf("  -?\t\tDisplay this help\n");
	printf("  -b:bufsize\tSize of send/recv buffer; in 1K increments (Def:%d)\n",
		pOptions->nBufSize);
	printf("  -e:port\tEndpoint number (port) to use (Def:%d)\n",
		pOptions->port);
	printf("  -n:host\tAct as the client and connect to 'host' (Def:%s)\n",
		pOptions->szHostname);
	printf("  -t:#\tNumber of threads to use\n");
	printf("  -v\t\tVerbose, print an ack when echo received and verified\n");
	return;
}

static BOOL WINAPI CtrlHandler(DWORD dwEvent) {

	int i = 0;
	DWORD dwRet = 0;

	switch (dwEvent) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
	case CTRL_CLOSE_EVENT:

		printf("Closing handles and sockets\n");

		//
		// Temporarily disables processing of CTRL_C_EVENT signal.
		//
		SetConsoleCtrlHandler(NULL, TRUE);

		g_bEndClient = TRUE;

		for (i = 0; i < g_Options.nTotalThreads; i++) {
			if (g_ThreadInfo.sd[i] != INVALID_SOCKET) {

				//
				// force the subsequent closesocket to be abortative.
				//
				LINGER  lingerStruct;

				lingerStruct.l_onoff = 1;
				lingerStruct.l_linger = 0;
				setsockopt(g_ThreadInfo.sd[i], SOL_SOCKET, SO_LINGER,
					(char*)&lingerStruct, sizeof(lingerStruct));
				closesocket(g_ThreadInfo.sd[i]);
				g_ThreadInfo.sd[i] = INVALID_SOCKET;

				if (g_ThreadInfo.hThread[i] != INVALID_HANDLE_VALUE) {

					dwRet = WaitForSingleObject(g_ThreadInfo.hThread[i], INFINITE);
					if (dwRet == WAIT_FAILED)
						printf("WaitForSingleObject(): %d\n", GetLastError());

					CloseHandle(g_ThreadInfo.hThread[i]);
					g_ThreadInfo.hThread[i] = INVALID_HANDLE_VALUE;
				}
			}
		}

		break;

	default:

		//
		// unknown type--better pass it on.
		//
		return(FALSE);
	}

	WSASetEvent(g_hCleanupEvent[0]);

	return(TRUE);
}