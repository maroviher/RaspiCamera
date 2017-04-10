/*
Copyright (c) 2013, Broadcom Europe Ltd
Copyright (c) 2013, James Hughes
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * \file RaspiVid.c
 * Command line program to capture a camera video stream and encode it to file.
 * Also optionally display a preview/viewfinder of current camera input.
 *
 * \date 28th Feb 2013
 * \Author: James Hughes
 *
 * Description
 *
 * 3 components are created; camera, preview and video encoder.
 * Camera component has three ports, preview, video and stills.
 * This program connects preview and video to the preview and video
 * encoder. Using mmal we don't need to worry about buffers between these
 * components, but we do need to handle buffers from the encoder, which
 * are simply written straight to the file in the requisite buffer callback.
 *
 * If raw option is selected, a video splitter component is connected between
 * camera and preview. This allows us to set up callback for raw camera data
 * (in YUV420 or RGB format) which might be useful for further image processing.
 *
 * We use the RaspiCamControl code to handle the specific camera settings.
 * We use the RaspiPreview code to handle the (generic) preview window
 */

// We use some GNU extensions (basename)
#ifndef _GNU_SOURCE
   #define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <sysexits.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define VERSION_STRING "v1.3.12"

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"

#include "RaspiCamControl.h"
#include "RaspiPreview.h"
#include "RaspiCLI.h"

#include <semaphore.h>

#include <stdbool.h>

// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

// Port configuration for the splitter component
#define SPLITTER_OUTPUT_PORT 0
#define SPLITTER_PREVIEW_PORT 1

// Video format information
// 0 implies variable
#define VIDEO_FRAME_RATE_NUM 30
#define VIDEO_FRAME_RATE_DEN 1

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

// Max bitrate we allow for recording
const int MAX_BITRATE_LEVEL4 = 25000000; // 25Mbits/s
const int MAX_BITRATE_LEVEL42 = 62500000; // 62.5Mbits/s

/// Interval at which we check for an failure abort during capture
const int ABORT_INTERVAL = 100; // ms


/// Capture/Pause switch method
/// Simply capture for time specified
#define WAIT_METHOD_NONE           0
/// Cycle between capture and pause for times specified
#define WAIT_METHOD_TIMED          1
/// Switch between capture and pause on keypress
#define WAIT_METHOD_KEYPRESS       2
/// Switch between capture and pause on signal
#define WAIT_METHOD_SIGNAL         3
/// Run/record forever
#define WAIT_METHOD_FOREVER        4


