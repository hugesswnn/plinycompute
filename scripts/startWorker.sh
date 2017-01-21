#  Copyright 2018 Rice University                                           
#                                                                           
#  Licensed under the Apache License, Version 2.0 (the "License");          
#  you may not use this file except in compliance with the License.         
#  You may obtain a copy of the License at                                  
#                                                                           
#      http://www.apache.org/licenses/LICENSE-2.0                           
#                                                                           
#  Unless required by applicable law or agreed to in writing, software      
#  distributed under the License is distributed on an "AS IS" BASIS,        
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
#  See the License for the specific language governing permissions and      
#  limitations under the License.                                           
#  ======================================================================== 
#!/bin/bash

numThreads=$1
sharedMemSize=$2
master_ip=$3
ip_addr=$4

echo -e "+++++++++++ start server: $ip_addr"
echo -e "+++++++++++ to kill existing processes"
pkill -9 test603
pkill -9 test404
pkill -9 pdb-server
pkill -9 pdb-cluster 

echo -e "+++++++++++ to start a pdb worker"
echo "bin/pdb-server $numThreads $sharedMemSize $master_ip $ip_addr &"
if [ -n "${PDB_SSH_FOREGROUND}" ]; then
   bin/pdb-server $numThreads $sharedMemSize $master_ip $ip_addr
else
   nohup bin/pdb-server $numThreads $sharedMemSize $master_ip $ip_addr  >> log.out 2>&1 < /dev/null &
fi
