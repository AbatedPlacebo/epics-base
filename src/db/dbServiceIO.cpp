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

#pragma message ( "file name needs to change" )

#include <limits.h>

#include "epicsMutex.h"
#include "tsFreeList.h"

#include "cadef.h" // this can be eliminated when the callbacks use the new interface
#include "db_access.h" // should be eliminated here in the future
#include "caerr.h" // should be eliminated here in the future
#include "epicsEvent.h"
#include "epicsThread.h"
#include "epicsSingleton.h"

#define epicsExportSharedSymbols
#include "db_access_routines.h"
#include "dbCAC.h"
#include "dbChannelIO.h"
#include "dbPutNotifyBlocker.h"

class dbService : public cacService {
public:
    cacContext & contextCreate ( 
        epicsMutex &, cacContextNotify & );
};

static dbService dbs;

cacContext & dbService::contextCreate ( 
    epicsMutex & mutex, cacContextNotify & notify )
{
    return * new dbContext ( mutex, notify );
}

extern "C" void dbServiceIOInit ()
{
    caInstallDefaultService ( dbs );
}

dbBaseIO::dbBaseIO () {}

dbContext::dbContext ( epicsMutex & mutexIn, cacContextNotify & notifyIn ) :
    readNotifyCache ( mutexIn ), ctx ( 0 ), 
    stateNotifyCacheSize ( 0 ), mutex ( mutexIn ),
    notify ( notifyIn ), pNetContext ( 0 ), pStateNotifyCache ( 0 )
{
}

dbContext::~dbContext ()
{
    delete [] this->pStateNotifyCache;
    if ( this->ctx ) {
        db_close_events ( this->ctx );
    }
}

cacChannel & dbContext::createChannel ( // X aCC 361
    epicsGuard < epicsMutex > & guard, const char * pName, 
    cacChannelNotify & notifyIn, cacChannel::priLev priority )
{
    guard.assertIdenticalMutex ( this->mutex );

    struct dbAddr addr;
    int status;
    {
        // dont know if the database might call a put callback 
        // while holding its lock ...
        epicsGuardRelease < epicsMutex > unguard ( guard );
        status = db_name_to_addr ( pName, & addr );
    }
    if ( status ) {
        if ( ! this->pNetContext.get() ) {
            this->pNetContext.reset (
                & this->notify.createNetworkContext ( this->mutex ) );
        }
        return this->pNetContext->createChannel (
                    guard, pName, notifyIn, priority );
    }
    else if ( ca_preemtive_callback_is_enabled () ) {
        return * new ( this->dbChannelIOFreeList )
            dbChannelIO ( this->mutex, notifyIn, addr, *this ); 
    }
    else {
        errlogPrintf ( 
            "dbContext: preemptive callback required for direct in\n"
            "memory interfacing of CA channels to the DB.\n" );
        throw cacChannel::unsupportedByService ();
    }
}

void dbContext::destroyChannel ( 
    epicsGuard < epicsMutex > & guard, dbChannelIO & chan )
{
    guard.assertIdenticalMutex ( this->mutex );
    chan.destructor ( guard );
    this->dbChannelIOFreeList.release ( & chan );
}

void dbContext::callStateNotify ( struct dbAddr & addr, 
        unsigned type, unsigned long count, 
        const struct db_field_log * pfl, 
        cacStateNotify & notify )
{
    unsigned long size = dbr_size_n ( type, count );

    if ( type > INT_MAX ) {
        epicsGuard < epicsMutex > guard ( this->mutex );
        notify.exception ( guard, ECA_BADTYPE, 
            "type code out of range (high side)", 
            type, count );
        return;
    }

    if ( count > INT_MAX ) {
        epicsGuard < epicsMutex > guard ( this->mutex );
        notify.exception ( guard, ECA_BADCOUNT, 
            "element count out of range (high side)",
            type, count);
        return;
    }

    // no need to lock this because state notify is 
    // called from only one event queue consumer thread
    if ( this->stateNotifyCacheSize < size) {
        char * pTmp = new char [size];
        delete [] this->pStateNotifyCache;
        this->pStateNotifyCache = pTmp;
        this->stateNotifyCacheSize = size;
    }
    void *pvfl = (void *) pfl;
    int status = db_get_field ( &addr, static_cast <int> ( type ), 
                    this->pStateNotifyCache, static_cast <int> ( count ), pvfl );
    if ( status ) {
        epicsGuard < epicsMutex > guard ( this->mutex );
        notify.exception ( guard, ECA_GETFAIL, 
            "db_get_field() completed unsuccessfuly", type, count );
    }
    else { 
        epicsGuard < epicsMutex > guard ( this->mutex );
        notify.current ( guard, type, count, this->pStateNotifyCache );
    }
}

