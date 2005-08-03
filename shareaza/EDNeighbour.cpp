//
// EDNeighbour.cpp
//
// Copyright (c) Shareaza Development Team, 2002-2005.
// This file is part of SHAREAZA (www.shareaza.com)
//
// Shareaza is free software; you can redistribute it
// and/or modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2 of
// the License, or (at your option) any later version.
//
// Shareaza is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Shareaza; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//

#include "StdAfx.h"
#include "Shareaza.h"
#include "Settings.h"
#include "Network.h"
#include "Statistics.h"
#include "Neighbours.h"
#include "EDNeighbour.h"
#include "EDPacket.h"
#include "Security.h"
#include "GProfile.h"
#include "HostCache.h"
#include "EDClients.h"

#include "Library.h"
#include "SharedFile.h"
#include "Transfers.h"
#include "Downloads.h"
#include "Download.h"
#include "QuerySearch.h"
#include "QueryHit.h"

#include "UploadQueues.h"
#include "Schema.h"
#include "XML.h"

#ifdef _DEBUG
#undef THIS_FILE
static char THIS_FILE[]=__FILE__;
#define new DEBUG_NEW
#endif


//////////////////////////////////////////////////////////////////////
// CEDNeighbour construction

CEDNeighbour::CEDNeighbour() : CNeighbour( PROTOCOL_ED2K )
{
	m_nNodeType		= ntHub;
	m_nClientID		= 0;
	m_nUserCount	= 0;
	m_nUserLimit	= 0;
	m_nFileLimit	= 1000;
	m_nTCPFlags		= 0;
	m_nUDPFlags		= 0;
	m_pMoreResultsGUID = NULL;
}

CEDNeighbour::~CEDNeighbour()
{
	for ( POSITION pos = m_pQueries.GetHeadPosition() ; pos ; )
	{
		delete (GGUID*)m_pQueries.GetNext( pos );
	}

	if ( m_pMoreResultsGUID != NULL ) delete m_pMoreResultsGUID;
}

//////////////////////////////////////////////////////////////////////
// CEDNeighbour connect to

BOOL CEDNeighbour::ConnectTo(IN_ADDR* pAddress, WORD nPort, BOOL bAutomatic)
{
	if ( CConnection::ConnectTo( pAddress, nPort ) )
	{
		WSAEventSelect( m_hSocket, Network.m_pWakeup, FD_CONNECT|FD_READ|FD_WRITE|FD_CLOSE );
		
		theApp.Message( MSG_DEFAULT, IDS_ED2K_SERVER_CONNECTING,
			(LPCTSTR)m_sAddress, htons( m_pHost.sin_port ) );
	}
	else
	{
		theApp.Message( MSG_ERROR, IDS_CONNECTION_CONNECT_FAIL,
			(LPCTSTR)CString( inet_ntoa( m_pHost.sin_addr ) ) );
		return FALSE;
	}
	
	m_nState		= nrsConnecting;
	m_bAutomatic	= bAutomatic;
	
	Neighbours.Add( this );
	
	return TRUE;
}

//////////////////////////////////////////////////////////////////////
// CEDNeighbour send packet

BOOL CEDNeighbour::Send(CPacket* pPacket, BOOL bRelease, BOOL bBuffered)
{
	BOOL bSuccess = FALSE;
	
	if ( ( m_nState == nrsHandshake1 || m_nState >= nrsConnected ) &&
		 pPacket->m_nProtocol == PROTOCOL_ED2K )
	{
		m_nOutputCount++;
		Statistics.Current.eDonkey.Outgoing++;
		
		pPacket->ToBuffer( m_pZOutput ? m_pZOutput : m_pOutput );
		QueueRun();
		
		pPacket->SmartDump( this, NULL, TRUE );
		// pPacket->Debug( _T("DonkeyOut") );
		bSuccess = TRUE;
	}
	
	if ( bRelease ) pPacket->Release();
	
	return bSuccess;
}

//////////////////////////////////////////////////////////////////////
// CEDNeighbour run event

