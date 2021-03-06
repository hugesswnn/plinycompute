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
#ifndef ABSTRACT_JOBSTAGE_H
#define ABSTRACT_JOBSTAGE_H

#include <DataTypes.h>
#include <PDBString.h>

namespace pdb {

//this class encapsulates the common interface for JobStages
class AbstractJobStage : public Object {

public:
    void setJobId(std::string jobId) {
        this->jobId = jobId;
    }

    std::string getJobId() {
        return this->jobId;
    }


    virtual int16_t getJobStageTypeID() = 0;
    virtual std::string getJobStageType() = 0;
    virtual JobStageID getStageId() = 0;
    virtual void print() = 0;

protected:
    String jobId;
};
}

#endif
