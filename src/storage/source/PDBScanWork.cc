/*****************************************************************************
 *                                                                           *
 *  Copyright 2018 Rice University                                           *
 *                                                                           *
 *  Licensed under the Apache License, Version 2.0 (the "License");          *
 *  you may not use this file except in compliance with the License.         *
 *  You may obtain a copy of the License at                                  *
 *                                                                           *
 *      http://www.apache.org/licenses/LICENSE-2.0                           *
 *                                                                           *
 *  Unless required by applicable law or agreed to in writing, software      *
 *  distributed under the License is distributed on an "AS IS" BASIS,        *
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. *
 *  See the License for the specific language governing permissions and      *
 *  limitations under the License.                                           *
 *                                                                           *
 *****************************************************************************/
/*
 * File:   PDBScanWork.cc
 * Author: Jia
 *
 */

#ifndef PDBSCANWORK_CC
#define PDBSCANWORK_CC

#include "PDBScanWork.h"
#include "PDBPage.h"
#include "PDBCommunicator.h"
#include "StoragePagePinned.h"
#include "SimpleRequestResult.h"
#include "UseTemporaryAllocationBlock.h"
#include "string.h"
using namespace std;


PDBScanWork::PDBScanWork(PageIteratorPtr iter, pdb :: PangeaStorageServer * storage, int & counter): counter(counter) {
    this->iter = iter;
    this->storage = storage;
}


bool PDBScanWork::sendPagePinned(pdb :: PDBCommunicatorPtr myCommunicator, bool morePagesToPin, NodeID nodeId, DatabaseID dbId, UserTypeID typeId,
        SetID setId, PageID pageId, size_t pageSize, size_t offset) {

        const pdb:: UseTemporaryAllocationBlock myBlock{1024};
        pdb::Handle<pdb :: StoragePagePinned> pagePinnedMsg = pdb::makeObject<pdb :: StoragePagePinned>();
	pagePinnedMsg->setMorePagesToLoad(morePagesToPin);
	pagePinnedMsg->setNodeID(nodeId);
	pagePinnedMsg->setDatabaseID(dbId);
	pagePinnedMsg->setUserTypeID(typeId);
	pagePinnedMsg->setSetID(setId);
	pagePinnedMsg->setPageID(pageId);
	pagePinnedMsg->setPageSize(pageSize);
	pagePinnedMsg->setSharedMemOffset(offset);

	string errMsg;
	if (!myCommunicator->sendObject<pdb :: StoragePagePinned> (pagePinnedMsg, errMsg)) {
		errMsg = "could not scan data: " + errMsg;
		return false;
	}
	return true;

}

bool PDBScanWork::acceptPagePinnedAck(pdb :: PDBCommunicatorPtr myCommunicator, bool & wasError, string & info, string & errMsg) {

	size_t sizeOfNextObject = myCommunicator->getSizeOfNextObject();
        const pdb :: UseTemporaryAllocationBlock myBlock{sizeOfNextObject};
	bool success;
	pdb :: Handle <pdb :: SimpleRequestResult> msg =
		myCommunicator->getNextObject<pdb :: SimpleRequestResult> (success, errMsg);

        if(!success) {
            wasError = true;
    	    return false;
        }

	wasError = false;
	return true;
}


// do the actual work
void PDBScanWork::execute(PDBBuzzerPtr callerBuzzer) {
    pdb :: PDBLoggerPtr logger = storage->getLogger();
    logger->writeLn("PDBScanWork: running...");
    //std :: cout << "PDBScanWork: running..." << std :: endl;
    PDBPagePtr page;
    string errMsg, info;
    bool wasError;

    logger->writeLn("PDBScanWork: connect to backend...");
    //create a new connection to backend server
    pdb :: PDBCommunicatorPtr communicatorToBackEnd = make_shared<pdb :: PDBCommunicator>();
    int retry = 0;
    while (communicatorToBackEnd->connectToLocalServer(logger, storage->getPathToBackEndServer(), errMsg)&&(retry < 10)) {
        retry ++;
        if (retry >= 10) {
    	    errMsg = "PDBScanWowrk: can not connect to local server.";
    	    cout << errMsg <<"\n";
    	    callerBuzzer->buzz(PDBAlarm::GenericError);
            return;
        }
        std :: cout << "PDBScanWork: Connect to local server failed, wait a while and retry..." << std :: endl;
        sleep (1);
    }
    if (retry > 0) {
        std :: cout << "PDBScanWork: Connected to local server" << std :: endl;
    }

    logger->writeLn("PDBScanWork: pin pages...");
    //for each loaded page retrieved from iterator, notify backend server!
    while (this->iter->hasNext()) {
        page = this->iter->next();
        if (page != nullptr) {
            //send PagePinned object to backend
                //std :: cout << "PDBScanWork: pin page with pageId ="<<page->getPageID()<<"\n";
        	logger->writeLn("PDBScanWork: pin pages with pageId = ");
        	logger->writeInt(page->getPageID());
        	this->sendPagePinned(communicatorToBackEnd, true, page->getNodeID(), page->getDbID(), page->getTypeID(), page->getSetID(), page->getPageID(), page->getSize(), page->getOffset());

        	//receive ack object from backend
                //std :: cout << "PDBScanWork: waiting for ack..." << std :: endl;
        	logger->writeLn("PDBScanWork: waiting for ack... ");
        	this->acceptPagePinnedAck(communicatorToBackEnd, wasError, info, errMsg);
        	logger->writeLn("PDBScanWork: ack received ");
                //std :: cout << "PDBScanWork: got ack!" << std :: endl;
        }
    }
    //close the connection
    //std :: cout << "PDBScanWork to close the loop" << std :: endl;
    this->sendPagePinned(communicatorToBackEnd, false, 0, 0, 0, 0, 0, 0, 0 );
    this->acceptPagePinnedAck(communicatorToBackEnd, wasError, info, errMsg);
    //notify the caller that this scan thread has finished work.
    //std :: cout << "PDBScanWork finished.\n";
    callerBuzzer->buzz(PDBAlarm::WorkAllDone, this->counter);
}

#endif



