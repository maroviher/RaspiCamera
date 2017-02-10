#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <OMX_Core.h>
#include <OMX_Component.h>

#include <bcm_host.h>
#include <ilclient.h>

#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <stdbool.h>

#define VIDEO_DECODE_PORT 130

int sockfd = -1;

int
SetupListenSocket (struct in_addr* ip, unsigned short port, unsigned short rcv_timeout, bool bVerbose)
{
   int sfd = -1;
   struct sockaddr_in saddr={};
   saddr.sin_family = AF_INET;
   saddr.sin_port = htons(port);
   saddr.sin_addr = *ip;

   int sockListen = socket(AF_INET, SOCK_STREAM, 0);
   if (sockListen >= 0)
   {
      int iTmp = 1;
      setsockopt(sockListen, SOL_SOCKET, SO_REUSEADDR, &iTmp, sizeof(int)); //no error handling, just go on
      if (bind(sockListen, (struct sockaddr *) &saddr, sizeof(saddr)) >= 0)
      {
         while ((-1 == (iTmp = listen(sockListen, 0))) && (EINTR == errno))
            ;
         if (-1 != iTmp)
         {
            fprintf(stderr, "Waiting for a TCP connection on %s:%"SCNu16"...", inet_ntoa(saddr.sin_addr), ntohs(saddr.sin_port));
            struct sockaddr_in cli_addr;
            socklen_t clilen = sizeof(cli_addr);
            while ((-1 == (sfd = accept(sockListen, (struct sockaddr *) &cli_addr, &clilen))) && (EINTR == errno))
               ;
            if (sfd >= 0)
            {
               struct timeval timeout;
               timeout.tv_sec = rcv_timeout;
               timeout.tv_usec = 0;
               if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout, sizeof(timeout)) < 0)
                  fprintf(stderr, "setsockopt failed\n");
               fprintf(stderr, "Client connected from %s:%"SCNu16"\n", inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));
            }
            else
               fprintf(stderr, "Error on accept: %s\n", strerror(errno));
         }
         else //if (-1 != iTmp)
         {
            fprintf(stderr, "Error trying to listen on a socket: %s\n", strerror(errno));
         }
      }
      else //if (bind(sockListen, (struct sockaddr *) &saddr, sizeof(saddr)) >= 0)
      {
         fprintf(stderr, "Error on binding socket: %s\n", strerror(errno));
      }
   }
   else //if (sockListen >= 0)
   {
      fprintf(stderr, "Error creating socket: %s\n", strerror(errno));
   }

   if (sockListen >= 0) //regardless success or error
      close(sockListen); //do not listen on a given port anymore

   return sfd;
}


