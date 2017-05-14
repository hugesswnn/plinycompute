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

#ifndef PANGEA_STORAGE_SERVER_C
#define PANGEA_STORAGE_SERVER_C

#include "PDBDebug.h"
#include "JoinMap.h"
#include "PangeaStorageServer.h"
#include "SimpleRequestResult.h"
#include "CatalogServer.h"
#include "StorageAddData.h"
#include "StorageAddObject.h"
#include "StorageAddDatabase.h"
#include "StorageAddSet.h"
#include "StorageClearSet.h"
#include "StorageGetData.h"
#include "StorageGetDataResponse.h"
#include "StorageGetSetPages.h"
#include "StoragePinPage.h"
#include "StorageUnpinPage.h"
#include "StoragePagePinned.h"
#include "StorageNoMorePage.h"
#include "StorageTestSetScan.h"
#include "BackendTestSetScan.h"
#include "StorageTestSetCopy.h"
#include "BackendTestSetCopy.h"
#include "StorageAddTempSet.h"
#include "StorageAddTempSetResult.h"
#include "StorageRemoveDatabase.h"
#include "StorageRemoveTempSet.h"
#include "StorageExportSet.h"
#include "StorageRemoveUserSet.h"
#include "StorageCleanup.h"
#include "PDBScanWork.h"
#include "UseTemporaryAllocationBlock.h"
#include "SimpleRequestHandler.h"
#include "Record.h"
#include "InterfaceFunctions.h"
#include "DeleteSet.h"
#include "DefaultDatabase.h"
#include "DataTypes.h"
#include "SharedMem.h"
#include "PDBFlushProducerWork.h"
#include "PDBFlushConsumerWork.h"
#include "ExportableObject.h"
//#include <hdfs/hdfs.h>
#include <cstdio>
#include <memory>
#include <string>
#include <iostream>
#include <signal.h>
#include <stdio.h>
#include <map>
#include <iterator>


#define FLUSH_BUFFER_SIZE 3


