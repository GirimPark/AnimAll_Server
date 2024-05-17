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
// ���Ͽ��� ����Ǵ� ��� I/O ���ۿ� ���õ� ������
//
typedef struct _PER_IO_CONTEXT {
    WSAOVERLAPPED               Overlapped;             // �񵿱� I/O �۾��� ���¸� ����, ����
    char                        Buffer[MAX_BUFF_SIZE];  // ������ �ۼ����� ���� �ӽ� ����
    WSABUF                      wsabuf;                 // ������ ����, ���� �ּ� ����
    int                         nTotalBytes;            // �������� �ѷ�
    int                         nSentBytes;             // �̹� �ۼ��ŵ� ����Ʈ ��, ���� ���� ����
    IO_OPERATION                IOOperation;            // I/O �۾��� ����
    //SOCKET                      SocketAccept;

    struct _PER_IO_CONTEXT* pIOContextForward;  // ���� I/O ���ؽ�Ʈ
} PER_IO_CONTEXT, * PPER_IO_CONTEXT;

//
// AcceptEx�� ���, IOCP Ű�� ���� ������ PER_SOCKET_CONTEXT�Դϴ�.
// ���� PER_IO_CONTEXT�� SocketAccept��� �ٸ� �ʵ尡 �ʿ��մϴ�.
// ���� ���� AcceptEx�� �Ϸ�Ǹ�, �� �ʵ�� �츮�� ���� ���� �ڵ��� �˴ϴ�.
//

//
// IOCP�� �߰��� ��� ����(�Ϸ� ��Ʈ�� ����� ����)�� ���õ� ������
//
typedef struct _PER_SOCKET_CONTEXT {
    SOCKET                      Socket;

    //LPFN_ACCEPTEX               fnAcceptEx;

    //
    // ������ ��� I/O �۾��� ���� ���� ���� ����Ʈ
    //
    PPER_IO_CONTEXT             pIOContext;     // ���Ͽ� �Ҵ�� I/O �۾� ����Ʈ
    struct _PER_SOCKET_CONTEXT* pCtxtBack;      // ���� ������ ���� ����Ʈ
    struct _PER_SOCKET_CONTEXT* pCtxtForward;
} PER_SOCKET_CONTEXT, * PPER_SOCKET_CONTEXT;

// �ַ�� ��� Valid Ȯ��
BOOL ValidOptions(int argc, char* argv[]);

// �ܼ� �̺�Ʈ �ݹ� �Լ�. ���� ���ῡ ���� �ٷ��.
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