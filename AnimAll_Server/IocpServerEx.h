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
// data to be associated for every I/O operation on a socket
//
typedef struct _PER_IO_CONTEXT {
    WSAOVERLAPPED               Overlapped;
    char                        Buffer[MAX_BUFF_SIZE];
    WSABUF                      wsabuf;
    int                         nTotalBytes;
    int                         nSentBytes;
    IO_OPERATION                IOOperation;
    SOCKET                      SocketAccept;

    struct _PER_IO_CONTEXT* pIOContextForward;
} PER_IO_CONTEXT, * PPER_IO_CONTEXT;

//
//AcceptEx의 경우, IOCP 키는 리슨 소켓의 PER_SOCKET_CONTEXT입니다.
//따라서 PER_IO_CONTEXT에 SocketAccept라는 또 다른 필드가 필요합니다.
//미완료된 AcceptEx가 완료되면, 이 필드는 우리의 연결 소켓 핸들이 됩니다.
//

//
// data to be associated with every socket added to the IOCP
//
typedef struct _PER_SOCKET_CONTEXT {
    SOCKET                      Socket;

    LPFN_ACCEPTEX               fnAcceptEx;

    //
    //linked list for all outstanding i/o on the socket
    //
    PPER_IO_CONTEXT             pIOContext;
    struct _PER_SOCKET_CONTEXT* pCtxtBack;
    struct _PER_SOCKET_CONTEXT* pCtxtForward;
} PER_SOCKET_CONTEXT, * PPER_SOCKET_CONTEXT;

BOOL ValidOptions(int argc, char* argv[]);

BOOL WINAPI CtrlHandler(
    DWORD dwEvent
);

BOOL CreateListenSocket(void);

BOOL CreateAcceptSocket(
    BOOL fUpdateIOCP
);

UINT WINAPI WorkerThread(
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