namespace pdb {




size_t PangeaStorageServer :: bufferRecord (pair <std :: string, std :: string> databaseAndSet, Record <Vector <Handle <Object>>> *addMe) {
	if (allRecords.count (databaseAndSet) == 0) {
		std :: vector <Record <Vector <Handle <Object>>> *> records;
		records.push_back (addMe);
		allRecords[databaseAndSet] = records;
		sizes[databaseAndSet] = addMe->numBytes ();
	} else {
		allRecords[databaseAndSet].push_back (addMe);
		sizes[databaseAndSet] += addMe->numBytes ();
	} 
	return sizes[databaseAndSet];
}

PangeaStorageServer :: PangeaStorageServer (SharedMemPtr shm,
                PDBWorkerQueuePtr workers, PDBLoggerPtr logger, ConfigurationPtr conf, bool standalone) {

        //server initialization
        //cout << "Storage server is initializing...\n";

        //configuring server
        this->nodeId = conf->getNodeID();
        this->serverName = conf->getServerName();
        this->shm = shm;
        this->workers = workers;
        this->conf = conf;
        this->logger = logger;
        //this->logger->setEnabled(conf->isLogEnabled());
        this->standalone = standalone;
        this->totalObjects = 0;
        //IPC file used to communicate with backend
        this->pathToBackEndServer = this->conf->getBackEndIpcFile();

        //initialize flush buffer
        //a producer work will periodically remove unpinned data from input buffer, and
        this->flushBuffer = make_shared<PageCircularBuffer>(FLUSH_BUFFER_SIZE, logger);

        //initialize cache, must be initialized before databases
        this->cache = make_shared<PageCache>(conf, workers, flushBuffer, logger, shm);

        //initialize and load databases, must be initialized after cache
        this->dbs = new std :: map<DatabaseID, DefaultDatabasePtr>();
        this->name2id = new std :: map< std :: string, DatabaseID>();
        this->tempSets = new std :: map<SetID, TempSetPtr>();
        this->name2tempSetId = new std :: map< std :: string, SetID>();
        this->userSets = new std :: map< std :: pair <DatabaseID, SetID>,  SetPtr>();
        this->names2ids = new std :: map< std :: pair <std :: string, std :: string>, std :: pair <DatabaseID, SetID>>();
        this->typename2id = new std :: map< std :: string, SetID>(); 



        //initialize meta data mutex
        pthread_mutex_init(&(this->databaseLock), nullptr);
        pthread_mutex_init(&(this->typeLock), nullptr);
        pthread_mutex_init(&(this->tempsetLock), nullptr);
        pthread_mutex_init(&(this->usersetLock), nullptr);


        this->databaseSeqId.initialize(1);//DatabaseID starting from 1
        this->usersetSeqIds = new std :: map<std :: string, SequenceID * >();
        //if meta/data/temp directories do not exist, create them.
        this->createRootDirs();
        this->initializeFromRootDirs(this->metaRootPath, this->dataRootPaths);
        this->createTempDirs();
        this->conf->createDir(this->metaTempPath);
        this->addType("UnknownUserData", 0); //user type starting from 1
}

SetPtr PangeaStorageServer :: getSet (pair <std :: string, std :: string> databaseAndSet) {
        if (names2ids->count(databaseAndSet) != 0) {
            pair<DatabaseID, SetID> ids = names2ids->at(databaseAndSet);
	    if (userSets->count(ids) != 0) {
                return (*userSets)[ids];
	    }
        }
	return nullptr;
}

void PangeaStorageServer :: cleanup() {
        PDB_COUT << "to clean up for storage..." << std :: endl; 
        for (auto &a : allRecords) {
                while (a.second.size () > 0)
                        writeBackRecords (a.first);
        }
        PDB_COUT << "Now there are " << totalObjects << " objects stored in storage" << std :: endl;
        //getFunctionality <PangeaStorageServer> ().getCache()->unpinAndEvictAllDirtyPages();
        PDB_COUT << "sleep for 1 second to wait for all data gets flushed" << std :: endl;
        sleep(1);
        PDB_COUT << "cleaned up for storage..." << std :: endl;
}

PangeaStorageServer :: ~PangeaStorageServer () {

        //clean();
        stopFlushConsumerThreads(); 
        pthread_mutex_destroy(&(this->databaseLock));
        pthread_mutex_destroy(&(this->typeLock));
        pthread_mutex_destroy(&(this->tempsetLock));
        pthread_mutex_destroy(&(this->usersetLock));
        delete this->dbs;
        delete this->name2id;
        delete this->tempSets;
        delete this->name2tempSetId;
        delete this->userSets;
        delete this->names2ids;
        delete this->typename2id;
}

PDBPagePtr PangeaStorageServer :: getNewPage (pair <std :: string, std :: string> databaseAndSet) {

	// and get that page
	//getFunctionality <CatalogServer> ().getNewPage (databaseAndSet.first, databaseAndSet.second);
	SetPtr whichSet = getSet (databaseAndSet);
        if (whichSet == nullptr) {
            return nullptr;
        } else {
	    return whichSet->addPage();
        }
}


void PangeaStorageServer :: writeBackRecords (pair <std :: string, std :: string> databaseAndSet) {

	// get all of the records
	auto &allRecs = allRecords[databaseAndSet];


        // the current size (in bytes) of records we need to process
        size_t numBytesToProcess = sizes[databaseAndSet];
        PDB_COUT << "buffer is full, to write to a storage page"<< std::endl;
 
	// now, get a page to write to
	PDBPagePtr myPage = getNewPage (databaseAndSet);
        if (myPage == nullptr) {
            std :: cout << "FATAL ERROR: set to store data doesn't exist!" << std :: endl;
            std :: cout << "databaseName" << databaseAndSet.first << std :: endl;
            std :: cout << "setName" << databaseAndSet.second << std :: endl;
            return ;
        }
        size_t pageSize = myPage->getSize();
        PDB_COUT << "Got new page with pageId=" << myPage->getPageID() << ", and size=" << pageSize << std :: endl;
	// the position in the output vector
	int pos = 0;
	
	// the number of items in the current record we are processing
	int numObjectsInRecord;


	// now, keep looping until we run out of records to process (in which case, we'll break)
	while (true) {

		// all allocations will be done to the page
		UseTemporaryAllocationBlock block (myPage->getBytes (), pageSize);
		Handle <Vector <Handle <Object>>> data = makeObject <Vector <Handle <Object>>> ();

		try {
			// while there are still records
			while (allRecs.size () > 0) {

				//std :: cout << "Processing a record!!\n";

				auto &allObjects = *(allRecs[allRecs.size () - 1]->getRootObject ());
				numObjectsInRecord = allObjects.size ();
              
				// put all of the data onto the page
				for (; pos < numObjectsInRecord; pos++) {
					data->push_back (allObjects[pos]);
                                        totalObjects++;
                                }
	
				// now kill this record
				numBytesToProcess -= allRecs[allRecs.size () - 1]->numBytes ();
				free (allRecs[allRecs.size () - 1]);
				allRecs.pop_back ();
				pos = 0;
			}

			// if we got here, all records have been processed

                        // comment the following three lines of code to allow Pangea to manage pages
			PDB_COUT << "Write all of the bytes in the record.\n";
                        getRecord(data);

                        CacheKey key;
                        key.dbId = myPage->getDbID();
                        key.typeId = myPage->getTypeID();
                        key.setId = myPage->getSetID();
                        key.pageId = myPage->getPageID();
                        this->getCache()->decPageRefCount(key);
                        this->getCache()->flushPageWithoutEviction(key);
			break;

		// put the extra objects tht we could not store back in the record
		} catch (NotEnoughSpace &n) {

                        // comment the following three lines of code to allow Pangea to manage pages						
			PDB_COUT << "Writing back a page!!\n";
			// write back the current page...
                        getRecord(data);
			//myPage->wroteBytes ();
			//myPage->flush ();

			//myPage->unpin ();
                        //this->getCache()->evictPage(myPage);
                        CacheKey key;
                        key.dbId = myPage->getDbID();
                        key.typeId = myPage->getTypeID();
                        key.setId = myPage->getSetID();
                        key.pageId = myPage->getPageID();
                        this->getCache()->decPageRefCount(key);
                        this->getCache()->flushPageWithoutEviction(key);

			// there are two cases... in the first case, we can make another page out of this data, since we have enough records to do so
			if (numBytesToProcess + (((numObjectsInRecord - pos) / numObjectsInRecord) * allRecs[allRecs.size () - 1]->numBytes ()) > pageSize) {
				
				PDB_COUT << "Are still enough records for another page.\n";
				myPage = getNewPage (databaseAndSet);
                                pageSize = myPage->getSize();
				continue;

			// in this case, we have a small bit of data left
			} else {
					
				// create the vector to hold these guys
				void *myRAM = malloc (allRecs[allRecs.size () - 1]->numBytes ());
				const UseTemporaryAllocationBlock block (myRAM, allRecs[allRecs.size () - 1]->numBytes ());
				Handle <Vector <Handle <Object>>> extraData = makeObject <Vector <Handle <Object>>> (numObjectsInRecord - pos);

				// write the objects to the vector
				auto &allObjects = *(allRecs[allRecs.size () - 1]->getRootObject ());
				for (; pos < numObjectsInRecord; pos++) {
					extraData->push_back (allObjects[pos]);	
				}
				PDB_COUT << "Putting the records back complete.\n";
	
				// destroy the record that we were copying from
				numBytesToProcess -= allRecs[allRecs.size () - 1]->numBytes ();
				free (allRecs[allRecs.size () - 1]);
	
				// and get the record that we copied to
				allRecs[allRecs.size () - 1] = getRecord (extraData);	
				numBytesToProcess += allRecs[allRecs.size () - 1]->numBytes ();
				break;
			}
		}
	}

	PDB_COUT << "Now all the records are back.\n";
	sizes[databaseAndSet] = numBytesToProcess;
}


//export to a local file
bool PangeaStorageServer :: exportToFile (std :: string dbName, std :: string setName, std :: string path, std :: string format, std :: string & errMsg) {

    FILE * myFile = fopen(path.c_str(), "w+");
    if (myFile == NULL) {
        errMsg = "Error opening file for writing: " + path;
        return false;
    }

    SetPtr setToExport = getFunctionality<PangeaStorageServer>().getSet(std :: make_pair (dbName, setName));
    if (setToExport == nullptr) {
        errMsg = "Error in exportToFile: set doesn't exist: " + dbName + ":" + setName;
        return false;
    }

    bool isHeadWritten = false;
    setToExport->setPinned(true);
    std :: vector<PageIteratorPtr> * pageIters = setToExport->getIterators();
    int numIterators = pageIters->size();
    for (int i = 0; i < numIterators; i++) {
        PageIteratorPtr iter = pageIters->at(i);
        while (iter->hasNext()) {
            PDBPagePtr nextPage = iter->next();
            if (nextPage != nullptr) {
                Record <Vector<Handle<Object>>> * myRec = (Record <Vector<Handle<Object>>> *) (nextPage->getBytes());
                Handle<Vector<Handle<Object>>> inputVec = myRec->getRootObject ();
                int vecSize = inputVec->size();
                for ( int j = 0; j < vecSize; j++) {
                    Handle<ExportableObject> objectToExport = unsafeCast<ExportableObject, Object>((*inputVec)[j]);
                    if (isHeadWritten == false) {
                        std :: string header = objectToExport->toSchemaString(format);
                        if (header != "") {
                            fprintf(myFile, "%s", header.c_str()); 
                        }
                        isHeadWritten = true;
                    }
                    std :: string value = objectToExport->toValueString(format);
                    if (value != "") {
                        fprintf(myFile, "%s", value.c_str());
                    }
                    
                }
                //to evict this page
                PageCachePtr cache = getFunctionality <PangeaStorageServer> ().getCache ();
                CacheKey key;
                key.dbId = nextPage->getDbID();
                key.typeId = nextPage->getTypeID();
                key.setId = nextPage->getSetID();
                key.pageId = nextPage->getPageID();
                cache->decPageRefCount(key);
                cache->evictPage(key);//try to modify this to something like evictPageWithoutFlush() or clear set in the end.
            }
        }
    }
    setToExport->setPinned(false);
    delete pageIters;
    fflush(myFile);
    fclose(myFile);
    return true;
}

//export to a HDFS partition
bool  PangeaStorageServer :: exportToHDFSFile (std :: string dbName, std :: string setName, std :: string hdfsNameNodeIp, int hdfsNameNodePort, std :: string path, std :: string format, std :: string & errMsg) {
    //hdfsFS fs = hdfsConnect (hdfsNameNodeIp, hdfsNameNodePort);

    /*    
    struct hdfsBuilder * builder = hdfsNewBuilder();
    hdfsBuilderSetNameNode(builder, hdfsNameNodeIp);
    hdfsBuilderSetNameNodePort(builder, hdfsNameNodePort);
    hdfsFS fs = hdfsBuilderConnect(builder);
    hdfsFile myFile = hdfsOpenFile(fs, path.c_str(), O_WRONLY|O_CREAT, 0, 0, 0);
    if (!writeFile) {
        errMsg = "Error opening file for writing: " + path;
        return false;
    }

    SetPtr setToExport = getFunctionality<PangeaStorageServer>().getSet(std :: make_pair (dbName, setName));
    if (setToExport == nullptr) {
        errMsg = "Error in exportToFile: set doesn't exist: " + dbName + ":" + setName;
        return false;
    }

    bool isHeadWritten = false;
    setToExport->setPinned(true);
    std :: vector<PageIteratorPtr> * pageIters = setToExport->getIterators();
    int numIterators = pageIters->size();
    for (int i = 0; i < numIterators; i++) {
        PageIteratorPtr iter = pageIters->at(i);
        while (iter->hasNext()) {
            PDBPagePtr nextPage = iter->next();
            if (nextPage != nullptr) {
                Record <Vector<Handle<Object>>> * myRec = (Record <Vector<Handle<Object>>> *) (nextPage->getBytes());
                Handle<Vector<Handle<Object>>> inputVec = myRec->getRootObject ();
                int vecSize = inputVec->size();
                for ( int j = 0; j < vecSize; j++) {
                    Handle<ExportableObject> objectToExport = unsafeCast<ExportableObject, Object>((*inputVec)[j]);
                    if (isHeadWritten == false) {
                        std :: string header = objectToExport->toSchemaString();
                        if (header != "") {
                            hdfsWrite(fs, myFile, header.c_str(), header.length()+1);
                        }
                        isHeadWritten = true;
                    }
                    std :: string value = objectToExport->toValueString();
                    if (value != "") {
                        hdfsWrite(fs, myFile, value.c_str(), value.length()+1);
                    }

                }
                //to evict this page
                PageCachePtr cache = getFunctionality <PangeaStorageServer> ().getCache ();
                CacheKey key;
                key.dbId = nextPage->getDbID();
                key.typeId = nextPage->getTypeID();
                key.setId = nextPage->getSetID();
                key.pageId = nextPage->getPageID();
                cache->decPageRefCount(key);
                cache->evictPage(key);//try to modify this to something like evictPageWithoutFlush() or clear set in the end.
            }
        }
    }
    setToExport->setPinned(false);
    delete pageIters;
    hdfsFlush(fs, myFile);
    hdfsCloseFile(fs, myFile);
    return true;
    */

    return false;

}

void PangeaStorageServer :: registerHandlers (PDBServer &forMe) {
       // this handler accepts a request to write back all buffered records
       forMe.registerHandler (StorageCleanup_TYPEID, make_shared<SimpleRequestHandler<StorageCleanup>> (
           [&] (Handle<StorageCleanup> request, PDBCommunicatorPtr sendUsingMe) {
               PDB_COUT << "received StorageCleanup" << std :: endl;
               std :: string errMsg;
               bool res = true;
               getFunctionality<PangeaStorageServer>().cleanup();

               const UseTemporaryAllocationBlock tempBlock{1024};
               Handle<SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

               res = sendUsingMe->sendObject (response, errMsg);
               return make_pair (res, errMsg);
           }

       ));


       // this handler accepts a request to add a database
       forMe.registerHandler (StorageAddDatabase_TYPEID, make_shared<SimpleRequestHandler<StorageAddDatabase>> (
                [&] (Handle <StorageAddDatabase> request, PDBCommunicatorPtr sendUsingMe) {
                        PDB_COUT << "received StorageAddDatabase" << std :: endl;
                        std :: string errMsg;
                        bool res = true;
                        if (standalone == true) {
                            res = getFunctionality<PangeaStorageServer>().addDatabase(request->getDatabase());
                            if (res == false) {
                                 errMsg = "Database already exists\n";
                            } else {
                                res = getFunctionality<CatalogServer>().addDatabase (request->getDatabase(), errMsg);
                            }

                        } else {
                            if ((res = getFunctionality<PangeaStorageServer>().addDatabase(request->getDatabase())) == false) {
                                 errMsg = "Database already exists\n";
                            }
                        }
                        //make response
                        const UseTemporaryAllocationBlock tempBlock{1024};
                        Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                        //return the result
                        res = sendUsingMe->sendObject (response, errMsg);
                        return make_pair (res, errMsg);
                }

       ));

       // this handler accepts a request to add a set
       forMe.registerHandler (StorageAddSet_TYPEID, make_shared<SimpleRequestHandler<StorageAddSet>> (
               [&] (Handle <StorageAddSet> request, PDBCommunicatorPtr sendUsingMe) {
                         PDB_COUT << "received StorageAddSet" << std :: endl;
                         std :: string errMsg;
                         bool res = true;
                         if (standalone == true) {
                               PDB_COUT << "adding set in standalone mode" << std :: endl;
                               res = getFunctionality<PangeaStorageServer>().addSet(request->getDatabase(), request->getTypeName(), request->getSetName());
                             if (res == false) {
                                 errMsg = "Set already exists\n";
                             } else {
                                          //int16_t typeID = getFunctionality<CatalogServer>().searchForObjectTypeName (request->getTypeName());
                                          int16_t typeID = VTableMap :: getIDByName (request->getTypeName(), false);
                                          PDB_COUT << "TypeID ="<< typeID << std :: endl;
                                          if (typeID == -1) {
                                                    errMsg = "Could not find type " + request->getTypeName();
                                                    res = false;
                                          } else {
                                                    PDB_COUT << "to add set in catalog" << std :: endl;
                                                    res = getFunctionality<CatalogServer>().addSet(typeID, request->getDatabase(), request->getSetName(), errMsg);
                                                    if (res == true) {
                                                        PDB_COUT << "success" << std :: endl;
                                                    } else {
                                                        PDB_COUT << "failed" << std :: endl;
                                                    }
                                          }

                             }
                         } else {
                            PDB_COUT << "creating set in Pangea in distributed environment...with setName=" << request->getSetName() << std :: endl;
                         if ((res = getFunctionality<PangeaStorageServer>().addSet(request->getDatabase(), request->getTypeName(), request->getSetName())) == false) {
                             errMsg = "Set already exists\n";
                             cout << errMsg << endl;
                         } else {
                             //int16_t typeID = getFunctionality<CatalogClient>().searchForObjectTypeName (request->getTypeName());
                             #ifdef CHECK_TYPE
                             int16_t typeID = VTableMap :: getIDByName(request->getTypeName(), false);
                             PDB_COUT << "TypeID ="<< typeID << std :: endl;
                             // make sure the type is registered in the catalog
                             if (typeID == -1) {
                                errMsg = "Could not find type " + request->getTypeName();
                                cout << errMsg << endl;
                                res = false;
                             }
                             #endif
                         }
                      }
                      // make the response
                      const UseTemporaryAllocationBlock tempBlock{1024};
                      Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                      // return the result
                      res = sendUsingMe->sendObject (response, errMsg);
                      return make_pair (res, errMsg);
               }
       ));


       // this handler requests to add a temp set
       forMe.registerHandler (StorageAddTempSet_TYPEID, make_shared<SimpleRequestHandler<StorageAddTempSet>> (
               [&] (Handle <StorageAddTempSet> request, PDBCommunicatorPtr sendUsingMe) {
                         std :: string errMsg;
                         //add a temp set in local
                         SetID setId;
                         bool res = getFunctionality<PangeaStorageServer>().addTempSet(request->getSetName(), setId);
                         if (res == false) {
                                 errMsg = "Set already exists\n";
                         } 
             
                         // make the response
                         const UseTemporaryAllocationBlock tempBlock{1024};
                         Handle <StorageAddTempSetResult> response = makeObject <StorageAddTempSetResult> (res, errMsg, setId);

         
                         // return the result
                         //std :: cout << "To send StorageAddTempSetResult object to backend..." << std :: endl;
                         res = sendUsingMe->sendObject<StorageAddTempSetResult> (response, errMsg);
                         //std :: cout << "Sent StorageAddTempSetResult object to backend." << std :: endl;
                         return make_pair (res, errMsg);
               }
       ));

        // this handler requests to add a temp set
        forMe.registerHandler (StorageRemoveDatabase_TYPEID, make_shared<SimpleRequestHandler<StorageRemoveDatabase>> (
                [&] (Handle <StorageRemoveDatabase> request, PDBCommunicatorPtr sendUsingMe) {
                    
                    std :: string errMsg;
                    std::string databaseName = request->getDatabase();
                    bool res;
                    PDB_COUT << "Deleting database " << databaseName << std::endl;
                    if (standalone == true) {
                         res = getFunctionality<PangeaStorageServer>(). removeDatabase(databaseName);
                         if (res == false) {
                             errMsg = "Failed to delete database\n";
                         } else {
                             res = getFunctionality<CatalogServer>().deleteDatabase(databaseName, errMsg);
                         }                      
                    } else {
                        res = getFunctionality<PangeaStorageServer>().removeDatabase(databaseName);
                        if (res == false) {
                            errMsg = "Failed to delete database\n";
                         }
                    }
                    // make the response
                    const UseTemporaryAllocationBlock tempBlock{1024};
                    Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                    res = sendUsingMe->sendObject<SimpleRequestResult> (response, errMsg);
                    return make_pair (res, errMsg);
                }
        ));

       // this handler requests to remove a user set
       forMe.registerHandler (StorageRemoveUserSet_TYPEID, make_shared<SimpleRequestHandler<StorageRemoveUserSet>> (
               [&] (Handle <StorageRemoveUserSet> request, PDBCommunicatorPtr sendUsingMe) {
                         std :: string errMsg;
                         std :: string databaseName = request->getDatabase();
                         std :: string typeName = request->getTypeName ();
                         std :: string setName = request->getSetName();
                         bool res = true;
                         #ifdef REMOVE_SET_WITH_EVICTION
                         SetPtr setToRemove = getSet(std :: pair <std :: string, std :: string>(databaseName, setName));
                         if (setToRemove == nullptr) {
                              // make the response
                              const UseTemporaryAllocationBlock tempBlock{1024};
                              errMsg = "Set doesn't exist.";
                              Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (false, errMsg);

                              // return the result
                              res = sendUsingMe->sendObject (response, errMsg);
                              return make_pair (res, errMsg);
                         } else {
                              setToRemove->evictPages();
                         }
                         #endif
                         if (standalone == true) {
                             res = getFunctionality<PangeaStorageServer>().removeSet(databaseName, typeName, setName);
                             if (res == false) {
                                 errMsg = "Set doesn't exist\n";
                             }

                             // deletes set in catalog
                             res = getFunctionality<CatalogServer>().deleteSet(request->getDatabase(), request->getSetName(), errMsg);


                         } else {
                            if ((res = getFunctionality<PangeaStorageServer>().removeSet(databaseName, setName)) == false) {
                                     errMsg = "Error removing set!\n";
                                     cout << errMsg << endl;
                            } 
                         }
                         // make the response
                         const UseTemporaryAllocationBlock tempBlock{1024};
                         Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                         // return the result
                         res = sendUsingMe->sendObject (response, errMsg);
                         return make_pair (res, errMsg);
               }

       ));

       forMe.registerHandler (StorageClearSet_TYPEID, make_shared<SimpleRequestHandler<StorageClearSet>> (
               [&] (Handle <StorageClearSet> request, PDBCommunicatorPtr sendUsingMe) {
                         std :: string errMsg;
                         std :: string databaseName = request->getDatabase();
                         std :: string typeName = request->getTypeName ();
                         std :: string setName = request->getSetName();
                         bool res = true;
                         if (standalone == true) {
                             PDB_COUT << "removing set in standalone mode" << std :: endl;
                             res = getFunctionality<PangeaStorageServer>().removeSet(databaseName, typeName, setName);
                             if (res == false) {
                                 errMsg = "Set doesn't exist\n";
                             } else {
                                 PDB_COUT << "adding set in standalone mode" << std :: endl;
                                 res = getFunctionality<PangeaStorageServer>().addSet(request->getDatabase(), request->getTypeName(), request->getSetName());
                             }

                         } else {
                            PDB_COUT << "removing set in cluster mode" << std :: endl;
                            if ((res = getFunctionality<PangeaStorageServer>().removeSet(databaseName, setName)) == false) {
                                     errMsg = "Error removing set!\n";
                                     cout << errMsg << endl;
                            } else {
                                     if ((res = getFunctionality<PangeaStorageServer>().addSet(request->getDatabase(), request->getTypeName(), request->getSetName())) == false) {
                                         errMsg = "Set already exists\n";
                                         cout << errMsg << endl;
                                     }
                            }
                         }
                         // make the response
                         const UseTemporaryAllocationBlock tempBlock{1024};
                         Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                         // return the result
                         res = sendUsingMe->sendObject (response, errMsg);
                         return make_pair (res, errMsg);
               }

       ));
/*
       // this handler requests to remove a user set
       forMe.registerHandler (DeleteSet_TYPEID, make_shared<SimpleRequestHandler<DeleteSet>> (
               [&] (Handle <DeleteSet> request, PDBCommunicatorPtr sendUsingMe) {
                         std :: string errMsg;
                         std :: string databaseName = request->whichDatabase();
                         std :: string setName = request->whichSet();
                         bool res = false;
                         if ((getFunctionality<CatalogServer> ().isSetRegistered (databaseName, setName)) == true) {
                             if ((res = getFunctionality<PangeaStorageServer>().removeSet(databaseName, setName)) == false) {
                                 errMsg = "Error removing set!\n";
                                 cout << errMsg << endl;
                             }
                         } else {
                                 errMsg = "Set doesn't exist\n";
                         }
                         // make the response
                         const UseTemporaryAllocationBlock tempBlock{1024};
                         Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                         // return the result
                         res = sendUsingMe->sendObject (response, errMsg);
                         return make_pair (res, errMsg);
               }

       ));
*/



       // this handler requests to remove a temp set
       forMe.registerHandler (StorageRemoveTempSet_TYPEID, make_shared<SimpleRequestHandler<StorageRemoveTempSet>> (
               [&] (Handle <StorageRemoveTempSet> request, PDBCommunicatorPtr sendUsingMe) {
                         std :: string errMsg;
                         //add a set in local
                         SetID setId = request->getSetID();                        
                         bool res = getFunctionality<PangeaStorageServer>().removeTempSet(setId);
                         if (res == false) {
                                 errMsg = "Set doesn't exist\n";
                         } 
                         // make the response
                         const UseTemporaryAllocationBlock tempBlock{1024};
                         Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                         // return the result
                         res = sendUsingMe->sendObject (response, errMsg);
                         return make_pair (res, errMsg);
               }
       ));

       // this handler requests to export a set to a local file
       forMe.registerHandler (StorageExportSet_TYPEID, make_shared<SimpleRequestHandler<StorageExportSet>> (
               [&] (Handle <StorageExportSet> request, PDBCommunicatorPtr sendUsingMe) {
                         std :: string errMsg;
                         bool res = getFunctionality<PangeaStorageServer>().exportToFile(request->getDbName(), request->getSetName(),
                                                   request->getOutputFilePath(), request->getFormat(), errMsg);
                         // make the response
                         const UseTemporaryAllocationBlock tempBlock{1024};
                         Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                         // return the result
                         res = sendUsingMe->sendObject (response, errMsg);
                         return make_pair (res, errMsg);

               }
       ));

       /*
       forMe.registerHandler (StorageShutDown_TYPEID, make_shared <SimpleRequestHandler <ShutDown>> (
                [&] (Handle<ShutDown> request, PDBCommunicatorPtr sendUsingMe) {
                         getFunctionality <PangeaStorageServer> ().clean();
                         bool res = true;
                         std :: string errMsg;
                         return make_pair (res, errMsg);
                }

       ));
       */


        // this handler accepts a request to store one large object (e.g. JoinMap) in one page
        forMe.registerHandler (StorageAddObject_TYPEID, make_shared <SimpleRequestHandler <StorageAddObject>> (
                [&] (Handle <StorageAddObject> request, PDBCommunicatorPtr sendUsingMe) {

                std :: string errMsg;
                bool everythingOK = true;
                bool typeCheckOrNot = request->isTypeCheck();
                if (typeCheckOrNot == true) {
#ifdef DEBUG_SET_TYPE
                        // first, check with the catalog to make sure that the given database, set, and type are correct
                        int16_t typeID = getFunctionality <CatalogServer> ().getObjectType (request->getDatabase (), request->getSetName ());
                        if (typeID < 0) {
                                everythingOK = false;
                        }
                                // if we made it here, the type is correct, as is the database and the set
                        //else if (typeID != getFunctionality <CatalogServer> ().searchForObjectTypeName (request->getType ())) {
                        else if (typeID != VTableMap :: getIDByName (request->getType(), false)) {
                                everythingOK = false;
                        }
#endif

                 }

                 // get the record
                 size_t numBytes = sendUsingMe->getSizeOfNextObject ();
                 void *readToHere = malloc (numBytes);
                 everythingOK = sendUsingMe->receiveBytes (readToHere, errMsg);
                 Record<JoinMap<Object>> * record = (Record<JoinMap<Object>> *)readToHere;
                 Handle<JoinMap<Object>> objectToStore = record->getRootObject();
                 std :: cout << "PangeaStorageServer: MapSize=" << objectToStore->size() << std :: endl;
                 if (everythingOK) {
                      auto databaseAndSet = make_pair ((std :: string) request->getDatabase (),
                                                        (std :: string) request->getSetName ());
                      // now, get a page to write to
                      PDBPagePtr myPage = getNewPage (databaseAndSet);
                      if (myPage == nullptr) {
                          std :: cout << "FATAL ERROR: set to store data doesn't exist!" << std :: endl;
                          std :: cout << "databaseName" << databaseAndSet.first << std :: endl;
                          std :: cout << "setName" << databaseAndSet.second << std :: endl;
                          return make_pair(false, std :: string("FATAL ERROR: set to store data doesn't exist!"));
                      }
                      size_t pageSize = myPage->getSize();
                      PDB_COUT << "Got new page with pageId=" << myPage->getPageID() << ", and size=" << pageSize << std :: endl;
                      memcpy (myPage->getBytes(), readToHere, numBytes);
                       
                      CacheKey key;
                      key.dbId = myPage->getDbID();
                      key.typeId = myPage->getTypeID();
                      key.setId = myPage->getSetID();
                      key.pageId = myPage->getPageID();
                      this->getCache()->decPageRefCount(key);

                 }
                 else {
                      errMsg = "Tried to add data of the wrong type to a database set or database set doesn't exit.\n";
                      everythingOK = false;
                 }

                 //std :: cout << "Making response object.\n";
                 const UseTemporaryAllocationBlock block{1024};
                 Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (everythingOK, errMsg);

                 // return the result
                 //std :: cout << "Sending response object.\n";
                 bool res = sendUsingMe->sendObject (response, errMsg);
                 return make_pair (res, errMsg);
             }
        ));



	// this handler accepts a request to store some data
	forMe.registerHandler (StorageAddData_TYPEID, make_shared <SimpleRequestHandler <StorageAddData>> (
		[&] (Handle <StorageAddData> request, PDBCommunicatorPtr sendUsingMe) {

	        std :: string errMsg;
		bool everythingOK = true;
                bool typeCheckOrNot = request->isTypeCheck();
                if (typeCheckOrNot == true) {
#ifdef DEBUG_SET_TYPE
			// first, check with the catalog to make sure that the given database, set, and type are correct
			int16_t typeID = getFunctionality <CatalogServer> ().getObjectType (request->getDatabase (), request->getSetName ());
			if (typeID < 0) {
                                everythingOK = false;
                        }
				// if we made it here, the type is correct, as is the database and the set
		        //else if (typeID != getFunctionality <CatalogServer> ().searchForObjectTypeName (request->getType ())) {
                        else if (typeID != VTableMap :: getIDByName (request->getType(), false)) {
                                everythingOK = false;
                        }
#endif

                 }
					
	         // get the record
		 size_t numBytes = sendUsingMe->getSizeOfNextObject ();
	         void *readToHere = malloc (numBytes);
		 Handle<Vector<Handle<Object>>> objectsToStore = sendUsingMe->getNextObject <Vector <Handle <Object>>> (readToHere, everythingOK, errMsg);
                 if (objectsToStore->size() == 0) {
                     everythingOK = false;
                     errMsg = "Warning: client attemps to store a vector that contains zero objects, simply ignores it";
                     std :: cout << errMsg << std :: endl;
                 }
		 if (everythingOK) {	
						// at this point, we have performed the serialization, so remember the record
				auto databaseAndSet = make_pair ((std :: string) request->getDatabase (),
        	                                        (std :: string) request->getSetName ());
				getFunctionality <PangeaStorageServer> ().bufferRecord 
							(databaseAndSet, (Record <Vector <Handle <Object>>> *) readToHere); 

             
                                 size_t numBytesToProcess = sizes[databaseAndSet];
                                                size_t rawPageSize = getFunctionality<PangeaStorageServer>().getConf()->getPageSize();

                                                if(numBytesToProcess < rawPageSize) {
                                                      PDB_COUT << "data is buffered, all buffered data size=" << numBytesToProcess << std :: endl;
                                                 }
                                                else {
						      // if we have enough space to fill up a page, do it
						      PDB_COUT << "Got the data.\n";
						      PDB_COUT << "Are " << sizes[databaseAndSet] << " bytes to write.\n";
						      getFunctionality <PangeaStorageServer> ().writeBackRecords (databaseAndSet);
						      PDB_COUT << "Done with write back.\n";
						      PDB_COUT << "Are " << sizes[databaseAndSet] << " bytes left.\n";
                                                }
		 }
		 else {
				errMsg = "Tried to add data of the wrong type to a database set or database set doesn't exit.\n";
				everythingOK = false;
		 }
                 //getFunctionality <PangeaStorageServer> ().getCache()->unpinAndEvictAllDirtyPages();

	         //std :: cout << "Making response object.\n";
                 const UseTemporaryAllocationBlock block{1024};                        
		 Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (everythingOK, errMsg);

                 // return the result
		 //std :: cout << "Sending response object.\n";
                 bool res = sendUsingMe->sendObject (response, errMsg);
                 return make_pair (res, errMsg);
             }
	));


       // this handler accepts a request to get data from a set
       forMe.registerHandler (StorageGetData_TYPEID, make_shared<SimpleRequestHandler<StorageGetData>> (
               [&] (Handle <StorageGetData> request, PDBCommunicatorPtr sendUsingMe) {
                         std :: string errMsg;
                         bool res;
                         //add a set in local
                         SetPtr set = getFunctionality<PangeaStorageServer>().getSet(std :: pair <std :: string, std :: string> (request->getDatabase(), request->getSetName()));
                         if (set == nullptr) {
                                 errMsg = "Set doesn't exist.\n";
                                 res = false;
                         } else {
                                 int numPages = set->getNumPages();
                                 {
                                     const UseTemporaryAllocationBlock tempBlock{1024};
                                     Handle<StorageGetDataResponse> response = makeObject<StorageGetDataResponse>(numPages, request->getDatabase(), request->getSetName(), conf->getPageSize(), conf->getPageSize()-(sizeof(NodeID) + sizeof(DatabaseID) + sizeof(UserTypeID) + sizeof(SetID) + sizeof(PageID)), res, errMsg);
                                     res = sendUsingMe->sendObject (response, errMsg);
                                 }
                                 if(getFunctionality<PangeaStorageServer>().isStandalone() == true) {
                                          //int16_t typeID = getFunctionality<CatalogServer>().searchForObjectTypeName (request->getType());
                                          int16_t typeID = VTableMap :: getIDByName(request->getType(), false);
                                          PDB_COUT << "TypeID ="<< typeID << std :: endl;
                                          if (typeID == -1) {
                                                    errMsg = "Could not find type " + request->getType();
                                                    res = false;
                                          } else {
                                                    getFunctionality<PangeaStorageServer>().getCache()->pin(set, MRU, Read);
                                                    std :: vector<PageIteratorPtr> * iterators = set->getIterators();
                                                    int numIterators = iterators->size();
                                                    int numPagesSent = 0;
                                                    for (int i = 0; i < numIterators; i++) {
                                                              PageIteratorPtr iter = iterators->at(i);
                                                              while (iter->hasNext()) {
                                                                      PDBPagePtr page = iter->next();
                                                                      if (page != nullptr) {
                                                                          PDB_COUT << "to send the " << numPagesSent <<"-th page" << std :: endl;
                                                                          //const UseTemporaryAllocationBlock block{128*1024*1024};
                                                                          //std :: cout << "sending the page..."<<std :: endl;
                                                                          res = sendUsingMe->sendBytes(page->getRawBytes(), page->getRawSize(), errMsg);
                                                                          //std :: cout << "page sent..."<<std :: endl;
                                                                          page->unpin();
                                                                          if (res == false) {
                                                                               std :: cout << "sending data failed\n";
                                                                               return make_pair (res, errMsg);
                                                                          }
                                                                          //std :: cout << "sending data succeeded\n";
                                                                          numPagesSent ++;
                                                                          //std :: cout << "to send the next page\n";
                                                                      }
                                                              }
                                                    }
                                                    PDB_COUT << "All done!\n";
                                                    delete iterators;
                                          }
                                 }
                         }
                         return make_pair (res, errMsg);
               }
       ));

        // this handler accepts a request to pin a page
        forMe.registerHandler (StoragePinPage_TYPEID, make_shared <SimpleRequestHandler <StoragePinPage>> (
                [&] (Handle <StoragePinPage> request, PDBCommunicatorPtr sendUsingMe) {
                        PDBLoggerPtr logger = make_shared<PDBLogger>("storagePinPage.log");
                        //NodeID nodeId = request->getNodeID();
                        DatabaseID dbId = request->getDatabaseID();
                        UserTypeID typeId = request->getUserTypeID();
                        SetID setId = request->getSetID();
                        PageID pageId = request->getPageID();
                        bool wasNewPage = request->getWasNewPage();

                        PDB_COUT << "to pin page in set with setId=" << setId << std :: endl;
                        bool res;
                        string errMsg;

                        PDBPagePtr page = nullptr;
                        SetPtr set = nullptr;

                        if((dbId == 0) && (typeId == 0)) {
                                //temp set
                                set = getFunctionality<PangeaStorageServer>().getTempSet(setId);
                        } else {
                                //user set
                                set = getFunctionality<PangeaStorageServer>().getSet(dbId, typeId, setId);
                        }

                        if (set != nullptr) {
                                if(wasNewPage == true) {
                                        page = set->addPage();
                                } else {
                                        PartitionedFilePtr file = set->getFile();
                                        PartitionedFileMetaDataPtr meta = file->getMetaData();
                                        PageIndex index = meta->getPageIndex(pageId);
                                        page = set->getPage(index.partitionId, index.pageSeqInPartition, pageId);
                                }
                        }
                        
                        if(page != nullptr) {
                                //std :: cout << "Handling StoragePinPage: page is not null, we build the StoragePagePinned message" << std :: endl;
                                logger->debug(std :: string("Handling StoragePinPage: page is not null, we build the StoragePagePinned message"));
                                const UseTemporaryAllocationBlock myBlock{2048};
                                Handle <StoragePagePinned> ack = makeObject<StoragePagePinned>();
                                ack->setMorePagesToLoad(true);
                                ack->setDatabaseID(dbId);
                                ack->setUserTypeID(typeId);
                                ack->setSetID(setId);
                                ack->setPageID(page->getPageID());
                                ack->setPageSize(page->getRawSize());
                                ack->setSharedMemOffset(page->getOffset());
                                //std :: cout << "Handling StoragePinPage: to send StoragePagePinned message" << std :: endl;
                                logger->debug(std :: string("Handling StoragePinPage: to send StoragePagePinned message"));
                                res = sendUsingMe->sendObject<StoragePagePinned>(ack, errMsg);
                                //std :: cout << "Handling StoragePinPage: sent StoragePagePinned message" << std :: endl;
                                logger->debug(std :: string("Handling StoragePinPage: sent StoragePagePinned message"));
                        }
                        else {
                                res = false;
                                errMsg = "Fatal Error: Page doesn't exist for pinning page.";
                                std :: cout << "dbId = " << dbId << ", typeId = " << typeId << ", setId = " << setId << std :: endl; 
                                std :: cout << errMsg << std :: endl;
                                logger->error(errMsg);
                                //exit(-1);
                        }
                        return make_pair(res, errMsg);
                }
        ));

        // this handler accepts a request to unpin a page
        forMe.registerHandler (StorageUnpinPage_TYPEID, make_shared <SimpleRequestHandler <StorageUnpinPage>> (
                [&] (Handle <StorageUnpinPage> request, PDBCommunicatorPtr sendUsingMe) {

                       PDBLoggerPtr logger = make_shared<PDBLogger>("storageUnpinPage.log");
                         
                       //NodeID nodeId = request->getNodeID();
                       DatabaseID dbId = request->getDatabaseID();
                       UserTypeID typeId = request->getUserTypeID();
                       SetID setId = request->getSetID();
                       PageID pageId = request->getPageID();
                       //bool wasDirty = msg->getWasDirty();
 
                       CacheKey key;
                       key.dbId = dbId;
                       key.typeId = typeId;
                       key.setId = setId;
                       key.pageId = pageId;

                       //std :: cout << "Frontend to unpin page with dbId=" << dbId << ", typeId=" << typeId << ", setId=" << setId << ", pageId=" << pageId << std :: endl;
                       logger->debug(std :: string("Frontend to unpin page with dbId=")+std :: to_string(dbId)+std :: string(", typeId=")+std :: to_string(typeId)+std :: string(", setId=")+std :: to_string(setId)+std :: string(", pageId=")+std :: to_string(pageId));

                       bool res;
                       std :: string errMsg;
                       //std :: cout << "page added to flush buffer" << std :: endl;
                       if(getFunctionality<PangeaStorageServer>().getCache()->decPageRefCount(key) == false) {
                                res = false;
                                errMsg = "Fatal Error: Page doesn't exist for unpinning page.";
                                std :: cout << "dbId=" << dbId << ", typeId=" << typeId << ", setId=" << setId << ", pageId=" << pageId << std :: endl;
                                std :: cout << errMsg << std :: endl;
                                logger->error(errMsg);
                       } else {
#ifdef ENABLE_EVICTION
                                getFunctionality<PangeaStorageServer>().getCache()->evictPage(key);
#endif
                                res = true;
                                //std :: cout << "Frontend unpinned page with dbId=" << dbId << ", typeId=" << typeId << ", setId=" << setId << ", pageId=" << pageId << std :: endl;
                       }

                       //std :: cout << "Making response object.\n";
                       logger->debug(std :: string( "Making response object.\n"));
                       const UseTemporaryAllocationBlock block{1024};
                       Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                       // return the result
                       //std :: cout << "Sending response object.\n";
                       logger->debug(std :: string("Sending response object.\n"));
                       res = sendUsingMe->sendObject (response, errMsg);
                       //std :: cout << "response sent for StorageUnpinPage.\n";
                       logger->debug(std :: string("response sent for StorageUnpinPage.\n"));

                       return make_pair(res, errMsg);

                }
        ));



        // this handler accepts a request to load all pages in a set to memory iteratively, and send back information about loaded pages
        forMe.registerHandler (StorageGetSetPages_TYPEID, make_shared <SimpleRequestHandler <StorageGetSetPages>> (
                [&] (Handle <StorageGetSetPages> request, PDBCommunicatorPtr sendUsingMe) {

                        //NodeID nodeId = request->getNodeID();
                        DatabaseID dbId = request->getDatabaseID();
                        UserTypeID typeId = request->getUserTypeID();
                        SetID setId = request->getSetID();

                        bool res = true;
                        std :: string errMsg;

                        SetPtr set = getFunctionality<PangeaStorageServer>().getSet(dbId, typeId, setId);
                        if (set == nullptr) {
                                  res = false;
                                  errMsg = "Fatal Error: Set doesn't exist.";
                                  std :: cout << errMsg << std :: endl;
                                  return make_pair (res, errMsg);
                        }                        
                        
                        //use frontend iterators: one iterator for in-memory dirty pages, and one iterator for each file partition
                        std :: vector<PageIteratorPtr> * iterators = set->getIterators();
                        getFunctionality<PangeaStorageServer>().getCache()->pin(set, MRU, Write);

                        set->setPinned(true);
                        int numIterators = iterators->size();
           
                        //std :: cout << "Buzzer is created in PDBScanWork\n";
                        PDBBuzzerPtr tempBuzzer = make_shared<PDBBuzzer>(
                           [&] (PDBAlarm myAlarm, int & counter) {
                                    counter ++;
                                    PDB_COUT << "counter = " << counter << std :: endl;
                           });
                        
                        //scan pages and load pages in a multi-threaded style
                     
                        int counter = 0;
                        for (int i = 0; i < numIterators; i++) {
                                  PDBWorkerPtr worker = getFunctionality<PangeaStorageServer>().getWorker();
                                  PDBScanWorkPtr scanWork = make_shared<PDBScanWork>(iterators->at(i), &getFunctionality<PangeaStorageServer>(), counter);
                                  worker->execute(scanWork, tempBuzzer);
                        }
                        //std :: cout << "Frontend number of iterataors:" << numIterators << std :: endl;

                        while (counter < numIterators) {
                                  tempBuzzer->wait();
                        }
                        //std :: cout << "Frontend to send NoMorePage message." << std :: endl;
                        set->setPinned(false);
                        delete iterators;
                         
                        //here, we have already loaded all pages, and sent all information about those pages to the other side, now we need inform the other side that this process has been done.
                        //The other side has closed connection, so first we need to create a separate connection to backend 
                        PDBCommunicatorPtr communicatorToBackEnd = make_shared<PDBCommunicator>();
                        if (communicatorToBackEnd->connectToLocalServer(getFunctionality<PangeaStorageServer>().getLogger(), getFunctionality<PangeaStorageServer>().getPathToBackEndServer(), errMsg)) {
                            res = false;
                            std :: cout << errMsg << std :: endl;
                            return make_pair(res, errMsg);
                        }

                        UseTemporaryAllocationBlock myBlock{1024};
                        Handle<StorageNoMorePage> noMorePage = makeObject<StorageNoMorePage>();
                        if(!communicatorToBackEnd->sendObject<StorageNoMorePage>(noMorePage, errMsg)) {
                            res = false;
                            std :: cout << errMsg << std :: endl;
                            return make_pair(res, errMsg);
                        }

                        return make_pair(res, errMsg);
                }
        ));

        // this handler accepts a request to translate <databaseName, setName> into <databaseId, typeId, setId> and forward to backend
        forMe.registerHandler (StorageTestSetScan_TYPEID, make_shared <SimpleRequestHandler <StorageTestSetScan>> (
                [&] (Handle <StorageTestSetScan> request, PDBCommunicatorPtr sendUsingMe) {

                    std :: string dbName = request->getDatabase();
                    std :: string setName = request->getSetName();
                    SetPtr set = getFunctionality<PangeaStorageServer>().getSet(std :: pair<std :: string, std::string>(dbName, setName));

                    bool res;
                    std :: string errMsg;

                    if(set == nullptr) {
                        res = false;
                        errMsg = "Fatal Error: Set doesn't exist!";
                        std :: cout << errMsg << std :: endl;
                        return make_pair(res, errMsg);
                    }
                    else {
                        //first we need to create a separate connection to backend
                        PDBCommunicatorPtr communicatorToBackend = make_shared<PDBCommunicator>();
                        if (communicatorToBackend->connectToLocalServer(getFunctionality<PangeaStorageServer>().getLogger(), getFunctionality<PangeaStorageServer>().getPathToBackEndServer(), errMsg)) {
                            res = false;
                            std :: cout << errMsg << std :: endl;
                            return make_pair(res, errMsg);
                        }

                        DatabaseID dbId = set->getDbID();
                        UserTypeID typeId = set->getTypeID();
                        SetID setId = set->getSetID();
                        
                        { 
                            const UseTemporaryAllocationBlock myBlock{1024};
                            Handle<BackendTestSetScan> msg = makeObject<BackendTestSetScan>(dbId, typeId, setId);
                            if(!communicatorToBackend->sendObject<BackendTestSetScan>(msg, errMsg)) {
                                res = false;
                                std :: cout << errMsg << std :: endl;
                                return make_pair(res, errMsg);
                            }
                        }
                        
                        {
                            const UseTemporaryAllocationBlock myBlock{communicatorToBackend->getSizeOfNextObject()};
                            communicatorToBackend->getNextObject<SimpleRequestResult>(res, errMsg);
                        }

                        //std :: cout << "Making response object.\n";
                        {
                            const UseTemporaryAllocationBlock block{1024};                      
                            Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);
                  
                             // return the result
                            //std :: cout << "Sending response object.\n";
                            res = sendUsingMe->sendObject (response, errMsg);
                        }
                        //std :: cout << "All Done!" << std :: endl;
                        return make_pair (res, errMsg);

                    } 

                }
        ));