int
ConnectToHost (struct in_addr* ip, unsigned short port, bool bVerbose)
{
   int sfd = -1;

   struct sockaddr_in saddr = {};
   saddr.sin_family = AF_INET;
   saddr.sin_port = htons(port);
   saddr.sin_addr = *ip;

   bool bConnected = false;
   int iConnectCnt = 0;
   while ((!bConnected) && (iConnectCnt++ < 10000000))
   {
      if (0 <= (sfd = socket(AF_INET, SOCK_STREAM, 0)))
      {
         fcntl(sfd, F_SETFL, O_NONBLOCK);

         if(bVerbose)
            fprintf(stderr, "Connecting(%d) to %s:%hu...", iConnectCnt, inet_ntoa(saddr.sin_addr), port);
         int iTmp = connect(sfd, (struct sockaddr *) &saddr, sizeof(struct sockaddr_in));
         if ((iTmp == -1) && (errno != EINPROGRESS))
         {
            fprintf(stderr, "connect error: %s\n", strerror(errno));
            return 1;
         }
         if (iTmp == 0)
         {
            bConnected = true;
            continue; //connected immediately, not realistic
         }
         fd_set fdset;
         FD_ZERO(&fdset);
         FD_SET(sfd, &fdset);
         struct timeval tv;
         tv.tv_sec = 1;
         tv.tv_usec = 0;

         iTmp = select(sfd + 1, NULL, &fdset, NULL, &tv);
         switch (iTmp)
         {
            case 1: // data to read
            {
               int so_error;
               socklen_t len = sizeof(so_error);
               getsockopt(sfd, SOL_SOCKET, SO_ERROR, &so_error, &len);
               if (so_error == 0)
               {
                  if(bVerbose)
                     fprintf(stderr, "connected, receiving data\n");
                  bConnected = true;
                  continue;
               }
               else
               { // error
                  if ((ECONNREFUSED == so_error)||
                      (EHOSTUNREACH == so_error))
                  {
                     if(bVerbose)
                        fprintf(stderr, "%s\n", strerror(so_error));
                     close(sfd);
                     usleep(100000);
                     continue;
                  }

                  fprintf(stderr, "socket select %d, %s\n", so_error, strerror(so_error));
                  return -1;
               }
            }
               break;
            case 0: //timeout
               if(bVerbose)
                  fprintf(stderr, "timeout connecting\n");
               close(sfd);
               break;
         }
      }
      else
      {
         fprintf(stderr, "Error creating socket: %s\n", strerror(errno));
         return -1;
      }
   }

   if (bConnected && (-1 != sfd))
   {
      int flags = fcntl(sfd, F_GETFL, 0);
      if (0 != fcntl(sfd, F_SETFL, flags ^ O_NONBLOCK))
      {
         fprintf(stderr, "fcntl O_NONBLOCK");
         close(sfd);
         exit(134);
      }
      return sfd;
   }
   return -1;
}

void
error (char *msg)
{
   perror(msg);
   exit(1);
}

void
printState (OMX_HANDLETYPE handle)
{
   OMX_STATETYPE state;
   OMX_ERRORTYPE err;

   err = OMX_GetState(handle, &state);
   if (err != OMX_ErrorNone)
   {
      fprintf(stderr, "Error on getting state\n");
      exit(1);
   }
   switch (state)
   {
      case OMX_StateLoaded:
         printf("StateLoaded\n");
         break;
      case OMX_StateIdle:
         printf("StateIdle\n");
         break;
      case OMX_StateExecuting:
         printf("StateExecuting\n");
         break;
      case OMX_StatePause:
         printf("StatePause\n");
         break;
      case OMX_StateWaitForResources:
         printf("StateWait\n");
         break;
      case OMX_StateInvalid:
         printf("StateInvalid\n");
         break;
      default:
         printf("State unknown\n");
         break;
   }
}

