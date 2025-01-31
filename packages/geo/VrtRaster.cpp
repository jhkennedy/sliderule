/*
 * Copyright (c) 2021, University of Washington
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the University of Washington nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY OF WASHINGTON AND CONTRIBUTORS
 * “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE UNIVERSITY OF WASHINGTON OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/******************************************************************************
 * INCLUDES
 ******************************************************************************/

#include "core.h"
#include "VrtRaster.h"
#include "TimeLib.h"

#include <uuid/uuid.h>
#include <ogr_geometry.h>
#include <ogrsf_frmts.h>
#include <gdal.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>
#include <gdal_priv.h>

#include "cpl_minixml.h"
#include "cpl_string.h"
#include "gdal.h"
#include "ogr_spatialref.h"

/******************************************************************************
 * STATIC DATA
 ******************************************************************************/

const char* VrtRaster::OBJECT_TYPE = "VrtRaster";
const char* VrtRaster::LuaMetaName = "VrtRaster";
const struct luaL_Reg VrtRaster::LuaMetaTable[] = {
    {"dim",         luaDimensions},
    {"bbox",        luaBoundingBox},
    {"cell",        luaCellSize},
    {"sample",      luaSamples},
    {NULL,          NULL}
};

Mutex VrtRaster::factoryMut;
Dictionary<VrtRaster::factory_t> VrtRaster::factories;

/******************************************************************************
 * PUBLIC METHODS
 ******************************************************************************/

/*----------------------------------------------------------------------------
 * init
 *----------------------------------------------------------------------------*/
void VrtRaster::init( void )
{
}

/*----------------------------------------------------------------------------
 * deinit
 *----------------------------------------------------------------------------*/
void VrtRaster::deinit( void )
{
}

/*----------------------------------------------------------------------------
 * luaCreate
 *----------------------------------------------------------------------------*/
int VrtRaster::luaCreate( lua_State* L )
{
    try
    {
        /* Get Parameters */
        const char* raster_name     = getLuaString(L, 1);
        const char* dem_sampling    = getLuaString(L, 2, true, "NearestNeighbour");
        const int   sampling_radius = getLuaInteger(L, 3, true, 1);

        /* Get Factory */
        factory_t _create = NULL;
        factoryMut.lock();
        {
            factories.find(raster_name, &_create);
        }
        factoryMut.unlock();

        /* Check Factory */
        if(_create == NULL) throw RunTimeException(CRITICAL, RTE_ERROR, "Failed to find registered raster for %s", raster_name);

        /* Create Raster */
        VrtRaster* _raster = _create(L, dem_sampling, sampling_radius);
        if(_raster == NULL) throw RunTimeException(CRITICAL, RTE_ERROR, "Failed to create raster of type: %s", raster_name);

        /* Return Object */
        return createLuaObject(L, _raster);
    }
    catch(const RunTimeException& e)
    {
        mlog(e.level(), "Error creating %s: %s", LuaMetaName, e.what());
        return returnLuaStatus(L, false);
    }
}

/*----------------------------------------------------------------------------
 * registerDriver
 *----------------------------------------------------------------------------*/
bool VrtRaster::registerRaster (const char* _name, factory_t create)
{
    bool status;

    factoryMut.lock();
    {
        status = factories.add(_name, create);
    }
    factoryMut.unlock();

    return status;
}

/*----------------------------------------------------------------------------
 * sample
 *----------------------------------------------------------------------------*/
int VrtRaster::sample (double lon, double lat, List<sample_t> &slist, void* param)
{
    (void)param;  /* Keep compiler happy, param not used for now */
    slist.clear();

    samplingMutex.lock(); /* Serialize sampling on the same object */

    try
    {
        /* Get samples */
        if (sample(lon, lat) > 0)
        {
            raster_t *raster = NULL;
            const char *key = rasterDict.first(&raster);
            while (key != NULL)
            {
                assert(raster);
                if (raster->enabled && raster->sampled)
                {
                    slist.add(raster->sample);
                }
                key = rasterDict.next(&raster);
            }
        }
    }
    catch (const RunTimeException &e)
    {
        mlog(e.level(), "Error getting samples: %s", e.what());
    }

    samplingMutex.unlock();

    return slist.length();
}


/*----------------------------------------------------------------------------
 * Destructor
 *----------------------------------------------------------------------------*/
