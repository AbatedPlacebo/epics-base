/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/* devBiSoftCallback.c */
/*
 *      Author:  Marty Kraimer
 *      Date:    23APR2008
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "callback.h"
#include "cantProceed.h"
#include "dbDefs.h"
#include "dbAccess.h"
#include "dbNotify.h"
#include "recGbl.h"
#include "recSup.h"
#include "devSup.h"
#include "link.h"
#include "biRecord.h"
#include "epicsExport.h"

/* Create the dset for devBiSoftCallback */
static long init_record();
static long read_bi();
struct {
	long		number;
	DEVSUPFUN	report;
	DEVSUPFUN	init;
	DEVSUPFUN	init_record;
	DEVSUPFUN	get_ioint_info;
	DEVSUPFUN	read_bi;
	DEVSUPFUN	special_linconv;
}devBiSoftCallback={
	6,
	NULL,
	NULL,
	init_record,
	NULL,
	read_bi,
	NULL};
epicsExportAddress(dset,devBiSoftCallback);

typedef struct notifyInfo {
    processNotify *ppn;
    CALLBACK *pcallback;
    unsigned short value;
    int    status;
}notifyInfo;

static void getCallback(processNotify *ppn,notifyGetType type)
{
    struct biRecord *pbi = (struct biRecord *)ppn->usrPvt;
    notifyInfo *pnotifyInfo = (notifyInfo *)pbi->dpvt;
    int status = 0;
    long no_elements = 1;
    long options = 0;

    if(ppn->status==notifyCanceled) {
        printf("dbtpn:getCallback notifyCanceled\n");
        return;
    }
    switch(type) {
    case getFieldType:
        status = dbGetField(ppn->paddr,DBR_USHORT,&pnotifyInfo->value,
                            &options,&no_elements,0);
        break;
    case getType:
        status = dbGet(ppn->paddr,DBR_USHORT,&pnotifyInfo->value,
                       &options,&no_elements,0);
        break;
    }
    pnotifyInfo->status = status;
}

static void doneCallback(processNotify *ppn)
{
    struct biRecord *pbi = (struct biRecord *)ppn->usrPvt;
    notifyInfo *pnotifyInfo = (notifyInfo *)pbi->dpvt;

    callbackRequestProcessCallback(pnotifyInfo->pcallback,pbi->prio,pbi);
}


static long init_record(struct biRecord *pbi)
{
    DBLINK *plink = &pbi->inp;
    struct instio *pinstio;
    char  *pvname;
    DBADDR *pdbaddr=NULL;
    long  status;
    notifyInfo *pnotifyInfo;
    CALLBACK *pcallback;
    processNotify  *ppn=NULL;

    if(plink->type!=INST_IO) {
        recGblRecordError(S_db_badField,(void *)pbi,
            "devBiSoftCallback (init_record) Illegal INP field");
        pbi->pact=TRUE;
        return(S_db_badField);
    }
    pinstio=(struct instio*)&(plink->value);
    pvname = pinstio->string;
    pdbaddr = callocMustSucceed(1, sizeof(*pdbaddr),
        "devBiSoftCallback::init_record");
    status = dbNameToAddr(pvname,pdbaddr);
    if(status) {
        recGblRecordError(status,(void *)pbi,
            "devBiSoftCallback (init_record) linked record not found");
        pbi->pact=TRUE;
        return(status);
    }
    pnotifyInfo = callocMustSucceed(1, sizeof(*pnotifyInfo),
        "devBiSoftCallback::init_record");
    pcallback = callocMustSucceed(1, sizeof(*pcallback),
        "devBiSoftCallback::init_record");
    ppn = callocMustSucceed(1, sizeof(*ppn),
        "devBiSoftCallback::init_record");
    pnotifyInfo->ppn = ppn;
    pnotifyInfo->pcallback = pcallback;
    ppn->usrPvt = pbi;
    ppn->paddr = pdbaddr;
    ppn->getCallback = getCallback;
    ppn->doneCallback = doneCallback;
    ppn->requestType = processGetRequest;
    pbi->dpvt = pnotifyInfo;
    return 0;
}

static long read_bi(biRecord *pbi)
{
    notifyInfo *pnotifyInfo = (notifyInfo *)pbi->dpvt;

    if(pbi->pact) {
        if(pnotifyInfo->status) {
            recGblSetSevr(pbi,READ_ALARM,INVALID_ALARM);
            return(2);
        }
        pbi->val = pnotifyInfo->value;
        pbi->udf = FALSE;
        return(2);
    }
    dbProcessNotify(pnotifyInfo->ppn);
    pbi->pact = TRUE;
    return(0);
}