BOOL CEDNeighbour::OnRun()
{
	DWORD tNow = GetTickCount();
	
	if ( m_nState != nrsConnected )
	{
		if ( tNow - m_tConnected > Settings.Connection.TimeoutConnect )
		{
			Close( IDS_CONNECTION_TIMEOUT_CONNECT );
			return FALSE;
		}
	}
	else if ( m_nClientID == 0 )
	{
		if ( tNow - m_tConnected > Settings.Connection.TimeoutHandshake )
		{
			Close( IDS_HANDSHAKE_TIMEOUT );
			return FALSE;
		}
	}
	else
	{
		if ( tNow - m_tLastPacket > 20 * 60 * 1000 )
		{
			Close( IDS_CONNECTION_TIMEOUT_TRAFFIC );
			return FALSE;
		}
	}
	
	return TRUE;
}

//////////////////////////////////////////////////////////////////////
// CEDNeighbour connection event

BOOL CEDNeighbour::OnConnected()
{
	DWORD nVersion =  ( ( ( ED2K_COMPATIBLECLIENT_ID & 0xFF ) << 24 ) | 
							( ( theApp.m_nVersion[0] & 0x7F ) << 17 ) | 
							( ( theApp.m_nVersion[1] & 0x7F ) << 10 ) |
							( ( theApp.m_nVersion[2] & 0x03 ) << 7  ) |
							( ( theApp.m_nVersion[3] & 0x7F )       ) );

	theApp.Message( MSG_DEFAULT, IDS_ED2K_SERVER_CONNECTED, (LPCTSTR)m_sAddress );
	
	CEDPacket* pPacket = CEDPacket::New(  ED2K_C2S_LOGINREQUEST );
	
	GGUID pGUID	= MyProfile.GUID;
	pGUID.n[5]	= 14;
	pGUID.n[14]	= 111;
	pPacket->Write( &pGUID, 16 );
	
	pPacket->WriteLongLE( m_nClientID );
	pPacket->WriteShortLE( htons( Network.m_pHost.sin_port ) );

	pPacket->WriteLongLE( 5 );	// Number of tags
	
	// Tags sent to the server

	// User name
	CEDTag( ED2K_CT_NAME, MyProfile.GetNick().Left( 255 ) ).Write( pPacket, 0 );
	// Version ('ed2k version')
	CEDTag( ED2K_CT_VERSION, ED2K_VERSION ).Write( pPacket, 0 );
	// Port
	CEDTag( ED2K_CT_PORT, htons( Network.m_pHost.sin_port ) ).Write( pPacket, 0 );
	// Software Version ('Client Version').	
	CEDTag( ED2K_CT_SOFTWAREVERSION, nVersion ).Write( pPacket, 0 );
	// Flags indicating capability
	CEDTag( ED2K_CT_FLAGS, ED2K_SERVER_TCP_DEFLATE | ED2K_SERVER_TCP_SMALLTAGS | ED2K_SERVER_TCP_UNICODE ).Write( pPacket, 0 );

	m_nState = nrsHandshake1;
	Send( pPacket );
	
	return TRUE;
}

void CEDNeighbour::OnDropped(BOOL bError)
{
	if ( m_nState < nrsConnected )
	{
		HostCache.OnFailure( &m_pHost.sin_addr, htons( m_pHost.sin_port ) );
	}
	
	if ( m_nState == nrsConnecting )
	{
		Close( IDS_CONNECTION_REFUSED );
	}
	else
	{
		Close( IDS_CONNECTION_DROPPED );
	}
}

//////////////////////////////////////////////////////////////////////
// CEDNeighbour read event

BOOL CEDNeighbour::OnRead()
{
	BOOL bSuccess = TRUE;
	CEDPacket* pPacket;
	
	CNeighbour::OnRead();
	
	while ( pPacket = CEDPacket::ReadBuffer( m_pZInput ? m_pZInput : m_pInput, ED2K_PROTOCOL_EDONKEY ) )
	{
		try
		{
			bSuccess = OnPacket( pPacket );
		}
		catch ( CException* pException )
		{
			pException->Delete();
		}
		
		pPacket->Release();
		if ( ! bSuccess ) break;
	}
	
	return bSuccess;
}

//////////////////////////////////////////////////////////////////////
// CEDNeighbour packet handler