        // this handler accepts a request to translate <<srcDatabaseName, srcSetName>, <destDatabaseName, destSetName>> into <<srcDatabaseId, srcTypeId, SrcSetId>, <destDatabaseId, destTypeId, destSetId>> and forward to backend
        forMe.registerHandler (StorageTestSetCopy_TYPEID, make_shared <SimpleRequestHandler <StorageTestSetCopy>> (
                [&] (Handle <StorageTestSetCopy> request, PDBCommunicatorPtr sendUsingMe) {

                    std :: string dbNameIn = request->getDatabaseIn();
                    std :: string setNameIn = request->getSetNameIn();
                    SetPtr setIn = getFunctionality<PangeaStorageServer>().getSet(std :: pair<std :: string, std::string>(dbNameIn, setNameIn));

                    std :: string dbNameOut = request->getDatabaseOut();
                    std :: string setNameOut = request->getSetNameOut();
                    SetPtr setOut = getFunctionality<PangeaStorageServer>().getSet(std :: pair<std :: string, std::string>(dbNameOut, setNameOut));


                    bool res;
                    std :: string errMsg;

                    if(setIn == nullptr) {
                        res = false;
                        errMsg = "Fatal Error: Input set doesn't exist!";
                        std :: cout << errMsg << std :: endl;
                        //return make_pair(res, errMsg);
                    }

                    if(setOut == nullptr) {
                        res = false;
                        errMsg += "Fatal Error: Output set doesn't exist!";
                        std :: cout << errMsg << std :: endl;
                    }


                    if ((setIn != nullptr) && (setOut != nullptr))
                    {
                        //first we need to create a separate connection to backend
                        PDBCommunicatorPtr communicatorToBackend = make_shared<PDBCommunicator>();
                        if (communicatorToBackend->connectToLocalServer(getFunctionality<PangeaStorageServer>().getLogger(), getFunctionality<PangeaStorageServer>().getPathToBackEndServer(), errMsg)) {
                            res = false;
                            std :: cout << errMsg << std :: endl;
                            return make_pair(res, errMsg);
                        }

                        DatabaseID dbIdIn = setIn->getDbID();
                        UserTypeID typeIdIn = setIn->getTypeID();
                        SetID setIdIn = setIn->getSetID();
                        DatabaseID dbIdOut = setOut->getDbID();
                        UserTypeID typeIdOut = setOut->getTypeID();
                        SetID setIdOut = setOut->getSetID();

                        
                        const UseTemporaryAllocationBlock myBlock{4096};
                        Handle<BackendTestSetCopy> msg = makeObject<BackendTestSetCopy>(dbIdIn, typeIdIn, setIdIn, dbIdOut, typeIdOut, setIdOut);
                        if(!communicatorToBackend->sendObject<BackendTestSetCopy>(msg, errMsg)) {
                                res = false;
                                std :: cout << errMsg << std :: endl;
                                //return make_pair(res, errMsg);
                        }
                        else {
                                const UseTemporaryAllocationBlock myBlock{communicatorToBackend->getSizeOfNextObject()};
                                communicatorToBackend->getNextObject<SimpleRequestResult>(res, errMsg);
                        }
                     }

                     //std :: cout << "Making response object.\n";
                     {
                         const UseTemporaryAllocationBlock block{1024};
                         Handle <SimpleRequestResult> response = makeObject <SimpleRequestResult> (res, errMsg);

                         // return the result
                         //std :: cout << "Sending response object.\n";
                         res = sendUsingMe->sendObject (response, errMsg);
                     }

                     //std :: cout << "All Done!" << std :: endl;
                     return make_pair (res, errMsg);

                }
        ));


}



