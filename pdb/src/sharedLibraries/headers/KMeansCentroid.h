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
#ifndef K_MEANS_CENTROID_H
#define K_MEANS_CENTROID_H

#include "Object.h"
#include "Handle.h"
#include "KMeansDoubleVector.h"

/* 
 * This class stores the centroid of a cluster, which contains the number 
 * of data points in this cluster, and the mean of these data points
 */
using namespace pdb;

class KMeansCentroid : public Object {

private:
    KMeansDoubleVector mean;
    size_t count;

public:
    ENABLE_DEEP_COPY

    KMeansCentroid() {}

    KMeansCentroid(size_t count, KMeansDoubleVector value) {
        this->count = count;
        this->mean = value;
    }

    ~KMeansCentroid() {}

    size_t getCount() {
        return this->count;
    }

    KMeansDoubleVector& getMean() {
        return this->mean;
    }

    KMeansCentroid& operator+(KMeansCentroid& other) {
        this->count += other.getCount();
        this->mean = this->mean + other.getMean();
        return *this;
    }
};

#endif