BOOL CEDNeighbour::OnPacket(CEDPacket* pPacket)
{
	pPacket->SmartDump( this, NULL, FALSE );
	// pPacket->Debug( _T("DonkeyIn") );
	
	m_nInputCount++;
	Statistics.Current.eDonkey.Incoming++;
	m_tLastPacket = GetTickCount();
	
	switch ( pPacket->m_nType )
	{
	case ED2K_S2C_REJECTED:
		return OnRejected( pPacket );
	case ED2K_S2C_SERVERMESSAGE:
		return OnServerMessage( pPacket );
	case ED2K_S2C_IDCHANGE:
		return OnIdChange( pPacket );
	case ED2K_S2C_SERVERLIST:
		return OnServerList( pPacket );
	case ED2K_S2C_SERVERSTATUS:
		return OnServerStatus( pPacket );
	case ED2K_S2C_SERVERIDENT:
		return OnServerIdent( pPacket );
	case ED2K_S2C_CALLBACKREQUESTED:
		return OnCallbackRequest( pPacket );
	case ED2K_S2C_SEARCHRESULTS:
		return OnSearchResults( pPacket );
	case ED2K_S2C_FOUNDSOURCES:
		return OnFoundSources( pPacket );
	default:
		{
		pPacket->Debug( _T("Unknown") );
		break;
		}
	}
	
	return TRUE;
}

//////////////////////////////////////////////////////////////////////
// CEDNeighbour server packets

BOOL CEDNeighbour::OnRejected(CEDPacket* pPacket)
{
	Close( IDS_ED2K_SERVER_REJECTED );
	return FALSE;
}

BOOL CEDNeighbour::OnServerMessage(CEDPacket* pPacket)
{
	if ( pPacket->GetRemaining() < 4 ) return TRUE;
	
	CString	strMessage = pPacket->ReadEDString( m_nTCPFlags );
	
	while ( strMessage.GetLength() > 0 )
	{
		CString strLine = strMessage.SpanExcluding( _T("\r\n") );
		
		if ( strLine.GetLength() > 0 )
		{
			strMessage = strMessage.Mid( strLine.GetLength() );
			theApp.Message( MSG_SYSTEM, IDS_ED2K_SERVER_MESSAGE,
				(LPCTSTR)m_sAddress, (LPCTSTR)strLine );
		}
		
		if ( strMessage.GetLength() > 0 ) strMessage = strMessage.Mid( 1 );
	}
	
	return TRUE;
}

BOOL CEDNeighbour::OnIdChange(CEDPacket* pPacket)
{
	if ( pPacket->GetRemaining() < 4 ) return TRUE;
	
	DWORD nClientID = pPacket->ReadLongLE();
	
	if ( nClientID == 0 )
	{
		Close( IDS_ED2K_SERVER_REFUSED );
		return FALSE;
	}
	
	if ( pPacket->GetRemaining() >= 4 )
	{
		m_nTCPFlags = pPacket->ReadLongLE();
	}
	
	if ( m_nClientID == 0 )
	{
		theApp.Message( MSG_DEFAULT, IDS_ED2K_SERVER_ONLINE, (LPCTSTR)m_sAddress, nClientID );
		SendSharedFiles();
		Send( CEDPacket::New(  ED2K_C2S_GETSERVERLIST ) );
		HostCache.eDonkey.Add( &m_pHost.sin_addr, htons( m_pHost.sin_port ) );
	}
	else
	{
		theApp.Message( MSG_DEFAULT, IDS_ED2K_SERVER_IDCHANGE, (LPCTSTR)m_sAddress, m_nClientID, nClientID );
	}
	
	m_nState	= nrsConnected;
	m_nClientID	= nClientID;
	
	if ( ! CEDPacket::IsLowID( m_nClientID ) )
	{
		if ( Settings.Connection.InHost.IsEmpty() )
		{
			Network.m_pHost.sin_addr.S_un.S_addr = m_nClientID;
		}
	}

	CString strServerFlags;
	strServerFlags.Format(
        _T( "Server Flags: Zlib: %d Short Tags: %d Unicode: %d GetSources2: %d (64): %d  (128): %d" ), 
		m_nTCPFlags & ED2K_SERVER_TCP_DEFLATE,
        m_nTCPFlags & ED2K_SERVER_TCP_SMALLTAGS,
        m_nTCPFlags & ED2K_SERVER_TCP_UNICODE,
        m_nTCPFlags & ED2K_SERVER_TCP_GETSOURCES2,
        m_nTCPFlags & 0x00000040,
        m_nTCPFlags & 0x00000080 );
	theApp.Message( MSG_DEBUG, strServerFlags );
	
	
	return TRUE;
}