extern "C" void cacAttachClientCtx ( void * pPrivate )
{
    int status = ca_attach_context ( (ca_client_context *) pPrivate );
    assert ( status == ECA_NORMAL );
}

void dbContext::subscribe ( 
    epicsGuard < epicsMutex > & guard,
    struct dbAddr & addr, dbChannelIO & chan,
    unsigned type, unsigned long count, unsigned mask, 
    cacStateNotify & notify, cacChannel::ioid * pId )
{
    guard.assertIdenticalMutex ( this->mutex );

    /*
     * the database uses type "int" to store these parameters
     */
    if ( type > INT_MAX ) {
        throw cacChannel::badType();
    }
    if ( count > INT_MAX ) {
        throw cacChannel::outOfBounds();
    }

    if ( ! this->ctx ) {
        dbEventCtx tmpctx = 0;
        {
            epicsGuardRelease < epicsMutex > unguard ( guard );
            tmpctx = db_init_events ();
            if ( ! tmpctx ) {
                throw std::bad_alloc ();
            }

            unsigned selfPriority = epicsThreadGetPrioritySelf ();
            unsigned above;
            epicsThreadBooleanStatus tbs = 
                epicsThreadLowestPriorityLevelAbove ( selfPriority, &above );
            if ( tbs != epicsThreadBooleanStatusSuccess ) {
                above = selfPriority;
            }
            int status = db_start_events ( tmpctx, "CAC-event", 
                cacAttachClientCtx, ca_current_context (), above );
            if ( status ) {
                db_close_events ( tmpctx );
                throw std::bad_alloc ();
            }
        }
        if ( this->ctx ) {
            // another thread tried to simultaneously setup 
            // the event system
            db_close_events ( tmpctx );
        }
        else {
            this->ctx = tmpctx;
        }
    }

    dbSubscriptionIO & subscr =
        * new ( this->dbSubscriptionIOFreeList ) 
        dbSubscriptionIO ( guard, this->mutex, *this, chan, 
            addr, notify, type, count, mask, this->ctx );
    chan.dbContextPrivateListOfIO::eventq.add ( subscr );
    this->ioTable.add ( subscr );

    if ( pId ) {
        *pId = subscr.getId ();
    }
}

void dbContext::initiatePutNotify ( 
    epicsGuard < epicsMutex > & guard,
    dbChannelIO & chan, struct dbAddr & addr, 
    unsigned type, unsigned long count, const void * pValue, 
    cacWriteNotify & notify, cacChannel::ioid * pId )
{
    guard.assertIdenticalMutex ( this->mutex );
    if ( ! chan.dbContextPrivateListOfIO::pBlocker ) {
        chan.dbContextPrivateListOfIO::pBlocker = 
            new ( this->dbPutNotifyBlockerFreeList ) 
                dbPutNotifyBlocker ( this->mutex );
        this->ioTable.add ( *chan.dbContextPrivateListOfIO::pBlocker );
    }
    chan.dbContextPrivateListOfIO::pBlocker->initiatePutNotify ( 
        guard, notify, addr, type, count, pValue );
    if ( pId ) {
        *pId = chan.dbContextPrivateListOfIO::pBlocker->getId ();
    }
}

