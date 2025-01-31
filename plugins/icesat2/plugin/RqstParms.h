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

#ifndef __rqst_parms__
#define __rqst_parms__

/******************************************************************************
 * INCLUDES
 ******************************************************************************/

#include "OsApi.h"
#include "LuaObject.h"
#include "GeoJsonRaster.h"
#include "List.h"
#include "MathLib.h"

/******************************************************************************
 * REQUEST PARAMETERS
 ******************************************************************************/

class RqstParms: public LuaObject
{
    public:

        /*--------------------------------------------------------------------
         * Constants
         *--------------------------------------------------------------------*/

        static const char* SURFACE_TYPE;
        static const char* ATL03_CNF;
        static const char* YAPC;
        static const char* YAPC_SCORE;
        static const char* YAPC_KNN;
        static const char* YAPC_MIN_KNN;
        static const char* YAPC_WIN_H;
        static const char* YAPC_WIN_X;
        static const char* YAPC_VERSION;
        static const char* ATL08_CLASS;
        static const char* QUALITY;
        static const char* POLYGON;
        static const char* RASTER;
        static const char* TRACK;
        static const char* STAGES;
        static const char* COMPACT;
        static const char* LATITUDE;
        static const char* LONGITUDE;
        static const char* ALONG_TRACK_SPREAD;
        static const char* MIN_PHOTON_COUNT;
        static const char* EXTENT_LENGTH;
        static const char* EXTENT_STEP;
        static const char* MAX_ITERATIONS;
        static const char* MIN_WINDOW;
        static const char* MAX_ROBUST_DISPERSION;
        static const char* PASS_INVALID;
        static const char* DISTANCE_IN_SEGMENTS;
        static const char* ATL03_GEO_FIELDS;
        static const char* ATL03_PH_FIELDS;
        static const char* RASTERS_TO_SAMPLE;
        static const char* RQST_TIMEOUT;
        static const char* NODE_TIMEOUT;
        static const char* READ_TIMEOUT;
        static const char* GLOBAL_TIMEOUT; // sets all timeouts at once
        static const char* OUTPUT;
        static const char* OUTPUT_PATH;
        static const char* OUTPUT_FORMAT;
        static const char* OUTPUT_OPEN_ON_COMPLETE;

        static const int NUM_PAIR_TRACKS            = 2;
        static const int RPT_L                      = 0;
        static const int RPT_R                      = 1;

        static const int EXTENT_ID_PHOTONS          = 0x0;
        static const int EXTENT_ID_ELEVATION        = 0x2;

        static const int EXPECTED_NUM_ANC_FIELDS    = 8; // a typical number of ancillary fields requested
        static const int EXPECTED_NUM_RASTERS       = 8; // a typical number of rasters to sample

        static const int DEFAULT_RQST_TIMEOUT       = 600; // seconds
        static const int DEFAULT_NODE_TIMEOUT       = 600; // seconds
        static const int DEFAULT_READ_TIMEOUT       = 600; // seconds

        static const char* OBJECT_TYPE;
        static const char* LuaMetaName;
        static const struct luaL_Reg LuaMetaTable[];

        /*--------------------------------------------------------------------
         * Typedefs
         *--------------------------------------------------------------------*/

        /* Pair Tracks */
        typedef enum {
            ALL_TRACKS = 0,
            RPT_1 = 1,
            RPT_2 = 2,
            RPT_3 = 3,
            NUM_TRACKS = 3
        } track_t;

        /* Ground Tracks */
        typedef enum {
            GT1L = 10,
            GT1R = 20,
            GT2L = 30,
            GT2R = 40,
            GT3L = 50,
            GT3R = 60
        } gt_t;

        /* Spots */
        typedef enum {
            SPOT_1 = 1,
            SPOT_2 = 2,
            SPOT_3 = 3,
            SPOT_4 = 4,
            SPOT_5 = 5,
            SPOT_6 = 6,
            NUM_SPOTS = 6
        } spot_t;

        /* Spacecraft Orientation */
        typedef enum {
            SC_BACKWARD = 0,
            SC_FORWARD = 1,
            SC_TRANSITION = 2
        } sc_orient_t;

        /* Signal Confidence per Photon */
        typedef enum {
            SIGNAL_CONF_OFFSET = 2, // added to value to get index
            CNF_POSSIBLE_TEP = -2,
            CNF_NOT_CONSIDERED = -1,
            CNF_BACKGROUND = 0,
            CNF_WITHIN_10M = 1,
            CNF_SURFACE_LOW = 2,
            CNF_SURFACE_MEDIUM = 3,
            CNF_SURFACE_HIGH = 4,
            NUM_SIGNAL_CONF = 7,
            ATL03_INVALID_CONFIDENCE = 8
        } signal_conf_t;

        /* Quality Level per Photon */
        typedef enum {
            QUALITY_NOMINAL = 0,
            QUALITY_POSSIBLE_AFTERPULSE = 1,
            QUALITY_POSSIBLE_IMPULSE_RESPONSE = 2,
            QUALITY_POSSIBLE_TEP = 3,
            NUM_PHOTON_QUALITY = 4,
            ATL03_INVALID_QUALITY = 5
        } quality_ph_t;

        /* Surface Types for Signal Confidence */
        typedef enum {
            SRT_LAND = 0,
            SRT_OCEAN = 1,
            SRT_SEA_ICE = 2,
            SRT_LAND_ICE = 3,
            SRT_INLAND_WATER = 4
        } surface_type_t;