BOOL CEDNeighbour::OnServerList(CEDPacket* pPacket)
{
	if ( pPacket->GetRemaining() < 1 ) return TRUE;
	
	int nCount = pPacket->ReadByte();
	if ( pPacket->GetRemaining() < nCount * 6 ) return TRUE;
	
	while ( nCount-- > 0 )
	{
		DWORD nAddress	= pPacket->ReadLongLE();
		WORD nPort		= pPacket->ReadShortLE();
		
		theApp.Message( MSG_DEBUG, _T("CEDNeighbour::OnServerList(): %s: %s:%i"),
			(LPCTSTR)m_sAddress,
			(LPCTSTR)CString( inet_ntoa( (IN_ADDR&)nAddress ) ), nPort );
		
		if ( Settings.eDonkey.LearnNewServers )
		{
			HostCache.eDonkey.Add( (IN_ADDR*)&nAddress, nPort );
		}
	}
	
	HostCache.eDonkey.Add( &m_pHost.sin_addr, htons( m_pHost.sin_port ) );
	
	return TRUE;
}

BOOL CEDNeighbour::OnServerStatus(CEDPacket* pPacket)
{
	if ( pPacket->GetRemaining() < 8 ) return TRUE;
	
	m_nUserCount	= pPacket->ReadLongLE();
	m_nFileCount	= pPacket->ReadLongLE();
	
	if ( CHostCacheHost* pHost = HostCache.eDonkey.Add( &m_pHost.sin_addr, htons( m_pHost.sin_port ) ) )
	{
		// pHost->m_nUserCount = max( pHost->m_nUserCount, m_nUserCount );
		pHost->m_nUserCount = m_nUserCount;
		pHost->m_nUserLimit = max( pHost->m_nUserLimit, m_nUserCount );
	}
	
	return TRUE;
}

BOOL CEDNeighbour::OnServerIdent(CEDPacket* pPacket)
{
	if ( pPacket->GetRemaining() < sizeof(GGUID) + 6 + 4 ) return TRUE;
	
	m_bGUID = TRUE;
	pPacket->Read( &m_pGUID, sizeof(GGUID) );
	
	pPacket->ReadLongLE();	// IP
	pPacket->ReadShortLE();	// Port
	
	DWORD nTags = pPacket->ReadLongLE();
	CString strDescription;
	
	while ( nTags-- > 0 && pPacket->GetRemaining() > 1 )
	{
		CEDTag pTag;
		if ( ! pTag.Read( pPacket, m_nTCPFlags ) ) break;

		switch ( pTag.m_nKey )
		{
		case ED2K_ST_SERVERNAME:
			// if ( pTag.m_nType == ED2K_TAG_STRING ) // "Short strings" may be possible...
			m_sServerName = pTag.m_sValue;
			break;
		case ED2K_ST_DESCRIPTION:
			strDescription = pTag.m_sValue;
			break;
		case ED2K_ST_MAXUSERS:
			m_nUserLimit = pTag.m_nValue;
			break;
/*
		case ED2K_ST_MAXFILES:
			nMaxFiles = pTag.m_nValue;
			break;
		case ED2K_ST_UDPFLAGS:
			nUDPFlags = pTag.m_nValue;
			break;
*/
		default:
			CString str;
			str.Format( _T("Unrecognised packet - IP: %s - opcode: 0x%x - in CEDNeighbour::OnServerIdent"),
				LPCTSTR( m_sAddress ), int( pTag.m_nKey ) );
			theApp.Message( MSG_DEBUG, LPCTSTR( str ) );
		}
	}

	if ( (DWORD&)m_pGUID == 0x2A2A2A2A )
		m_sUserAgent = _T("eFarm Server");
	else
		m_sUserAgent = _T("eDonkey2000 Server");
	
	if ( CHostCacheHost* pHost = HostCache.eDonkey.Add( &m_pHost.sin_addr, htons( m_pHost.sin_port ) ) )
	{
		pHost->m_sName			= m_sServerName;
		pHost->m_sDescription	= strDescription;
		pHost->m_nUserLimit		= m_nUserLimit;
		pHost->m_nTCPFlags		= m_nTCPFlags;

		// We can assume some UDP flags based on TCP flags
		if ( m_nTCPFlags & ED2K_SERVER_TCP_DEFLATE )
		{
			pHost->m_nUDPFlags |= ED2K_SERVER_UDP_GETSOURCES;
			pHost->m_nUDPFlags |= ED2K_SERVER_UDP_GETFILES;
		}
		if ( m_nTCPFlags & ED2K_SERVER_TCP_UNICODE )
			pHost->m_nUDPFlags |= ED2K_SERVER_UDP_UNICODE;
		if ( m_nTCPFlags & ED2K_SERVER_TCP_GETSOURCES2 )
			pHost->m_nUDPFlags |= ED2K_SERVER_UDP_GETSOURCES2;

	}
	
	theApp.Message( MSG_DEFAULT, IDS_ED2K_SERVER_IDENT, (LPCTSTR)m_sAddress, (LPCTSTR)m_sServerName );
	
	return TRUE;
}