VrtRaster::~VrtRaster(void)
{
    /* Terminate all reader threads */
    for (uint32_t i=0; i < readerCount; i++)
    {
        reader_t *reader = &rasterRreader[i];
        if (reader->thread != NULL)
        {
            reader->sync->lock();
            {
                reader->raster = NULL; /* No raster to read     */
                reader->run = false;   /* Set run flag to false */
                reader->sync->signal(0, Cond::NOTIFY_ONE);
            }
            reader->sync->unlock();

            delete reader->thread;  /* delte thread waits on thread to join */
            delete reader->sync;
        }
    }

    delete [] rasterRreader;

    /* Close all rasters */
    if (vrt.dset) GDALClose((GDALDatasetH)vrt.dset);

    raster_t* raster = NULL;
    const char* key  = rasterDict.first(&raster);
    while (key != NULL)
    {
        assert(raster);
        if (raster->dset) GDALClose((GDALDatasetH)raster->dset);
        delete raster;
        raster = NULL;
        key = rasterDict.next(&raster);
    }

    if (vrt.transf) OGRCoordinateTransformation::DestroyCT(vrt.transf);

    if (tifList) delete tifList;
}

/*----------------------------------------------------------------------------
 * sample
 *----------------------------------------------------------------------------*/
int VrtRaster::sample(double lon, double lat)
{
    invalidateRastersCache();

    /* Initial call, open vrt file if not already opened */
    if (vrt.dset == NULL)
    {
        if (!openVrtDset(lon, lat))
            throw RunTimeException(CRITICAL, RTE_ERROR, "Could not open VRT file for point lon: %lf, lat: %lf", lon, lat);
    }

    OGRPoint p = {lon, lat};
    if (p.transform(vrt.transf) != OGRERR_NONE)
        throw RunTimeException(CRITICAL, RTE_ERROR, "transform failed for point: lon: %lf, lat: %lf", lon, lat);

    /* If point is not in current scene VRT dataset, open new scene */
    if (!vrtContainsPoint(p))
    {
        if (!openVrtDset(lon, lat))
            throw RunTimeException(CRITICAL, RTE_ERROR, "Could not open VRT file for point lon: %lf, lat: %lf", lon, lat);
    }

    bool findNewTifFiles = true;

    if( checkCacheFirst )
    {
        raster_t *raster = NULL;
        if (findCachedRasterWithPoint(p, &raster))
        {
            raster->enabled = true;
            raster->point = p;

            /* Found raster with point in cache, no need to look for new tif file */
            findNewTifFiles = false;
        }
    }

    if (findNewTifFiles && findTIFfilesWithPoint(p))
    {
        updateRastersCache(p);
    }

    sampleRasters();

    return getSampledRastersCount();
}

/******************************************************************************
 * PROTECTED METHODS
 ******************************************************************************/

/*----------------------------------------------------------------------------
 * Constructor
 *----------------------------------------------------------------------------*/
VrtRaster::VrtRaster(lua_State *L, const char *dem_sampling, const int sampling_radius):
    LuaObject(L, OBJECT_TYPE, LuaMetaName, LuaMetaTable)
{
    CHECKPTR(dem_sampling);

    if      (!strcasecmp(dem_sampling, "NearestNeighbour")) sampleAlg = GRIORA_NearestNeighbour;
    else if (!strcasecmp(dem_sampling, "Bilinear"))         sampleAlg = GRIORA_Bilinear;
    else if (!strcasecmp(dem_sampling, "Cubic"))            sampleAlg = GRIORA_Cubic;
    else if (!strcasecmp(dem_sampling, "CubicSpline"))      sampleAlg = GRIORA_CubicSpline;
    else if (!strcasecmp(dem_sampling, "Lanczos"))          sampleAlg = GRIORA_Lanczos;
    else if (!strcasecmp(dem_sampling, "Average"))          sampleAlg = GRIORA_Average;
    else if (!strcasecmp(dem_sampling, "Mode"))             sampleAlg = GRIORA_Mode;
    else if (!strcasecmp(dem_sampling, "Gauss"))            sampleAlg = GRIORA_Gauss;
    else
        throw RunTimeException(CRITICAL, RTE_ERROR, "Invalid sampling algorithm: %s:", dem_sampling);

    if (sampling_radius >= 0)
        radius = sampling_radius;
    else throw RunTimeException(CRITICAL, RTE_ERROR, "Invalid sampling radius: %d:", sampling_radius);

    /* Initialize Class Data Members */
    clearVrt( &vrt);

    tifList = new List<std::string>;
    tifList->clear();
    rasterDict.clear();
    rasterRreader = new reader_t[MAX_READER_THREADS];
    bzero(rasterRreader, sizeof(reader_t)*MAX_READER_THREADS);
    readerCount = 0;
    checkCacheFirst = false;
}

/*----------------------------------------------------------------------------
 * openVrtDset
 *----------------------------------------------------------------------------*/
