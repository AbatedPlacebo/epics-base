/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/*  
 *  $Id$
 *
 *                              
 *                    L O S  A L A M O S
 *              Los Alamos National Laboratory
 *               Los Alamos, New Mexico 87545
 *                                  
 *  Copyright, 1986, The Regents of the University of California.
 *                                  
 *           
 *	Author Jeffrey O. Hill
 *	johill@lanl.gov
 *	505 665 1831
 */

#ifndef cach
#define cach

#ifdef epicsExportSharedSymbols
#   define cach_restore_epicsExportSharedSymbols
#   undef epicsExportSharedSymbols
#endif

#include "compilerDependencies.h"
#include "ipAddrToAsciiAsynchronous.h"
#include "msgForMultiplyDefinedPV.h"
#include "epicsTimer.h"
#include "epicsEvent.h"
#include "freeList.h"

#ifdef cach_restore_epicsExportSharedSymbols
#   define epicsExportSharedSymbols
#   include "shareLib.h"
#endif

#include "nciu.h"
#include "comBuf.h"
#include "bhe.h"
#include "cacIO.h"
#include "netIO.h"
#include "localHostName.h"
#include "virtualCircuit.h"

class netWriteNotifyIO;
class netReadNotifyIO;
class netSubscription;

// used to control access to cac's recycle routines which
// should only be indirectly invoked by CAC when its lock
// is applied
class cacRecycle {              // X aCC 655
public:
    virtual void recycleReadNotifyIO ( 
        epicsGuard < epicsMutex > &, netReadNotifyIO &io ) = 0;
    virtual void recycleWriteNotifyIO ( 
        epicsGuard < epicsMutex > &, netWriteNotifyIO &io ) = 0;
    virtual void recycleSubscription ( 
        epicsGuard < epicsMutex > &, netSubscription &io ) = 0;
};

struct CASG;
class inetAddrID;
class caServerID;
struct caHdrLargeArray;

extern epicsThreadPrivateId caClientCallbackThreadId;

class cacComBufMemoryManager : public comBufMemoryManager
{
public:
    void * allocate ( size_t );
    void release ( void * );
private:
    tsFreeList < comBuf, 0x20 > freeList;
};

class cacDisconnectChannelPrivate { // X aCC 655
public:
    virtual void disconnectChannel ( 
        const epicsTime & currentTime, 
        epicsGuard < callbackMutex > &, 
        epicsGuard < epicsMutex > &, nciu & chan ) = 0;
};

class callbackMutex {
public:
    callbackMutex ( cacContextNotify & );
    ~callbackMutex ();
    void lock ();
    void unlock ();
private:
    cacContextNotify & notify;
    callbackMutex ( callbackMutex & );
    callbackMutex & operator = ( callbackMutex & );
};

