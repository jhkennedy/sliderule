/*
 * Licensed to the University of Washington under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The University of Washington
 * licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef __h5_lib__
#define __h5_lib__

/******************************************************************************
 * INCLUDES
 ******************************************************************************/

#include "RecordObject.h"

/******************************************************************************
 * HDF5 I/O CLASS
 ******************************************************************************/

struct H5Lib
{
    static const int MAX_NDIMS = 8;

    static void init(void);
    static void deinit(void);

    static int read (const char* url, const char* datasetname, int col, size_t datatypesize, uint8_t** data);
    static int readAs (const char* url, const char* datasetname, RecordObject::valType_t valtype, uint8_t** data);
    static bool traverse (const char* url, int max_depth, const char* start_group);
};

#endif  /* __h5_lib__ */