bool VrtRaster::openVrtDset(double lon, double lat)
{
    bool objCreated = false;
    std::string newVrtFile;

    getVrtFileName(newVrtFile, lon, lat);

    /* Is it already open vrt? */
    if (vrt.dset != NULL && vrt.fileName == newVrtFile)
        return true;

    try
    {
        /* Cleanup previous vrtDset */
        if (vrt.dset != NULL)
        {
            GDALClose((GDALDatasetH)vrt.dset);
            vrt.dset = NULL;
        }

        /* Open new vrtDset */
        vrt.dset = (VRTDataset *)GDALOpenEx(newVrtFile.c_str(), GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR, NULL, NULL, NULL);
        if (vrt.dset == NULL)
            throw RunTimeException(CRITICAL, RTE_ERROR, "Failed to open VRT file: %s:", newVrtFile.c_str());


        vrt.fileName = newVrtFile;
        vrt.band = vrt.dset->GetRasterBand(1);
        CHECKPTR(vrt.band);

        /* Get inverted geo transfer for vrt */
        double geot[6] = {0};
        CPLErr err = GDALGetGeoTransform(vrt.dset, geot);
        CHECK_GDALERR(err);
        if (!GDALInvGeoTransform(geot, vrt.invGeot))
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot invert geotransform");
            CHECK_GDALERR(CE_Failure);
        }

        /* Store information about vrt raster */
        vrt.cols = vrt.dset->GetRasterXSize();
        vrt.rows = vrt.dset->GetRasterYSize();

        /* Get raster boundry box */
        bzero(geot, sizeof(geot));
        err = vrt.dset->GetGeoTransform(geot);
        CHECK_GDALERR(err);
        vrt.bbox.lon_min = geot[0];
        vrt.bbox.lon_max = geot[0] + vrt.cols * geot[1];
        vrt.bbox.lat_max = geot[3];
        vrt.bbox.lat_min = geot[3] + vrt.rows * geot[5];

        vrt.cellSize = geot[1];

        OGRErr ogrerr = vrt.srcSrs.importFromEPSG(PHOTON_CRS);
        CHECK_GDALERR(ogrerr);
        const char *projref = vrt.dset->GetProjectionRef();
        CHECKPTR(projref);
        mlog(DEBUG, "%s", projref);
        ogrerr = vrt.trgSrs.importFromProj4(projref);
        CHECK_GDALERR(ogrerr);

        /* Force traditional axis order to avoid lat,lon and lon,lat API madness */
        vrt.trgSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        vrt.srcSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        /* Create coordinates transformation */

        /* Get transform for this VRT file */
        OGRCoordinateTransformation *newTransf = OGRCreateCoordinateTransformation(&vrt.srcSrs, &vrt.trgSrs);
        if (newTransf)
        {
            /* Delete the transform from last vrt file */
            if (vrt.transf) OGRCoordinateTransformation::DestroyCT(vrt.transf);

            /* Use the new one (they should be the same but just in case they are not...) */
            vrt.transf = newTransf;
        }
        else mlog(ERROR, "Failed to create new transform, reusing transform from previous VRT file.\n");

        objCreated = true;
    }
    catch (const RunTimeException &e)
    {
        mlog(e.level(), "Error creating new VRT dataset: %s", e.what());
    }

    if (!objCreated && vrt.dset)
    {
        GDALClose((GDALDatasetH)vrt.dset);
        vrt.dset = NULL;
        vrt.band = NULL;

        bzero(vrt.invGeot, sizeof(vrt.invGeot));
        vrt.rows = vrt.cols = vrt.cellSize = 0;
        bzero(&vrt.bbox, sizeof(vrt.bbox));
    }

    return objCreated;
}

/******************************************************************************
 * PRIVATE METHODS
 ******************************************************************************/

/*----------------------------------------------------------------------------
 * findTIFfilesWithPoint
 *----------------------------------------------------------------------------*/
bool VrtRaster::findTIFfilesWithPoint(OGRPoint& p)
{
    bool foundFile = false;

    try
    {
        const int32_t col = static_cast<int32_t>(floor(vrt.invGeot[0] + vrt.invGeot[1] * p.getX() + vrt.invGeot[2] * p.getY()));
        const int32_t row = static_cast<int32_t>(floor(vrt.invGeot[3] + vrt.invGeot[4] * p.getX() + vrt.invGeot[5] * p.getY()));

        tifList->clear();

        bool validPixel = (col >= 0) && (row >= 0) && (col < vrt.dset->GetRasterXSize()) && (row < vrt.dset->GetRasterYSize());
        if (!validPixel) return false;

        CPLString str;
        str.Printf("Pixel_%d_%d", col, row);

        const char *mdata = vrt.band->GetMetadataItem(str, "LocationInfo");
        if (mdata == NULL) return false; /* Pixel not in VRT file */

        CPLXMLNode *root = CPLParseXMLString(mdata);
        if (root == NULL) return false;  /* Pixel is in VRT file, but parser did not find its node  */

        if (root->psChild && (root->eType == CXT_Element) && EQUAL(root->pszValue, "LocationInfo"))
        {
            for (CPLXMLNode *psNode = root->psChild; psNode != NULL; psNode = psNode->psNext)
            {
                if ((psNode->eType == CXT_Element) && EQUAL(psNode->pszValue, "File") && psNode->psChild)
                {
                    char *fname = CPLUnescapeString(psNode->psChild->pszValue, NULL, CPLES_XML);
                    CHECKPTR(fname);
                    tifList->add(fname);
                    foundFile = true; /* There may be more than one file.. */
                    CPLFree(fname);
                }
            }
        }
        CPLDestroyXMLNode(root);
    }
    catch (const RunTimeException &e)
    {
        mlog(e.level(), "Error finding raster in VRT file: %s", e.what());
    }

    return foundFile;
}