int mmal_status_to_int(MMAL_STATUS_T status);
void my_annotate (MMAL_COMPONENT_T *camera, const char *string);
static void encoder_buffer_callback_raw_tcp(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
static void encoder_buffer_callback_android_dimon(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
static void encoder_buffer_callback_android_motion(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
static void encoder_buffer_callback_android(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);

static struct
{
   const char* strCallbackName;
   void* pCallbackFunc;
} callback_modes[] =
{
      {"raw_tcp",        encoder_buffer_callback_raw_tcp},
      {"android_dimon",  encoder_buffer_callback_android_dimon},
      {"android_motion", encoder_buffer_callback_android_motion},
      {"android",        encoder_buffer_callback_android},
};

static int callback_modes_count = sizeof(callback_modes) / sizeof(callback_modes[0]);

void print_callbacks()
{
   int i;
   for(i = 0; i < callback_modes_count; i++)
      fprintf(stderr, "%s\n", callback_modes[i].strCallbackName);
}

void* find_callback_by_name(const char* _strCallbackName)
{
   int i;
   for(i = 0; i < callback_modes_count; i++)
   {
      if(0 == strcmp(callback_modes[i].strCallbackName, _strCallbackName))
         return callback_modes[i].pCallbackFunc;
   }
   return 0;
}

MMAL_PORT_T *camera_preview_port = NULL;
MMAL_PORT_T *camera_video_port = NULL;
MMAL_PORT_T *encoder_output_port = NULL;
MMAL_PORT_T *preview_input_port = NULL;
MMAL_PORT_T *encoder_input_port = NULL;

// Forward
typedef struct RASPIVID_STATE_S RASPIVID_STATE;

/** Struct used to pass information in encoder port userdata to callback
 */
typedef struct
{
   int sockFD;
   FILE *file_handle;                   /// File handle to write buffer data to.
   RASPIVID_STATE *pstate;              /// pointer to our state in case required in callback
   int abort;                           /// Set to 1 in callback if an error occurs to attempt to abort the capture
   unsigned char lastFrameMotion;
   unsigned char lastFrameKeyFr;
   char  header_bytes[29];
   int  header_wptr;
   long unsigned int ulValidCallbackCnt;
   int runTimeShowStat;
} PORT_USERDATA;

/** Structure containing all state information for the current run
 */
struct RASPIVID_STATE_S
{
   int width;                          /// Requested width of image
   int height;                         /// requested height of image
   int bitrate;                        /// Requested bitrate
   int framerate;                      /// Requested frame rate (fps)
   int intraperiod;                    /// Intra-refresh period (key frame rate)
   int quantisationParameter;          /// Quantisation parameter - quality. Set bitrate 0 and set this for variable bitrate
   int bInlineHeaders;                  /// Insert inline headers to stream (SPS, PPS)
   char *filename;                     /// filename of output file
   int verbose;                        /// !0 if want detailed run information
   int demoMode;                       /// Run app in demo mode
   int demoInterval;                   /// Interval between camera settings changes
   int immutableInput;                 /// Flag to specify whether encoder works in place or creates a new buffer. Result is preview can display either
                                       /// the camera output or the encoder output (with compression artifacts)

   int motion_verbose;
   int motion_threshold;

   int profile;                        /// H264 profile to use for encoding
   int level;                          /// H264 level to use for encoding
   int waitMethod;                     /// Method for switching between pause and capture

   int onTime;                         /// In timed cycle mode, the amount of time the capture is on per cycle
   int offTime;                        /// In timed cycle mode, the amount of time the capture is off per cycle

   int segmentSize;                    /// Segment mode In timed cycle mode, the amount of time the capture is off per cycle
   int segmentWrap;                    /// Point at which to wrap segment counter
   int segmentNumber;                  /// Current segment counter
   int splitNow;                       /// Split at next possible i-frame if set to 1.
   int splitWait;                      /// Switch if user wants splited files

   unsigned short mbx;                 /// number of Macroblocks in x direction
   unsigned short mby;                 /// number of Macroblocks in y direction

   RASPIPREVIEW_PARAMETERS preview_parameters;   /// Preview setup parameters
   RASPICAM_CAMERA_PARAMETERS camera_parameters; /// Camera setup parameters

   MMAL_COMPONENT_T *camera_component;    /// Pointer to the camera component
   MMAL_COMPONENT_T *splitter_component;  /// Pointer to the splitter component
   MMAL_COMPONENT_T *encoder_component;   /// Pointer to the encoder component
   MMAL_CONNECTION_T *preview_connection; /// Pointer to the connection from camera or splitter to preview
   MMAL_CONNECTION_T *splitter_connection;/// Pointer to the connection from camera to splitter
   MMAL_CONNECTION_T *encoder_connection; /// Pointer to the connection from camera to encoder

   MMAL_POOL_T *splitter_pool; /// Pointer to the pool of buffers used by splitter output port 0
   MMAL_POOL_T *encoder_pool; /// Pointer to the pool of buffers used by encoder output port

   PORT_USERDATA callback_data;        /// Used to move data to the encoder callback

   int cameraNum;                       /// Camera number
   int settings;                        /// Request settings from the camera
   int sensor_mode;			            /// Sensor mode. 0=auto. Check docs/forum for modes selected by other values.
   int intra_refresh_type;              /// What intra refresh type to use. -1 to not set.
   int frame;
   int save_pts;
   int64_t starttime;
   int64_t lasttime;

   bool netListen;

   int64_t i64FramesCnt;
   int64_t i64FramesSkip;

   MMAL_PORT_BH_CB_T enc_cb_func;
};


/// Structure to cross reference H264 profile strings against the MMAL parameter equivalent
static XREF_T  profile_map[] =
{
   {"baseline",     MMAL_VIDEO_PROFILE_H264_BASELINE},
   {"main",         MMAL_VIDEO_PROFILE_H264_MAIN},
   {"high",         MMAL_VIDEO_PROFILE_H264_HIGH},
//   {"constrained",  MMAL_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE} // Does anyone need this?
};

static int profile_map_size = sizeof(profile_map) / sizeof(profile_map[0]);

/// Structure to cross reference H264 level strings against the MMAL parameter equivalent
static XREF_T  level_map[] =
{
   {"4",           MMAL_VIDEO_LEVEL_H264_4},
   {"4.1",         MMAL_VIDEO_LEVEL_H264_41},
   {"4.2",         MMAL_VIDEO_LEVEL_H264_42},
};

static int level_map_size = sizeof(level_map) / sizeof(level_map[0]);

/// Command ID's and Structure defining our command line options
#define CommandHelp         0
#define CommandWidth        1
#define CommandHeight       2
#define CommandBitrate      3
#define CommandOutput       4
#define CommandVerbose      5
#define CommandTimeout      6
#define CommandDemoMode     7
#define CommandFramerate    8
#define CommandPreviewEnc   9
#define CommandIntraPeriod  10
#define CommandProfile      11
#define CommandTimed        12
#define CommandSignal       13
#define CommandKeypress     14
#define CommandInitialState 15
#define CommandQP           16
#define CommandInlineHeaders 17
#define CommandSegmentFile  18
#define CommandSegmentWrap  19
#define CommandSegmentStart 20
#define CommandSplitWait    21
#define CommandCircular     22
#define CommandMode          23
#define CommandCamSelect    24
#define CommandSettings     25
#define CommandSensorMode   26
#define CommandIntraRefreshType 27
#define CommandSavePTS      29
#define CommandCodec        30
#define CommandLevel        31
#define CommandRawFormat    33
#define CommandNetListen    34

static COMMAND_LIST cmdline_commands[] =
{
   { CommandHelp,          "-help",       "?",  "This help information", 0 },
   { CommandWidth,         "-width",      "w",  "Set image width <size>. Default 1920", 1 },
   { CommandHeight,        "-height",     "h",  "Set image height <size>. Default 1080", 1 },
   { CommandBitrate,       "-bitrate",    "b",  "Set bitrate. Use bits per second (e.g. 10MBits/s would be -b 10000000)", 1 },
   { CommandOutput,        "-output",     "o",  "Output filename <filename> (to write to stdout, use '-o -').\n"
         "\t\t  Connect to a remote IPv4 host (e.g. tcp://192.168.1.2:1234, udp://192.168.1.2:1234)\n"
         "\t\t  To listen on a TCP port (IPv4) and wait for an incoming connection use -l\n"
         "\t\t  (e.g. raspvid -l -o tcp://0.0.0.0:3333 -> bind to all network interfaces, raspvid -l -o tcp://192.168.1.1:3333 -> bind to a certain local IPv4)", 1 },
   { CommandDemoMode,      "-demo",       "d",  "Run a demo mode (cycle through range of camera options, no capture)", 1},
   { CommandFramerate,     "-framerate",  "fps","Specify the frames per second to record", 1},
   { CommandPreviewEnc,    "-penc",       "e",  "Display preview image *after* encoding (shows compression artifacts)", 0},
   { CommandIntraPeriod,   "-intra",      "g",  "Specify the intra refresh period (key frame rate/GoP size). Zero to produce an initial I-frame and then just P-frames.", 1},
   { CommandProfile,       "-profile",    "pf", "Specify H264 profile to use for encoding", 1},
   { CommandTimed,         "-timed",      "td", "Cycle between capture and pause. -cycle on,off where on is record time and off is pause time in ms", 0},
   { CommandSignal,        "-signal",     "s",  "Cycle between capture and pause on Signal", 0},
   { CommandKeypress,      "-keypress",   "k",  "Cycle between capture and pause on ENTER", 0},
   { CommandInitialState,  "-initial",    "i",  "Initial state. Use 'record' or 'pause'. Default 'record'", 1},
   { CommandQP,            "-qp",         "qp", "Quantisation parameter. Use approximately 10-40. Default 0 (off)", 1},
   { CommandInlineHeaders, "-inline",     "ih", "Insert inline headers (SPS, PPS) to stream", 0},
   { CommandSplitWait,     "-split",      "sp", "In wait mode, create new output file for each start event", 0},
   { CommandMode,     "-mode",     "m", "android or raw_tcp", 1},
   { CommandCamSelect,     "-camselect",  "cs", "Select camera <number>. Default 0", 1 },
   { CommandSettings,      "-settings",   "set","Retrieve camera settings and write to stdout", 0},
   { CommandSensorMode,    "-mode",       "md", "Force sensor mode. 0=auto. See docs for other modes available", 1},
   { CommandIntraRefreshType,"-irefresh", "if", "Set intra refresh type", 1},
   { CommandSavePTS,       "-save-pts",   "pts","Save Timestamps to file for mkvmerge", 1 },
   { CommandLevel,         "-level",      "lev","Specify H264 level to use for encoding", 1},
   { CommandNetListen,     "-listen",     "l", "Listen on a TCP socket", 0},
};

static int cmdline_commands_size = sizeof(cmdline_commands) / sizeof(cmdline_commands[0]);
MMAL_PORT_T *g_encoder_output = NULL;
int gMotionAlarm = 0;

static struct
{
   char *description;
   int nextWaitMethod;
} wait_method_description[] =
{
      {"Simple capture",         WAIT_METHOD_NONE},
      {"Capture forever",        WAIT_METHOD_FOREVER},
      {"Cycle on time",          WAIT_METHOD_TIMED},
      {"Cycle on keypress",      WAIT_METHOD_KEYPRESS},
      {"Cycle on signal",        WAIT_METHOD_SIGNAL},
};

static int wait_method_description_size = sizeof(wait_method_description) / sizeof(wait_method_description[0]);

int my_raspicamcontrol_zoom_in_zoom_out(MMAL_COMPONENT_T *camera, char direction)
{
    MMAL_PARAMETER_INPUT_CROP_T crop;
    crop.hdr.id = MMAL_PARAMETER_INPUT_CROP;
    crop.hdr.size = sizeof(crop);

    if (mmal_port_parameter_get(camera->control, &crop.hdr) != MMAL_SUCCESS)
    {
        vcos_log_error("mmal_port_parameter_get(camera->control, &crop.hdr) failed, skip it");
        return 0;
    }
    printf("->crop.rect.x=%d,crop.rect.y=%d,crop.rect.width=%d,crop.rect.height=%d\n",
            crop.rect.x, crop.rect.y, crop.rect.width, crop.rect.height);

    int iMoveStep = 500, zoom_increment_16P16 = 65536/10;
    switch(direction)
    {
    case 'l':
        crop.rect.x -= iMoveStep;
        if(crop.rect.x < 0)
            crop.rect.x = 0;
        break;
    case 'r':
        if(crop.rect.x + iMoveStep + crop.rect.width > 65536)
            iMoveStep = 65536 - (crop.rect.x + crop.rect.width);
        crop.rect.x += iMoveStep;
        break;
    case 'u':
        crop.rect.y -= iMoveStep;
        if(crop.rect.y < 0)
            crop.rect.y = 0;
        break;
    case 'd':
        if(crop.rect.y + iMoveStep + crop.rect.height > 65536)
            iMoveStep = 65536 - (crop.rect.y + crop.rect.height);
        crop.rect.y += iMoveStep;
        break;
    case 'i':
        crop.rect.width -= zoom_increment_16P16;
        crop.rect.height -= zoom_increment_16P16;
        crop.rect.x += zoom_increment_16P16/2;
        crop.rect.y += zoom_increment_16P16/2;
        break;
    case 'R':
        crop.rect.width = 65536;
        crop.rect.height = 65536;
        crop.rect.x = 0;
        crop.rect.y = 0;
        break;
    case 'o':
        if((crop.rect.x + crop.rect.width + zoom_increment_16P16) < 65536)
        {//ok
            crop.rect.width += zoom_increment_16P16;
            crop.rect.x -= zoom_increment_16P16/2;
        }
        else
        {//out of bound
            printf("overflow x=%d\n", crop.rect.x + crop.rect.width + zoom_increment_16P16);
            crop.rect.x -= zoom_increment_16P16/2;
            crop.rect.width = 65536 - crop.rect.x;
        }

        if((crop.rect.y + crop.rect.height + zoom_increment_16P16) < 65536)
        {//ok
            crop.rect.height += zoom_increment_16P16;
            crop.rect.y -= zoom_increment_16P16/2;
        }
        else
        {//out of bound
            printf("overflow y=%d\n", crop.rect.y + crop.rect.height + zoom_increment_16P16);
            crop.rect.y -= zoom_increment_16P16/2;
            crop.rect.height = 65536 - crop.rect.y;
        }

        if((crop.rect.y + crop.rect.height + zoom_increment_16P16) < 65536)
        {//ok
            crop.rect.height += zoom_increment_16P16;
            crop.rect.y -= zoom_increment_16P16/2;
        }
        else
        {//out of bound
            printf("overflow y=%d\n", crop.rect.y + crop.rect.height + zoom_increment_16P16);
            crop.rect.y -= zoom_increment_16P16/2;
            crop.rect.height = 65536 - crop.rect.y;
        }

        if(crop.rect.y < 0)
            crop.rect.y = 0;
        if(crop.rect.x < 0)
            crop.rect.x = 0;
        if(crop.rect.y > 65536)
            crop.rect.y = 65536;
        if(crop.rect.x > 65536)
            crop.rect.x = 65536;
        break;
    }
    printf("<-crop.rect.x=%d,crop.rect.y=%d,crop.rect.width=%d,crop.rect.height=%d\n",
            crop.rect.x, crop.rect.y, crop.rect.width, crop.rect.height);

    int ret = mmal_status_to_int(mmal_port_parameter_set(camera->control, &crop.hdr));

    return ret;
}


/**
 * Assign a default set of parameters to the state passed in
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void default_status(RASPIVID_STATE *state)
{
   if (!state)
   {
      vcos_assert(0);
      return;
   }

   // Default everything to zero
   memset(state, 0, sizeof(RASPIVID_STATE));

   // Now set anything non-zero
   state->width = 1920;       // Default to 1080p
   state->height = 1080;
   state->bitrate = 17000000; // This is a decent default bitrate for 1080p
   state->framerate = VIDEO_FRAME_RATE_NUM;
   state->intraperiod = -1;    // Not set
   state->quantisationParameter = 0;
   state->demoMode = 0;
   state->demoInterval = 250; // ms
   state->immutableInput = 1;
   state->profile = MMAL_VIDEO_PROFILE_H264_HIGH;
   state->level = MMAL_VIDEO_LEVEL_H264_4;
   state->waitMethod = WAIT_METHOD_NONE;
   state->onTime = 5000;
   state->offTime = 5000;

   state->bInlineHeaders = 0;

   state->segmentSize = 0;  // 0 = not segmenting the file.
   state->segmentNumber = 1;
   state->segmentWrap = 0; // Point at which to wrap segment number back to 1. 0 = no wrap
   state->splitNow = 0;
   state->splitWait = 0;

   state->cameraNum = 0;
   state->settings = 0;
   state->sensor_mode = 0;

   state->intra_refresh_type = -1;

   state->frame = 0;
   state->save_pts = 0;

   state->netListen = false;


   // Setup preview window defaults
   raspipreview_set_defaults(&state->preview_parameters);

   // Set up the camera_parameters to default
   raspicamcontrol_set_defaults(&state->camera_parameters);
}


/**
 * Dump image state parameters to stderr.
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void dump_status(RASPIVID_STATE *state)
{
   int i;

   if (!state)
   {
      vcos_assert(0);
      return;
   }

   fprintf(stderr, "Width %d, Height %d, filename %s\n", state->width, state->height, state->filename);
   fprintf(stderr, "H264 Profile %s\n", raspicli_unmap_xref(state->profile, profile_map, profile_map_size));
   fprintf(stderr, "H264 Level %s\n", raspicli_unmap_xref(state->level, level_map, level_map_size));
   fprintf(stderr, "H264 Quantisation level %d, Inline headers %s\n", state->quantisationParameter, state->bInlineHeaders ? "Yes" : "No");

   // Not going to display segment data unless asked for it.
   if (state->segmentSize)
      fprintf(stderr, "Segment size %d, segment wrap value %d, initial segment number %d\n", state->segmentSize, state->segmentWrap, state->segmentNumber);

   fprintf(stderr, "Wait method : ");
   for (i=0;i<wait_method_description_size;i++)
   {
      if (state->waitMethod == wait_method_description[i].nextWaitMethod)
         fprintf(stderr, "%s", wait_method_description[i].description);
   }
   fprintf(stderr, "\n\n");

   raspipreview_dump_parameters(&state->preview_parameters);
   raspicamcontrol_dump_parameters(&state->camera_parameters);
}

/**
 * Parse the incoming command line and put resulting parameters in to the state
 *
 * @param argc Number of arguments in command line
 * @param argv Array of pointers to strings from command line
 * @param state Pointer to state structure to assign any discovered parameters to
 * @return Non-0 if failed for some reason, 0 otherwise
 */
static int parse_cmdline(int argc, const char **argv, RASPIVID_STATE *state)
{
   // Parse the command line arguments.
   // We are looking for --<something> or -<abbreviation of something>

   int valid = 1;
   int i;

   for (i = 1; i < argc && valid; i++)
   {
      int command_id, num_parameters;

      if (!argv[i])
         continue;

      if (argv[i][0] != '-')
      {
         valid = 0;
         continue;
      }

      // Assume parameter is valid until proven otherwise
      valid = 1;

      command_id = raspicli_get_command_id(cmdline_commands, cmdline_commands_size, &argv[i][1], &num_parameters);

      // If we found a command but are missing a parameter, continue (and we will drop out of the loop)
      if (command_id != -1 && num_parameters > 0 && (i + 1 >= argc) )
         continue;

      //  We are now dealing with a command line option
      switch (command_id)
        {
        case CommandHelp:
            return -1;

        case CommandMode:
           if(NULL == (state->enc_cb_func = find_callback_by_name(argv[i + 1])))
           {
              fprintf(stderr, "'%s' is an unknown operation mode, use one of:\n", argv[i + 1]);
              print_callbacks();
              exit(-1);
           }
           else
              i++;
         break;

      case CommandWidth: // Width > 0
         if (sscanf(argv[i + 1], "%u", &state->width) != 1)
            valid = 0;
         else
            i++;
         break;

      case CommandHeight: // Height > 0
         if (sscanf(argv[i + 1], "%u", &state->height) != 1)
            valid = 0;
         else
            i++;
         break;

      case CommandBitrate: // 1-100
         if (sscanf(argv[i + 1], "%u", &state->bitrate) == 1)
         {
            i++;
         }
         else
            valid = 0;

         break;

      case CommandOutput:  // output filename
      {
         int len = strlen(argv[i + 1]);
         if (len)
         {
            state->filename = malloc(len + 1);
            vcos_assert(state->filename);
            if (state->filename)
               strncpy(state->filename, argv[i + 1], len+1);
            i++;
         }
         else
            valid = 0;
         break;
      }

      case CommandVerbose: // display lots of data during run
         state->verbose = 1;
         break;

      case CommandFramerate: // fps to record
      {
         if (sscanf(argv[i + 1], "%u", &state->framerate) == 1)
         {
            // TODO : What limits do we need for fps 1 - 30 - 120??
            i++;
         }
         else
            valid = 0;
         break;
      }

      case CommandPreviewEnc:
         state->immutableInput = 0;
         break;

      case CommandIntraPeriod: // key frame rate
      {
         if (sscanf(argv[i + 1], "%u", &state->intraperiod) == 1)
            i++;
         else
            valid = 0;
         break;
      }

      case CommandQP: // quantisation parameter
      {
         if (sscanf(argv[i + 1], "%u", &state->quantisationParameter) == 1)
            i++;
         else
            valid = 0;
         break;
      }

      case CommandProfile: // H264 profile
      {
         state->profile = raspicli_map_xref(argv[i + 1], profile_map, profile_map_size);

         if( state->profile == -1)
            state->profile = MMAL_VIDEO_PROFILE_H264_HIGH;

         i++;
         break;
      }

      case CommandInlineHeaders: // H264 inline headers
      {
         state->bInlineHeaders = 1;
         break;
      }

      case CommandTimed:
      {
         if (sscanf(argv[i + 1], "%u,%u", &state->onTime, &state->offTime) == 2)
         {
            i++;

            if (state->onTime < 1000)
               state->onTime = 1000;

            if (state->offTime < 1000)
               state->offTime = 1000;

            state->waitMethod = WAIT_METHOD_TIMED;
         }
         else
            valid = 0;
         break;
      }

      case CommandCamSelect:  //Select camera input port
      {
         if (sscanf(argv[i + 1], "%u", &state->cameraNum) == 1)
         {
            i++;
         }
         else
            valid = 0;
         break;
      }

      case CommandSettings:
         state->settings = 1;
         break;

      case CommandSensorMode:
      {
         if (sscanf(argv[i + 1], "%u", &state->sensor_mode) == 1)
         {
            i++;
         }
         else
            valid = 0;
         break;
      }

      case CommandLevel: // H264 level
      {
         state->level = raspicli_map_xref(argv[i + 1], level_map, level_map_size);

         if( state->level == -1)
            state->level = MMAL_VIDEO_LEVEL_H264_4;

         i++;
         break;
      }

      case CommandNetListen:
      {
         state->netListen = true;

         break;
      }

      default:
      {
         // Try parsing for any image specific parameters
         // result indicates how many parameters were used up, 0,1,2
         // but we adjust by -1 as we have used one already
         const char *second_arg = (i + 1 < argc) ? argv[i + 1] : NULL;
         int parms_used = (raspicamcontrol_parse_cmdline(&state->camera_parameters, &argv[i][1], second_arg));

         // Still unused, try preview options
         if (!parms_used)
            parms_used = raspipreview_parse_cmdline(&state->preview_parameters, &argv[i][1], second_arg);


         // If no parms were used, this must be a bad parameters
         if (!parms_used)
            valid = 0;
         else
            i += parms_used - 1;

         break;
      }
      }
   }

   if (!valid)
   {
      fprintf(stderr, "Invalid command line option (%s)\n", argv[i-1]);
      return 1;
   }

   return 0;
}


/**
 *  buffer header callback function for camera control
 *
 *  Callback will dump buffer data to the specific file
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{

   if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED)
   {
      MMAL_EVENT_PARAMETER_CHANGED_T *param = (MMAL_EVENT_PARAMETER_CHANGED_T *) buffer->data;
      printf("param->hdr.id=%.8x\n", param->hdr.id);
      /*switch (param->hdr.id)
      {
         case MMAL_PARAMETER_ISO:
         {
            printf("MMAL_PARAMETER_ISO=%d\n", ((MMAL_PARAMETER_UINT32_T*) param)->value);
         }
            break;
         case MMAL_PARAMETER_SHUTTER_SPEED:
         {
            printf("MMAL_PARAMETER_SHUTTER_SPEED=" SCNu32 "\n", ((MMAL_PARAMETER_UINT32_T*) param)->value);
         }
            break;

         case MMAL_PARAMETER_CAMERA_SETTINGS:
         {
            MMAL_PARAMETER_CAMERA_SETTINGS_T *settings = (MMAL_PARAMETER_CAMERA_SETTINGS_T*)param;
            vcos_log_error("Exposure now %u, analog gain %u/%u, digital gain %u/%u",
			settings->exposure,
                        settings->analog_gain.num, settings->analog_gain.den,
                        settings->digital_gain.num, settings->digital_gain.den);
            vcos_log_error("AWB R=%u/%u, B=%u/%u",
                        settings->awb_red_gain.num, settings->awb_red_gain.den,
                        settings->awb_blue_gain.num, settings->awb_blue_gain.den
                        );
         }
         break;
      }*/
   }
   else if (buffer->cmd == MMAL_EVENT_ERROR)
   {
      vcos_log_error("No data received from sensor. Check all connections, including the Sunny one on the camera board");
   }
   else
   {
      vcos_log_error("Received unexpected camera control callback event, 0x%08x", buffer->cmd);
   }

   mmal_buffer_header_release(buffer);
}

/* not sure if here everything is correct */
static void SwitchMotionVectorsOnFly(RASPIVID_STATE* pState, int bTurnOn)
{
   //first check if we already have the needed state
   MMAL_BOOL_T currState;
   if (MMAL_SUCCESS != mmal_port_parameter_get_boolean(g_encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, &currState))
   {
       fprintf(stderr, "mmal_port_parameter_get_boolean error at line:%d\n", __LINE__);
       return;
   }
   if(currState == bTurnOn)
      return;

    if (MMAL_SUCCESS != mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 0))
        fprintf(stderr, "%d\n", __LINE__);
    if (MMAL_SUCCESS != mmal_connection_disable(pState->encoder_connection))
        fprintf(stderr, "%d\n", __LINE__);

    if (MMAL_SUCCESS != mmal_port_disable(encoder_output_port))
        fprintf(stderr, "%d\n", __LINE__);
    /*if (MMAL_SUCCESS != mmal_port_flush(encoder_output_port))
        fprintf(stderr, "%d\n", __LINE__);*/
    //mmal_port_flush does not work, https://github.com/raspberrypi/firmware/issues/457
    //use sleep
    sleep(1);

    if (MMAL_SUCCESS != mmal_port_parameter_set_boolean(g_encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, bTurnOn))
        fprintf(stderr, "%d\n", __LINE__);
    if (MMAL_SUCCESS != mmal_connection_enable(pState->encoder_connection))
        fprintf(stderr, "%d\n", __LINE__);

    if (MMAL_SUCCESS != mmal_port_enable(encoder_output_port, pState->enc_cb_func))
        fprintf(stderr, "%d\n", __LINE__);

    // Send all the buffers to the encoder output port
    int num = mmal_queue_length(pState->encoder_pool->queue);
    int q;
    for (q = 0; q < num; q++)
    {
        MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(pState->encoder_pool->queue);

        if (!buffer)
            vcos_log_error("Unable to get a required buffer %d from pool queue", q);

      if (mmal_port_send_buffer(encoder_output_port, buffer) != MMAL_SUCCESS)
         vcos_log_error("Unable to send a buffer to encoder output port (%d)", q);
   }

   if(MMAL_SUCCESS != mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1))
      fprintf(stderr, "%d\n", __LINE__);
}

