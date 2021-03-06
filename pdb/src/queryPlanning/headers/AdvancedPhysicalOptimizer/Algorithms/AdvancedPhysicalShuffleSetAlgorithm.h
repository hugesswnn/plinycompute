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

#ifndef PDB_ADVANCEDPHYSICALSHUFFLESETALGORITHM_H
#define PDB_ADVANCEDPHYSICALSHUFFLESETALGORITHM_H

#include <AdvancedPhysicalOptimizer/AdvancedPhysicalAbstractAlgorithm.h>

namespace pdb {

class AdvancedPhysicalShuffleSetAlgorithm : public AdvancedPhysicalAbstractAlgorithm {

public:

  AdvancedPhysicalShuffleSetAlgorithm(const AdvancedPhysicalPipelineNodePtr &handle,
                                      const std::string &jobID,
                                      bool isProbing,
                                      bool isOutput,
                                      const Handle<SetIdentifier> &source,
                                      const Handle<ComputePlan> &computePlan,
                                      const LogicalPlanPtr &logicalPlan,
                                      const ConfigurationPtr &conf);

  PhysicalOptimizerResultPtr generate(int nextStageID, const StatisticsPtr &stats) override;

  AdvancedPhysicalAbstractAlgorithmTypeID getType() override;

};

}
#endif //PDB_ADVANCEDPHYSICALSHUFFLESETALGORITHM_H
