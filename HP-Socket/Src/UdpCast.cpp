/*
 * Copyright: JessMA Open Source (ldcsaa@gmail.com)
 *
 * Version	: 3.4.1
 * Author	: Bruce Liang
 * Website	: http://www.jessma.org
 * Project	: https://github.com/ldcsaa
 * Blog		: http://www.cnblogs.com/ldcsaa
 * Wiki		: http://www.oschina.net/p/hp-socket
 * QQ Group	: 75375912
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include "stdafx.h"
#include "UdpCast.h"
#include "../../Common/Src/WaitFor.h"

#include <process.h>

BOOL CUdpCast::Start(LPCTSTR pszRemoteAddress, USHORT usPort, BOOL bAsyncConnect)
{
	if(!CheckParams() || !CheckStarting())
		return FALSE;

	BOOL isOK = FALSE;

	if(CreateClientSocket())
	{
		if(FirePrepareConnect(this, m_soClient) != HR_ERROR)
		{
			if(ConnectToGroup(pszRemoteAddress, usPort))
			{
				if(CreateWorkerThread())
				{
						isOK = TRUE;
				}
				else
					SetLastError(SE_WORKER_THREAD_CREATE, __FUNCTION__, ERROR_CREATE_FAILED);
			}
			else
				SetLastError(SE_CONNECT_SERVER, __FUNCTION__, ::WSAGetLastError());
		}
		else
			SetLastError(SE_SOCKET_PREPARE, __FUNCTION__, ERROR_CANCELLED);
	}
	else
		SetLastError(SE_SOCKET_CREATE, __FUNCTION__, ::WSAGetLastError());

	if(!isOK) Stop();

	return isOK;
}

BOOL CUdpCast::CheckParams()
{
	m_itPool.SetItemCapacity((int)m_dwMaxDatagramSize);
	m_itPool.SetPoolSize((int)m_dwFreeBufferPoolSize);
	m_itPool.SetPoolHold((int)m_dwFreeBufferPoolHold);

	if((int)m_dwMaxDatagramSize > 0)
		if((int)m_dwFreeBufferPoolSize >= 0)
			if((int)m_dwFreeBufferPoolHold >= 0)
				if(m_enCastMode >= CM_MULTICAST && m_enCastMode <= CM_BROADCAST)
					if(m_iMCTtl >= 0 && m_iMCTtl <= 255)
						if(m_bMCLoop >= 0 && m_bMCLoop <= 1)
							if(::IsIPAddress(m_strBindAddress))
								return TRUE;

	SetLastError(SE_INVALID_PARAM, __FUNCTION__, ERROR_INVALID_PARAMETER);
	return FALSE;
}

BOOL CUdpCast::CheckStarting()
{
	CSpinLock locallock(m_csState);

	if(m_enState == SS_STOPPED)
		m_enState = SS_STARTING;
	else
	{
		SetLastError(SE_ILLEGAL_STATE, __FUNCTION__, ERROR_INVALID_OPERATION);
		return FALSE;
	}

	return TRUE;
}

BOOL CUdpCast::CheckStoping()
{
	CSpinLock locallock(m_csState);

	if(m_enState == SS_STARTED || m_enState == SS_STARTING)
		m_enState = SS_STOPPING;
	else
	{
		SetLastError(SE_ILLEGAL_STATE, __FUNCTION__, ERROR_INVALID_OPERATION);
		return FALSE;
	}

	return TRUE;
}

BOOL CUdpCast::CreateClientSocket()
{
	m_soClient = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if(m_soClient != INVALID_SOCKET)
	{
		VERIFY(::SSO_UDP_ConnReset(m_soClient, FALSE) == NO_ERROR);
		VERIFY(::SSO_ReuseAddress(m_soClient, m_bReuseAddress) != SOCKET_ERROR);

		m_evSocket = ::WSACreateEvent();
		ASSERT(m_evSocket != WSA_INVALID_EVENT);

		m_dwConnID = ::GenerateConnectionID();

		return TRUE;
	}

	return FALSE;
}

BOOL CUdpCast::ConnectToGroup(LPCTSTR pszRemoteAddress, USHORT usPort)
{
	if(m_enCastMode == CM_MULTICAST)
	{
		TCHAR szAddress[40];
		int iAddressLen = sizeof(szAddress) / sizeof(TCHAR);

		if(!::GetIPAddress(pszRemoteAddress, szAddress, iAddressLen))
		{
			::WSASetLastError(WSAEADDRNOTAVAIL);
			return FALSE;
		}

		if(!::sockaddr_A_2_IN(AF_INET, szAddress, usPort, m_castAddr))
		{
			::WSASetLastError(WSAEADDRNOTAVAIL);
			return FALSE;
		}

		VERIFY(::SSO_SetSocketOption(m_soClient, IPPROTO_IP, IP_MULTICAST_TTL, &m_iMCTtl, sizeof(int)) != SOCKET_ERROR);
		VERIFY(::SSO_SetSocketOption(m_soClient, IPPROTO_IP, IP_MULTICAST_LOOP, &m_bMCLoop, sizeof(BOOL)) != SOCKET_ERROR);
	}
	else
	{
		m_castAddr.sin_family		= AF_INET;
		m_castAddr.sin_addr.s_addr	= INADDR_BROADCAST;
		m_castAddr.sin_port			= htons(usPort);

		BOOL bSet = TRUE;
		VERIFY(::SSO_SetSocketOption(m_soClient, SOL_SOCKET, SO_BROADCAST, &bSet, sizeof(BOOL)) != SOCKET_ERROR);
	}

	SOCKADDR_IN bindAddr;
	if(!::sockaddr_A_2_IN(AF_INET, m_strBindAddress, usPort, bindAddr))
	{
		::WSASetLastError(WSAEADDRNOTAVAIL);
		return FALSE;
	}

	if(::bind(m_soClient, (struct sockaddr*)&bindAddr, sizeof(SOCKADDR_IN)) == SOCKET_ERROR)
		return FALSE;
	else
	{
		if(m_enCastMode == CM_MULTICAST)
		{
			ip_mreq mcast;
			::ZeroMemory(&mcast, sizeof(ip_mreq));

			mcast.imr_multiaddr = m_castAddr.sin_addr;
			mcast.imr_interface = bindAddr.sin_addr;

			if(::SSO_SetSocketOption(m_soClient, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mcast, sizeof(ip_mreq)) == SOCKET_ERROR)
				return FALSE;
		}
	}

	BOOL isOK = FALSE;

	if(::WSAEventSelect(m_soClient, m_evSocket, FD_READ | FD_WRITE | FD_CLOSE) != SOCKET_ERROR)
	{
		if(FireConnect(this) != HR_ERROR)
		{
			m_enState	= SS_STARTED;
			isOK		= TRUE;
		}
	}

	return isOK;
}

BOOL CUdpCast::CreateWorkerThread()
{
	m_hWorker = (HANDLE)_beginthreadex(nullptr, 0, WorkerThreadProc, (LPVOID)this, 0, &m_dwWorkerID);

	return m_hWorker != nullptr;
}

UINT WINAPI CUdpCast::WorkerThreadProc(LPVOID pv)
{
	TRACE("---------------> Client Worker Thread 0x%08X started <---------------\n", ::GetCurrentThreadId());

	CUdpCast* pClient	= (CUdpCast*)pv;
	HANDLE hEvents[]	= {pClient->m_evSocket, pClient->m_evBuffer, pClient->m_evWorker};

	pClient->m_rcBuffer.Malloc(pClient->m_dwMaxDatagramSize);

	while(pClient->HasStarted())
	{
		DWORD retval = ::WSAWaitForMultipleEvents(3, hEvents, FALSE, WSA_INFINITE, FALSE);

		if(retval == WSA_WAIT_EVENT_0)
		{
			if(!pClient->ProcessNetworkEvent())
			{
				if(pClient->HasStarted())
					pClient->Stop();

				break;
			}
		}
		else if(retval == WSA_WAIT_EVENT_0 + 1)
		{
			if(!pClient->SendData())
			{
				if(pClient->HasStarted())
					pClient->Stop();

				break;
			}
		}
		else if(retval == WSA_WAIT_EVENT_0 + 2)
			break;
		else
			ASSERT(FALSE);
	}

	TRACE("---------------> Client Worker Thread 0x%08X stoped <---------------\n", ::GetCurrentThreadId());

	return 0;
}

BOOL CUdpCast::ProcessNetworkEvent()
{
	BOOL bContinue = TRUE;
	WSANETWORKEVENTS events;
	
	int rc = ::WSAEnumNetworkEvents(m_soClient, m_evSocket, &events);

	if(rc == SOCKET_ERROR)
		bContinue = HandleError(events);

	if(bContinue && events.lNetworkEvents & FD_READ)
		bContinue = HandleRead(events);

	if(bContinue && events.lNetworkEvents & FD_WRITE)
		bContinue = HandleWrite(events);

	if(bContinue && events.lNetworkEvents & FD_CLOSE)
		bContinue = HandleClose(events);

	return bContinue;
}

BOOL CUdpCast::HandleError(WSANETWORKEVENTS& events)
{
	int iCode = ::WSAGetLastError();
	SetLastError(SE_NETWORK, __FUNCTION__, iCode);

	EnSocketOperation enOperation = SO_UNKNOWN;

	if(events.lNetworkEvents & FD_CLOSE)
		enOperation = SO_CLOSE;
	else if(events.lNetworkEvents & FD_READ)
		enOperation = SO_RECEIVE;
	else if(events.lNetworkEvents & FD_WRITE)
		enOperation = SO_SEND;

	VERIFY(::WSAResetEvent(m_evSocket));
	FireClose(this, enOperation, iCode);

	return FALSE;
}

BOOL CUdpCast::HandleRead(WSANETWORKEVENTS& events)
{
	BOOL bContinue	= TRUE;
	int iCode		= events.iErrorCode[FD_READ_BIT];

	if(iCode == 0)
		bContinue = ReadData();
	else
	{
		SetLastError(SE_NETWORK, __FUNCTION__, iCode);
		FireClose(this, SO_RECEIVE, iCode);
		bContinue = FALSE;
	}

	return bContinue;
}

BOOL CUdpCast::HandleWrite(WSANETWORKEVENTS& events)
{
	BOOL bContinue	= TRUE;
	int iCode		= events.iErrorCode[FD_WRITE_BIT];

	if(iCode == 0)
		bContinue = SendData();
	else
	{
		SetLastError(SE_NETWORK, __FUNCTION__, iCode);
		FireClose(this, SO_SEND, iCode);
		bContinue = FALSE;
	}

	return bContinue;
}

BOOL CUdpCast::HandleClose(WSANETWORKEVENTS& events)
{
	int iCode = events.iErrorCode[FD_CLOSE_BIT];

	if(iCode == 0)
		FireClose(this, SO_CLOSE, SE_OK);
	else
	{
		SetLastError(SE_NETWORK, __FUNCTION__, iCode);
		FireClose(this, SO_CLOSE, iCode);
	}

	return FALSE;
}

BOOL CUdpCast::ReadData()
{
	while(TRUE)
	{
		int addrLen	= sizeof(SOCKADDR_IN);
		int rc		= recvfrom(m_soClient, (char*)(BYTE*)m_rcBuffer, m_dwMaxDatagramSize, 0, (sockaddr*)&m_remoteAddr, &addrLen);

		if(rc >= 0)
		{
			if(FireReceive(this, m_rcBuffer, rc) == HR_ERROR)
			{
				TRACE("<C-CNNID: %Iu> OnReceive() event return 'HR_ERROR', connection will be closed !\n", m_dwConnID);

				SetLastError(SE_DATA_PROC, __FUNCTION__, ERROR_CANCELLED);
				FireClose(this, SO_RECEIVE, ERROR_CANCELLED);

				return FALSE;
			}
		}
		else if(rc == SOCKET_ERROR)
		{
			int code = ::WSAGetLastError();

			if(code == WSAEWOULDBLOCK)
				break;
			else
			{
				SetLastError(SE_NETWORK, __FUNCTION__, code);
				FireClose(this, SO_RECEIVE, code);

				return FALSE;
			}
		}
		else
			ASSERT(FALSE);
	}

	return TRUE;
}

BOOL CUdpCast::SendData()
{
	while(TRUE)
	{
		TItemPtr itPtr(m_itPool, GetSendBuffer());

		if(itPtr.IsValid())
		{
			ASSERT(!itPtr->IsEmpty());

			int rc = 0;

			{
				CCriSecLock locallock(m_csSend);

				rc = sendto(m_soClient, (char*)itPtr->Ptr(), itPtr->Size(), 0, (sockaddr*)&m_castAddr, sizeof(SOCKADDR_IN));
				if(rc > 0) m_iPending -= rc;
			}

			if(rc > 0)
			{
				ASSERT(rc == itPtr->Size());

				if(FireSend(this, itPtr->Ptr(), rc) == HR_ERROR)
				{
					TRACE("<C-CNNID: %Iu> OnSend() event should not return 'HR_ERROR' !!\n", m_dwConnID);
					ASSERT(FALSE);
				}
			}
			else if(rc == SOCKET_ERROR)
			{
				int iCode = ::WSAGetLastError();

				if(iCode == WSAEWOULDBLOCK)
				{
					CCriSecLock locallock(m_csSend);
					m_lsSend.PushFront(itPtr.Detach());
					break;
				}
				else
				{
					SetLastError(SE_NETWORK, __FUNCTION__, iCode);
					FireClose(this, SO_SEND, iCode);

					return FALSE;
				}
			}
			else
				ASSERT(FALSE);
		}
		else
			break;
	}

	return TRUE;
}

TItem* CUdpCast::GetSendBuffer()
{
	TItem* pItem = nullptr;

	if(m_lsSend.Size() > 0)
	{
		CCriSecLock locallock(m_csSend);

		if(m_lsSend.Size() > 0)
			pItem = m_lsSend.PopFront();
	}

	return pItem;
}

BOOL CUdpCast::Stop()
{
	BOOL bNeedFireClose			= FALSE;
	EnServiceState enCurState	= m_enState;
	DWORD dwCurrentThreadID		= ::GetCurrentThreadId();

	if(!CheckStoping())
		return FALSE;

	if(	enCurState == SS_STARTED			&&
		dwCurrentThreadID != m_dwWorkerID	)
		bNeedFireClose = TRUE;

	WaitForWorkerThreadEnd(dwCurrentThreadID);

	if(bNeedFireClose)
		FireClose(this, SO_CLOSE, SE_OK);

	if(m_evSocket != nullptr)
	{
		::WSACloseEvent(m_evSocket);
		m_evSocket	= nullptr;
	}

	if(m_soClient != INVALID_SOCKET)
	{
		shutdown(m_soClient, SD_SEND);
		closesocket(m_soClient);
		m_soClient	= INVALID_SOCKET;
	}

	Reset();

	return TRUE;
}

void CUdpCast::Reset(BOOL bAll)
{
	if(bAll)
	{
		m_rcBuffer.Free();
		m_evBuffer.Reset();
		m_evWorker.Reset();
		m_evDetector.Reset();
		m_lsSend.Clear();
		m_itPool.Clear();
	}

	::ZeroMemory(&m_castAddr, sizeof(SOCKADDR_IN));
	::ZeroMemory(&m_remoteAddr, sizeof(SOCKADDR_IN));

	m_iPending		= 0;
	m_enState		= SS_STOPPED;
}

void CUdpCast::WaitForWorkerThreadEnd(DWORD dwCurrentThreadID)
{
	if(m_hWorker != nullptr)
	{
		if(dwCurrentThreadID != m_dwWorkerID)
		{
			m_evWorker.Set();
			VERIFY(::WaitForSingleObject(m_hWorker, INFINITE) == WAIT_OBJECT_0);
		}

		::CloseHandle(m_hWorker);

		m_hWorker		= nullptr;
		m_dwWorkerID	= 0;
	}
}

BOOL CUdpCast::Send(const BYTE* pBuffer, int iLength, int iOffset)
{
	int result			 = NO_ERROR;
	EnSocketError enCode = SE_OK;

	ASSERT(pBuffer && iLength > 0 && iLength <= (int)m_dwMaxDatagramSize);

	if(pBuffer && iLength > 0 && iLength <= (int)m_dwMaxDatagramSize)
	{
		if(iOffset != 0) pBuffer += iOffset;
		result = SendInternal(pBuffer, iLength, enCode);
	}
	else
	{
		result = ERROR_INVALID_PARAMETER;
		enCode = SE_INVALID_PARAM;
	}

	if(result != NO_ERROR)
		SetLastError(enCode, __FUNCTION__, result);

	return (result == NO_ERROR);
}

BOOL CUdpCast::SendPackets(const WSABUF pBuffers[], int iCount)
{
	int result			 = NO_ERROR;
	EnSocketError enCode = SE_OK;

	ASSERT(pBuffers && iCount > 0);

	if(pBuffers && iCount > 0)
	{
		int iLength = 0;
		int iMaxLen = (int)m_dwMaxDatagramSize;

		TItemPtr itPtr(m_itPool, m_itPool.PickFreeItem());

		for(int i = 0; i < iCount; i++)
		{
			int iBufLen = pBuffers[i].len;

			if(iBufLen > 0)
			{
				BYTE* pBuffer = (BYTE*)pBuffers[i].buf;
				ASSERT(pBuffer);

				iLength += iBufLen;

				if(iLength <= iMaxLen)
					itPtr->Cat(pBuffer, iBufLen);
				else
					break;
			}
		}

		if(iLength > 0 && iLength <= iMaxLen)
			result = SendInternal(itPtr->Ptr(), iLength, enCode);
		else
		{
			result = ERROR_INCORRECT_SIZE;
			enCode = SE_INVALID_PARAM;
		}
	}
	else
	{
		result = ERROR_INVALID_PARAMETER;
		enCode = SE_INVALID_PARAM;
	}

	if(result != NO_ERROR)
		SetLastError(enCode, __FUNCTION__, result);

	return (result == NO_ERROR);
}

int CUdpCast::SendInternal(const BYTE* pBuffer, int iLength, EnSocketError& enCode)
{
	int result = NO_ERROR;

	if(HasStarted())
	{
		CCriSecLock locallock(m_csSend);

		if(HasStarted())
		{
			ASSERT(m_iPending >= 0);

			BOOL isPending = m_iPending > 0;

			TItem* pItem = m_itPool.PickFreeItem();
			pItem->Cat(pBuffer, iLength);
			m_lsSend.PushBack(pItem);

			m_iPending += iLength;

			if(!isPending) m_evBuffer.Set();
		}
		else
		{
			result = ERROR_INVALID_STATE;
			enCode = SE_ILLEGAL_STATE;
		}
	}
	else
	{
		result = ERROR_INVALID_STATE;
		enCode = SE_ILLEGAL_STATE;
	}

	return result;
}

void CUdpCast::SetLastError(EnSocketError code, LPCSTR func, int ec)
{
	TRACE("%s --> Error: %d, EC: %d\n", func, code, ec);

	m_enLastError = code;
	::SetLastError(ec);
}

BOOL CUdpCast::GetLocalAddress(TCHAR lpszAddress[], int& iAddressLen, USHORT& usPort)
{
	ASSERT(lpszAddress != nullptr && iAddressLen > 0);

	return ::GetSocketLocalAddress(m_soClient, lpszAddress, iAddressLen, usPort);
}