void receive_commands(RASPIVID_STATE* pState)
{
    FILE* fpSock = fdopen(pState->callback_data.sockFD, "r");
    if (fpSock)
    {
        char * line = NULL;
        size_t len = 0;
        ssize_t read;
        while ((read = getline(&line, &len, fpSock)) != -1)
        {
            int iPar;
            //printf(line);
            if (!strncmp("iso=", line, 4))
            {
                if (1 == sscanf(line, "iso=%d\n", &iPar))
                {
                    raspicamcontrol_set_ISO(pState->camera_component, iPar);
                    //printf("%d, %d\n", iPar, tt);
                }
            }
            else if (!strncmp("ss=", line, 3))
            {
                if (1 == sscanf(line, "ss=%d\n", &iPar))
                {
                    raspicamcontrol_set_shutter_speed(pState->camera_component,
                            iPar);
                    //printf("ss=%d, %d\n", iPar, tt);
                }
            }
            else if (!strncmp("stat=", line, 5))
            {
                sscanf(line, "stat=%d\n",
                        &pState->callback_data.runTimeShowStat);
                //fprintf(stderr, "stat=%d\n", pState->callback_data.runTimeShowStat);
                if (0 == pState->callback_data.runTimeShowStat)
                    my_annotate(pState->camera_component, "");
            }
            else if (!strncmp("motion=", line, 7))
            {
               //only switch off motion vectors if motion alarm is turned off
               if (gMotionAlarm == 0)
               {
                  if (1 == sscanf(line, "motion=%d\n", &iPar))
                     SwitchMotionVectorsOnFly(pState, iPar);
               }
            }
            else if (!strncmp("move=", line, 5))
            {
                my_raspicamcontrol_zoom_in_zoom_out(pState->camera_component, line[5]);
            }
            else if (!strncmp("mot_alarm=", line, 10))
            {
               if (1 == sscanf(line, "mot_alarm=%d\n", &gMotionAlarm))
               {
                  SwitchMotionVectorsOnFly(pState, (gMotionAlarm==0)?(0):(1));
               }
            }

        }//while ((read = getline(&line, &len, fpSock)) != -1)
        fclose(fpSock);
    }
}