char *
err2str (int err)
{
   switch (err)
   {
      case OMX_ErrorInsufficientResources:
         return "OMX_ErrorInsufficientResources";
      case OMX_ErrorUndefined:
         return "OMX_ErrorUndefined";
      case OMX_ErrorInvalidComponentName:
         return "OMX_ErrorInvalidComponentName";
      case OMX_ErrorComponentNotFound:
         return "OMX_ErrorComponentNotFound";
      case OMX_ErrorInvalidComponent:
         return "OMX_ErrorInvalidComponent";
      case OMX_ErrorBadParameter:
         return "OMX_ErrorBadParameter";
      case OMX_ErrorNotImplemented:
         return "OMX_ErrorNotImplemented";
      case OMX_ErrorUnderflow:
         return "OMX_ErrorUnderflow";
      case OMX_ErrorOverflow:
         return "OMX_ErrorOverflow";
      case OMX_ErrorHardware:
         return "OMX_ErrorHardware";
      case OMX_ErrorInvalidState:
         return "OMX_ErrorInvalidState";
      case OMX_ErrorStreamCorrupt:
         return "OMX_ErrorStreamCorrupt";
      case OMX_ErrorPortsNotCompatible:
         return "OMX_ErrorPortsNotCompatible";
      case OMX_ErrorResourcesLost:
         return "OMX_ErrorResourcesLost";
      case OMX_ErrorNoMore:
         return "OMX_ErrorNoMore";
      case OMX_ErrorVersionMismatch:
         return "OMX_ErrorVersionMismatch";
      case OMX_ErrorNotReady:
         return "OMX_ErrorNotReady";
      case OMX_ErrorTimeout:
         return "OMX_ErrorTimeout";
      case OMX_ErrorSameState:
         return "OMX_ErrorSameState";
      case OMX_ErrorResourcesPreempted:
         return "OMX_ErrorResourcesPreempted";
      case OMX_ErrorPortUnresponsiveDuringAllocation:
         return "OMX_ErrorPortUnresponsiveDuringAllocation";
      case OMX_ErrorPortUnresponsiveDuringDeallocation:
         return "OMX_ErrorPortUnresponsiveDuringDeallocation";
      case OMX_ErrorPortUnresponsiveDuringStop:
         return "OMX_ErrorPortUnresponsiveDuringStop";
      case OMX_ErrorIncorrectStateTransition:
         return "OMX_ErrorIncorrectStateTransition";
      case OMX_ErrorIncorrectStateOperation:
         return "OMX_ErrorIncorrectStateOperation";
      case OMX_ErrorUnsupportedSetting:
         return "OMX_ErrorUnsupportedSetting";
      case OMX_ErrorUnsupportedIndex:
         return "OMX_ErrorUnsupportedIndex";
      case OMX_ErrorBadPortIndex:
         return "OMX_ErrorBadPortIndex";
      case OMX_ErrorPortUnpopulated:
         return "OMX_ErrorPortUnpopulated";
      case OMX_ErrorComponentSuspended:
         return "OMX_ErrorComponentSuspended";
      case OMX_ErrorDynamicResourcesUnavailable:
         return "OMX_ErrorDynamicResourcesUnavailable";
      case OMX_ErrorMbErrorsInFrame:
         return "OMX_ErrorMbErrorsInFrame";
      case OMX_ErrorFormatNotDetected:
         return "OMX_ErrorFormatNotDetected";
      case OMX_ErrorContentPipeOpenFailed:
         return "OMX_ErrorContentPipeOpenFailed";
      case OMX_ErrorContentPipeCreationFailed:
         return "OMX_ErrorContentPipeCreationFailed";
      case OMX_ErrorSeperateTablesUsed:
         return "OMX_ErrorSeperateTablesUsed";
      case OMX_ErrorTunnelingUnsupported:
         return "OMX_ErrorTunnelingUnsupported";
      default:
         return "unknown error";
   }
}

void
eos_callback (void *userdata, COMPONENT_T *comp, OMX_U32 data)
{
   fprintf(stderr, "Got eos event\n");
}

void
error_callback (void *userdata, COMPONENT_T *comp, OMX_U32 data)
{
   fprintf(stderr, "OMX error %s\n", err2str(data));
}

unsigned int ui = 0;
OMX_ERRORTYPE
read_into_buffer_and_empty (COMPONENT_T *component, OMX_BUFFERHEADERTYPE *buff_header)
{
   OMX_ERRORTYPE r;

   buff_header->nFilledLen = recv(sockfd, buff_header->pBuffer, buff_header->nAllocLen, 0);
   if (buff_header->nFilledLen <= 0)
   {
      exit(1);
   }
   //buff_header->nFlags |= OMX_BUFFERFLAG_EOS;

   r = OMX_EmptyThisBuffer(ilclient_get_handle(component), buff_header);
   if (r != OMX_ErrorNone)
   {
      fprintf(stderr, "Empty buffer error %s\n", err2str(r));
   }
   return r;
}