void dbContext::destroyAllIO ( 
    epicsGuard < epicsMutex > & guard, dbChannelIO & chan )
{
    guard.assertIdenticalMutex ( this->mutex );
    dbSubscriptionIO * pIO;
    tsDLList < dbSubscriptionIO > tmp;

    while ( ( pIO = chan.dbContextPrivateListOfIO::eventq.get() ) ) {
        this->ioTable.remove ( *pIO );
        tmp.add ( *pIO );
    }
    if ( chan.dbContextPrivateListOfIO::pBlocker ) {
        this->ioTable.remove ( *chan.dbContextPrivateListOfIO::pBlocker );
    }

    while ( ( pIO = tmp.get() ) ) {
        // This prevents a db event callback from coming 
        // through after the notify IO is deleted
        pIO->unsubscribe ( guard );
        // If they call ioCancel() here it will be ignored
        // because the IO has been unregistered above.
        pIO->channelDeleteException ( guard );
        pIO->destructor ( guard );
        this->dbSubscriptionIOFreeList.release ( pIO );
    }

    if ( chan.dbContextPrivateListOfIO::pBlocker ) {
        chan.dbContextPrivateListOfIO::pBlocker->destructor ( guard );
        this->dbPutNotifyBlockerFreeList.release ( chan.dbContextPrivateListOfIO::pBlocker );
        chan.dbContextPrivateListOfIO::pBlocker = 0;
    }
}

void dbContext::ioCancel ( 
    epicsGuard < epicsMutex > & guard, dbChannelIO & chan, 
    const cacChannel::ioid &id )
{
    guard.assertIdenticalMutex ( this->mutex );
    dbBaseIO * pIO = this->ioTable.remove ( id );
    if ( pIO ) {
        dbSubscriptionIO *pSIO = pIO->isSubscription ();
        if ( pSIO ) {
            chan.dbContextPrivateListOfIO::eventq.remove ( *pSIO );
            pSIO->destructor ( guard );
            this->dbSubscriptionIOFreeList.release ( pSIO );
        }
        else if ( pIO == chan.dbContextPrivateListOfIO::pBlocker ) {
            chan.dbContextPrivateListOfIO::pBlocker->cancel ( guard );
        }
        else {
            errlogPrintf ( "dbContext::ioCancel() unrecognized IO was probably leaked or not canceled\n" );
        }
    }
}

void dbContext::ioShow ( 
    epicsGuard < epicsMutex > & guard, const cacChannel::ioid &id, 
    unsigned level ) const
{
    guard.assertIdenticalMutex ( this->mutex );
    const dbBaseIO * pIO = this->ioTable.lookup ( id );
    if ( pIO ) {
        pIO->show ( guard, level );
    }
}

void dbContext::showAllIO ( const dbChannelIO & chan, unsigned level ) const
{
    epicsGuard < epicsMutex > guard ( this->mutex );
    tsDLIterConst < dbSubscriptionIO > pItem = 
        chan.dbContextPrivateListOfIO::eventq.firstIter ();
    while ( pItem.valid () ) {
        pItem->show ( guard, level );
        pItem++;
    }
    if ( chan.dbContextPrivateListOfIO::pBlocker ) {
        chan.dbContextPrivateListOfIO::pBlocker->show ( guard, level );
    }
}

void dbContext::show ( unsigned level ) const
{
    epicsGuard < epicsMutex > guard ( this->mutex );
}

void dbContext::show ( 
    epicsGuard < epicsMutex > & guard, unsigned level ) const
{
    guard.assertIdenticalMutex ( this->mutex );
    printf ( "dbContext at %p\n", 
        static_cast <const void *> ( this ) );
    if ( level > 0u ) {
        printf ( "\tevent call back cache location %p, and its size %lu\n", 
            static_cast <void *> ( this->pStateNotifyCache ), this->stateNotifyCacheSize );
        this->readNotifyCache.show ( guard, level - 1 );
    }
    if ( level > 1u ) {
        this->mutex.show ( level - 2u );
    }
}

void dbContext::flush ( 
    epicsGuard < epicsMutex > & )
{
}

unsigned dbContext::circuitCount (
    epicsGuard < epicsMutex > & ) const
{
    return 0u;
}

void dbContext::selfTest (
    epicsGuard < epicsMutex > & guard ) const
{
    guard.assertIdenticalMutex ( this->mutex );
    this->ioTable.verify ();
}

unsigned dbContext::beaconAnomaliesSinceProgramStart (
    epicsGuard < epicsMutex > & ) const
{
    return 0u;
}