//Returns ipc path to backend
string PangeaStorageServer::getPathToBackEndServer() {
        return this->pathToBackEndServer;
}

//Returns server name
string PangeaStorageServer::getServerName() {
        return this->serverName;
}

//Returns nodeId
NodeID PangeaStorageServer::getNodeId() {
        return this->nodeId;
}

//Encode database path
string PangeaStorageServer::encodeDBPath(string rootPath, DatabaseID dbId,
                string dbName) {
        char buffer[500];
        sprintf(buffer, "%s/%d_%s", rootPath.c_str(), dbId, dbName.c_str());
        return string(buffer);
}

//create temp directories
void PangeaStorageServer::createTempDirs() {
        if ((this->metaTempPath = this->conf->getMetaTempDir()).compare("") != 0) {
                boost::filesystem::remove_all(metaTempPath);
                this->conf->createDir(metaTempPath);
                PDB_COUT <<"metaTempPath:"<<metaTempPath<<"\n";
        }
        string strDataTempPaths = this->conf->getDataTempDirs();
        string curDataTempPath;
        size_t startPos = 0;
        size_t curPos;
        if ((curPos = strDataTempPaths.find(',')) == string::npos) {
                boost::filesystem::remove_all(strDataTempPaths);
                this->conf->createDir(strDataTempPaths);
                PDB_COUT << "dataTempPath:"<< strDataTempPaths <<"\n";
                dataTempPaths.push_back(strDataTempPaths);
        } else {
                while ((curPos = strDataTempPaths.find(',')) != string::npos) {
                        curDataTempPath = strDataTempPaths.substr(startPos, curPos);
                        boost::filesystem::remove_all(curDataTempPath);
                        this->conf->createDir(curDataTempPath);
                        PDB_COUT << "dataTempPath:"<<curDataTempPath<<"\n";
                        dataTempPaths.push_back(curDataTempPath);
                        strDataTempPaths = strDataTempPaths.substr(curPos + 1,
                                        strDataTempPaths.length() + 1);
                }
                boost::filesystem::remove_all(strDataTempPaths);
                this->conf->createDir(strDataTempPaths);
                PDB_COUT << "dataTempPath:"<<strDataTempPaths<<"\n";
                dataTempPaths.push_back(strDataTempPaths);
        }
}