/*----------------------------------------------------------------------------
 * clearRaster
 *----------------------------------------------------------------------------*/
void VrtRaster::clearVrt(vrt_t *_vrt)
{
    _vrt->fileName.clear();
    _vrt->dset = NULL;
    _vrt->band = NULL;
    bzero(_vrt->invGeot, sizeof(_vrt->invGeot));
    _vrt->rows = 0;
    _vrt->cols = 0;
    _vrt->cellSize = 0;
    bzero(&_vrt->bbox, sizeof(bbox_t));
    _vrt->transf = NULL;
    _vrt->srcSrs.Clear();
    _vrt->trgSrs.Clear();
}

/*----------------------------------------------------------------------------
 * clearRaster
 *----------------------------------------------------------------------------*/
void VrtRaster::clearRaster(raster_t *raster)
{
    raster->enabled = false;
    raster->sampled = false;
    raster->dset = NULL;
    raster->band = NULL;
    raster->fileName.clear();
    raster->dataType = GDT_Unknown;

    raster->rows = 0;
    raster->cols = 0;
    bzero(&raster->bbox, sizeof(bbox_t));
    raster->cellSize = 0;
    raster->xBlockSize = 0;
    raster->yBlockSize = 0;
    raster->gpsTime = 0;
    raster->point.empty();
    bzero(&raster->sample, sizeof(sample_t));
}

/*----------------------------------------------------------------------------
 * invaldiateAllRasters
 *----------------------------------------------------------------------------*/
void VrtRaster::invalidateRastersCache(void)
{
    raster_t *raster = NULL;
    const char* key = rasterDict.first(&raster);
    while (key != NULL)
    {
        assert(raster);
        raster->enabled = false;
        raster->sampled = false;
        raster->point.empty();
        raster->sample.value = INVALID_SAMPLE_VALUE;
        raster->sample.time = 0.0;
        key = rasterDict.next(&raster);
    }
}

/*----------------------------------------------------------------------------
 * updateRastersCache
 *----------------------------------------------------------------------------*/
void VrtRaster::updateRastersCache(OGRPoint& p)
{
    if (tifList->length() == 0)
        return;

    const char *key  = NULL;
    raster_t *raster = NULL;

    /* Check new tif file list against rasters in dictionary */
    for (int i = 0; i < tifList->length(); i++)
    {
        std::string& fileName = tifList->get(i);
        key = fileName.c_str();

        if (rasterDict.find(key, &raster))
        {
            /* Update point to be sampled, mark raster enabled for next sampling */
            assert(raster);
            raster->enabled = true;
            raster->point = p;
        }
        else
        {
            /* Create new raster for this tif file since it is not in the dictionary */
            raster = new raster_t;
            assert(raster);
            clearRaster(raster);
            raster->enabled = true;
            raster->point = p;
            raster->sample.value = INVALID_SAMPLE_VALUE;
            raster->fileName = fileName;
            rasterDict.add(key, raster);
        }
    }

    /* Maintain cache from getting too big */
    key = rasterDict.first(&raster);
    while (key != NULL)
    {
        if (rasterDict.length() <= MAX_CACHED_RASTERS)
            break;

        assert(raster);
        if (!raster->enabled)
        {
            /* Main thread closing multiple rasters is OK */
            if (raster->dset) GDALClose((GDALDatasetH)raster->dset);
            rasterDict.remove(key);
            delete raster;
        }
        key = rasterDict.next(&raster);
    }
}


/*----------------------------------------------------------------------------
 * createReaderThreads
 *----------------------------------------------------------------------------*/
void VrtRaster::createReaderThreads(void)
{
    uint32_t threadsNeeded = rasterDict.length();
    if (threadsNeeded <= readerCount)
        return;

    uint32_t newThreadsCnt = threadsNeeded - readerCount;
    // print2term("Creating %d new threads, readerCount: %d, neededThreads: %d\n", newThreadsCnt, readerCount, threadsNeeded);

    for (uint32_t i=0; i < newThreadsCnt; i++)
    {
        if (readerCount < MAX_READER_THREADS)
        {
            reader_t *reader = &rasterRreader[readerCount];
            reader->raster = NULL;
            reader->run = true;
            reader->sync = new Cond();
            reader->obj = this;
            reader->sync->lock();
            {
                reader->thread = new Thread(readingThread, reader);
            }
            reader->sync->unlock();
            readerCount++;
        }
        else
        {
            throw RunTimeException(CRITICAL, RTE_ERROR,
                                   "number of rasters to read: %d, is greater than max reading threads %d\n",
                                   rasterDict.length(), MAX_READER_THREADS);
        }
    }
    assert(readerCount == threadsNeeded);
}