static FILE *open_filename(RASPIVID_STATE *pState, char *filename, int* pSockFD)
{
   FILE *new_handle = NULL;
   char *tempname = NULL;

   if (pState->segmentSize || pState->splitWait)
   {
      // Create a new filename string
      asprintf(&tempname, filename, pState->segmentNumber);
      filename = tempname;
   }

   if (filename)
   {
      bool bNetwork = false;
      int sfd, socktype;

      if(!strncmp("tcp://", filename, 6))
      {
         bNetwork = true;
         socktype = SOCK_STREAM;
      }
      else if(!strncmp("udp://", filename, 6))
      {
         if (pState->netListen)
         {
            fprintf(stderr, "No support for listening in UDP mode\n");
            exit(131);
         }
         bNetwork = true;
         socktype = SOCK_DGRAM;
      }

      if(bNetwork)
      {
         unsigned short port;
         filename += 6;
         char *colon;
         if(NULL == (colon = strchr(filename, ':')))
         {
            fprintf(stderr, "%s is not a valid IPv4:port, use something like tcp://1.2.3.4:1234 or udp://1.2.3.4:1234\n",
                    filename);
            exit(132);
         }
         if(1 != sscanf(colon + 1, "%hu", &port))
         {
            fprintf(stderr,
                    "Port parse failed. %s is not a valid network file name, use something like tcp://1.2.3.4:1234 or udp://1.2.3.4:1234\n",
                    filename);
            exit(133);
         }
         char chTmp = *colon;
         *colon = 0;

         struct sockaddr_in saddr={};
         saddr.sin_family = AF_INET;
         saddr.sin_port = htons(port);
         if(0 == inet_aton(filename, &saddr.sin_addr))
         {
            fprintf(stderr, "inet_aton failed. %s is not a valid IPv4 address\n",
                    filename);
            exit(134);
         }
         *colon = chTmp;

         if (pState->netListen)
         {
            int sockListen = socket(AF_INET, SOCK_STREAM, 0);
            if (sockListen >= 0)
            {
               int iTmp = 1;
               setsockopt(sockListen, SOL_SOCKET, SO_REUSEADDR, &iTmp, sizeof(int));//no error handling, just go on
               if (bind(sockListen, (struct sockaddr *) &saddr, sizeof(saddr)) >= 0)
               {
                  while ((-1 == (iTmp = listen(sockListen, 0))) && (EINTR == errno))
                     ;
                  if (-1 != iTmp)
                  {
                     fprintf(stderr, "Waiting for a TCP connection on %s:%"SCNu16"...",
                             inet_ntoa(saddr.sin_addr), ntohs(saddr.sin_port));
                     struct sockaddr_in cli_addr;
                     socklen_t clilen = sizeof(cli_addr);
                     while ((-1 == (sfd = accept(sockListen, (struct sockaddr *) &cli_addr, &clilen))) && (EINTR == errno))
                        ;
                     if (sfd >= 0)
                     {
                        struct timeval timeout;
                        timeout.tv_sec = 3;
                        timeout.tv_usec = 0;
                        if (setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, (char *) &timeout, sizeof(timeout)) < 0)
                           fprintf(stderr, "setsockopt failed\n");
                        fprintf(stderr, "Client connected from %s:%"SCNu16"\n", inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));
                     }
                     else
                        fprintf(stderr, "Error on accept: %s\n", strerror(errno));
                  }
                  else//if (-1 != iTmp)
                  {
                     fprintf(stderr, "Error trying to listen on a socket: %s\n", strerror(errno));
                  }
               }
               else//if (bind(sockListen, (struct sockaddr *) &saddr, sizeof(saddr)) >= 0)
               {
                  fprintf(stderr, "Error on binding socket: %s\n", strerror(errno));
               }
            }
            else//if (sockListen >= 0)
            {
               fprintf(stderr, "Error creating socket: %s\n", strerror(errno));
            }

            if (sockListen >= 0)//regardless success or error
               close(sockListen);//do not listen on a given port anymore
         }
         else//if (pState->netListen)
         {
            if(0 <= (sfd = socket(AF_INET, socktype, 0)))
            {
               fprintf(stderr, "Connecting to %s:%hu...", inet_ntoa(saddr.sin_addr), port);

               int iTmp = 1;
               while ((-1 == (iTmp = connect(sfd, (struct sockaddr *) &saddr, sizeof(struct sockaddr_in)))) && (EINTR == errno))
                  ;
               if (iTmp < 0)
                  fprintf(stderr, "error: %s\n", strerror(errno));
               else
                  fprintf(stderr, "connected, sending video...\n");
            }
            else
               fprintf(stderr, "Error creating socket: %s\n", strerror(errno));
         }

         new_handle = fdopen(sfd, "w");
         if(pSockFD)
            *pSockFD = sfd;
      }
      else
      {
         new_handle = fopen(filename, "wb");
      }
   }
   if (tempname)
      free(tempname);

   return new_handle;
}


void my_annotate (MMAL_COMPONENT_T *camera, const char *string)
{
   MMAL_PARAMETER_CAMERA_ANNOTATE_V3_T annotate = {
         { MMAL_PARAMETER_ANNOTATE, sizeof(MMAL_PARAMETER_CAMERA_ANNOTATE_V3_T) } };
   memset(&annotate.enable, sizeof(MMAL_PARAMETER_CAMERA_ANNOTATE_V3_T) - sizeof(annotate.hdr) - sizeof(annotate.text), 0);
   annotate.enable = 1;
   strncpy(annotate.text, string, MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3);
   mmal_port_parameter_set(camera->control, &annotate.hdr);
}

void handle_frame_end(PORT_USERDATA *pData)
{
   pData->pstate->i64FramesCnt++;
   if(0 == pData->pstate->callback_data.runTimeShowStat)
      return;
   int64_t time_us = vcos_getmicrosecs64();
   static int64_t last_frame_time_us = -1;
   char strFPS[164];
   float fFPS = 1000000.0/(time_us-last_frame_time_us);
   snprintf(strFPS, sizeof(strFPS), "FPS=%2.1f, %llu, %.2" SCNu8 ", %llu", fFPS, pData->pstate->i64FramesCnt, pData->lastFrameMotion, pData->pstate->i64FramesSkip);
   my_annotate(pData->pstate->camera_component, strFPS);
   last_frame_time_us = time_us;
}


typedef struct
{
   signed char x_vector;
   signed char y_vector;
   short sad;
} INLINE_MOTION_VECTOR;