BOOL CEDNeighbour::OnCallbackRequest(CEDPacket* pPacket)
{
	if ( pPacket->GetRemaining() < 6 ) return TRUE;
	
	DWORD nAddress	= pPacket->ReadLongLE();
	WORD nPort		= pPacket->ReadShortLE();
	
	if ( Network.IsFirewalledAddress( &nAddress ) ) return TRUE;
	
	EDClients.PushTo( nAddress, nPort );
	
	return TRUE;
}

BOOL CEDNeighbour::OnSearchResults(CEDPacket* pPacket)
{
	if ( m_pQueries.GetCount() == 0 )
	{
		Statistics.Current.eDonkey.Dropped++;
		m_nDropCount++;
		return TRUE;
	}

	GGUID* pGUID		= (GGUID*)m_pQueries.RemoveHead();
	CQueryHit* pHits	= CQueryHit::FromPacket( pPacket, &m_pHost, m_nTCPFlags, pGUID );
	
	if ( pHits == NULL )
	{
		delete pGUID;
		
		if ( pPacket->m_nLength != 17 && pPacket->m_nLength != 5 )
		{
			pPacket->Debug( _T("BadSearchResult") );
			theApp.Message( MSG_ERROR, IDS_PROTOCOL_BAD_HIT, (LPCTSTR)m_sAddress );
			Statistics.Current.eDonkey.Dropped++;
			m_nDropCount++;
		}
		
		return TRUE;
	}
	
	// Process the 'more results available' packet.
	if ( pPacket->m_nType == ED2K_S2C_SEARCHRESULTS && pPacket->GetRemaining() == 1 && m_pQueries.IsEmpty() )
	{
		if ( pPacket->ReadByte() == TRUE )
		{	// This will be remembered by the neighbour, and if the search continues, more results can be requested.
			m_pMoreResultsGUID = pGUID;
			pGUID = NULL;
			theApp.Message( MSG_DEBUG, _T("Additional results packet received.") );
		}
	}
	
	Network.OnQueryHits( pHits );
	
	if ( pGUID != NULL ) delete pGUID;
	
	return TRUE;
}

BOOL CEDNeighbour::OnFoundSources(CEDPacket* pPacket)
{
	CQueryHit* pHits	= CQueryHit::FromPacket( pPacket, &m_pHost, m_nTCPFlags );
	
	if ( pHits == NULL )
	{
		
		if ( pPacket->m_nLength != 17 && pPacket->m_nLength != 5 )
		{
			pPacket->Debug( _T("BadSearchResult") );
			theApp.Message( MSG_ERROR, IDS_PROTOCOL_BAD_HIT, (LPCTSTR)m_sAddress );
			Statistics.Current.eDonkey.Dropped++;
			m_nDropCount++;
		}
		
		return TRUE;
	}
	
	Network.OnQueryHits( pHits );
	
	return TRUE;
}

//////////////////////////////////////////////////////////////////////
// CEDNeighbour file adverising