        /* ATL08 Surface Classification */
        typedef enum {
            ATL08_NOISE = 0,
            ATL08_GROUND = 1,
            ATL08_CANOPY = 2,
            ATL08_TOP_OF_CANOPY = 3,
            ATL08_UNCLASSIFIED = 4,
            NUM_ATL08_CLASSES = 5,
            ATL08_INVALID_CLASSIFICATION = 6
        } atl08_classification_t;

        /* Algorithm Stages */
        typedef enum {
            STAGE_LSF = 0,      // least squares fit
            STAGE_ATL08 = 1,    // use ATL08 photon classifications
            STAGE_YAPC = 2,     // yet another photon classifier
            NUM_STAGES = 3
        } atl06_stages_t;

        /* Output Formats */
        typedef enum {
            OUTPUT_FORMAT_NATIVE = 0,
            OUTPUT_FORMAT_FEATHER = 1,
            OUTPUT_FORMAT_PARQUET = 2,
            OUTPUT_FORMAT_CSV = 3,
            OUTPUT_FORMAT_UNSUPPORTED = 4
        } output_format_t;

        /* List of Strings */
        typedef List<SafeString, EXPECTED_NUM_ANC_FIELDS> string_list_t;

        /* YAPC Settings */
        typedef struct {
            uint8_t                 score;                      // minimum allowed weight of photon using yapc algorithm
            int                     version;                    // version of the yapc algorithm to run
            int                     knn;                        // (version 2 only) k-nearest neighbors
            int                     min_knn;                    // (version 3 only) minimum number of k-nearest neighors
            double                  win_h;                      // window height (overrides calculated value if non-zero)
            double                  win_x;                      // window width
        } yapc_t;

        /* Output Settings */
        typedef struct {
            const char*             path;                       // file system path to the file (includes filename)
            output_format_t         format;                     // format of the file
            bool                    open_on_complete;           // flag to client to open file on completion
        } output_file_t;

        /*--------------------------------------------------------------------
         * Methods
         *--------------------------------------------------------------------*/

        static int          luaCreate           (lua_State* L);
        static uint8_t      getSpotNumber       (sc_orient_t sc_orient, track_t track, int pair);
        static uint8_t      getGroundTrack      (sc_orient_t sc_orient, track_t track, int pair);

        /*--------------------------------------------------------------------
         * Data
         *--------------------------------------------------------------------*/

        surface_type_t          surface_type;                   // surface reference type (used to select signal confidence column)
        bool                    pass_invalid;                   // post extent even if each pair is invalid
        bool                    dist_in_seg;                    // the extent length and step are expressed in segments, not meters
        bool                    compact;                        // return compact (only lat,lon,height,time) elevation information
        bool                    atl03_cnf[NUM_SIGNAL_CONF];     // list of desired signal confidences of photons from atl03 classification
        bool                    quality_ph[NUM_PHOTON_QUALITY]; // list of desired photon quality levels from atl03
        bool                    atl08_class[NUM_ATL08_CLASSES]; // list of surface classifications to use (leave empty to skip)
        bool                    stages[NUM_STAGES];             // algorithm iterations
        yapc_t                  yapc;                           // settings used in YAPC algorithm
        List<MathLib::coord_t>  polygon;                        // polygon of region of interest
        GeoJsonRaster*          raster;                         // raster of region of interest, created from geojson file
        int                     track;                          // reference pair track number (1, 2, 3, or 0 for all tracks)
        int                     max_iterations;                 // least squares fit iterations
        int                     minimum_photon_count;           // PE
        double                  along_track_spread;             // meters
        double                  minimum_window;                 // H_win minimum
        double                  maximum_robust_dispersion;      // sigma_r
        double                  extent_length;                  // length of ATL06 extent (meters or segments if dist_in_seg is true)
        double                  extent_step;                    // resolution of the ATL06 extent (meters or segments if dist_in_seg is true)
        string_list_t*          atl03_geo_fields;               // list of geolocation and geophys_corr fields to associate with an extent
        string_list_t*          atl03_ph_fields;                // list of per-photon fields to associate with an extent
        string_list_t*          rasters_to_sample;              // list of rasters to sample
        int                     rqst_timeout;                   // total time in seconds for request to be processed
        int                     node_timeout;                   // time in seconds for a single node to work on a distributed request (used for proxied requests)
        int                     read_timeout;                   // time in seconds for a single read of an asset to take
        output_file_t           output;                         // output file parameters

    private:

        /*--------------------------------------------------------------------
         * Methods
         *--------------------------------------------------------------------*/

                                RqstParms               (lua_State* L, int index);
                                ~RqstParms              (void);

        signal_conf_t           str2atl03cnf            (const char* confidence_str);
        quality_ph_t            str2atl03quality        (const char* quality_ph_str);
        atl08_classification_t  str2atl08class          (const char* classifiction_str);
        output_format_t         str2outputformat        (const char* fmt_str);
        void                    get_lua_atl03_cnf       (lua_State* L, int index, bool* provided);
        void                    get_lua_atl03_quality   (lua_State* L, int index, bool* provided);
        void                    get_lua_atl08_class     (lua_State* L, int index, bool* provided);
        void                    get_lua_polygon         (lua_State* L, int index, bool* provided);
        void                    get_lua_raster          (lua_State* L, int index, bool* provided);
        void                    get_lua_yapc            (lua_State* L, int index, bool* provided);
        void                    get_lua_string_list     (lua_State* L, int index, string_list_t** string_list, bool* provided);
        void                    get_lua_output          (lua_State* L, int index, bool* provided);
};

#endif  /* __rqst_parms__ */