#define MOTION_DEBUG_STRONGNESS (1<<0)//1
#define MOTION_DEBUG_STATISTICS (1<<1)//2
#include <math.h>
unsigned char DetectMotion(INLINE_MOTION_VECTOR *imv, RASPIVID_STATE *pstate)
{
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
   int64_t t_b;
   if(pstate->motion_verbose & MOTION_DEBUG_STRONGNESS)
      t_b = vcos_getmicrosecs64();
   unsigned short x,j;
   double sum = 0;
   unsigned char max_vx=0, max_vy=0, max_vxvy=0;
   //printf("\033[2J");//clear terminal
   for(j=0; j<pstate->mby; j++)
   {
      for(x=0; x<pstate->mbx; x++)
      {
         signed char vx =-imv[x+(pstate->mbx+1)*j].x_vector;
         signed char vy = imv[x+(pstate->mbx+1)*j].y_vector;
         max_vxvy = MAX(max_vxvy, (unsigned char)sqrt(vx*vx+vy*vy));
         /*max_vx=MAX(max_vx, abs(vx));
         max_vy=MAX(max_vy, abs(vy));*/
         //fprintf(stderr, "%.3d,%.3d|", vx,vy);

         //sum += sqrt(vx*vx+vy*vy);
         /*if(sum > pstate->motion_threshold)
            goto _loop_end;*/
         /*if((abs(vx) > pstate->motion_threshold)||
               (abs(vy) > pstate->motion_threshold))
         {
            if(pstate->motion_verbose & MOTION_DEBUG_STRONGNESS)
                  fprintf(stderr, "max_vx=%d, max_vy=%d, time us=%llu\n", max_vx, max_vy, vcos_getmicrosecs64() - t_b);
            return 1;
         }*/
      }
   }
return max_vxvy;
   if(pstate->motion_verbose & MOTION_DEBUG_STRONGNESS)
      fprintf(stderr, "max_vx=%d, max_vy=%d, time us=%llu\n", max_vx, max_vy, vcos_getmicrosecs64() - t_b);
   return 0;
   //_loop_end:
   if(sum > pstate->motion_threshold)
      return 1;
   return 0;
}

void PrintDataType(PORT_USERDATA *pData, MMAL_BUFFER_HEADER_T *buffer)
{
   int64_t time_now = vcos_getmicrosecs64();
   static int64_t time_us_prev = 0;
   int uiTimeDiff;
   if(time_us_prev == 0)
      uiTimeDiff = 0;
   else
      uiTimeDiff = time_now - time_us_prev;
   time_us_prev = time_now;
   fprintf(stderr, "time(us)=%.6d, frame=%.3llu, callbacks=%.3lu, buffer->length=%.6d, buffer->flags=0x%.2x: ",
           uiTimeDiff, pData->pstate->i64FramesCnt, pData->ulValidCallbackCnt++, buffer->length, buffer->flags);
   if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
          fprintf(stderr, "KEYFRAME, ");
   if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_START)
          fprintf(stderr, "FRAME_START, ");
   if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
          fprintf(stderr, "FRAME_END, ");
   if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
          fprintf(stderr, "FLAG_CONFIG, ");
   if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
          fprintf(stderr, "FLAG_CODECSIDEINFO, ");

   fprintf(stderr, "\n");
}

typedef enum ANDROID_DATA_TYPES
{
    CurrentResolution=0,
    RegularFrame,
    MotionInFrame,
    MotionAlarm
} ANDROID_DATA_TYPES;


void SendToAndroid(int sockFD, void* buf, size_t len)
{
   //size of an unsent data in skb
   /*#include <sys/ioctl.h>
   unsigned long size;
   ioctl( sockFD, TIOCOUTQ, &size );
   fprintf(stderr, "%d\n", size);*/

   if(len != send(sockFD, buf, len, MSG_NOSIGNAL))
      exit(__LINE__);//TCP connection closed, stop program
}
MMAL_BUFFER_HEADER_T* p_buf_partial_begin = NULL;

static void encoder_buffer_callback_android_dimon(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   MMAL_BUFFER_HEADER_T *new_buffer;
   PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

   if (pData)
   {
      if (buffer->length)
      {
         mmal_buffer_header_mem_lock(buffer);

         //PrintDataType(pData, buffer);

         if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
         {
            SendToAndroid(pData->sockFD, &buffer->length, 4);
            SendToAndroid(pData->sockFD, buffer->data, buffer->length);
         }
         else
         {
            //H264 data comes first, then comes motion vectors
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
            {   //motion vectors
            }
            else
            {   //H264 data
               if (0 == buffer->flags)
               {//begin of a partial frame
                  if(p_buf_partial_begin)
                  {
                     printf("Error in logic, p_buf_partial_begin\n");
                     exit(12);
                  }
                  p_buf_partial_begin = buffer;
               }
               else if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
               {//a complete frame or the end of a partial frame
                  if(p_buf_partial_begin)
                  {
                     uint32_t all_length = p_buf_partial_begin->length + buffer->length;

                     //printf("    partial, p_buf_partial_begin->length=%u, buffer->length=%u, all_length=0x%x\n", p_buf_partial_begin->length, buffer->length, all_length);
                     SendToAndroid(pData->sockFD, &all_length,                      4);   //send first the length of a frame
                     SendToAndroid(pData->sockFD, p_buf_partial_begin->data, p_buf_partial_begin->length);   //send the frame
                     SendToAndroid(pData->sockFD, buffer->data,                    buffer->length);   //send the frame
                     mmal_buffer_header_mem_unlock(p_buf_partial_begin);
                     mmal_buffer_header_release(p_buf_partial_begin);
                     if (port->is_enabled)
                     {
                        if (NULL == (new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue)))
                           vcos_log_error("mmal_queue_get");
                        else
                        {
                           MMAL_STATUS_T status = mmal_port_send_buffer(port, new_buffer);
                           if (status != MMAL_SUCCESS)
                              vcos_log_error("mmal_port_send_buffer=%d", status);
                        }
                     }// if (port->is_enabled)
                     p_buf_partial_begin = NULL;
                  }
                  else
                  {
                     //printf("not buffer->length=%d, buffer->length=0x%x\n", buffer->length, buffer->length);
                     SendToAndroid(pData->sockFD, &buffer->length, 4);   //send first the length of a frame
                     SendToAndroid(pData->sockFD, buffer->data, buffer->length);   //send the frame
                  }
                  handle_frame_end(pData);
               }
            }
         }//if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)

         if(!p_buf_partial_begin)
            mmal_buffer_header_mem_unlock(buffer);
      }
   }
   else
   {
      vcos_log_error("Received a encoder buffer callback with no state");
   }

   // release buffer back to the pool
   if(!p_buf_partial_begin)
      mmal_buffer_header_release(buffer);

   // and send one back to the port (if still open)
   if (port->is_enabled && !p_buf_partial_begin)
   {
      if (NULL == (new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue)))
         vcos_log_error("mmal_queue_get");
      else
      {
         MMAL_STATUS_T status = mmal_port_send_buffer(port, new_buffer);
         if (status != MMAL_SUCCESS)
            vcos_log_error("mmal_port_send_buffer=%d", status);
      }
   }// if (port->is_enabled)
}

static void encoder_buffer_callback_android_motion(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   MMAL_BUFFER_HEADER_T *new_buffer;
   PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

   if (pData)
   {
      if (buffer->length)
      {
         uint8_t dataType;
         mmal_buffer_header_mem_lock(buffer);

         //PrintDataType(pData, buffer);
         static char b1_config_sent = 0;//sent SPS/PPS only one time on the beginning
         if ((b1_config_sent < 2) && buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
         {
            b1_config_sent++;
            SendToAndroid(pData->sockFD, &buffer->length, 4);
            SendToAndroid(pData->sockFD, buffer->data, buffer->length);
         }
         else
         {
            //H264 data comes first, then comes motion vectors
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
            {   //motion vectors
               dataType = (uint8_t)MotionInFrame;
               SendToAndroid(pData->sockFD, &dataType, 1);
               unsigned char mot = DetectMotion((INLINE_MOTION_VECTOR*) &buffer->data[0], pData->pstate);
               SendToAndroid(pData->sockFD, &mot, 1);

               if((gMotionAlarm != 0) && ((int)mot) > gMotionAlarm)
               {
                  dataType = (uint8_t)MotionAlarm;
                  SendToAndroid(pData->sockFD, &dataType, 1);
               }
            }
            else
            {   //H264 data
               if (0 == buffer->flags)
               {//begin of a partial frame
                  if(p_buf_partial_begin)
                  {
                     printf("Error in logic, p_buf_partial_begin\n");
                     exit(12);
                  }
                  p_buf_partial_begin = buffer;
               }
               else if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
               {//a complete frame or the end of a partial frame
                  dataType = (uint8_t)RegularFrame;
                  if(p_buf_partial_begin)
                  {
                     uint32_t all_length = p_buf_partial_begin->length + buffer->length;

                     //printf("    partial, p_buf_partial_begin->length=%u, buffer->length=%u, all_length=0x%x\n", p_buf_partial_begin->length, buffer->length, all_length);

                     SendToAndroid(pData->sockFD, &dataType,                        1);
                     SendToAndroid(pData->sockFD, &all_length,                      4);   //send first the length of a frame
                     SendToAndroid(pData->sockFD, p_buf_partial_begin->data, p_buf_partial_begin->length);   //send the frame
                     SendToAndroid(pData->sockFD, buffer->data,                    buffer->length);   //send the frame
                     mmal_buffer_header_mem_unlock(p_buf_partial_begin);
                     mmal_buffer_header_release(p_buf_partial_begin);
                     if (port->is_enabled)
                     {
                        if (NULL == (new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue)))
                           vcos_log_error("mmal_queue_get");
                        else
                        {
                           MMAL_STATUS_T status = mmal_port_send_buffer(port, new_buffer);
                           if (status != MMAL_SUCCESS)
                              vcos_log_error("mmal_port_send_buffer=%d", status);
                        }
                     }// if (port->is_enabled)
                     p_buf_partial_begin = NULL;
                  }
                  else
                  {
                     //printf("not buffer->length=%d, buffer->length=0x%x\n", buffer->length, buffer->length);
                     SendToAndroid(pData->sockFD, &dataType,                        1);
                     SendToAndroid(pData->sockFD, &buffer->length, 4);   //send first the length of a frame
                     SendToAndroid(pData->sockFD, buffer->data, buffer->length);   //send the frame
                  }
                  handle_frame_end(pData);
               }
            }
         }//if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)

         if(!p_buf_partial_begin)
            mmal_buffer_header_mem_unlock(buffer);
      }
   }
   else
   {
      vcos_log_error("Received a encoder buffer callback with no state");
   }

   // release buffer back to the pool
   if(!p_buf_partial_begin)
      mmal_buffer_header_release(buffer);

   // and send one back to the port (if still open)
   if (port->is_enabled && !p_buf_partial_begin)
   {
      if (NULL == (new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue)))
         vcos_log_error("mmal_queue_get");
      else
      {
         MMAL_STATUS_T status = mmal_port_send_buffer(port, new_buffer);
         if (status != MMAL_SUCCESS)
            vcos_log_error("mmal_port_send_buffer=%d", status);
      }
   }// if (port->is_enabled)
}