//Create database directories
void PangeaStorageServer::createRootDirs() {

        if ((this->metaRootPath = this->conf->getMetaDir()).compare("") != 0) {
                this->conf->createDir(metaRootPath);
        }
        //cout <<"Meta root path: "<<this->metaRootPath<<"\n";
        string strDataRootPaths = this->conf->getDataDirs();
        //cout <<"Data root paths: "<<strDataRootPaths<<"\n";
        string curDataRootPath;
        size_t startPos = 0;
        size_t curPos;
        if ((curPos = strDataRootPaths.find(',')) == string::npos) {
                this->conf->createDir(strDataRootPaths);
                dataRootPaths.push_back(strDataRootPaths);
        } else {
                while ((curPos = strDataRootPaths.find(',')) != string::npos) {
                        curDataRootPath = strDataRootPaths.substr(startPos, curPos);
                        //cout<<"Cur data root path: "<<curDataRootPath<<"\n";
                        this->conf->createDir(curDataRootPath);
                        dataRootPaths.push_back(curDataRootPath);
                        strDataRootPaths = strDataRootPaths.substr(curPos + 1,
                                        strDataRootPaths.length() + 1);
                        //cout<<"Next data root path(s): "<<strDataRootPaths<<"\n";
                }
                this->conf->createDir(strDataRootPaths);
                dataRootPaths.push_back(strDataRootPaths);
        }
}