static void
set_video_decoder_input_format (COMPONENT_T *component)
{
   int err;

   // set input video format
   OMX_VIDEO_PARAM_PORTFORMATTYPE videoPortFormat;
   //setHeader(&videoPortFormat,  sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
   memset(&videoPortFormat, 0, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
   videoPortFormat.nSize = sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE);
   videoPortFormat.nVersion.nVersion = OMX_VERSION;

   videoPortFormat.nPortIndex = VIDEO_DECODE_PORT;
   videoPortFormat.eCompressionFormat = OMX_VIDEO_CodingAVC;

   err = OMX_SetParameter(ilclient_get_handle(component), OMX_IndexParamVideoPortFormat, &videoPortFormat);
   if (err != OMX_ErrorNone)
   {
      fprintf(stderr, "Error setting video decoder format %s\n", err2str(err));
      exit(1);
   }
}

void
setup_decodeComponent (ILCLIENT_T *handle, char *decodeComponentName, COMPONENT_T **decodeComponent)
{
   int err;

   err = ilclient_create_component(handle, decodeComponent, decodeComponentName,
                                   ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS | ILCLIENT_ENABLE_OUTPUT_BUFFERS);
   if (err == -1)
      error("DecodeComponent create failed\n");

   printState(ilclient_get_handle(*decodeComponent));

   err = ilclient_change_component_state(*decodeComponent, OMX_StateIdle);
   if (err < 0)
      error("Couldn't change state to Idle\n");
   printState(ilclient_get_handle(*decodeComponent));

   // must be before we enable buffers
   set_video_decoder_input_format(*decodeComponent);
}

void
setup_renderComponent (ILCLIENT_T *handle, char *renderComponentName, COMPONENT_T **renderComponent)
{
   int err;

   err = ilclient_create_component(handle, renderComponent, renderComponentName,
                                   ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS);
   if (err == -1)
   {
      fprintf(stderr, "RenderComponent create failed\n");
      exit(1);
   }
   printState(ilclient_get_handle(*renderComponent));

   err = ilclient_change_component_state(*renderComponent, OMX_StateIdle);
   if (err < 0)
   {
      fprintf(stderr, "Couldn't change state to Idle\n");
      exit(1);
   }
   printState(ilclient_get_handle(*renderComponent));
}

void show_usage_and_exit(char** argv)
{
   char* bname = strdupa(argv[0]);
   fprintf(stderr,
         "Usage: %s [-l port] [-t timeout sec] -p port"
         "\n\tconnect: %s -h 1.2.3.4 -l -p 1234 -t 3"
         "\n\twait for incoming: %s -l -p 1234\n", bname, bname, bname);
   exit(EXIT_FAILURE);
}