static void encoder_buffer_callback_android(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   MMAL_BUFFER_HEADER_T *new_buffer;
   PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

   if (pData)
   {
      if (buffer->length)
      {
         mmal_buffer_header_mem_lock(buffer);

         //PrintDataType(pData, buffer);

         if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
         {
            SendToAndroid(pData->sockFD, &buffer->length, 4);
            SendToAndroid(pData->sockFD, buffer->data, buffer->length);
         }
         else
         {
            //H264 data comes first, then comes motion vectors
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
            {   //motion vectors
            }
            else
            {   //H264 data
               if (0 == buffer->flags)
               {//begin of a partial frame
                  if(p_buf_partial_begin)
                  {
                     printf("Error in logic, p_buf_partial_begin\n");
                     exit(12);
                  }
                  p_buf_partial_begin = buffer;
               }
               else if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
               {//a complete frame or the end of a partial frame
                  if(p_buf_partial_begin)
                  {
                     uint32_t all_length = p_buf_partial_begin->length + buffer->length;

                     //printf("    partial, p_buf_partial_begin->length=%u, buffer->length=%u, all_length=0x%x\n", p_buf_partial_begin->length, buffer->length, all_length);
                     SendToAndroid(pData->sockFD, &all_length,                      4);   //send first the length of a frame
                     SendToAndroid(pData->sockFD, p_buf_partial_begin->data, p_buf_partial_begin->length);   //send the frame
                     SendToAndroid(pData->sockFD, buffer->data,                    buffer->length);   //send the frame
                     mmal_buffer_header_mem_unlock(p_buf_partial_begin);
                     mmal_buffer_header_release(p_buf_partial_begin);
                     if (port->is_enabled)
                     {
                        if (NULL == (new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue)))
                           vcos_log_error("mmal_queue_get");
                        else
                        {
                           MMAL_STATUS_T status = mmal_port_send_buffer(port, new_buffer);
                           if (status != MMAL_SUCCESS)
                              vcos_log_error("mmal_port_send_buffer=%d", status);
                        }
                     }// if (port->is_enabled)
                     p_buf_partial_begin = NULL;
                  }
                  else
                  {
                     //printf("not buffer->length=%d, buffer->length=0x%x\n", buffer->length, buffer->length);
                     SendToAndroid(pData->sockFD, &buffer->length, 4);   //send first the length of a frame
                     SendToAndroid(pData->sockFD, buffer->data, buffer->length);   //send the frame
                  }
                  handle_frame_end(pData);
               }
            }
         }//if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)

         if(!p_buf_partial_begin)
            mmal_buffer_header_mem_unlock(buffer);
      }
   }
   else
   {
      vcos_log_error("Received a encoder buffer callback with no state");
   }

   // release buffer back to the pool
   if(!p_buf_partial_begin)
      mmal_buffer_header_release(buffer);

   // and send one back to the port (if still open)
   if (port->is_enabled && !p_buf_partial_begin)
   {
      if (NULL == (new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue)))
         vcos_log_error("mmal_queue_get");
      else
      {
         MMAL_STATUS_T status = mmal_port_send_buffer(port, new_buffer);
         if (status != MMAL_SUCCESS)
            vcos_log_error("mmal_port_send_buffer=%d", status);
      }
   }// if (port->is_enabled)
}

static void encoder_buffer_callback_empty(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   MMAL_BUFFER_HEADER_T *new_buffer;
   PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

   if (pData)
   {
      if (buffer->length)
      {
         pData->ulValidCallbackCnt = 0;
         mmal_buffer_header_mem_lock(buffer);

         PrintDataType(pData, buffer);

         //H264 data comes first, then comes motion vectors
         if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
         {//motion vectors
         }
         else
         {//H264 data
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
            {
               handle_frame_end(pData);
            }
            else
            {

            }
         }

         mmal_buffer_header_mem_unlock(buffer);
      }
   }
   else
   {
      vcos_log_error("Received a encoder buffer callback with no state");
   }

   // release buffer back to the pool
   mmal_buffer_header_release(buffer);

   // and send one back to the port (if still open)
   if (port->is_enabled)
   {
      MMAL_STATUS_T status;

      new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue);

      if (new_buffer)
         status = mmal_port_send_buffer(port, new_buffer);

      if (!new_buffer || status != MMAL_SUCCESS)
         vcos_log_error("Unable to return a buffer to the encoder port");
   }
}

static void encoder_buffer_callback_raw_tcp(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   MMAL_BUFFER_HEADER_T *new_buffer;
   PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

   if (pData)
   {
      if (buffer->length)
      {
         mmal_buffer_header_mem_lock(buffer);

         //H264 data comes first, then comes motion vectors
         if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
         {//motion vectors
         }
         else
         {//H264 data
            if(buffer->length != send(pData->sockFD, buffer->data, buffer->length, MSG_NOSIGNAL))
               exit(__LINE__);//TCP connection closed, stop program
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
               handle_frame_end(pData);
         }

         mmal_buffer_header_mem_unlock(buffer);
      }
   }
   else
   {
      vcos_log_error("Received a encoder buffer callback with no state");
   }

   // release buffer back to the pool
   mmal_buffer_header_release(buffer);

   // and send one back to the port (if still open)
   if (port->is_enabled)
   {
      MMAL_STATUS_T status;

      new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue);

      if (new_buffer)
         status = mmal_port_send_buffer(port, new_buffer);

      if (!new_buffer || status != MMAL_SUCCESS)
         vcos_log_error("Unable to return a buffer to the encoder port");
   }
}

char buf_prev[256000];
int buf_len = 0;
static void encoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   MMAL_BUFFER_HEADER_T *new_buffer;
   PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

   if (pData)
   {
      if (buffer->length)
      {
         mmal_buffer_header_mem_lock(buffer);

         static char bHeaderSaved = 0;
         if ((buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)&&(!bHeaderSaved))
         {
            if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
               bHeaderSaved = 1;
            fwrite(buffer->data, 1, buffer->length, pData->file_handle);
         }

         //H264 data comes first, then comes motion vectors
         if (!(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG))
         {
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
            {//motion vectors
               if(!pData->lastFrameKeyFr)
               {//detect motion only if it not a key frame
                  pData->lastFrameMotion = DetectMotion((INLINE_MOTION_VECTOR*) &buffer->data[0], pData->pstate);
                  fprintf(stderr, "motion=%d\n", (int) pData->lastFrameMotion);
               }
               else
               {
                  pData->lastFrameMotion = 0;
                  fprintf(stderr, "no motion, key frame\n");
               }
            }
            else
            {//H264 data

               if ((!pData->lastFrameMotion)&&(buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END))
                  buf_len = 0;
               pData->lastFrameKeyFr = buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME;

               if((pData->lastFrameMotion)&&(buf_len != 0))
               {
                  fwrite(buf_prev, 1, buf_len, pData->file_handle);
                  fprintf(stderr, "saved %d bytes\n", buf_len);
                  buf_len = 0;
               }
               else
                  pData->pstate->i64FramesSkip++;

               if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
               {
                  fwrite(buffer->data, 1, buffer->length, pData->file_handle);
                  buf_len = 0;
                  fprintf(stderr, "saved keyframe %d bytes\n", buffer->length);
               }
               else
               {
                  if(buf_len >= 256000)
                  {
                     fprintf(stderr, "pizdec\n");
                     exit(123);
                  }
                  memcpy(buf_prev + buf_len, buffer->data, buffer->length);
                  buf_len += buffer->length;
               }

               //fwrite(buffer->data, 1, buffer->length, pData->file_handle);

               if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
                  handle_frame_end(pData);
            }
         }

         mmal_buffer_header_mem_unlock(buffer);
      }
   }
   else
   {
      vcos_log_error("Received a encoder buffer callback with no state");
   }

   // release buffer back to the pool
   mmal_buffer_header_release(buffer);

   // and send one back to the port (if still open)
   if (port->is_enabled)
   {
      MMAL_STATUS_T status;

      new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue);

      if (new_buffer)
         status = mmal_port_send_buffer(port, new_buffer);

      if (!new_buffer || status != MMAL_SUCCESS)
         vcos_log_error("Unable to return a buffer to the encoder port");
   }
}

static void encoder_buffer_callback_ok(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   MMAL_BUFFER_HEADER_T *new_buffer;
   PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

   if (pData)
   {
      if (buffer->length)
      {
         mmal_buffer_header_mem_lock(buffer);

         static char bHeaderSaved = 0;
         if ((buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)&&(!bHeaderSaved))
         {
            if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
               bHeaderSaved = 1;
            fwrite(buffer->data, 1, buffer->length, pData->file_handle);
         }

         if (!(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG))
         {
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
            {//motion vectors
               pData->lastFrameMotion = DetectMotion((INLINE_MOTION_VECTOR*) &buffer->data[0], pData->pstate);
               fprintf(stderr, "motion=%d ", (int) pData->lastFrameMotion);
            }
            else
            {//H264 data

               fwrite(buffer->data, 1, buffer->length, pData->file_handle);

               if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
                  handle_frame_end(pData);
            }
         }

         mmal_buffer_header_mem_unlock(buffer);
      }
   }
   else
   {
      vcos_log_error("Received a encoder buffer callback with no state");
   }

   // release buffer back to the pool
   mmal_buffer_header_release(buffer);

   // and send one back to the port (if still open)
   if (port->is_enabled)
   {
      MMAL_STATUS_T status;

      new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue);

      if (new_buffer)
         status = mmal_port_send_buffer(port, new_buffer);

      if (!new_buffer || status != MMAL_SUCCESS)
         vcos_log_error("Unable to return a buffer to the encoder port");
   }
}