class cac : 
    public cacContext,
    private cacRecycle, 
    private cacDisconnectChannelPrivate,
    private callbackForMultiplyDefinedPV
{
public:
    cac ( epicsMutex &, cacContextNotify & );
    virtual ~cac ();

    // beacon management
    void beaconNotify ( const inetAddrID & addr, const epicsTime & currentTime, 
        ca_uint32_t beaconNumber, unsigned protocolRevision );
    void repeaterSubscribeConfirmNotify ();
    unsigned beaconAnomaliesSinceProgramStart (
        epicsGuard < epicsMutex > & ) const;

    // IO management
    void flush ( epicsGuard < epicsMutex > & guard );
    bool executeResponse ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, caHdrLargeArray &, char *pMsgBody );

    // channel routines
    bool transferChanToVirtCircuit ( 
        epicsGuard < callbackMutex > &,
        unsigned cid, unsigned sid, 
        ca_uint16_t typeCode, arrayElementCount count, 
        unsigned minorVersionNumber, const osiSockAddr & );
    cacChannel & createChannel ( 
        epicsGuard < epicsMutex > & guard, const char * pChannelName, 
        cacChannelNotify &, cacChannel::priLev );
    void destroyChannel ( 
        epicsGuard < epicsMutex > &, nciu & );
    void initiateConnect ( 
        epicsGuard < epicsMutex > &, nciu & );

    // IO requests
    void writeRequest ( epicsGuard < epicsMutex > &, nciu &, unsigned type, 
        arrayElementCount nElem, const void * pValue );
    netWriteNotifyIO & writeNotifyRequest ( 
        epicsGuard < epicsMutex > &, nciu &, privateInterfaceForIO &,
        unsigned type, arrayElementCount nElem, const void * pValue, 
        cacWriteNotify & );
    netReadNotifyIO & readNotifyRequest ( 
        epicsGuard < epicsMutex > &, nciu &, privateInterfaceForIO &,
        unsigned type, arrayElementCount nElem, 
        cacReadNotify & );
    netSubscription & subscriptionRequest ( 
        epicsGuard < epicsMutex > &, nciu &, privateInterfaceForIO &,
        unsigned type, arrayElementCount nElem, unsigned mask, 
        cacStateNotify & );
    baseNMIU * destroyIO (
        epicsGuard < epicsMutex > & guard, 
        const cacChannel::ioid & idIn, 
        nciu & chan );
    void disconnectAllIO ( 
        epicsGuard < callbackMutex > &,
        epicsGuard < epicsMutex > &, 
        nciu &, tsDLList < baseNMIU > & ioList );

    void ioShow ( const cacChannel::ioid &id, unsigned level ) const;

    // sync group routines
    CASG * lookupCASG ( epicsGuard < epicsMutex > &, unsigned id );
    void installCASG ( epicsGuard < epicsMutex > &, CASG & );
    void uninstallCASG ( epicsGuard < epicsMutex > &, CASG & );

    // exception generation
    void exception ( epicsGuard < callbackMutex > &, 
        int status, const char * pContext,
        const char * pFileName, unsigned lineNo );

    // diagnostics
    unsigned circuitCount ( epicsGuard < epicsMutex > & ) const;
    void show ( epicsGuard < epicsMutex > &, unsigned level ) const;
    int printf ( const char *pformat, ... ) const;
    int vPrintf ( const char *pformat, va_list args ) const;

    // buffer management
    char * allocateSmallBufferTCP ();
    void releaseSmallBufferTCP ( char * );
    unsigned largeBufferSizeTCP () const;
    char * allocateLargeBufferTCP ();
    void releaseLargeBufferTCP ( char * );

    // misc
    const char * userNamePointer () const;
    unsigned getInitializingThreadsPriority () const;
    epicsMutex & mutexRef ();
    void attachToClientCtx ();
    void selfTest ( epicsGuard < epicsMutex > & ) const;
    double beaconPeriod ( const nciu & chan ) const;
    static unsigned lowestPriorityLevelAbove ( unsigned priority );
    static unsigned highestPriorityLevelBelow ( unsigned priority );
    void initiateAbortShutdown ( tcpiiu & );
    void destroyIIU ( tcpiiu & iiu ); 
    void flushIfRequired ( epicsGuard < epicsMutex > &, netiiu & ); 