//This function sends the list of shared files to the ed2k server. Note that it's best to
//keep it as brief as possible to reduce the load. This means the metadata should be limited, 
//and only files that are actually available for upload right now should be sent.
void CEDNeighbour::SendSharedFiles()
{	
	m_nFileLimit = Settings.eDonkey.MaxShareCount;
	
	CHostCacheHost *pServer = HostCache.eDonkey.Find( &m_pHost.sin_addr );
	if ( pServer && ( pServer->m_nFileLimit > 10 ) )
	{
		m_nFileLimit = pServer->m_nFileLimit;
		//m_nFileLimit = min( Settings.eDonkey.MaxShareCount, pServer->m_nFileLimit );
	}

	CEDPacket* pPacket = CEDPacket::New( ED2K_C2S_OFFERFILES );
	POSITION pos;
	
	DWORD nCount = 0;
	
	pPacket->WriteLongLE( nCount );			//Write number of files. (update this later)
	
	
	CSingleLock pTransfersLock( &Transfers.m_pSection );
	CSingleLock pLibraryLock( &Library.m_pSection );

	//Send files on download list to ed2k server (partials)
	pTransfersLock.Lock();
	for ( pos = Downloads.GetIterator() ; pos != NULL && ( nCount < m_nFileLimit ) ; )
	{
		CDownload* pDownload = Downloads.GetNext( pos );
		
		if ( pDownload->m_bED2K && pDownload->IsStarted() && ! pDownload->IsMoving() &&
			 pDownload->NeedHashset() == FALSE )	//If the file has an ed2k hash, has started, etc...
		{
			//Send the file hash to the ed2k server
			pPacket->Write( &pDownload->m_pED2K, sizeof(MD4) );

			//If we have a 'new' ed2k server
			if ( m_nTCPFlags & ED2K_SERVER_TCP_DEFLATE )
			{
				//Tell the server this is a partial using special code
				pPacket->WriteLongLE( 0xFCFCFCFC );
				pPacket->WriteShortLE ( 0xFCFC );
			}
			else
			{	//Send IP stuff. (Doesn't seem to be used for anything)
				if ( CEDPacket::IsLowID( m_nClientID ) )	//If we have a low ID
				{	//Send 0
					pPacket->WriteLongLE( 0 );
					pPacket->WriteShortLE ( 0 );
				}
				else	//High ID
				{	//send our IP
					pPacket->WriteLongLE( Network.m_pHost.sin_addr.S_un.S_addr );
					pPacket->WriteShortLE ( Network.m_pHost.sin_port );
				}
			}

			//Send tags
			pPacket->WriteLongLE( 2 ); //Number of Tags

			//Send the file name to the ed2k server
			CEDTag( ED2K_FT_FILENAME, pDownload->m_sRemoteName ).Write( pPacket, m_nTCPFlags );
			//Send the file size to the ed2k server
			CEDTag( ED2K_FT_FILESIZE, (DWORD)pDownload->m_nSize ).Write( pPacket, m_nTCPFlags );
			//Send the file type to the ed2k server
			//CEDTag( ED2K_FT_FILETYPE, strType ).Write( pPacket ); // We don't know it for certain with
			// incomplete files. Might be okay to assume from the extention, since they are usually correct.

			// Don't send metadata for incomplete files, since we have not verified it yet.
			// We should be careful not propagate unchecked metadata over ed2k, since there seems
			// to be a bit of a problem with bad data there... (Possibly old clients?)

			//Increment count of files sent
			nCount++;
		}
	}
	pTransfersLock.Unlock();
	
	//Send files in library to ed2k server (Complete files)
	pLibraryLock.Lock();
	for ( pos = LibraryMaps.GetFileIterator() ; pos != NULL && ( nCount < m_nFileLimit ) ; )
	{
		CLibraryFile* pFile = LibraryMaps.GetNextFile( pos );
		
		if ( pFile->IsShared() && pFile->m_bED2K )	//If file is shared and has an ed2k hash
		{
			if ( ( Settings.eDonkey.MinServerFileSize == 0 ) || ( nCount < ( m_nFileLimit / 2 ) ) || ( pFile->GetSize() > Settings.eDonkey.MinServerFileSize * 1024 * 1024 ) ) // If file is large enough to meet minimum requirement
			{
				if ( UploadQueues.CanUpload( PROTOCOL_ED2K, pFile ) ) // Check if a queue exists
				{
					// Initialise variables
					DWORD nTags;
					CString strType(_T("")), strCodec(_T(""));
					DWORD nBitrate = 0, nLength = 0;
					BYTE nRating = 0;

					// Send the file hash to the ed2k server
					pPacket->Write( &pFile->m_pED2K, sizeof(MD4) );

					//Send client ID stuff
					if ( m_nTCPFlags & ED2K_SERVER_TCP_DEFLATE ) // If we have a 'new' ed2k server
					{
						//Tell the server this is a complete file using the special code
						pPacket->WriteLongLE( 0xFBFBFBFB );
						pPacket->WriteShortLE ( 0xFBFB );
					}
					else	//'old' server
					{	//Send IP stuff. (unused?)
						if ( CEDPacket::IsLowID( m_nClientID ) )	//If we have a low ID
						{	//Send 0
							pPacket->WriteLongLE( 0 );
							pPacket->WriteShortLE ( 0 );
						}
						else	//High ID
						{	// send our IP
							pPacket->WriteLongLE( Network.m_pHost.sin_addr.S_un.S_addr );
							pPacket->WriteShortLE ( Network.m_pHost.sin_port );
						}
					}

					//// Set the file tags (Metadata) for the server

					// First, figure out what tags should be sent.
					nTags = 2; // File name and size are always present
					if ( pFile->m_pSchema != NULL )	// We need a schema to have extended details
					{
						// Do we have a file type?
						if ( pFile->m_pSchema->m_sDonkeyType.GetLength() )
						{
							strType = pFile->m_pSchema->m_sDonkeyType;
							nTags ++;
						}

						//Does this server support the new tags?
						if ( m_nTCPFlags & ED2K_SERVER_TCP_SMALLTAGS )	
						{					
							if ( pFile->IsSchemaURI( CSchema::uriAudio ) )	//If it's an audio file
							{
								// Bitrate
								if ( pFile->m_pMetadata->GetAttributeValue( _T("bitrate") ).GetLength() )	//And has a bitrate
								{	//Read in the bitrate
									_stscanf( pFile->m_pMetadata->GetAttributeValue( _T("bitrate") ), _T("%i"), &nBitrate );
									if ( nBitrate ) nTags ++;
								}

								// Length
								if ( pFile->m_pMetadata->GetAttributeValue( _T("seconds") ).GetLength() )	//And has seconds
								{	//Read in the no. seconds
									_stscanf( pFile->m_pMetadata->GetAttributeValue( _T("seconds") ), _T("%i"), &nLength );
									if ( nLength ) nTags ++;
								}
							}

							
							if ( pFile->IsSchemaURI( CSchema::uriVideo ) )	//If it's a video file
							{
								// Codec
								if ( pFile->m_pMetadata->GetAttributeValue( _T("codec") ).GetLength() )	//And has a codec
								{
									strCodec = pFile->m_pMetadata->GetAttributeValue( _T("codec") );
									if ( strCodec.GetLength() ) nTags ++;
								}

								// Length
								if ( pFile->m_pMetadata->GetAttributeValue( _T("minutes") ).GetLength() )	//And has minutes
								{	
									double nMins;
									//Read in the no. seconds
									_stscanf( pFile->m_pMetadata->GetAttributeValue( _T("minutes") ), _T("%.3lf"), &nMins );
									nLength = (DWORD)( nMins * (double)60 );	//Convert to seconds
									if ( nLength ) nTags ++;
								}
							}
						}
					}

					// Only newer servers support file ratings
					if ( ( m_nTCPFlags & ED2K_SERVER_TCP_SMALLTAGS ) && ( pFile->m_nRating ) )
					{
						nRating = min ( pFile->m_nRating, 5 );
						nTags ++;
					}

					//// End of server metadata calculation

					// Set the number of tags present
					pPacket->WriteLongLE( nTags );
	
					// Send the file name to the ed2k server
					CEDTag( ED2K_FT_FILENAME, pFile->m_sName ).Write( pPacket, m_nTCPFlags );
					// Send the file size to the ed2k server
					CEDTag( ED2K_FT_FILESIZE, (DWORD)pFile->GetSize() ).Write( pPacket, m_nTCPFlags );
					// Send the file type to the ed2k server
					if ( strType.GetLength() ) CEDTag( ED2K_FT_FILETYPE, strType ).Write( pPacket, m_nTCPFlags );
					// Send the bitrate to the ed2k server
					if ( nBitrate )	CEDTag( ED2K_FT_BITRATE, nBitrate ).Write( pPacket, m_nTCPFlags );
					// Send the length to the ed2k server
					if ( nLength )	CEDTag( ED2K_FT_LENGTH, nLength ).Write( pPacket, m_nTCPFlags );
					// Send the codec to the ed2k server
					if ( strCodec.GetLength() ) CEDTag( ED2K_FT_CODEC, strCodec ).Write( pPacket, m_nTCPFlags );
					// Send the file rating to the ed2k server
					if ( nRating ) CEDTag( ED2K_FT_FILERATING, nRating ).Write( pPacket, m_nTCPFlags );

					// Increment count of files sent
					nCount++;
				}
			}
		}
	}
	pLibraryLock.Unlock();

	*(DWORD*)pPacket->m_pBuffer = nCount;	// Correct the number of files sent
	
	if ( m_nTCPFlags & ED2K_SERVER_TCP_DEFLATE ) pPacket->Deflate();	// ZLIB compress if available

	Send( pPacket );	// Send the packet
}

