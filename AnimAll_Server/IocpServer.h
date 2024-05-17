// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (C) 1998 - 2000  Microsoft Corporation.  All Rights Reserved.
//
// Module:
//      iocpserver.h
//

#ifndef IOCPSERVER_H
#define IOCPSERVER_H

#include <mswsock.h>

#define DEFAULT_PORT        "5001"
#define MAX_BUFF_SIZE       8192
#define MAX_WORKER_THREAD   128

typedef enum _IO_OPERATION {
    ClientIoAccept,
    ClientIoRead,
    ClientIoWrite
} IO_OPERATION, * PIO_OPERATION;

//
// 소켓에서 수행되는 모든 I/O 동작에 관련된 데이터
//
typedef struct _PER_IO_CONTEXT {
    WSAOVERLAPPED               Overlapped;             // 비동기 I/O 작업의 상태를 추적, 관리
    char                        Buffer[MAX_BUFF_SIZE];  // 데이터 송수신을 위한 임시 버퍼
    WSABUF                      wsabuf;                 // 버퍼의 길이, 시작 주소 정보
    int                         nTotalBytes;            // 데이터의 총량
    int                         nSentBytes;             // 이미 송수신된 바이트 수, 진행 상태 추적
    IO_OPERATION                IOOperation;            // I/O 작업의 유형
    //SOCKET                      SocketAccept;

    struct _PER_IO_CONTEXT* pIOContextForward;  // 다음 I/O 컨텍스트
} PER_IO_CONTEXT, * PPER_IO_CONTEXT;

//
// AcceptEx의 경우, IOCP 키는 수신 소켓의 PER_SOCKET_CONTEXT입니다.
// 따라서 PER_IO_CONTEXT에 SocketAccept라는 다른 필드가 필요합니다.
// 진행 중인 AcceptEx가 완료되면, 이 필드는 우리의 연결 소켓 핸들이 됩니다.
//

//
// IOCP에 추가된 모든 소켓(완료 포트와 연결된 소켓)에 관련된 데이터
//
typedef struct _PER_SOCKET_CONTEXT {
    SOCKET                      Socket;

    //LPFN_ACCEPTEX               fnAcceptEx;

    //
    // 소켓의 모든 I/O 작업을 위한 이중 연결 리스트
    //
    PPER_IO_CONTEXT             pIOContext;     // 소켓에 할당된 I/O 작업 리스트
    struct _PER_SOCKET_CONTEXT* pCtxtBack;      // 소켓 관리를 위한 리스트
    struct _PER_SOCKET_CONTEXT* pCtxtForward;
} PER_SOCKET_CONTEXT, * PPER_SOCKET_CONTEXT;

// 솔루션 경로 Valid 확인
BOOL ValidOptions(int argc, char* argv[]);

// 콘솔 이벤트 콜백 함수. 서버 종료에 대해 다룬다.
BOOL WINAPI CtrlHandler(
    DWORD dwEvent
);

BOOL CreateListenSocket(void);

BOOL CreateAcceptSocket(
    BOOL fUpdateIOCP
);

DWORD WINAPI WorkerThread(
    LPVOID WorkContext
);

PPER_SOCKET_CONTEXT UpdateCompletionPort(
    SOCKET s,
    IO_OPERATION ClientIo,
    BOOL bAddToList
);
//
// bAddToList is FALSE for listening socket, and TRUE for connection sockets.
// As we maintain the context for listening socket in a global structure, we
// don't need to add it to the list.
//

VOID CloseClient(
    PPER_SOCKET_CONTEXT lpPerSocketContext,
    BOOL bGraceful
);

PPER_SOCKET_CONTEXT CtxtAllocate(
    SOCKET s,
    IO_OPERATION ClientIO
);

VOID CtxtListFree(
);

VOID CtxtListAddTo(
    PPER_SOCKET_CONTEXT lpPerSocketContext
);

VOID CtxtListDeleteFrom(
    PPER_SOCKET_CONTEXT lpPerSocketContext
);

#endif