int
main (int argc, char** argv)
{
   int err;
   ILCLIENT_T *handle;
   COMPONENT_T *decodeComponent;
   COMPONENT_T *renderComponent;
   OMX_BUFFERHEADERTYPE *buff_header;

   if(argc < 3)
      show_usage_and_exit(argv);

   bool bListen = false, bVerbose = false;
   unsigned short port, recv_timeout = 3;
   struct in_addr ip;
   int opt;
   while ((opt = getopt(argc, argv, "t:vlh:p:")) != -1)
   {
      switch (opt)
      {
         case 'l':
            bListen = true;
            break;
         case 'v':
            bVerbose = true;
            break;
         case 'h':
            if (0 == inet_aton(optarg, &ip))
            {
               fprintf(stderr, "inet_aton failed. %s is not a valid IPv4 address\n", optarg);
               exit(134);
            }
            break;
         case 'p':
            if (1 != sscanf(optarg, "%hu", &port))
            {
               fprintf(stderr, "error port\n");
               exit(EXIT_FAILURE);
            }
            break;
         case 't':
            if (1 != sscanf(optarg, "%hu", &recv_timeout))
            {
               fprintf(stderr, "error recv_timeout\n");
               exit(EXIT_FAILURE);
            }
            break;
         default: /* '?' */
            show_usage_and_exit(argv);
      }
   }

   bcm_host_init();

   handle = ilclient_init();
   if (handle == NULL)
      error("IL client init failed\n");

   if (OMX_Init() != OMX_ErrorNone)
   {
      ilclient_destroy(handle);
      error("OMX init failed\n");
   }

   ilclient_set_error_callback(handle, error_callback, NULL);
   ilclient_set_eos_callback(handle, eos_callback, NULL);

   setup_decodeComponent(handle, "video_decode", &decodeComponent);
   setup_renderComponent(handle, "video_render", &renderComponent);
   // both components now in Idle state, no buffers, ports disabled

   // input port
   ilclient_enable_port_buffers(decodeComponent, VIDEO_DECODE_PORT, NULL, NULL, NULL);
   ilclient_enable_port(decodeComponent, VIDEO_DECODE_PORT);

   if (0 > ilclient_change_component_state(decodeComponent, OMX_StateExecuting))
      error("Couldn't change state to Executing\n");

   printState(ilclient_get_handle(decodeComponent));

   if(bListen)
      sockfd = SetupListenSocket(&ip, port, 3, bVerbose);
   else
      sockfd = ConnectToHost(&ip, port, bVerbose);

   if (sockfd < 0)
   {
      fprintf(stderr, "connect failed");
      exit(133);
   }

   // Read the first block so that the decodeComponent can get
   // the dimensions of the video and call port settings
   // changed on the output port to configure it
   while (1)
   {
      buff_header = ilclient_get_input_buffer(decodeComponent, VIDEO_DECODE_PORT, 1 /* block */);
      if (buff_header != NULL)
         read_into_buffer_and_empty(decodeComponent, buff_header);

      if (ilclient_remove_event(decodeComponent, OMX_EventPortSettingsChanged, 131, 0, 0, 1) == 0)
      {
         printf("Removed port settings event\n");
         break;
      }
#if 0
      // wait for first input block to set params for output port
      if (toread == 0)
      {
         err = ilclient_wait_for_event(decodeComponent,
               OMX_EventPortSettingsChanged, 131, 0, 0, 1,
               ILCLIENT_EVENT_ERROR | ILCLIENT_PARAMETER_CHANGED, 2000);
         if (err < 0)
         {
            fprintf(stderr, "No port settings change\n");
            //exit(1);
         }
         else
         {
            printf("Port settings changed\n");
            break;
         }
      }
#endif
   }

   // set the decode component to idle and disable its ports
   if (0 > ilclient_change_component_state(decodeComponent, OMX_StateIdle))
      error("Couldn't change state to Idle\n");
   ilclient_disable_port(decodeComponent, 131);
   ilclient_disable_port_buffers(decodeComponent, 131, NULL, NULL, NULL);

   // set up the tunnel between decode and render ports
   err = OMX_SetupTunnel(ilclient_get_handle(decodeComponent), 131, ilclient_get_handle(renderComponent), 90);
   if (err != OMX_ErrorNone)
      error("Error setting up tunnel\n");

   // Okay to go back to processing data
   // enable the decode output ports

   OMX_SendCommand(ilclient_get_handle(decodeComponent), OMX_CommandPortEnable, 131, NULL);

   ilclient_enable_port(decodeComponent, 131);

   OMX_SendCommand(ilclient_get_handle(renderComponent), OMX_CommandPortEnable, 90, NULL);

   ilclient_enable_port(renderComponent, 90);

   // set both components to executing state
   if (0 > ilclient_change_component_state(decodeComponent, OMX_StateExecuting))
      error("OMX_StateExecuting");
   if (0 > ilclient_change_component_state(renderComponent, OMX_StateExecuting))
      error("OMX_StateExecuting");

   // main loop
   while (1)
   {
      // do we have a decode input buffer we can fill and empty?
      buff_header = ilclient_get_input_buffer(decodeComponent, 130, 1 /* block */);
      if (buff_header != NULL)
      {
         read_into_buffer_and_empty(decodeComponent, buff_header);
      }
   }

   /*ilclient_wait_for_event(renderComponent, OMX_EventBufferFlag, 90, 0,
    OMX_BUFFERFLAG_EOS, 0, ILCLIENT_BUFFER_FLAG_EOS, 10000);*/
   return 0;
}