/*----------------------------------------------------------------------------
 * sampleRasters
 *----------------------------------------------------------------------------*/
void VrtRaster::sampleRasters(void)
{
    /* Create additional reader threads if needed */
    createReaderThreads();

    /* For each raster which is marked to be sampled, give it to the reader thread */
    int signaledReaders = 0;
    raster_t *raster = NULL;
    const char *key = rasterDict.first(&raster);
    int i = 0;
    while (key != NULL)
    {
        assert(raster);
        if (raster->enabled)
        {
            reader_t *reader = &rasterRreader[i++];
            reader->sync->lock();
            {
                reader->raster = raster;
                reader->sync->signal(0, Cond::NOTIFY_ONE);
                signaledReaders++;
            }
            reader->sync->unlock();
        }
        key = rasterDict.next(&raster);
    }

    /* Did not signal any reader threads, don't wait */
    if (signaledReaders == 0)
        return;

    /* Wait for all reader threads to finish sampling */
    for (uint32_t j=0; j<readerCount; j++)
    {
        reader_t *reader = &rasterRreader[j];
        raster = NULL;
        do
        {
            reader->sync->lock();
            {
                raster = reader->raster;
            }
            reader->sync->unlock();
        } while (raster != NULL);
    }
}

/*----------------------------------------------------------------------------
 * readingThread
 *----------------------------------------------------------------------------*/
void* VrtRaster::readingThread(void *param)
{
    reader_t *reader = (reader_t*)param;
    bool run = true;

    while (run)
    {
        reader->sync->lock();
        {
            /* Wait for raster to work on */
            while ((reader->raster == NULL) && reader->run)
                reader->sync->wait(0, SYS_TIMEOUT);

            if(reader->raster != NULL)
            {
                reader->obj->processRaster(reader->raster, reader->obj);
                reader->raster = NULL; /* Done with this raster */
            }

            run = reader->run;
        }
        reader->sync->unlock();
    }

    return NULL;
}



/*----------------------------------------------------------------------------
 * processRaster
 * Thread-safe, can be called directly from main thread or reader thread
 *----------------------------------------------------------------------------*/
