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

#ifndef PDBSCANWORK_CC
#define PDBSCANWORK_CC

#include "PDBDebug.h"
#include "PDBScanWork.h"
#include "PDBPage.h"
#include "PDBCommunicator.h"
#include "StoragePagePinned.h"
#include "SimpleRequestResult.h"
#include "UseTemporaryAllocationBlock.h"
#include "string.h"
#include <thread>
#include <sstream>
using namespace std;

#ifndef HEADER_SIZE
#define HEADER_SIZE 20
#endif

#ifndef MAX_RETRIES
#define MAX_RETRIES 5
#endif

PDBScanWork::PDBScanWork(PageIteratorPtr iter, pdb::PangeaStorageServer* storage, int& counter)
    : counter(counter) {
    this->iter = iter;
    this->storage = storage;
    pthread_mutex_init(&connection_mutex, nullptr);
}

PDBScanWork::~PDBScanWork() {
    pthread_mutex_destroy(&connection_mutex);
}

bool PDBScanWork::sendPagePinned(pdb::PDBCommunicatorPtr myCommunicator,
                                 bool morePagesToPin,
                                 NodeID nodeId,
                                 DatabaseID dbId,
                                 UserTypeID typeId,
                                 SetID setId,
                                 PageID pageId,
                                 size_t pageSize,
                                 size_t offset) {

    const pdb::UseTemporaryAllocationBlock myBlock{2048};
    pdb::Handle<pdb::StoragePagePinned> pagePinnedMsg = pdb::makeObject<pdb::StoragePagePinned>();
    pagePinnedMsg->setMorePagesToLoad(morePagesToPin);
    pagePinnedMsg->setNodeID(nodeId);
    pagePinnedMsg->setDatabaseID(dbId);
    pagePinnedMsg->setUserTypeID(typeId);
    pagePinnedMsg->setSetID(setId);
    pagePinnedMsg->setPageID(pageId);
    pagePinnedMsg->setPageSize(pageSize);
    pagePinnedMsg->setSharedMemOffset(offset);

    string errMsg;
    if (!myCommunicator->sendObject<pdb::StoragePagePinned>(pagePinnedMsg, errMsg)) {
        errMsg = "could not scan data: " + errMsg;
        return false;
    }
    return true;
}

bool PDBScanWork::acceptPagePinnedAck(pdb::PDBCommunicatorPtr myCommunicator,
                                      bool& wasError,
                                      string& info,
                                      string& errMsg) {

    size_t sizeOfNextObject = myCommunicator->getSizeOfNextObject();
    if (sizeOfNextObject < HEADER_SIZE) {
        wasError = true;
        return false;
    }
    PDB_COUT << "PDBScanWork: to create memory block for SimpleRequestResult" << std::endl;
    const pdb::UseTemporaryAllocationBlock myBlock{sizeOfNextObject};
    PDB_COUT << "PDBScanWork: memory block allocated" << std::endl;
    bool success;
    pdb::Handle<pdb::SimpleRequestResult> msg =
        myCommunicator->getNextObject<pdb::SimpleRequestResult>(success, errMsg);

    if (!success) {
        wasError = true;
        return false;
    }

    wasError = false;
    return true;
}


// do the actual work
void PDBScanWork::execute(PDBBuzzerPtr callerBuzzer) {
    pdb::PDBLoggerPtr logger = make_shared<pdb::PDBLogger>("pdbScanWorks.log");
    logger->debug("PDBScanWork: running...");
    PDBPagePtr page;
    string errMsg, info;
    bool wasError;

    logger->debug("PDBScanWork: connect to backend...");
    pthread_mutex_lock(&connection_mutex);
    pdb::PDBCommunicatorPtr communicatorToBackEnd = make_shared<pdb::PDBCommunicator>();
    int retry = 0;
    while (communicatorToBackEnd->connectToLocalServer(
               logger, storage->getPathToBackEndServer(), errMsg) &&
           (retry < MAX_RETRIES)) {
        retry++;
        if (retry >= MAX_RETRIES) {
            errMsg = "PDBScanWowrk: can not connect to local server.";
            cout << errMsg << "\n";
            callerBuzzer->buzz(PDBAlarm::GenericError);
            pthread_mutex_unlock(&connection_mutex);
            return;
        }
        sleep(0);
    }
    pthread_mutex_unlock(&connection_mutex);
    if (retry > 0) {
    }

    logger->debug("PDBScanWork: pin pages...");
    // for each loaded page retrieved from iterator, notify backend server!
    while (this->iter->hasNext()) {
        page = this->iter->next();
        if (page != nullptr) {
            // send PagePinned object to backend
            PDB_COUT << "PDBScanWork: pin page with pageId =" << page->getPageID() << "\n";
            retry = 0;

            while (retry < MAX_RETRIES) {
                logger->debug(string("PDBScanWork: pin pages with pageId = ") +
                              to_string(page->getPageID()));
                bool ret = this->sendPagePinned(communicatorToBackEnd,
                                                true,
                                                page->getNodeID(),
                                                page->getDbID(),
                                                page->getTypeID(),
                                                page->getSetID(),
                                                page->getPageID(),
                                                page->getSize(),
                                                page->getOffset());
                if (ret == false) {
                    communicatorToBackEnd->reconnect(errMsg);
                    retry++;
                    continue;
                }
                // receive ack object from backend
                PDB_COUT << "PDBScanWork: waiting for ack..." << std::endl;
                logger->debug("PDBScanWork: waiting for ack... ");
                ret = this->acceptPagePinnedAck(communicatorToBackEnd, wasError, info, errMsg);
                if (ret == false) {
                    communicatorToBackEnd->reconnect(errMsg);
                    retry++;
                    continue;
                }
                logger->debug("PDBScanWork: ack received ");
                PDB_COUT << "PDBScanWork: got ack!" << std::endl;
                break;
            }
        }
    }
    // close the connection
    PDB_COUT << "PDBScanWork to close the loop" << std::endl;
    logger->debug("PDBScanWork to close the loop");
    retry = 0;
    while (retry < MAX_RETRIES) {
        bool ret = this->sendPagePinned(communicatorToBackEnd, false, 0, 0, 0, 0, 0, 0, 0);
        if (ret == false) {
            communicatorToBackEnd->reconnect(errMsg);
            retry++;
            continue;
        }
        ret = this->acceptPagePinnedAck(communicatorToBackEnd, wasError, info, errMsg);
        if (ret == false) {
            communicatorToBackEnd->reconnect(errMsg);
            retry++;
            continue;
        }
        // notify the caller that this scan thread has finished work.
        PDB_COUT << "PDBScanWork finished.\n";
        logger->debug("PDBScanWork finished.\n");
        break;
    }
    callerBuzzer->buzz(PDBAlarm::WorkAllDone, this->counter);
}

#endif
