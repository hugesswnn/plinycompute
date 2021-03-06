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
#ifndef AGGREGATION_PROCESSOR_H
#define AGGREGATION_PROCESSOR_H


#include "UseTemporaryAllocationBlock.h"
#include "InterfaceFunctions.h"
#include "AggregationMap.h"
#include "PDBMap.h"
#include "PDBVector.h"
#include "Handle.h"
#include "SimpleSingleTableQueryProcessor.h"

namespace pdb {

template <class KeyType, class ValueType>
class AggregationProcessor : public SimpleSingleTableQueryProcessor {

public:
    ~AggregationProcessor(){};
    AggregationProcessor(){};
    AggregationProcessor(HashPartitionID id);
    void initialize() override;
    void loadInputPage(void* pageToProcess) override;
    void loadInputObject(Handle<Object>& objectToProcess) override;
    void loadOutputPage(void* pageToWriteTo, size_t numBytesInPage) override;
    bool fillNextOutputPage() override;
    void finalize() override;
    void clearOutputPage() override;
    void clearInputPage() override;
    bool needsProcessInput() override;

private:
    UseTemporaryAllocationBlockPtr blockPtr;
    Handle<Vector<Handle<AggregationMap<KeyType, ValueType>>>> inputData;
    Handle<Map<KeyType, ValueType>> outputData;
    bool finalized;
    Handle<AggregationMap<KeyType, ValueType>> curMap;
    int id;

    // the iterators for current map partition
    PDBMapIterator<KeyType, ValueType>* begin;
    PDBMapIterator<KeyType, ValueType>* end;

    int count;
};
}


#include "AggregationProcessor.cc"


#endif