void VrtRaster::processRaster(raster_t* raster, VrtRaster* obj)
{
    try
    {
        /* Open raster if first time reading from it */
        if (raster->dset == NULL)
        {
            raster->dset = (GDALDataset *)GDALOpenEx(raster->fileName.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, NULL, NULL, NULL);
            if (raster->dset == NULL)
                throw RunTimeException(CRITICAL, RTE_ERROR, "Failed to open raster: %s:", raster->fileName.c_str());

            /* Store information about raster */
            raster->cols = raster->dset->GetRasterXSize();
            raster->rows = raster->dset->GetRasterYSize();

            /* Get raster boundry box */
            double geot[6] = {0};
            CPLErr err = raster->dset->GetGeoTransform(geot);
            CHECK_GDALERR(err);
            raster->bbox.lon_min = geot[0];
            raster->bbox.lon_max = geot[0] + raster->cols * geot[1];
            raster->bbox.lat_max = geot[3];
            raster->bbox.lat_min = geot[3] + raster->rows * geot[5];

            raster->cellSize = geot[1];

            /* Get raster block size */
            raster->band = raster->dset->GetRasterBand(1);
            CHECKPTR(raster->band);
            raster->band->GetBlockSize(&raster->xBlockSize, &raster->yBlockSize);
            mlog(DEBUG, "Raster xBlockSize: %d, yBlockSize: %d", raster->xBlockSize, raster->yBlockSize);

            /* Get raster data type */
            raster->dataType = raster->band->GetRasterDataType();

            /* Get raster date as gps time in seconds */
            raster->gpsTime = static_cast<double>(obj->getRasterDate(raster->fileName) / 1000);
        }

        /*
         * Attempt to read the raster only if it contains a point of interest.
         */
        if (!rasterContainsPoint(raster, raster->point))
            return;

        /*
         * Read the raster
         */
        const int32_t col = static_cast<int32_t>(floor((raster->point.getX() - raster->bbox.lon_min) / raster->cellSize));
        const int32_t row = static_cast<int32_t>(floor((raster->bbox.lat_max - raster->point.getY()) / raster->cellSize));

        /* Use fast 'lookup' method for nearest neighbour. */
        if (obj->sampleAlg == GRIORA_NearestNeighbour)
        {
            /* Raster offsets to block of interest */
            uint32_t xblk = col / raster->xBlockSize;
            uint32_t yblk = row / raster->yBlockSize;

            GDALRasterBlock *block = NULL;
            int cnt = 2;
            do
            {
                /* Retry read if error */
                block = raster->band->GetLockedBlockRef(xblk, yblk, false);
            } while (block == NULL && cnt--);
            CHECKPTR(block);

            /* Get data block pointer, no copy but block is locked */
            void *data = block->GetDataRef();
            if (data == NULL) block->DropLock();
            CHECKPTR(data);

            /* Calculate col, row inside of block */
            uint32_t _col = col % raster->xBlockSize;
            uint32_t _row = row % raster->yBlockSize;
            uint32_t  offset = _row * raster->xBlockSize + _col;

            /* Assume most data is stored as 32 bit floats (default case for elevation models) */
            if (raster->dataType == GDT_Float32)
            {
                float *p = (float*) data;
                raster->sample.value = (double) p[offset];
            }
            /* All other data types */
            else if (raster->dataType == GDT_Float64)
            {
                double *p = (double*) data;
                raster->sample.value = (double) p[offset];
            }
            else if (raster->dataType == GDT_Byte)
            {
                uint8_t *p = (uint8_t*) data;
                raster->sample.value = (double) p[offset];
            }
            else if (raster->dataType == GDT_UInt16)
            {
                uint16_t *p = (uint16_t*) data;
                raster->sample.value = (double) p[offset];
            }
            else if (raster->dataType == GDT_Int16)
            {
                int16_t *p = (int16_t*) data;
                raster->sample.value = (double) p[offset];
            }
            else if (raster->dataType == GDT_UInt32)
            {
                uint32_t *p = (uint32_t*) data;
                raster->sample.value = (double) p[offset];
            }
            else if (raster->dataType == GDT_Int32)
            {
                int32_t *p = (int32_t*) data;
                raster->sample.value = (double) p[offset];
            }
            else
            {
                /*
                 * This version of GDAL does not support 64bit integers
                 * Complex numbers are supported by GDAL but not suported by this code
                 */
                block->DropLock();
                throw RunTimeException(CRITICAL, RTE_ERROR, "Unsuported dataType in raster: %s:", raster->fileName.c_str());
            }

            /* Done reading, release block lock */
            block->DropLock();

            mlog(DEBUG, "Value: %lf, col: %u, row: %u, xblk: %u, yblk: %u, bcol: %u, brow: %u, offset: %u",
                 raster->sample.value, col, row, xblk, yblk, _col, _row, offset);
        }
        else
        {
            double rbuf[1] = {INVALID_SAMPLE_VALUE};
            int _col, _row, _colSize, _rowSize;

            /* If zero radius provided, use defaul kernels for each sampling algorithm */
            if (obj->radius == 0)
            {
                int kernel = 0;

                if      (sampleAlg == GRIORA_Bilinear)    kernel = 2; /* 2x2 kernel */
                else if (sampleAlg == GRIORA_Cubic)       kernel = 4; /* 4x4 kernel */
                else if (sampleAlg == GRIORA_CubicSpline) kernel = 4; /* 4x4 kernel */
                else if (sampleAlg == GRIORA_Lanczos)     kernel = 6; /* 6x6 kernel */
                else if (sampleAlg == GRIORA_Average)     kernel = 6; /* No default kernel, pick something */
                else if (sampleAlg == GRIORA_Mode)        kernel = 6; /* No default kernel, pick something */
                else if (sampleAlg == GRIORA_Gauss)       kernel = 6; /* No default kernel, pick something */

                _col = col - (kernel/2);
                _row = row - (kernel/2);
                _colSize = _rowSize = kernel; /* Always use kernel size, even for edge case... */
            }
            else
            {
                /* For now, use box/window where the window side length is radius*2 */
                int csize = raster->cellSize;
                int radius_in_meters = ((obj->radius + csize - 1) / csize) * csize; // Round to multiple of cells size
                int radius_in_pixels = radius_in_meters / csize;

                _col = col - radius_in_pixels;
                _row = row - radius_in_pixels;
                _colSize = _rowSize = radius_in_pixels*2;

                /* It is user error if they specify radius smaller than algorithm default kernel */
            }

            /* Handle left and top edge */
            if (_col < 0) _col = 0;
            if (_row < 0) _row = 0;

            /* Handle right and bottom edge */
            if (_col + _colSize >= (int)raster->cols)
                _colSize = raster->cols - _col;

            if (_row + _rowSize >= (int)raster->rows)
                _rowSize = raster->rows - _row;

            /*
             * If window is too big GDAL RasterIO will return an error.
             * This code will throw and no sample for this lon/lat will be returned to caller.
             */

            GDALRasterIOExtraArg args;
            INIT_RASTERIO_EXTRA_ARG(args);
            args.eResampleAlg = obj->sampleAlg;
            CPLErr err = CE_None;
            int cnt = 2;
            do
            {
                /* Retry read if error */
                err = raster->band->RasterIO(GF_Read, _col, _row, _colSize, _rowSize, rbuf, 1, 1, GDT_Float64, 0, 0, &args);
            } while (err != CE_None && cnt--);
            CHECK_GDALERR(err);
            raster->sample.value = rbuf[0];
        }

        /* Sample time comes from raster collection GPS time */
        raster->sample.time = raster->gpsTime;
        raster->sampled = true;
    }
    catch (const RunTimeException &e)
    {
        mlog(e.level(), "Error reading raster: %s", e.what());
    }
}