//add a new and empty database
bool PangeaStorageServer::addDatabase(string dbName, DatabaseID dbId) {
        if (this->dbs->find(dbId) != this->dbs->end()) {
                this->logger->writeLn("PDBStorage: database exists.");
                return false;
        }
        string metaDBPath;
        if (this->metaRootPath.compare("") != 0) {
                metaDBPath = encodeDBPath(this->metaRootPath, dbId, dbName);
                this->conf->createDir(metaDBPath);
        } else {
                metaDBPath = "";
        }
        vector<string> * dataDBPaths = new vector<string>();
        unsigned int i;
        string curDataDBPath;
        for (i = 0; i < this->dataRootPaths.size(); i++) {
                curDataDBPath = encodeDBPath(this->dataRootPaths.at(i), dbId, dbName);
                dataDBPaths->push_back(curDataDBPath);
        }
        DefaultDatabasePtr db = make_shared<DefaultDatabase>(this->nodeId, dbId,
                        dbName, this->conf, this->logger, this->shm, metaDBPath,
                        dataDBPaths, this->cache, this->flushBuffer);
        
        pthread_mutex_lock(&this->databaseLock);
        this->dbs->insert(pair<DatabaseID, DefaultDatabasePtr>(dbId, db));
        this->name2id->insert(pair<string, DatabaseID>(dbName, dbId));
        SequenceID * seqId = new SequenceID();
        this->usersetSeqIds->insert(pair<string, SequenceID*>(dbName, seqId));
        pthread_mutex_unlock(&this->databaseLock);
        return true;
}


//add a new and empty database using only name
bool PangeaStorageServer::addDatabase(std :: string dbName) {
     pthread_mutex_lock(&this->databaseLock);
     DatabaseID dbId = databaseSeqId.getNextSequenceID();
     pthread_mutex_unlock(&this->databaseLock);
     return this->addDatabase(dbName, dbId);
}



//clear database data and disk files for removal
void PangeaStorageServer::clearDB(DatabaseID dbId, string dbName) {
        unsigned int i;
        string path;
        path = this->encodeDBPath(this->metaRootPath, dbId, dbName);
        boost::filesystem::remove_all(path.c_str());
        for (i = 0; i < this->dataRootPaths.size(); i++) {
                path = this->encodeDBPath(this->dataRootPaths.at(i), dbId, dbName);
                boost::filesystem::remove_all(path.c_str());
        }
}


//remove database
bool PangeaStorageServer::removeDatabase(std :: string dbName) {
        DatabaseID dbId;
        if(name2id->count(dbName) != 0) {
            dbId = name2id->at(dbName);
        }
        else {
            //database doesn't exist;
            return false;
        }
        //TODO we need to delete files on disk
        map<DatabaseID, DefaultDatabasePtr>::iterator it = this->dbs->find(dbId);
        if (it != this->dbs->end()) {
                pthread_mutex_lock(&this->databaseLock);
                string dbName = it->second->getDatabaseName();
                clearDB(dbId, dbName);
                map<string, DatabaseID>::iterator name2idIt = this->name2id->find(
                                dbName);
                name2id->erase(name2idIt);
                dbs->erase(it);
                usersetSeqIds->erase(dbName);
                pthread_mutex_unlock(&this->databaseLock);
                return true;
        } else {
                this->logger->writeLn("Database doesn't exist:");
                this->logger->writeInt(dbId);
                return false;
        }
}