// This function adds a download to the ed2k server file list.
BOOL CEDNeighbour::SendSharedDownload(CDownload* pDownload)
{
	if ( m_nState < nrsConnected ) return FALSE;
	if ( ! pDownload->m_bED2K || pDownload->NeedHashset() ) return FALSE;
	
	CEDPacket* pPacket = CEDPacket::New(  ED2K_C2S_OFFERFILES );
	pPacket->WriteLongLE( 1 );		// Number of files that will be sent

	// Send the file hash to the ed2k server
	pPacket->Write( &pDownload->m_pED2K, sizeof(MD4) );

	// If we have a 'new' ed2k server
	if ( m_nTCPFlags & ED2K_SERVER_TCP_DEFLATE )
	{
		// Tell the server this is a partial using special code
		pPacket->WriteLongLE( 0xFCFCFCFC );
		pPacket->WriteShortLE ( 0xFCFC );
	}
	else
	{	// Send IP stuff. (Doesn't seem to be used for anything)
		if ( CEDPacket::IsLowID( m_nClientID ) )	// If we have a low ID
		{	// Send 0
			pPacket->WriteLongLE( 0 );
			pPacket->WriteShortLE ( 0 );
		}
		else	//High ID
		{	//send our IP
			pPacket->WriteLongLE( Network.m_pHost.sin_addr.S_un.S_addr );
			pPacket->WriteShortLE ( Network.m_pHost.sin_port );
		}
	}

	//Send tags
	pPacket->WriteLongLE( 2 ); // Number of Tags

	// Send the file name to the ed2k server
	CEDTag( ED2K_FT_FILENAME, pDownload->m_sRemoteName ).Write( pPacket, m_nTCPFlags );
	// Send the file size to the ed2k server
	CEDTag( ED2K_FT_FILESIZE, (DWORD)pDownload->m_nSize ).Write( pPacket, m_nTCPFlags );

	// Compress if the server supports it
	if ( m_nTCPFlags & ED2K_SERVER_TCP_DEFLATE ) pPacket->Deflate();
	Send( pPacket );
	
	return TRUE;
}

//////////////////////////////////////////////////////////////////////
// CEDNeighbour file adverising

BOOL CEDNeighbour::SendQuery(CQuerySearch* pSearch, CPacket* pPacket, BOOL bLocal)
{
	if ( m_nState != nrsConnected || m_nClientID == 0 )
	{
		return FALSE;	// We're not ready
	}
	else if ( pPacket == NULL || pPacket->m_nProtocol != PROTOCOL_ED2K || ! bLocal )
	{
		return FALSE;	// Packet is bad
	}

	// Don't add the GUID for GetSources
	if ( ( ! pSearch->m_bED2K ) || ( pSearch->m_bWantDN && Settings.eDonkey.MagnetSearch ) )
		m_pQueries.AddTail( new GGUID( pSearch->m_pGUID ) );

	Send( pPacket, FALSE, FALSE );
	
	return TRUE;
}
