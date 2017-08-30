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
#ifndef DOUBLE_VECTOR_H
#define DOUBLE_VECTOR_H

//by Jia, May 2017

#include "Object.h"
#include "Handle.h"
#include "PDBVector.h"
#include "Configuration.h"
#include <math.h>
//PRELOAD %DoubleVector%


#ifndef KMEANS_EPSILON
   #define KMEANS_EPSILON 0.01
#endif

namespace pdb {

class DoubleVector: public Object {

public:
     Handle<Vector<double>> data = nullptr;
     size_t size = 0;
     double norm = -1;
public:

     DoubleVector () { size = 0; }

     DoubleVector ( size_t size ) {
         this->size = size;
         data = makeObject<Vector<double>> ( size, size );
     }

     ~DoubleVector () {
     }

     void setValues ( std :: vector <double> dataToMe) {
         double *  rawData = data->c_ptr();
         if (dataToMe.size() >= size) {
             for (int i = 0; i < size; i++) {
                rawData[i] = dataToMe[i];
             }
         } else {
             std :: cout << "my size is " << size << ", and input's size is " << dataToMe.size()<<std :: endl;
         }
         this->print();
     }

     size_t getSize () {
         return this->size;
     }

     Handle<Vector<double>> & getData () {
         return data;
     }

     double * getRawData () {
         if (data == nullptr) {
             return nullptr;
         }
         return data->c_ptr();
     }

     double getDouble (int i) {
         if (i < this->size) {
            return (*data)[i];
         } else {
            return -1;
         }
     }

     void setDouble (int i, double val) {
         if (i < this->size) {
             (*data)[i] = val;
         } else {
            std :: cout << "Cannot assign the value " << val << "to the pos " << i << std :: endl;;
         }
     }

     //following implementation of Spark MLLib
     //https://github.com/apache/spark/blob/master/mllib/src/main/scala/org/apache/spark/mllib/linalg/Vectors.scala
     double getNorm2() {
         double * rawData = data->c_ptr();
         if (norm < 0 ) {
             size_t mySize = this->getSize();
             for ( int i = 0; i < mySize; i++) {
                 norm += rawData[i] * rawData[i];
             }
             norm = sqrt(norm);
         }
         return norm;
     }
     
     double dot (DoubleVector &other) {
          size_t mySize = this->getSize();
          size_t otherSize = other.getSize();
          double * rawData = data->c_ptr();
          double * otherRawData = other.getRawData();
          if (mySize != otherSize) {
              std :: cout << "Error in DoubleVector: dot size doesn't match" << std :: endl;
              exit(-1);
          }
          else {
               double dotSum = 0;
               for (int i = 0; i < mySize; i++) {
                   dotSum += rawData[i] * otherRawData[i];
               }
               return dotSum;
          }
     }

     //to get squared distance following SparkMLLib
    
     double getSquaredDistance (DoubleVector &other) {
          size_t mySize = this->getSize();
          size_t otherSize = other.getSize();
          double * rawData = data->c_ptr();
          double * otherRawData = other.getRawData();
          if (mySize != otherSize) {
              std :: cout << "Error in DoubleVector: dot size doesn't match" << std :: endl;
              exit(-1);
          }
          double distance = 0;
          size_t kv = 0;
          while (kv < mySize) {
               double score = rawData[kv] - otherRawData[kv];
               distance += score * score;
               kv ++;
          }
          return distance;

     }


     void print() {
         double * rawData = data->c_ptr();
         for (int i = 0; i < this->getSize(); i++) {
             std :: cout << i << ": " << rawData[i] << "; ";
         }
         std :: cout << std :: endl;
     }



     //this implementation is following Spark MLLib
     //https://github.com/apache/spark/blob/master/mllib/src/main/scala/org/apache/spark/mllib/util/MLUtils.scala
     double getFastSquaredDistance (DoubleVector &other) {
         double precision = 0.000001;
         double myNorm = norm;
         double otherNorm = other.getNorm2();
         if (norm < 0) {
             myNorm = getNorm2();
         }
         double sumSquaredNorm = myNorm * myNorm + otherNorm * otherNorm;
         double normDiff = myNorm - otherNorm;
         double sqDist = 0.0;
         double precisionBound1 = 2.0 * KMEANS_EPSILON * sumSquaredNorm / (normDiff * normDiff + KMEANS_EPSILON);
         if (precisionBound1 < precision) {
             sqDist = sumSquaredNorm - 2.0 * dot (other);
         } else {
             sqDist = getSquaredDistance(other);
         }
         return sqDist;
     }


     DoubleVector& operator + (DoubleVector &other) {
         //std :: cout << "me:" << this->getSize() << std :: endl;
         //this->print();
         //std :: cout << "other:" << other.getSize() << std :: endl;
         //other.print();
         size_t mySize = this->getSize();
         size_t otherSize = other.getSize();
         if (mySize != otherSize) {
              std :: cout << "Error in DoubleVector: dot size doesn't match" << std :: endl;
              exit(-1);
         } 
         double * rawData = data->c_ptr();
         double * otherRawData = other.getRawData();
         for (int i = 0; i < mySize; i++) {
              rawData[i] += otherRawData[i];
         }
         return *this;
         
     }

     
     DoubleVector& operator / (int val) {
         if (val == 0) {
             std :: cout << "Error in DoubleVector: division by zero" << std :: endl;
             exit(-1);
         }	 
         size_t mySize = this->getSize();
         Handle<DoubleVector> result = makeObject<DoubleVector>(mySize);
         double * rawData = data->c_ptr();
         double * otherRawData = result->getRawData();
     	 for (int i = 0; i < mySize; i++) {
		otherRawData[i] = rawData[i] / val;
     	 }

         return *result;
         
     }




     /*
     DoubleVector operator + (DoubleVector other) {
	 
	 if (this->getSize() != other.getSize()) {
		std :: cout << "Can not add two DoubleVector with different sizes!" << std :: endl;
		return *this;
	 }

	 DoubleVector result = DoubleVector(this->getSize());
     	 for (int i = 0; i < this->getSize(); i++) {
		result.setDouble(i, (*data)[i] + other.getDouble(i));
     	 }

         return result;
         
     }
     */


     ENABLE_DEEP_COPY

     


};

}



#endif