/*----------------------------------------------------------------------------
 * vrtContainsPoint
 *----------------------------------------------------------------------------*/
inline bool VrtRaster::vrtContainsPoint(OGRPoint& p)
{
    return (vrt.dset &&
           (p.getX() >= vrt.bbox.lon_min) && (p.getX() <= vrt.bbox.lon_max) &&
           (p.getY() >= vrt.bbox.lat_min) && (p.getY() <= vrt.bbox.lat_max));
}


/*----------------------------------------------------------------------------
 * rasterContainsPoint
 *----------------------------------------------------------------------------*/
inline bool VrtRaster::rasterContainsPoint(raster_t *raster, OGRPoint& p)
{
    return (raster && raster->dset &&
           (p.getX() >= raster->bbox.lon_min) && (p.getX() <= raster->bbox.lon_max) &&
           (p.getY() >= raster->bbox.lat_min) && (p.getY() <= raster->bbox.lat_max));
}


/*----------------------------------------------------------------------------
 * findCachedRasterWithPoint
 *----------------------------------------------------------------------------*/
bool VrtRaster::findCachedRasterWithPoint(OGRPoint& p, VrtRaster::raster_t** raster)
{
    bool foundRaster = false;
    const char *key = rasterDict.first(raster);
    while (key != NULL)
    {
        assert(*raster);
        if (rasterContainsPoint(*raster, p))
        {
            foundRaster = true;
            break; /* Only one raster with this point in mosaic rasters */
        }
        key = rasterDict.next(raster);
    }
    return foundRaster;
}


/*----------------------------------------------------------------------------
 * getRasterDate
 *----------------------------------------------------------------------------*/