private:
    localHostName hostNameCache;
    chronIntIdResTable < nciu > chanTable;
    //
    // !!!! There is at this point no good reason
    // !!!! to maintain one IO table for all types of
    // !!!! IO. It would probably be better to maintain
    // !!!! an independent table for each IO type. The
    // !!!! new adaptive sized hash table will not 
    // !!!! waste memory. Making this change will 
    // !!!! avoid virtual function overhead when 
    // !!!! accessing the different types of IO. This
    // !!!! approach would also probably be safer in 
    // !!!! terms of detecting damaged protocol.
    //
    chronIntIdResTable < baseNMIU > ioTable;
    resTable < bhe, inetAddrID > beaconTable;
    resTable < tcpiiu, caServerID > serverTable;
    tsDLList < tcpiiu > serverList;
    tsFreeList 
        < class tcpiiu, 32, epicsMutexNOOP > 
            freeListVirtualCircuit;
    tsFreeList 
        < class netReadNotifyIO, 1024, epicsMutexNOOP > 
            freeListReadNotifyIO;
    tsFreeList 
        < class netWriteNotifyIO, 1024, epicsMutexNOOP > 
            freeListWriteNotifyIO;
    tsFreeList 
        < class netSubscription, 1024, epicsMutexNOOP > 
            freeListSubscription;
    tsFreeList 
        < class nciu, 1024, epicsMutexNOOP > 
            channelFreeList;
    tsFreeList 
        < class msgForMultiplyDefinedPV, 16 > 
            mdpvFreeList;
    cacComBufMemoryManager comBufMemMgr;
    bheFreeStore bheFreeList;
    epicsTime programBeginTime;
    double connTMO;
    // **** lock hierarchy ****
    // callback lock must always be acquired before
    // the primary mutex if both locks are needed
    callbackMutex cbMutex;
    mutable epicsMutex & mutex; 
    epicsEvent iiuUninstall;
    ipAddrToAsciiEngine & ipToAEngine;
    epicsTimerQueueActive & timerQueue;
    char * pUserName;
    class udpiiu * pudpiiu;
    void * tcpSmallRecvBufFreeList;
    void * tcpLargeRecvBufFreeList;
    cacContextNotify & notify;
    epicsThreadId initializingThreadsId;
    unsigned initializingThreadsPriority;
    unsigned maxRecvBytesTCP;
    unsigned beaconAnomalyCount;

    void run ();
    void recycleReadNotifyIO ( 
        epicsGuard < epicsMutex > &, netReadNotifyIO &io );
    void recycleWriteNotifyIO ( 
        epicsGuard < epicsMutex > &, netWriteNotifyIO &io );
    void recycleSubscription ( 
        epicsGuard < epicsMutex > &, netSubscription &io );

    void disconnectChannel ( 
        const epicsTime & currentTime, 
        epicsGuard < callbackMutex > &, 
        epicsGuard < epicsMutex > &, nciu & chan );

    void ioExceptionNotify ( unsigned id, int status, 
        const char * pContext, unsigned type, arrayElementCount count );
    void ioExceptionNotifyAndUninstall ( unsigned id, int status, 
        const char * pContext, unsigned type, arrayElementCount count );

    void pvMultiplyDefinedNotify ( msgForMultiplyDefinedPV & mfmdpv, 
        const char * pChannelName, const char * pAcc, const char * pRej );

    // recv protocol stubs
    bool versionAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool echoRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool writeNotifyRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool readNotifyRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool eventRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool readRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool clearChannelRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool exceptionRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool accessRightsRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool createChannelRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool verifyAndDisconnectChan ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    bool badTCPRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );

    typedef bool ( cac::*pProtoStubTCP ) ( 
        epicsGuard < callbackMutex > &, tcpiiu &, 
        const epicsTime & currentTime, const caHdrLargeArray &, void *pMsgBdy );
    static const pProtoStubTCP tcpJumpTableCAC [];

    bool defaultExcep ( epicsGuard < callbackMutex > &, tcpiiu &iiu, const caHdrLargeArray &hdr, 
                    const char *pCtx, unsigned status );
    bool eventAddExcep ( epicsGuard < callbackMutex > &, tcpiiu &iiu, const caHdrLargeArray &hdr, 
                    const char *pCtx, unsigned status );
    bool readExcep ( epicsGuard < callbackMutex > &, tcpiiu &iiu, const caHdrLargeArray &hdr, 
                    const char *pCtx, unsigned status );
    bool writeExcep ( epicsGuard < callbackMutex > &, tcpiiu &iiu, const caHdrLargeArray &hdr, 
                    const char *pCtx, unsigned status );
    bool clearChanExcep ( epicsGuard < callbackMutex > &, tcpiiu &iiu, const caHdrLargeArray &hdr, 
                    const char *pCtx, unsigned status );
    bool readNotifyExcep ( epicsGuard < callbackMutex > &, tcpiiu &iiu, const caHdrLargeArray &hdr, 
                    const char *pCtx, unsigned status );
    bool writeNotifyExcep ( epicsGuard < callbackMutex > &, tcpiiu &iiu, const caHdrLargeArray &hdr, 
                    const char *pCtx, unsigned status );
    typedef bool ( cac::*pExcepProtoStubTCP ) ( 
                    epicsGuard < callbackMutex > &, tcpiiu &iiu, const caHdrLargeArray &hdr, 
                    const char *pCtx, unsigned status );
    static const pExcepProtoStubTCP tcpExcepJumpTableCAC [];

	cac ( const cac & );
	cac & operator = ( const cac & );
};

inline const char * cac::userNamePointer () const
{
    return this->pUserName;
}

inline unsigned cac::getInitializingThreadsPriority () const
{
    return this->initializingThreadsPriority;
}

inline epicsMutex & cac::mutexRef ()
{
    return this->mutex;
}

inline int cac::vPrintf ( const char *pformat, va_list args ) const
{
    return this->notify.vPrintf ( pformat, args );
}

inline void cac::attachToClientCtx ()
{
    this->notify.attachToClientCtx ();
}

inline char * cac::allocateSmallBufferTCP ()
{
    // this locks internally
    return ( char * ) freeListMalloc ( this->tcpSmallRecvBufFreeList );
}

inline void cac::releaseSmallBufferTCP ( char *pBuf )
{
    // this locks internally
    freeListFree ( this->tcpSmallRecvBufFreeList, pBuf );
}

inline unsigned cac::largeBufferSizeTCP () const
{
    return this->maxRecvBytesTCP;
}

inline char * cac::allocateLargeBufferTCP ()
{
    // this locks internally
    return ( char * ) freeListMalloc ( this->tcpLargeRecvBufFreeList );
}

inline void cac::releaseLargeBufferTCP ( char *pBuf )
{
    // this locks internally
    freeListFree ( this->tcpLargeRecvBufFreeList, pBuf );
}

inline unsigned cac::beaconAnomaliesSinceProgramStart (
    epicsGuard < epicsMutex > & guard ) const
{
    guard.assertIdenticalMutex ( this->mutex );
    return this->beaconAnomalyCount;
}

inline callbackMutex::callbackMutex ( cacContextNotify & notifyIn ) :
    notify ( notifyIn )
{
}

inline callbackMutex::~callbackMutex ()
{
}

inline void callbackMutex::lock ()
{
    this->notify.callbackLock ();
}

inline void callbackMutex::unlock ()
{
    this->notify.callbackUnlock ();
}

#endif // ifdef cach