/**
 * Create the camera component, set up its ports
 *
 * @param state Pointer to state control struct
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
static MMAL_STATUS_T create_camera_component(RASPIVID_STATE *state)
{
   MMAL_COMPONENT_T *camera = 0;
   MMAL_ES_FORMAT_T *format;
   MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
   MMAL_STATUS_T status;

   /* Create the component */
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Failed to create camera component");
      goto error;
   }

   status = raspicamcontrol_set_stereo_mode(camera->output[0], &state->camera_parameters.stereo_mode);
   status += raspicamcontrol_set_stereo_mode(camera->output[1], &state->camera_parameters.stereo_mode);
   status += raspicamcontrol_set_stereo_mode(camera->output[2], &state->camera_parameters.stereo_mode);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Could not set stereo mode : error %d", status);
      goto error;
   }

   MMAL_PARAMETER_INT32_T camera_num =
      {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, state->cameraNum};

   status = mmal_port_parameter_set(camera->control, &camera_num.hdr);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Could not select camera : error %d", status);
      goto error;
   }

   if (!camera->output_num)
   {
      status = MMAL_ENOSYS;
      vcos_log_error("Camera doesn't have output ports");
      goto error;
   }

   status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG, state->sensor_mode);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Could not set sensor mode : error %d", status);
      goto error;
   }

   preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
   video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
   still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

   if (state->settings)
   {
      MMAL_PARAMETER_CHANGE_EVENT_REQUEST_T change_event_request =
         {{MMAL_PARAMETER_CHANGE_EVENT_REQUEST, sizeof(MMAL_PARAMETER_CHANGE_EVENT_REQUEST_T)},
          MMAL_PARAMETER_CAMERA_SETTINGS, 1};
      //works : MMAL_PARAMETER_CAMERA_NUM, MMAL_PARAMETER_FOCUS_STATUS, MMAL_PARAMETER_CAPTURE_STATUS, MMAL_PARAMETER_FIELD_OF_VIEW, MMAL_PARAMETER_CAMERA_SETTINGS

      status = mmal_port_parameter_set(camera->control, &change_event_request.hdr);
      if ( status != MMAL_SUCCESS )
      {
         vcos_log_error("No camera settings events");
      }
   }

   // Enable the camera, and tell it its control callback function
   status = mmal_port_enable(camera->control, camera_control_callback);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to enable control port : error %d", status);
      goto error;
   }

   //  set up the camera configuration
   {
      MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
      {
         { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) },
         .max_stills_w = state->width,
         .max_stills_h = state->height,
         .stills_yuv422 = 0,
         .one_shot_stills = 0,
         .max_preview_video_w = state->width,
         .max_preview_video_h = state->height,
         .num_preview_video_frames = 3 + vcos_max(0, (state->framerate-30)/10),
         .stills_capture_circular_buffer_height = 0,
         .fast_preview_resume = 0,
         .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC
      };
      mmal_port_parameter_set(camera->control, &cam_config.hdr);
   }

   // Now set up the port formats

   // Set the encode format on the Preview port
   // HW limitations mean we need the preview to be the same size as the required recorded output

   format = preview_port->format;

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->encoding_variant = MMAL_ENCODING_I420;

   if(state->camera_parameters.shutter_speed > 6000000)
   {
        MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
                                                     { 50, 1000 }, {166, 1000}};
        mmal_port_parameter_set(preview_port, &fps_range.hdr);
   }
   else if(state->camera_parameters.shutter_speed > 1000000)
   {
        MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
                                                     { 166, 1000 }, {999, 1000}};
        mmal_port_parameter_set(preview_port, &fps_range.hdr);
   }

   //enable dynamic framerate if necessary
   if (state->camera_parameters.shutter_speed)
   {
      if (state->framerate > 1000000./state->camera_parameters.shutter_speed)
      {
         state->framerate=0;
         if (state->verbose)
            fprintf(stderr, "Enable dynamic frame rate to fulfil shutter speed requirement\n");
      }
   }

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
   format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->width;
   format->es->video.crop.height = state->height;
   format->es->video.frame_rate.num = PREVIEW_FRAME_RATE_NUM;
   format->es->video.frame_rate.den = PREVIEW_FRAME_RATE_DEN;

   status = mmal_port_format_commit(preview_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera viewfinder format couldn't be set");
      goto error;
   }

   // Set the encode format on the video  port

   format = video_port->format;
   format->encoding_variant = MMAL_ENCODING_I420;

   if(state->camera_parameters.shutter_speed > 6000000)
   {
        MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
                                                     { 50, 1000 }, {166, 1000}};
        mmal_port_parameter_set(video_port, &fps_range.hdr);
   }
   else if(state->camera_parameters.shutter_speed > 1000000)
   {
        MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
                                                     { 167, 1000 }, {999, 1000}};
        mmal_port_parameter_set(video_port, &fps_range.hdr);
   }

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
   format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->width;
   format->es->video.crop.height = state->height;
   format->es->video.frame_rate.num = state->framerate;
   format->es->video.frame_rate.den = VIDEO_FRAME_RATE_DEN;

   status = mmal_port_format_commit(video_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera video format couldn't be set");
      goto error;
   }

   // Ensure there are enough buffers to avoid dropping frames
   if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;


   // Set the encode format on the still  port

   format = still_port->format;

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->encoding_variant = MMAL_ENCODING_I420;

   format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
   format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->width;
   format->es->video.crop.height = state->height;
   format->es->video.frame_rate.num = 0;
   format->es->video.frame_rate.den = 1;

   status = mmal_port_format_commit(still_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera still format couldn't be set");
      goto error;
   }

   /* Ensure there are enough buffers to avoid dropping frames */
   if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   /* Enable component */
   status = mmal_component_enable(camera);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera component couldn't be enabled");
      goto error;
   }

   raspicamcontrol_set_all_parameters(camera, &state->camera_parameters);

   state->camera_component = camera;

   return status;

error:

   if (camera)
      mmal_component_destroy(camera);

   return status;
}

/**
 * Destroy the camera component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_camera_component(RASPIVID_STATE *state)
{
   if (state->camera_component)
   {
      mmal_component_destroy(state->camera_component);
      state->camera_component = NULL;
   }
}



/**
 * Create the encoder component, set up its ports
 *
 * @param state Pointer to state control struct
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */

static MMAL_STATUS_T create_encoder_component(RASPIVID_STATE *state)
{
   MMAL_COMPONENT_T *encoder = 0;
   MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
   MMAL_STATUS_T status;
   MMAL_POOL_T *pool;

   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to create video encoder component");
      goto error;
   }

   if (!encoder->input_num || !encoder->output_num)
   {
      status = MMAL_ENOSYS;
      vcos_log_error("Video encoder doesn't have input/output ports");
      goto error;
   }

   encoder_input = encoder->input[0];
   g_encoder_output = encoder_output = encoder->output[0];

   // We want same format on input and output
   mmal_format_copy(encoder_output->format, encoder_input->format);

   // Only supporting H264 at the moment
   encoder_output->format->encoding = MMAL_ENCODING_H264;

   if (state->level == MMAL_VIDEO_LEVEL_H264_4)
   {
      if (state->bitrate > MAX_BITRATE_LEVEL4)
      {
         fprintf(stderr, "Bitrate too high: Reducing to 25MBit/s\n");
         state->bitrate = MAX_BITRATE_LEVEL4;
      }
   }
   else
   {
      if (state->bitrate > MAX_BITRATE_LEVEL42)
      {
         fprintf(stderr, "Bitrate too high: Reducing to 62.5MBit/s\n");
         state->bitrate = MAX_BITRATE_LEVEL42;
      }
   }

   encoder_output->format->bitrate = state->bitrate;
   encoder_output->buffer_size = 1024*1024;//encoder_output->buffer_size_recommended;//encoder_output->buffer_size_min
   encoder_output->buffer_num = 2; //encoder_output->buffer_num_recommended;//encoder_output->buffer_num_min

   //fprintf(stderr, "encoder_output->buffer_size=%d, encoder_output->buffer_num=%d\n", encoder_output->buffer_size , encoder_output->buffer_num);exit(1);

   // We need to set the frame rate on output to 0, to ensure it gets
   // updated correctly from the input framerate when port connected
   encoder_output->format->es->video.frame_rate.num = 0;
   encoder_output->format->es->video.frame_rate.den = 1;

   // Commit the port changes to the output port
   status = mmal_port_format_commit(encoder_output);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to set format on video encoder output port");
      goto error;
   }

   // Set the rate control parameter
   //if (0)
   {
      MMAL_PARAMETER_VIDEO_RATECONTROL_T param = {{ MMAL_PARAMETER_RATECONTROL, sizeof(param)}, MMAL_VIDEO_RATECONTROL_VARIABLE_SKIP_FRAMES};
      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set ratecontrol");
         goto error;
      }
   }

   if (state->intraperiod != -1)
   {
      MMAL_PARAMETER_UINT32_T param = {{ MMAL_PARAMETER_INTRAPERIOD, sizeof(param)}, state->intraperiod};
      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set intraperiod");
         goto error;
      }
   }

   if (state->quantisationParameter)
   {
      MMAL_PARAMETER_UINT32_T param = {{ MMAL_PARAMETER_VIDEO_ENCODE_INITIAL_QUANT, sizeof(param)}, state->quantisationParameter};
      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set initial QP");
         goto error;
      }

      MMAL_PARAMETER_UINT32_T param2 = {{ MMAL_PARAMETER_VIDEO_ENCODE_MIN_QUANT, sizeof(param)}, state->quantisationParameter};
      status = mmal_port_parameter_set(encoder_output, &param2.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set min QP");
         goto error;
      }

      MMAL_PARAMETER_UINT32_T param3 = {{ MMAL_PARAMETER_VIDEO_ENCODE_MAX_QUANT, sizeof(param)}, state->quantisationParameter};
      status = mmal_port_parameter_set(encoder_output, &param3.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set max QP");
         goto error;
      }
   }

   MMAL_PARAMETER_VIDEO_PROFILE_T param;
   param.hdr.id = MMAL_PARAMETER_PROFILE;
   param.hdr.size = sizeof(param);

   param.profile[0].profile = state->profile;

   if ((VCOS_ALIGN_UP(state->width,16) >> 4) * (VCOS_ALIGN_UP(state->height,16) >> 4) * state->framerate > 245760)
   {
      if ((VCOS_ALIGN_UP(state->width,16) >> 4) * (VCOS_ALIGN_UP(state->height,16) >> 4) * state->framerate <= 522240)
      {
         fprintf(stderr, "Too many macroblocks/s: Increasing H264 Level to 4.2\n");
         state->level = MMAL_VIDEO_LEVEL_H264_42;
      }
      else
      {
         vcos_log_error("Too many macroblocks/s requested");
         goto error;
      }
   }

   param.profile[0].level = state->level;

   status = mmal_port_parameter_set(encoder_output, &param.hdr);
   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to set H264 profile");
      goto error;
   }

   if (mmal_port_parameter_set_boolean(encoder_input, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, 1) != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to set immutable input flag");
      // Continue rather than abort..
   }

   //set INLINE HEADER flag to generate SPS and PPS for every IDR if requested
   if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, state->bInlineHeaders) != MMAL_SUCCESS)
   {
      vcos_log_error("failed to set INLINE HEADER FLAG parameters");
      // Continue rather than abort..
   }

   // Adaptive intra refresh settings
   if (state->intra_refresh_type != -1)
   {
      MMAL_PARAMETER_VIDEO_INTRA_REFRESH_T  param;
      param.hdr.id = MMAL_PARAMETER_VIDEO_INTRA_REFRESH;
      param.hdr.size = sizeof(param);

      // Get first so we don't overwrite anything unexpectedly
      status = mmal_port_parameter_get(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_warn("Unable to get existing H264 intra-refresh values. Please update your firmware");
         // Set some defaults, don't just pass random stack data
         param.air_mbs = param.air_ref = param.cir_mbs = param.pir_mbs = 0;
      }

      param.refresh_mode = state->intra_refresh_type;

      //if (state->intra_refresh_type == MMAL_VIDEO_INTRA_REFRESH_CYCLIC_MROWS)
      //   param.cir_mbs = 10;

      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set H264 intra-refresh values");
         goto error;
      }
   }

   //printf("motion_on=%d\n", mmal_port_parameter_set_boolean(g_encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, 1));
   //  Enable component
   status = mmal_component_enable(encoder);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to enable video encoder component");
      goto error;
   }

   /* Create pool of buffer headers for the output port to consume */
   pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);

   if (!pool)
   {
      vcos_log_error("Failed to create buffer header pool for encoder output port %s", encoder_output->name);
   }

   state->encoder_pool = pool;
   state->encoder_component = encoder;

   return status;

   error:
   if (encoder)
      mmal_component_destroy(encoder);

   state->encoder_component = NULL;

   return status;
}

/**
 * Destroy the encoder component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_encoder_component(RASPIVID_STATE *state)
{
   // Get rid of any port buffers first
   if (state->encoder_pool)
   {
      mmal_port_pool_destroy(state->encoder_component->output[0], state->encoder_pool);
   }

   if (state->encoder_component)
   {
      mmal_component_destroy(state->encoder_component);
      state->encoder_component = NULL;
   }
}

/**
 * Connect two specific ports together
 *
 * @param output_port Pointer the output port
 * @param input_port Pointer the input port
 * @param Pointer to a mmal connection pointer, reassigned if function successful
 * @return Returns a MMAL_STATUS_T giving result of operation
 *
 */
static MMAL_STATUS_T connect_ports(MMAL_PORT_T *output_port, MMAL_PORT_T *input_port, MMAL_CONNECTION_T **connection)
{
   MMAL_STATUS_T status;

   status =  mmal_connection_create(connection, output_port, input_port, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);

   if (status == MMAL_SUCCESS)
   {
      status =  mmal_connection_enable(*connection);
      if (status != MMAL_SUCCESS)
         mmal_connection_destroy(*connection);
   }

   return status;
}

int main(int argc, const char **argv)
{
   // Our main data storage vessel..
   RASPIVID_STATE state;
   int exit_code = EX_OK;

   MMAL_STATUS_T status = MMAL_SUCCESS;

   bcm_host_init();

   // Register our application with the logging system
   vcos_log_register("RaspiVid", VCOS_LOG_CATEGORY);

   default_status(&state);

   // Parse the command line and put options in to our status structure
   if (parse_cmdline(argc, argv, &state))
   {
      status = -1;
      exit(EX_USAGE);
   }

   if (state.filename)
   {
      state.callback_data.file_handle = open_filename(&state, state.filename, &state.callback_data.sockFD);

      if (!state.callback_data.file_handle)
      {
         // Notify user, carry on but discarding encoded output buffers
         vcos_log_error("%s: Error opening output file: %s\nNo output file will be generated\n", __func__, state.filename);
         exit(1);
      }
   }

   // OK, we have a nice set of parameters. Now set up our components
   // We have three components. Camera, Preview and encoder.

   if ((status = create_camera_component(&state)) != MMAL_SUCCESS)
   {
      vcos_log_error("%s: Failed to create camera component", __func__);
      exit_code = EX_SOFTWARE;
   }
   else if ((status = raspipreview_create(&state.preview_parameters)) != MMAL_SUCCESS)
   {
      vcos_log_error("%s: Failed to create preview component", __func__);
      destroy_camera_component(&state);
      exit_code = EX_SOFTWARE;
   }
   else if ((status = create_encoder_component(&state)) != MMAL_SUCCESS)
   {
      vcos_log_error("%s: Failed to create encode component", __func__);
      raspipreview_destroy(&state.preview_parameters);
      destroy_camera_component(&state);
      exit_code = EX_SOFTWARE;
   }
   else
   {
      camera_preview_port = state.camera_component->output[MMAL_CAMERA_PREVIEW_PORT];
      camera_video_port   = state.camera_component->output[MMAL_CAMERA_VIDEO_PORT];
      //camera_still_port   = state.camera_component->output[MMAL_CAMERA_CAPTURE_PORT];
      preview_input_port  = state.preview_parameters.preview_component->input[0];
      encoder_input_port  = state.encoder_component->input[0];
      encoder_output_port = state.encoder_component->output[0];

      if (status == MMAL_SUCCESS)
      {
         // Now connect the camera to the encoder
         status = connect_ports(camera_video_port, encoder_input_port, &state.encoder_connection);

         if (status != MMAL_SUCCESS)
         {
            state.encoder_connection = NULL;
            vcos_log_error("%s: Failed to connect camera video port to encoder input", __func__);
            goto error;
         }
      }

      if (status == MMAL_SUCCESS)
      {
         // Set up our userdata - this is passed though to the callback where we need the information.
         state.callback_data.pstate = &state;
         state.callback_data.abort = 0;
         state.callback_data.file_handle = NULL;

         // Set up our userdata - this is passed though to the callback where we need the information.
         encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T *)&state.callback_data;

         //if(state.inlineMotionVectors)
         {
            state.mbx=state.width/16;
            state.mby=state.height/16;
            if(state.width%16!=0)state.mbx++;
            if(state.height%16!=0)state.mby++;
         }


         // Enable the encoder output port and tell it its callback function
         status = mmal_port_enable(encoder_output_port, state.enc_cb_func);
         if (status != MMAL_SUCCESS)
         {
            vcos_log_error("Failed to setup encoder output");
            goto error;
         }

         // Send all the buffers to the encoder output port
         int num = mmal_queue_length(state.encoder_pool->queue);
         int q;
         for(q = 0; q < num; q++)
         {
            MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(state.encoder_pool->queue);

            if (!buffer)
               vcos_log_error("Unable to get a required buffer %d from pool queue", q);

            if (mmal_port_send_buffer(encoder_output_port, buffer) != MMAL_SUCCESS)
               vcos_log_error("Unable to send a buffer to encoder output port (%d)", q);
         }
         mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1);



         receive_commands(&state);
            /*
         state.callback_data.runTimeShowStat = 1;
         receiveUDPcommand(&state);*/
      }
      else
      {
         mmal_status_to_int(status);
         vcos_log_error("%s: Failed to connect camera to preview", __func__);
      }

error:

      mmal_status_to_int(status);

      // Disable all our ports that are not handled by connections
      mmal_port_disable(encoder_output_port);

      if (state.preview_parameters.wantPreview && state.preview_connection)
         mmal_connection_destroy(state.preview_connection);

      if (state.encoder_connection)
         mmal_connection_destroy(state.encoder_connection);

      if (state.splitter_connection)
         mmal_connection_destroy(state.splitter_connection);

      /* Disable components */
      if (state.encoder_component)
         mmal_component_disable(state.encoder_component);

      if (state.preview_parameters.preview_component)
         mmal_component_disable(state.preview_parameters.preview_component);

      if (state.splitter_component)
         mmal_component_disable(state.splitter_component);

      if (state.camera_component)
         mmal_component_disable(state.camera_component);

      destroy_encoder_component(&state);
      raspipreview_destroy(&state.preview_parameters);
      destroy_camera_component(&state);
   }

   if (status != MMAL_SUCCESS)
      raspicamcontrol_check_configuration(128);

   return exit_code;
}
