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

    struct _PER_IO_CONTEXT* pIOContextForward;  // ���� I/O ���ؽ�Ʈ
} PER_IO_CONTEXT, * PPER_IO_CONTEXT;

//
// IOCP�� �߰��� ��� ����(�Ϸ� ��Ʈ�� ����� ����)�� ���õ� ������
//
typedef struct _PER_SOCKET_CONTEXT {
    SOCKET                      Socket;

    // ������ ��� I/O �۾��� ���� ���� ���� ����Ʈ
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

// Ư�� ������, ��Ʈ ������ �ּ� ������ ��� ������ ����, �����Ѵ�.
BOOL CreateListenSocket(void);

UINT WINAPI WorkerThread(
    LPVOID WorkContext
);

// ���Ͽ� ���� ���ؽ�Ʈ�� �Ҵ��Ͽ� �Ϸ� ��Ʈ�� �����ϰ�,
// ���� ���ؽ�Ʈ�� ��Ͽ� �߰��Ѵ�.
// (���� ������ �������̹Ƿ� ��Ͽ� �߰����� �ʴ´�.)
PPER_SOCKET_CONTEXT UpdateCompletionPort(
    SOCKET s,
    IO_OPERATION ClientIo,
    BOOL bAddToList
);

// ���� �߻� �� ���� ���ؽ�Ʈ, io ���ؽ�Ʈ ����
// ���ؽ�Ʈ ����Ʈ ����
VOID CloseClient(
    PPER_SOCKET_CONTEXT lpPerSocketContext,
    BOOL bGraceful
);

// ���� ���ؽ�Ʈ ����, �ʱ�ȭ
PPER_SOCKET_CONTEXT CtxtAllocate(
    SOCKET s,
    IO_OPERATION ClientIO
);

// ���� ���ؽ�Ʈ ����Ʈ ����
VOID CtxtListFree(
);

VOID CtxtListAddTo(
    PPER_SOCKET_CONTEXT lpPerSocketContext
);

VOID CtxtListDeleteFrom(
    PPER_SOCKET_CONTEXT lpPerSocketContext
);

#endif