//return database
DefaultDatabasePtr PangeaStorageServer::getDatabase(DatabaseID dbId) {
        //this->logger->writeLn("Searching for database:");
        //this->logger->writeInt(dbId);
        map<DatabaseID, DefaultDatabasePtr>::iterator it = this->dbs->find(dbId);
        if (it != this->dbs->end()) {
                return it->second;
        }
        //this->logger->writeLn("Database doesn't exist:");
        //this->logger->writeInt(dbId);
        return nullptr;

}

//to add a new and empty type
bool PangeaStorageServer::addType(std :: string typeName, UserTypeID typeId) {
        if(this->typename2id->count(typeName) != 0) {
                //the type exists!
                return false;
        } else {

                pthread_mutex_lock (&this->typeLock);
                this->typename2id->insert(std :: pair<std :: string, UserTypeID>(typeName, typeId));
                pthread_mutex_unlock(&this->typeLock);
        }
        return true;
}


//to remove a type from a database, and also all sets in the database having that type
bool PangeaStorageServer::removeTypeFromDatabase(std :: string dbName, std :: string typeName) {
        if (this->name2id->count(dbName) == 0) {
           //database doesn't exist
           return false;
        } else {
           //database exists
           if (this->typename2id->count(typeName) == 0) {
               //type doesn't exist
               return false;
           } else {
               DatabaseID dbId = this->name2id->at(dbName);
               DefaultDatabasePtr db = this->getDatabase(dbId);
               UserTypeID typeId = this->typename2id->at(typeName);
               db->removeType(typeId);
           }
        }
        return true;
}

//to remove a type from the typeName to typeId mapping
bool PangeaStorageServer::removeType(std :: string typeName) {
        if (this->typename2id->count(typeName) == 0) {
              //the type doesn't exist
              return false;
        } else {
              pthread_mutex_lock (&this->typeLock);
              this->typename2id->erase(typeName);
              pthread_mutex_unlock (&this->typeLock);
        }
        return true;
}



//to add a new and empty set
bool PangeaStorageServer::addSet (std :: string dbName, std :: string typeName, std :: string setName, SetID setId) {
       SetPtr set = getSet(std :: pair <std :: string, std :: string>(dbName, setName));
       if (set != nullptr) {
           //set exists
           std :: cout << "Set exists with setName=" << setName << std :: endl;
           return false;
       }
       if (this->name2id->count(dbName) == 0) {
           //database doesn't exist
           std :: cout << "Database doesn't exist with dbName=" << dbName << std :: endl;
           return false;
       } else {
           //database exists
           if (this->typename2id->count(typeName) == 0) {
               //type doesn't exist
               //now we fetch the type id through catalog
               //if(standalone == true) {
                   //int typeId = getFunctionality <CatalogServer> ().searchForObjectTypeName(typeName);
                   int typeId = VTableMap :: getIDByName(typeName, false);
                   if((typeId <= 0) || (typeId == 8191)) {
                       PDB_COUT << "type doesn't  exist for name="<< typeName << ", and we store it as default type" << std :: endl;
                       typeName = "UnknownUserData";
                       this->addType(typeName, (UserTypeID)-1);
                   } else {
                       PDB_COUT << "Pangea add new type when add set: typeName="<< typeName << std :: endl;
                       PDB_COUT << "Pangea add new type when add set: typeId="<< typeId << std :: endl; 
                       this->addType(typeName, (UserTypeID)typeId);
                   }
               /*} else {
                   //in distributed mode
                   //TODO: we should work as a client to send a message to remote catalog server to figure it out
                   typeName = "UnknownUserData";
               }*/
           }
       }         
       DatabaseID dbId = this->name2id->at(dbName);
       DefaultDatabasePtr db = this->getDatabase(dbId);
       UserTypeID typeId = this->typename2id->at(typeName);
       TypePtr type = db->getType(typeId);
       if(type == nullptr) {
                    //type hasn't been added to the database, so we need to add it first for creating hierarchical directory so that optimization like compression can be applied at type and database level.
                    db->addType(typeName, typeId);
                    type = db->getType(typeId);
       } else {
             set = type->getSet(setId);
             if (set != nullptr) {
                    return false;   
             }
       } 
       type->addSet(setName, setId);
       set = type->getSet(setId);
       this->getCache()->pin(set, MRU, Write);
       PDB_COUT << "to get usersetLock" << std :: endl;
       pthread_mutex_lock(&this->usersetLock);
       this->userSets->insert(std :: pair<std :: pair<DatabaseID, SetID>, SetPtr> (std :: pair<DatabaseID, SetID>(dbId, setId), set));
       this->names2ids->insert(std :: pair<std :: pair<std :: string, std :: string>, std :: pair<DatabaseID, SetID>> (std :: pair<std :: string, std :: string> (dbName, setName), std :: pair<DatabaseID, SetID>(dbId, setId)));
       pthread_mutex_unlock(&this->usersetLock);
       PDB_COUT << "released usersetLock" << std :: endl;
       return true;
}


//to add a new and empty set using only name
bool PangeaStorageServer::addSet (std :: string dbName, std :: string typeName, std :: string setName) {
       pthread_mutex_lock(&this->usersetLock);
       if(usersetSeqIds->count(dbName) == 0) {
           //database doesn't exist
           pthread_mutex_unlock(&this->usersetLock);
           return false;
       }
       SetID setId = usersetSeqIds->at(dbName)->getNextSequenceID();
       PDB_COUT << "to add set with dbName=" << dbName << ", typeName=" << typeName << ", setName=" << setName << ", setId=" << setId << std :: endl;
       pthread_mutex_unlock(&this->usersetLock);
       return addSet (dbName, typeName, setName, setId);
}



// to add a set using only database name and set name
bool PangeaStorageServer::addSet (std :: string dbName, std :: string setName) {
      return addSet(dbName, "UnknownUserData", setName);
}

bool PangeaStorageServer :: removeSet (std :: string dbName, std :: string setName) {
     SetPtr set = getSet(std :: pair <std :: string, std :: string>(dbName, setName));
     if (set == nullptr) {
        PDB_COUT << "set with dbName=" << dbName << " and setName=" << setName << " doesn't exist" << std :: endl;
        return false;
     }
     DatabaseID dbId = set->getDbID();
     UserTypeID typeId = set->getTypeID();
     SetID setId = set->getSetID();
     DefaultDatabasePtr database = dbs->at(dbId);
     TypePtr type = database->getType(typeId);
     pthread_mutex_lock(&this->usersetLock);
     type->removeSet(setId);
     int numRemoved = userSets->erase(std :: pair <DatabaseID, SetID>(dbId, setId)); 
     PDB_COUT << "numItems removed from userSets:" << numRemoved << std :: endl;
     numRemoved = names2ids->erase(std :: pair <std :: string, std :: string> (dbName, setName));
     PDB_COUT << "numItems removed from names2ids:" << numRemoved << std :: endl;
     pthread_mutex_unlock(&this->usersetLock);
     return true;
}

//to remove an existing set
bool PangeaStorageServer:: removeSet (std :: string dbName, std :: string typeName, std :: string setName) {
       //get the type
       DatabaseID dbId;
       if(name2id->count(dbName) == 0) {
          //database doesn't exist
          return false;
       } else {
          dbId = name2id->at(dbName);
          DefaultDatabasePtr database = dbs->at(dbId);
          UserTypeID typeId;
          if(typename2id->count(typeName) == 0) {
              //type doesn't exist
              return false;
          } else {
              typeId = typename2id->at(typeName);
              TypePtr type = database->getType(typeId);
              if (type != nullptr) {
                  SetPtr set = getSet(std :: pair <std :: string, std :: string>(dbName, setName));
                  if (set != nullptr) {
                       SetID setId = set->getSetID();
                       pthread_mutex_lock(&this->usersetLock);
                       type->removeSet(setId);
                       userSets->erase(std :: pair <DatabaseID, SetID>(dbId, setId));
                       names2ids->erase(std :: pair <std :: string, std :: string> (dbName, setName));
                       
                       pthread_mutex_unlock(&this->usersetLock);
                  } else {
                       return false;
                  }
              }
          }
       }
       return true;
}



bool PangeaStorageServer::addTempSet(string setName, SetID &setId) {
        this->logger->writeLn("To add temp set with setName=");
        this->logger->writeLn(setName);
        if (this->name2tempSetId->find(setName) != this->name2tempSetId->end()) {
                cout << "TempSet exists!\n";
                this->logger->writeLn("TempSet exists for setName=");
                this->logger->writeLn(setName);
                return false;
        }
        setId = this->tempsetSeqId.getNextSequenceID();
        this->logger->writeLn("SetId=");
        this->logger->writeInt(setId);
        //cout << "setId:" << setId << "\n";
        TempSetPtr tempSet = make_shared<TempSet>(setId, setName, this->metaTempPath, this->dataTempPaths,
                        this->shm, this->cache, this->logger);
        this->getCache()->pin(tempSet, MRU, Write);
        this->logger->writeLn("temp set created!");
        //cout << "TempSet created!\n";
        pthread_mutex_lock(&this->tempsetLock);
        this->tempSets->insert(pair<SetID, TempSetPtr>(setId, tempSet));
        //cout << "Update map!\n";
        this->name2tempSetId->insert(pair<string, SetID>(setName, setId));
        pthread_mutex_unlock(&this->tempsetLock);
        return true;
}

bool PangeaStorageServer::removeTempSet(SetID setId) {
        map<SetID, TempSetPtr>::iterator it = this->tempSets->find(setId);
        if (it != this->tempSets->end()) {
                string setName = it->second->getSetName();
                it->second->clear();
                pthread_mutex_lock(&this->tempsetLock);
                this->name2tempSetId->erase(setName);
                this->tempSets->erase(it);
                pthread_mutex_unlock(&this->tempsetLock);
                return true;
        } else {
                return false;
        }
}

//returns specified temp set
TempSetPtr PangeaStorageServer::getTempSet(SetID setId) {
        this->logger->writeLn("PDBStorage: Searching for temp set:");
        this->logger->writeInt(setId);
        map<SetID, TempSetPtr>::iterator it = this->tempSets->find(setId);
        if (it != this->tempSets->end()) {
                return it->second;
        }
        this->logger->writeLn("PDBStorage: TempSet doesn't exist:");
        this->logger->writeInt(setId);
        return nullptr;
}