int64_t VrtRaster::getRasterDate(std::string &tifFile)
{
    /* This code supports all OGR vector files */
    std::string featureFile = tifFile;
    int64_t gpsTime = 0;

    GDALDataset *dset = NULL;

    std::string key, fieldName, fileType;
    getDateTokens (key, fieldName, fileType);

    try
    {
        std::size_t pos = featureFile.rfind(key);
        if (pos == std::string::npos)
            throw RunTimeException(ERROR, RTE_ERROR, "Could not find marker %s in file", key.c_str());

        featureFile.replace(pos, key.length(), fileType.c_str());

        dset = (GDALDataset *)GDALOpenEx(featureFile.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
        if (dset == NULL)
            throw RunTimeException(ERROR, RTE_ERROR, "Could not open %s file", featureFile.c_str());

        /* For now assume the first layer has the feature we need */
        OGRLayer *layer = dset->GetLayer(0);
        if (layer == NULL)
            throw RunTimeException(ERROR, RTE_ERROR, "No layers found in feature file: %s", featureFile.c_str());

        OGRFeature *feature;
        layer->ResetReading();
        while ((feature = layer->GetNextFeature()) != NULL)
        {
            int i = feature->GetFieldIndex(fieldName.c_str());
            if (i != -1)
            {
                int year, month, day, hour, minute, second, timeZone;
                if (feature->GetFieldAsDateTime(i, &year, &month, &day, &hour, &minute, &second, &timeZone))
                {
                    /*
                     * Time Zone flag: 100 is GMT, 1 is localtime, 0 unknown
                     */
                    if (timeZone == 100)
                    {
                        TimeLib::gmt_time_t gmt;
                        gmt.year = year;
                        gmt.doy = TimeLib::dayofyear(year, month, day);
                        gmt.hour = hour;
                        gmt.minute = minute;
                        gmt.second = second;
                        gmt.millisecond = 0;
                        gpsTime = TimeLib::gmt2gpstime(gmt); // returns milliseconds from gps epoch to time specified in gmt_time
                    }
                    break;
                }
            }
        }

        if (gpsTime == 0) throw RunTimeException(ERROR, RTE_ERROR, "Failed to find time");

    }
    catch (const RunTimeException &e)
    {
        mlog(e.level(), "Error getting time from raster feature file: %s", e.what());
    }

    if (dset) GDALClose((GDALDatasetH)dset);

    return gpsTime;
}

/*----------------------------------------------------------------------------
 * luaDimensions - :dim() --> rows, cols
 *----------------------------------------------------------------------------*/
int VrtRaster::luaDimensions(lua_State *L)
{
    bool status = false;
    int num_ret = 1;

    try
    {
        /* Get Self */
        VrtRaster *lua_obj = (VrtRaster *)getLuaSelf(L, 1);

        /* Set Return Values */
        lua_pushinteger(L, lua_obj->vrt.rows);
        lua_pushinteger(L, lua_obj->vrt.cols);
        num_ret += 2;

        /* Set Return Status */
        status = true;
    }
    catch (const RunTimeException &e)
    {
        mlog(e.level(), "Error getting dimensions: %s", e.what());
    }

    /* Return Status */
    return returnLuaStatus(L, status, num_ret);
}

/*----------------------------------------------------------------------------
 * luaBoundingBox - :bbox() --> (lon_min, lat_min, lon_max, lat_max)
 *----------------------------------------------------------------------------*/
int VrtRaster::luaBoundingBox(lua_State *L)
{
    bool status = false;
    int num_ret = 1;

    try
    {
        /* Get Self */
        VrtRaster *lua_obj = (VrtRaster *)getLuaSelf(L, 1);

        /* Set Return Values */
        lua_pushnumber(L, lua_obj->vrt.bbox.lon_min);
        lua_pushnumber(L, lua_obj->vrt.bbox.lat_min);
        lua_pushnumber(L, lua_obj->vrt.bbox.lon_max);
        lua_pushnumber(L, lua_obj->vrt.bbox.lat_max);
        num_ret += 4;

        /* Set Return Status */
        status = true;
    }
    catch (const RunTimeException &e)
    {
        mlog(e.level(), "Error getting bounding box: %s", e.what());
    }

    /* Return Status */
    return returnLuaStatus(L, status, num_ret);
}

/*----------------------------------------------------------------------------
 * luaCellSize - :cell() --> cell size
 *----------------------------------------------------------------------------*/
int VrtRaster::luaCellSize(lua_State *L)
{
    bool status = false;
    int num_ret = 1;

    try
    {
        /* Get Self */
        VrtRaster *lua_obj = (VrtRaster *)getLuaSelf(L, 1);

        /* Set Return Values */
        lua_pushnumber(L, lua_obj->vrt.cellSize);
        num_ret += 1;

        /* Set Return Status */
        status = true;
    }
    catch (const RunTimeException &e)
    {
        mlog(e.level(), "Error getting cell size: %s", e.what());
    }

    /* Return Status */
    return returnLuaStatus(L, status, num_ret);
}


/*----------------------------------------------------------------------------
 * getSampledRastersCount
 *----------------------------------------------------------------------------*/
int VrtRaster::getSampledRastersCount(void)
{
    raster_t *raster = NULL;
    int cnt = 0;

    /* Not all rasters in dictionary were sampled, find out how many were */
    const char *key = rasterDict.first(&raster);
    while (key != NULL)
    {
        assert(raster);
        if (raster->enabled && raster->sampled) cnt++;
        key = rasterDict.next(&raster);
    }

    return cnt;
}

/*----------------------------------------------------------------------------
 * luaSamples - :sample(lon, lat) --> in|out
 *----------------------------------------------------------------------------*/
int VrtRaster::luaSamples(lua_State *L)
{
    bool status = false;
    int num_ret = 1;

    VrtRaster *lua_obj = NULL;

    try
    {
        /* Get Self */
        lua_obj = (VrtRaster *)getLuaSelf(L, 1);

        lua_obj->samplingMutex.lock(); /* Serialize sampling on the same object */

        /* Get Coordinates */
        double lon = getLuaFloat(L, 2);
        double lat = getLuaFloat(L, 3);

        /* Get samples */
        int sampledRasters = lua_obj->sample(lon, lat);
        if (sampledRasters > 0)
        {
            raster_t   *raster = NULL;
            const char *key    = NULL;

            /* Create return table */
            lua_createtable(L, sampledRasters, 0);

            /* Populate the return table */
            int i = 0;
            key = lua_obj->rasterDict.first(&raster);
            while (key != NULL)
            {
                assert(raster);
                if (raster->enabled && raster->sampled)
                {
                    lua_createtable(L, 0, 2);
                    LuaEngine::setAttrStr(L, "file", raster->fileName.c_str());
                    LuaEngine::setAttrNum(L, "value", raster->sample.value);
                    lua_rawseti(L, -2, ++i);
                }
                key = lua_obj->rasterDict.next(&raster);
            }

            num_ret++;
            status = true;
        }
    }
    catch (const RunTimeException &e)
    {
        mlog(e.level(), "Error getting samples: %s", e.what());
    }

    if (lua_obj) lua_obj->samplingMutex.unlock();

    /* Return Status */
    return returnLuaStatus(L, status, num_ret);
}


