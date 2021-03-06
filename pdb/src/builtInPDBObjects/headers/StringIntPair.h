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

#ifndef STRING_INT_PAIR_H
#define STRING_INT_PAIR_H

#include "Object.h"
#include "PDBString.h"
#include "Handle.h"

//  PRELOAD %StringIntPair%

namespace pdb {

class StringIntPair : public Object {

public:
    Handle<String> myString;
    int myInt;

    ENABLE_DEEP_COPY

    ~StringIntPair() {}
    StringIntPair() {}

    StringIntPair(std::string fromMe, int meTo) {
        myString = makeObject<String>(fromMe);
        myInt = meTo;
    }

    int getInt() {
        return myInt;
    }

    int getSimpleInt() {
        return myInt + myString->size();
    }

    Handle<String> getString() {
        return myString;
    }

    Handle<String> getSimpleString() {
        return makeObject<String>(std::string("Simple") + std::to_string(getSimpleInt()));
    }


    StringIntPair getSimplePair() {
        return StringIntPair(std::string("Simple") + std::to_string(getSimpleInt()),
                             myInt + myString->size());
    }

    bool operator==(const StringIntPair& toMe) {
        return ((myString == toMe.myString) && (myInt == toMe.myInt));
    }

    size_t hash() const {
        return hashMe(myString->c_str(), myString->size() - 1);
    }
};
}

#endif