//returns a specified set
SetPtr PangeaStorageServer::getSet(DatabaseID dbId, UserTypeID typeId, SetID setId) {
     //cout << "to get database...\n";
     if((dbId == 0) && (typeId == 0)) {
         SetPtr set = this->getTempSet(setId);
         //this->getCache()->pin(set, MRU, Write);
         return set;
     }
     DefaultDatabasePtr db = this->getDatabase(dbId);
     if (db == nullptr) {
        this->logger->writeLn("PDBStorage: Database doesn't exist.");
        return nullptr;
     }
   
     //cout << "to get type...\n";
     TypePtr type = db->getType(typeId);
     if (type == nullptr) {
        this->logger->writeLn("PDBStorage: Type doesn't exist.");
        return nullptr;
     }

     //cout << "to get set...\n";
     SetPtr set = type->getSet(setId);
     //this->getCache()->pin(set, MRU, Write);
     return set;
}

/**
 * Start flushing main threads, which are also consumer threads,
 * to flush data in the flush buffer to disk files.
 */
void PangeaStorageServer::startFlushConsumerThreads() {
        //get the number of threads to start
        int numThreads = this->dataRootPaths.size();
        PDB_COUT<<"number of partitions:"<<numThreads<<"\n";
        int i;
        PDBFlushConsumerWorkPtr flusher;
        PDBWorkerPtr worker;
        for (i = 0; i < numThreads; i++) {
                //create a flush worker
                flusher = make_shared<PDBFlushConsumerWork>(i, this);
                flushers.push_back(flusher);
                //find a thread in thread pool, if we can not find a thread, we block.
                while ((worker = this->getWorker()) == nullptr) {
                        sched_yield();
                }
                worker->execute(flusher, flusher->getLinkedBuzzer());
                PDB_COUT<<"flushing thread started for partition: "<<i<<"\n";
        }

}

/**
 * Stop flushing main threads, and close flushBuffer.
 */
void PangeaStorageServer::stopFlushConsumerThreads() {

        unsigned int i;
        for(i=0; i<this->flushers.size(); i++) {
                dynamic_pointer_cast<PDBFlushConsumerWork>(flushers.at(i))->stop();
        }
        this->flushBuffer->close();
}

/**
 * returns a worker from thread pool
 */
PDBWorkerPtr PangeaStorageServer::getWorker() {
        return this->workers->getWorker();
}

/**
 * returns the flush buffer
 */
PageCircularBufferPtr PangeaStorageServer::getFlushBuffer() {
        return this->flushBuffer;
}

using namespace boost::filesystem;

/**
 * Initialize databases in the storage from data already exists at configured root paths.
 * Return true if successful;
 * Return false, if data persisted on disk is not consistent.
 */
bool PangeaStorageServer::initializeFromRootDirs(string metaRootPath,
                vector<string> dataRootPath) {
        FileType curFileType;
        path root;
        if (metaRootPath.compare("") == 0) {
                //This is a SequenceFile instance
                curFileType = FileType::SequenceFileType;
                //Then there is only one root directory,
                //and we only check dataRootPath.at(0), all other data paths will be ignored!
                root = path(dataRootPath.at(0));
        } else {
                //This is a PartitionedFile instance
                curFileType = FileType::PartitionedFileType;
                //Then there is only one root directory,
                //and we only check dataRootPath.at(0), all other data paths will be ignored!
                root = path(metaRootPath);
        }
        if (exists(root)) {
                if (is_directory(root)) {
                        vector<path> dbDirs;
                        copy(directory_iterator(root), directory_iterator(),
                                        back_inserter(dbDirs));
                        vector<path>::iterator iter;
                        std::string path;
                        std::string dirName;
                        std::string name;
                        std::string strId;
                        DatabaseID dbId;
                        DatabaseID maxDbId = 0;
                        for (iter = dbDirs.begin(); iter != dbDirs.end(); iter++) {
                                if (is_directory(*iter)) {
                                        //find a database
                                        path = std::string(iter->c_str());

                                        //get the directory name
                                        dirName = path.substr(path.find_last_of('/') + 1,
                                                        path.length() - 1);

                                        //parse database id from directory name
                                        strId = dirName.substr(0, dirName.find('_'));
                                        dbId = stoul(strId);
                                        if (maxDbId < dbId) {
                                             maxDbId = dbId;
                                        }
                                        //parse database name from directory name
                                        name = dirName.substr(dirName.find('_') + 1,
                                                        dirName.length() - 1);



                                        //cout << "Storage Server: detect database at path: " << path << "\n";
                                        //cout << "Database name: " << name << "\n";
                                        //cout << "Database ID:" << dbId << "\n";

                                        //initialize the database instance based on existing data stored in this directory.
                                        if(curFileType == FileType::SequenceFileType) {
                                                this->addDatabaseBySequenceFiles(name, dbId, path);
                                        } else {
                                                this->addDatabaseByPartitionedFiles(name, dbId, path);
                                        }
                                        this->databaseSeqId.initialize(maxDbId);
                                } else {
                                        //Meet a problem when trying to recover database instance from existing data.
                                        //Because database directory doesn't exist.
                                        return false;
                                }
                        }
                } else {
                        //we can't recover database instances from existing data, because root directory doesn't exist.
                        return false;
                }
        } else {
                //we can't recover database instances from existing data, because root directory doesn't exist.
                return false;
        }
        
        return true;
}


//add database based on sequence file
void PangeaStorageServer::addDatabaseBySequenceFiles(string dbName, DatabaseID dbId,
                path dbPath) {
        if (this->dbs->find(dbId) != this->dbs->end()) {
                this->logger->writeLn("PDBStorage: database exists.");
                return;
        }
        //create a database instance
        vector<string> * dataDBPaths = new vector<string>();
        dataDBPaths->push_back(std::string(dbPath.c_str()));
        DefaultDatabasePtr db = make_shared<DefaultDatabase>(this->nodeId, dbId,
                        dbName, this->conf, this->logger, this->shm, "", dataDBPaths,
                        this->cache, this->flushBuffer);
        if (db == nullptr) {
                this->logger->writeLn("PDBStorage: Out of Memory.");
                std :: cout << "FATAL ERROR: PDBStorage Out of Memory" << std :: endl;
                exit(1);
        }
        //initialize it
        db->initializeFromDBDir(dbPath);
        //add it to map
        pthread_mutex_lock(&this->databaseLock);
        this->dbs->insert(pair<DatabaseID, DefaultDatabasePtr>(dbId, db));
        this->name2id->insert(pair<string, DatabaseID>(dbName, dbId));
        pthread_mutex_unlock(&this->databaseLock);
}

/**
 * Add an existing database based on Partitioned file
 */
void PangeaStorageServer::addDatabaseByPartitionedFiles(string dbName, DatabaseID dbId,
                path metaDBPath) {
        if (this->dbs->find(dbId) != this->dbs->end()) {
                this->logger->writeLn("PDBStorage: database exists.");
                return;
        }
        //create a database instance
        vector<string> * dataDBPaths = new vector<string>();
        string dataDBPath;
        unsigned int i;
        for (i = 0; i < dataRootPaths.size(); i++) {
                dataDBPath = this->encodeDBPath(this->dataRootPaths.at(i), dbId,
                                dbName);
                dataDBPaths->push_back(dataDBPath);
        }
        DefaultDatabasePtr db = make_shared<DefaultDatabase>(this->nodeId, dbId,
                        dbName, this->conf, this->logger, this->shm, string(metaDBPath.c_str()), dataDBPaths,
                        this->cache, this->flushBuffer);
        if (db == nullptr) {
                this->logger->writeLn("PDBStorage: Out of Memory.");
                std :: cout << "FATAL ERROR: PDBStorage Out of Memory" << std :: endl;
                exit(-1);
        }
        //initialize it
        db->initializeFromMetaDBDir(metaDBPath);
        //add it to map
        pthread_mutex_lock(&this->databaseLock);
        this->dbs->insert(pair<DatabaseID, DefaultDatabasePtr>(dbId, db));
        this->name2id->insert(pair<string, DatabaseID>(dbName, dbId));
        pthread_mutex_unlock(&this->databaseLock);

        std :: map<UserTypeID, TypePtr> * types = db->getTypes();
        std :: map<UserTypeID, TypePtr>::iterator typeIter;

        //to update the sequence generator
        SetID maxSetId = 0;
        for (typeIter = types->begin(); typeIter != types->end(); typeIter++) {
                UserTypeID typeId = typeIter->first;
                TypePtr type = typeIter->second;
                std :: string typeName = type->getName();
                pthread_mutex_lock(&this->typeLock);
                this->typename2id->insert(std :: pair<std :: string, UserTypeID>(typeName, typeId));
                pthread_mutex_unlock(&this->typeLock);
                std :: map<SetID, SetPtr> * sets = type->getSets();
                std :: map<SetID, SetPtr>::iterator setIter;
                for (setIter = sets->begin(); setIter != sets->end(); setIter++) {
                        SetID setId = setIter->first;
                        if (maxSetId <=  setId) {
                            maxSetId =  setId + 1;
                        }
                        SetPtr set = setIter->second;
                        PDB_COUT << "Loaded existing set with database: "<<dbName<<", type: "<<typeName<<", set: "<<set->getSetName()<<std::endl;
                        pthread_mutex_lock(&this->usersetLock);
                        this->userSets->insert(
                                std :: pair<std :: pair <DatabaseID, SetID>,  SetPtr>(
                                         std :: pair<DatabaseID, SetID>(dbId, setId), set));
                        this->names2ids->insert(
                                std :: pair<std :: pair <std :: string, std :: string>, std :: pair <DatabaseID, SetID>>(
                                         std :: pair <std :: string, std :: string> (dbName, set->getSetName()),
                                         std :: pair <DatabaseID, SetID> (dbId, setId)));
                        pthread_mutex_unlock(&this->usersetLock);
                }
        }
        SequenceID * seqId = new SequenceID();
        seqId->initialize(maxSetId);
        this->usersetSeqIds->insert(std :: pair < std :: string, SequenceID *> (dbName, seqId));
        
}

PDBLoggerPtr PangeaStorageServer::getLogger() {
        return this->logger;
}

ConfigurationPtr PangeaStorageServer::getConf() {
        return this->conf;
}

SharedMemPtr PangeaStorageServer::getSharedMem() {
        return this->shm;
}

PageCachePtr PangeaStorageServer::getCache() {
        return this->cache;
}

//return whether the PangeaStorageServer instance is running standalone or in cluster mode.
bool PangeaStorageServer::isStandalone() {
        return this->standalone;
}

}


#endif
