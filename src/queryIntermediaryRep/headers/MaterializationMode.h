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
//
// Created by barnett on 9/27/16.
//

#ifndef PDB_QUERYINTERMEDIARYREP_MATERIALIZATIONMODE_H
#define PDB_QUERYINTERMEDIARYREP_MATERIALIZATIONMODE_H

#include <string>

using std::string;

namespace pdb_detail
{
    class MaterializationMode
    {
        virtual bool isNone() = 0;
    };

    class MaterializationModeNone : public MaterializationMode
    {
        bool isNone() override;

    };

    class MaterializationModeNamedSet : public MaterializationMode
    {

    public:

        MaterializationModeNamedSet(string databaseName, string setName);

        bool isNone() override;

        string getDatabaseName();

        string getSetName();

    private:

        string _databaseName;

        string _setName;

    };
}

#endif //PDB_QUERYINTERMEDIARYREP_MATERIALIZATIONMODE_H
