#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <arpa/inet.h>

#define OPUS_SET_BITRATE 64000
#include <opus/opus.h>
#include <opus/opus_multistream.h>
#include <opus/opus_defines.h>
#include <opus/opus_types.h>

#include "libns/libns.h"
//data flow.
//arecord -> zsy.noise -> zns ->zsy.clean -> aplay
//                            ->zsy.opus  -> android
#define FILE_NOISE	"/tmp/zsy/zsy.noise"
#define FILE_CLEAN	"/tmp/zsy/zsy.clean"
#define FILE_OPUS	"/tmp/zsy/zsy.opus"

//json ctrl.
#include "cJSON.h"
#define FILE_JSON_RX "/tmp/zsy/zsy.json.rx"
#define FILE_JSON_TX "/tmp/zsy/zsy.json.tx"
#define FILE_JSON_CFG "zns.json"
//pid file.
#define FILE_PID    "/tmp/zsy/zns.pid"

//Android APP use 48khz,so here keep 48khz.
#define SAMPLE_RATE  48000 //48khz.
//for opus encode/decode.
#define CHANNELS_NUM            2  //2 channels.
#define OPUS_PER_CH_10MS     (480*sizeof(opus_int16)) //per channel 48KHz,10ms=480bytes.per channel 48KHz,10ms=480bytes,we use 16-bit ,so here is 480*2=960bytes.
#define OPUS_TWO_CH_10MS     (480*sizeof(opus_int16)*2)

int g_bExitFlag=0;
typedef struct
{
    char m_cam1xy[32];
    char m_cam2xy[32];
    char m_cam3xy[32];
    //"DeNoise":"off/Strong/WebRTC/mmse/Bevis/NRAE/query"
    int m_iDeNoise;
    //"StrongMode":"mode1/mode2/mode3/mode4/mode5/mode6/mode7/mode8/mode9/mode10/query"
    int m_iStrongMode;
}ZCamPara;
ZCamPara gCamPara;

void gSIGHandler(int sigNo)
{
    switch(sigNo)
    {
    case SIGINT:
    case SIGKILL:
    case SIGTERM:
        printf("zns:got Ctrl-C...\n");
        g_bExitFlag=1;
        break;
    default:
        break;
    }
}
int gLoadCfgFile(ZCamPara *para);
int gSaveCfgFile(ZCamPara *para);
void *gThreadJson(void *para);
int gParseJson(char *jsonData,int jsonLen,int fd);
void *gThreadNs(void *para);

int main(int argc ,char **argv)
{
    pthread_t tidJson,tidNs;
    //write pid to file.
    int fd=open(FILE_PID,O_RDWR|O_CREAT);
    if(fd<0)
    {
        printf("failed to open %s.\n",FILE_PID);
        return -1;
    }
    char pidBuffer[32];
    memset(pidBuffer,0,sizeof(pidBuffer));
    sprintf(pidBuffer,"%d",getpid());
    write(fd,pidBuffer,strlen(pidBuffer));
    close(fd);

    //create threads.
    if(pthread_create(&tidJson,NULL,gThreadJson,NULL))
    {
        printf("failed to create json thread!\n");
        return -1;
    }
    if(pthread_create(&tidNs,NULL,gThreadNs,NULL))
    {
        printf("failed to create ns thread!\n");
        return -1;
    }

    //Set the signal callback for Ctrl-C
    signal(SIGINT,gSIGHandler);

    pthread_join(tidJson,NULL);
    pthread_join(tidNs,NULL);

    printf("zns:done.\n");
    return 0;
}
void *gThreadNs(void *para)
{
    //for mkfifo in&out.
    int fd_noise,fd_clean,fd_opus;
    fd_noise=open(FILE_NOISE,O_RDWR);
    if(fd_noise<0)
    {
        printf("failed to open %s\n",FILE_NOISE);
        g_bExitFlag=1;
        return NULL;
    }
    fd_clean=open(FILE_CLEAN,O_RDWR);
    if(fd_clean<0)
    {
        printf("failed to open %s\n",FILE_CLEAN);
        g_bExitFlag=1;
        return NULL;
    }
    fd_opus=open(FILE_OPUS,O_RDWR|O_NONBLOCK);
    if(fd_opus<0)
    {
        printf("failed to open %s\n",FILE_OPUS);
        g_bExitFlag=1;
        return NULL;
    }
    //for libns.
    //here call ns_init() or ns_custom_init() for call ns_uninit() later.
    ns_init(0);//0~5.
    int denoiseAlgorithm = 3;
    int denoiseLevel = 0;
    int enhancedType = 0;
    int enhancedLevel = 0;
    char customBandGains[8];
    char preEmphasisFlag = 0;
    memset(customBandGains,0,8);
    if(ns_custom_init(denoiseAlgorithm, denoiseLevel, enhancedType, enhancedLevel, customBandGains, preEmphasisFlag))
    {
        printf("NoiseCut,failed to init ns_custom_init().\n");
        g_bExitFlag=1;
        return NULL;
    }

    char *pOpusEnc=(char*)malloc(480*sizeof(opus_int16)*2*2);
    if(NULL==pOpusEnc)
    {
        printf("error at malloc for opusenc!\n");
        g_bExitFlag=1;
        return NULL;
    }

    /** Allocates and initializes a multistream encoder state.
      * Call opus_multistream_encoder_destroy() to release
      * this object when finished.

      * @param coupled_streams <tt>int</tt>: Number of coupled (2 channel) streams
      *                                      to encode.
      *                                      This must be no larger than the total
      *                                      number of streams.
      *                                      Additionally, The total number of
      *                                      encoded channels (<code>streams +
      *                                      coupled_streams</code>) must be no
      *                                      more than the number of input channels.
      * @param[in] mapping <code>const unsigned char[channels]</code>: Mapping from
      *                    encoded channels to input channels, as described in
      *                    @ref opus_multistream. As an extra constraint, the
      *                    multistream encoder does not allow encoding coupled
      *                    streams for which one channel is unused since this
      *                    is never a good idea.
      * @param application <tt>int</tt>: The target encoder application.
      *                                  This must be one of the following:
      * <dl>
      * <dt>#OPUS_APPLICATION_VOIP</dt>
      * <dd>Process signal for improved speech intelligibility.</dd>
      * <dt>#OPUS_APPLICATION_AUDIO</dt>
      * <dd>Favor faithfulness to the original input.</dd>
      * <dt>#OPUS_APPLICATION_RESTRICTED_LOWDELAY</dt>
      * <dd>Configure the minimum possible coding delay by disabling certain modes
      * of operation.</dd>
      * </dl>
      * @param[out] error <tt>int *</tt>: Returns #OPUS_OK on success, or an error
      *                                   code (see @ref opus_errorcodes) on
      *                                   failure.
      */
    //1.Sampling rate of the input signal (in Hz).48000.
    //2.Number of channels in the input signal.2 channels.
    //3.The total number of streams to encode from the input.This must be no more than the number of channels. 0.
    //4.
    /*
     * opus_int32 Fs,
      int channels,
      int mapping_family,
      int *streams,
      int *coupled_streams,
      unsigned char *mapping,
      int application,
      int *error
      */
    int err;
    int mapping_family=0;
    int streams=1;
    int coupled_streams=1;
    unsigned char stream_map[255];
    //Sampling rate of the input signal (in Hz).
    //This must be one of 8000, 12000, 16000,24000, or 48000.
    OpusMSEncoder *encoder=opus_multistream_surround_encoder_create(48000,///<
                                                                    2,///<
                                                                    mapping_family,///<
                                                                    &streams,///<
                                                                    &coupled_streams,///<
                                                                    stream_map,///<
                                                                    OPUS_APPLICATION_AUDIO,///<
                                                                    &err);
    if(err!=OPUS_OK || encoder==NULL)
    {
        printf("error at create opus encode:%s.\n",opus_strerror(err));
        g_bExitFlag=1;
        return NULL;
    }

#ifdef ZDECODER
    //param1:Fs <tt>opus_int32</tt>: Sampling rate to decode at (in Hz).This must be one of 8000, 12000, 16000,24000, or 48000.
    //param2:channels <tt>int</tt>: Number of channels to output.
    //param3:streams <tt>int</tt>: The total number of streams coded in the input.This must be no more than 255.
    //param4:coupled_streams <tt>int</tt>: Number of streams to decode as coupled (2 channel) streams.
    //param5:mapping <code>const unsigned char[channels]</code>.
    //return:@param[out] error <tt>int *</tt>: Returns #OPUS_OK on success.
    OpusMSDecoder *decoder=opus_multistream_decoder_create(48000,///<
                                                           2,///<
                                                           streams,///<
                                                           coupled_streams,///<
                                                           stream_map,///<
                                                           &err);
    if(err!=OPUS_OK || decoder==NULL)
    {
        printf("error at create opus decoder:%s.\n",opus_strerror(err));
        g_bExitFlag=1;
        return NULL;
    }
    char *cBufOpusDec=(char*)malloc(480*sizeof(opus_int16)*2*2);
    if(cBufOpusDec==NULL)
    {
        printf("error at allocate cBufOpusDec\n");
        g_bExitFlag=1;
        return NULL;
    }
#endif

    printf("gThreadNs:enter loop\n");
    while(!g_bExitFlag)
    {
        int len;
        char buffer[480*sizeof(opus_int16)*2];
        //1.read pcm from fifo.
        len=read(fd_noise,buffer,sizeof(buffer));
        if(len<0)
        {
            printf("error at read %s\n",FILE_NOISE);
            break;
        }
        //2.process pcm.
        switch(gCamPara.m_iDeNoise)
        {
        case 0://DeNoise Disabled.
            break;
        case 1://RNNoise.
            //libns.so only process 960 bytes each time.
            ns_processing(buffer,sizeof(buffer));
            break;
        case 2://WebRTC.
            //libns.so only process 960 bytes each time.
            ns_processing(buffer,sizeof(buffer));
            break;
        case 3://NRAE.
            ns_processing(buffer,sizeof(buffer));
            break;
        default:
            break;
        }

        //4.try to write pcm to zsy.opus.
        //param1:<tt>OpusMSEncoder*</tt>: Multistream encoder state.
        //param2:<tt>const opus_int16*</tt>: The input signal as interleaved samples.
        //       This must contain <code>frame_size*channels</code> samples.
        //param3:<tt>int</tt>:Number of samples per channel in the input signal.
        //       This must be an Opus frame size for the encoder's sampling rate.
        //       For example, at 48 kHz the permitted values are 120, 240, 480, 960, 1920, and 2880.
        //       Passing in a duration of less than 10 ms (480 samples at 48 kHz) will prevent the encoder from using the LPC or hybrid modes.
        //param4:<tt>unsigned char*</tt>:Output payload.This must contain storage for at least max_data_bytes.
        //param5:<tt>opus_int32</tt>: Size of the allocated memory for the output  payload.
        //       This may be used to impose an upper limit on the instant bitrate, but should not be used as the only bitrate control.
        //       Use #OPUS_SET_BITRATE to control the bitrate.
        int nBytes=opus_multistream_encode(encoder,///<
                                           //frame_size*channels.
                                           //frame_size=480*sizeof(opus_int16).
                                           (const opus_int16*)buffer,///<
                                           //Number of samples per channel in the input signal.
                                           //each sample is 480*sizeof(opus_int16).
                                           480*sizeof(opus_int16),///<
                                           pOpusEnc,///<
                                           480*sizeof(opus_int16)*2*2);
        if(nBytes<0)
        {
            printf("error at opus_encode():%s\n",opus_strerror(nBytes));
        }else if(nBytes==0)
        {
            printf("warning,opus encode bytes is zero.\n");
        }else if(nBytes>0){
	    //int nBytesHtonl=htonl(nBytes);
#ifdef ZDEBUG	   
            printf("opus encoder:%d,%08x\n",nBytes,nBytes);
#endif
            len=write(fd_opus,&nBytes,sizeof(nBytes));
            if(len<0)
            {
#ifdef ZDEBUG
                printf("broken fifo at write len:%s\n",FILE_OPUS);
#endif
            }
            len=write(fd_opus,pOpusEnc,nBytes);
            if(len<0)
            {
#ifdef ZDEBUG
                printf("broken fifo at write data:%s\n",FILE_OPUS);
#endif
            }

#ifdef ZDECODER
            //decoder & write to zsy.clean for local playback.
            //param1:<tt>OpusMSDecoder*</tt>: Multistream decoder state.
            //param2:<tt>const unsigned char*</tt>: Input payload.
            //       Use a <code>NULL</code> pointer to indicate packet loss.
            //param3:<tt>opus_int32</tt>: Number of bytes in payload.
            //param4:<tt>opus_int16*</tt>: Output signal, with interleaved samples.
            //       This must contain room for <code>frame_size*channels</code> samples.
            //param5:<tt>int</tt>: The number of samples per channel of available space in \a pcm.
            //param6:decode_fec <tt>int</tt>: Flag (0 or 1) to request that any in-band
            //       forward error correction data be decoded.If no such data is available, the frame is decoded as if it were lost.
            //@returns Number of samples decoded on success or a negative error code.
            int iDecBytes=opus_multistream_decode(decoder,///<
                                                  pOpusEnc,///<
                                                  nBytes,///<
                                                  (opus_int16*)cBufOpusDec,///<
                                                  480*sizeof(opus_int16),///<
                                                  0);
            if(iDecBytes>0)
            {
                printf("decoder %d bytes\n",iDecBytes);
                //write decoder pcm to zsy.clean for local playback.
        	len=write(fd_clean,buffer,sizeof(buffer));
        	if(len<0)
		{
			printf("failed to write zsy.clean!\n");
		}
            }else{
		    printf("decoder failed!\n");
	    }
#else
            //write pcm to zsy.clean for local playback.
            len=write(fd_clean,buffer,sizeof(buffer));
            if(len<0)
            {
                printf("error at write %s\n",FILE_CLEAN);
            }
#endif
        }
    }
    close(fd_noise);
    close(fd_clean);
    close(fd_opus);
    free(pOpusEnc);
    opus_multistream_encoder_destroy(encoder);
#ifdef ZDECODER
    opus_multistream_decoder_destroy(decoder);
#endif
    printf("gThreadNs:exit.\n");
    return NULL;
}
void *gThreadJson(void *para)
{
    //for mkfifo in&out.
    int fd_json_rx,fd_json_tx;
    fd_json_rx=open(FILE_JSON_RX,O_RDWR);
    if(fd_json_rx<0)
    {
        printf("failed to open %s\n",FILE_JSON_RX);
        return NULL;
    }
    fd_json_tx=open(FILE_JSON_TX,O_RDWR);
    if(fd_json_tx<0)
    {
        printf("failed to open %s\n",FILE_JSON_TX);
        return NULL;
    }

    //load  zctrl.json.
    gLoadCfgFile(&gCamPara);

    printf("gThreadJson:enter loop\n");
    while(!g_bExitFlag)
    {
        int len=0;
        int iJsonLen=0;
        char cJsonRx[512];
        //1.read 4 bytes json length from rx fifo.
        len=read(fd_json_rx,&iJsonLen,sizeof(iJsonLen));
        if(len<0)
        {
            printf("error at read %s\n",FILE_JSON_RX);
            break;
        }
        if(iJsonLen<=0 || iJsonLen>=512)
        {
            //something wrong.
            usleep(1000*100);
            continue;
        }
        printf("len:%d\n",iJsonLen);

        //2.read N bytes json data from rx fifo.
        len=read(fd_json_rx,cJsonRx,iJsonLen);
        if(len<=0)
        {
            printf("error at read %s\n",FILE_JSON_RX);
	    continue;
        }
        cJsonRx[len]='\0';

        //3.parse out json.
        printf("%s\n\n",cJsonRx);
        (void)gParseJson(cJsonRx,len,fd_json_tx);

        //reduce heavy cpu load.
        usleep(1000*100);
    }
    close(fd_json_rx);
    close(fd_json_tx);
    printf("gThreadJson:exit.\n");
    return NULL;
}
int gParseJson(char *jsonData,int jsonLen,int fd)
{
    int bWrCfgFile=1;
    //Ns:Noise Suppression Progress.
    (void)jsonLen;
    cJSON *rootRx=cJSON_Parse(jsonData);
    if(rootRx==NULL)
    {
        printf("parse error\n");
        return -1;
    }
    cJSON *jCam1CenterXY=cJSON_GetObjectItem(rootRx,"Cam1CenterXY");
    if(jCam1CenterXY)
    {
        char *cValue=cJSON_Print(jCam1CenterXY);
        if(!strcmp(cValue,"query"))
        {
            //only query,no need to write.
            bWrCfgFile=0;
        }else{
            if(strlen(cValue)<sizeof(gCamPara.m_cam1xy))
            {
                strcpy(gCamPara.m_cam1xy,cValue);
            }else{
                printf("too long Cam1CenterXY!\n");
            }
        }
        //write feedback to tx fifo.
        cJSON *rootTx=cJSON_CreateObject();
        cJSON *item=cJSON_CreateString(gCamPara.m_cam1xy);
        cJSON_AddItemToObject(rootTx,"Cam1CenterXY",item);
        char *pJson=cJSON_Print(rootTx);
        int iJsonLen=strlen(pJson);
        write(fd,&iJsonLen,sizeof(iJsonLen));
        write(fd,pJson,iJsonLen);
        cJSON_Delete(rootTx);
    }
    cJSON *jCam2CenterXY=cJSON_GetObjectItem(rootRx,"Cam2CenterXY");
    if(jCam2CenterXY)
    {
        char *cValue=cJSON_Print(jCam2CenterXY);
        if(!strcmp(cValue,"query"))
        {
            //only query,no need to write.
            bWrCfgFile=0;
        }else{
            if(strlen(cValue)<sizeof(gCamPara.m_cam2xy))
            {
                strcpy(gCamPara.m_cam2xy,cValue);
            }else{
                printf("too long Cam2CenterXY!\n");
            }
        }
        //write feedback to tx fifo.
        cJSON *rootTx=cJSON_CreateObject();
        cJSON *item=cJSON_CreateString(gCamPara.m_cam2xy);
        cJSON_AddItemToObject(rootTx,"Cam2CenterXY",item);
        char *pJson=cJSON_Print(rootTx);
        int iJsonLen=strlen(pJson);
        write(fd,&iJsonLen,sizeof(iJsonLen));
        write(fd,pJson,iJsonLen);
        cJSON_Delete(rootTx);
    }
    cJSON *jCam3CenterXY=cJSON_GetObjectItem(rootRx,"Cam3CenterXY");
    if(jCam3CenterXY)
    {
        char *cValue=cJSON_Print(jCam3CenterXY);
        if(!strcmp(cValue,"query"))
        {
            //only query,no need to write.
            bWrCfgFile=0;
        }else{
            if(strlen(cValue)<sizeof(gCamPara.m_cam3xy))
            {
                strcpy(gCamPara.m_cam3xy,cValue);
            }else{
                printf("too long Cam3CenterXY!\n");
            }
        }
        //write feedback to tx fifo.
        cJSON *rootTx=cJSON_CreateObject();
        cJSON *item=cJSON_CreateString(gCamPara.m_cam2xy);
        cJSON_AddItemToObject(rootTx,"Cam3CenterXY",item);
        char *pJson=cJSON_Print(rootTx);
        int iJsonLen=strlen(pJson);
        write(fd,&iJsonLen,sizeof(iJsonLen));
        write(fd,pJson,iJsonLen);
        cJSON_Delete(rootTx);
    }
    //"DeNoise":"off/Strong/WebRTC/mmse/Bevis/NRAE/query"
    cJSON *jDeNoise=cJSON_GetObjectItem(rootRx,"DeNoise");
    if(jDeNoise)
    {
        char *cValue=cJSON_Print(jDeNoise);
        if(!strcmp(cValue,"query"))
        {
            //only query,no need to write.
            bWrCfgFile=0;
        }else if(!strcmp(cValue,"off")){
            gCamPara.m_iDeNoise=0;
        }else if(!strcmp(cValue,"Strong")){
            gCamPara.m_iDeNoise=1;
        }else if(!strcmp(cValue,"WebRTC")){
            gCamPara.m_iDeNoise=2;
        }else if(!strcmp(cValue,"NRAE")){
            gCamPara.m_iDeNoise=3;
        }
        //write feedback to tx fifo.
        cJSON *rootTx=cJSON_CreateObject();
        cJSON *item=cJSON_CreateString(cValue);
        cJSON_AddItemToObject(rootTx,"DeNoise",item);
        char *pJson=cJSON_Print(rootTx);
        int iJsonLen=strlen(pJson);
        write(fd,&iJsonLen,sizeof(iJsonLen));
        write(fd,pJson,iJsonLen);
        cJSON_Delete(rootTx);
    }
    //"StrongMode":"mode1/mode2/mode3/mode4/mode5/mode6/mode7/mode8/mode9/mode10/query"
    cJSON *jStrongMode=cJSON_GetObjectItem(rootRx,"StrongMode");
    if(jStrongMode)
    {
        char *cValue=cJSON_Print(jStrongMode);
        if(!strcmp(cValue,"query"))
        {
            //only query,no need to write.
            bWrCfgFile=0;
        }else if(!strcmp(cValue,"mode1")){
            gCamPara.m_iStrongMode=1;
        }else if(!strcmp(cValue,"mode2")){
            gCamPara.m_iStrongMode=2;
        }else if(!strcmp(cValue,"mode3")){
            gCamPara.m_iStrongMode=3;
        }else if(!strcmp(cValue,"mode4")){
            gCamPara.m_iStrongMode=4;
        }else if(!strcmp(cValue,"mode5")){
            gCamPara.m_iStrongMode=5;
        }else if(!strcmp(cValue,"mode6")){
            gCamPara.m_iStrongMode=6;
        }else if(!strcmp(cValue,"mode7")){
            gCamPara.m_iStrongMode=7;
        }else if(!strcmp(cValue,"mode8")){
            gCamPara.m_iStrongMode=8;
        }else if(!strcmp(cValue,"mode9")){
            gCamPara.m_iStrongMode=9;
        }else if(!strcmp(cValue,"mode10")){
            gCamPara.m_iStrongMode=10;
        }
        //4.write feedback to tx fifo.
        cJSON *root=cJSON_CreateObject();
        cJSON *item=cJSON_CreateString(cValue);
        cJSON_AddItemToObject(root,"StrongMode",item);
        char *pJson=cJSON_Print(root);
        int iJsonLen=strlen(pJson);
        write(fd,&iJsonLen,sizeof(iJsonLen));
        write(fd,pJson,iJsonLen);
        cJSON_Delete(root);
    }

    cJSON *jSpkPlaybackVol=cJSON_GetObjectItem(rootRx,"SpkPlaybackVol");
    if(jSpkPlaybackVol)
    {
        char *cValue=cJSON_Print(jSpkPlaybackVol);
        printf("SpkPlaybackVol:%s\n",cValue);
        //numid=682,iface=MIXER,name='MVC1 Vol'
        //  ; type=INTEGER,access=rw------,values=1,min=0,max=16000,step=0
        //  : values=13000
        int iValue=atoi(cValue);
        if(iValue>=0 && iValue<=30)
        {
            char command[512];
            sprintf(command,"amixer -c tegrasndt186ref cset name=\"MVC1 Vol\" %d",iValue*534);
            system(command);
        }
        //write feedback to tx fifo.
        cJSON *root=cJSON_CreateObject();
        cJSON *item=cJSON_CreateString(cValue);
        cJSON_AddItemToObject(root,"SpkPlaybackVol",item);
        char *pJson=cJSON_Print(root);
        int iJsonLen=strlen(pJson);
        write(fd,&iJsonLen,sizeof(iJsonLen));
        write(fd,pJson,iJsonLen);
        cJSON_Delete(root);
    }
    cJSON_Delete(rootRx);

    if(bWrCfgFile)
    {
        gSaveCfgFile(&gCamPara);
    }
    return 0;
}
int gLoadCfgFile(ZCamPara *para)
{
    int fd;
    char buffer[256];
    int len;
    if(access(FILE_JSON_CFG,F_OK)<0)
    {
        strcpy(para->m_cam1xy,"0,0");
        strcpy(para->m_cam2xy,"0,0");
        strcpy(para->m_cam3xy,"0,0");
        para->m_iDeNoise=0;
        para->m_iStrongMode=1;
        return -1;
    }

    if((fd=open(FILE_JSON_CFG,O_RDWR))<0)
    {
        printf("failed to open %s\n",FILE_JSON_CFG);
        return -1;
    }
    len=read(fd,buffer,sizeof(buffer));
    if(len<0)
    {
        printf("failed to read %s\n",FILE_JSON_CFG);
        return -1;
    }
    buffer[len]='\0';
    close(fd);

    //parse out.
    cJSON *root=cJSON_Parse(buffer);
    if(root==NULL)
    {
        printf("parse error\n");
        return -1;
    }
    cJSON *jCam1CenterXY=cJSON_GetObjectItem(root,"Cam1CenterXY");
    if(jCam1CenterXY)
    {
        char *cValue=cJSON_Print(jCam1CenterXY);
        if(strlen(cValue)<sizeof(para->m_cam1xy))
        {
            strcpy(para->m_cam1xy,cValue);
        }else{
            printf("Cam1CenterXY is too long!\n");
            strcpy(para->m_cam1xy,"0,0");
        }
    }
    cJSON *jCam2CenterXY=cJSON_GetObjectItem(root,"Cam2CenterXY");
    if(jCam2CenterXY)
    {
        char *cValue=cJSON_Print(jCam2CenterXY);
        if(strlen(cValue)<sizeof(para->m_cam2xy))
        {
            strcpy(para->m_cam2xy,cValue);
        }else{
            printf("Cam2CenterXY is too long!\n");
            strcpy(para->m_cam2xy,"0,0");
        }
    }
    cJSON *jCam3CenterXY=cJSON_GetObjectItem(root,"Cam3CenterXY");
    if(jCam3CenterXY)
    {
        char *cValue=cJSON_Print(jCam3CenterXY);
        if(strlen(cValue)<sizeof(para->m_cam3xy))
        {
            strcpy(para->m_cam3xy,cValue);
        }else{
            printf("Cam3CenterXY is too long!\n");
            strcpy(para->m_cam3xy,"0,0");
        }
    }
    cJSON_Delete(root);
    return 0;
}
int gSaveCfgFile(ZCamPara *para)
{
    int fd;
    if((fd=open(FILE_JSON_CFG,O_RDWR|O_CREAT))<0)
    {
        printf("failed to open %s\n",FILE_JSON_CFG);
        return -1;
    }
    cJSON *root=cJSON_CreateObject();

    cJSON *item1=cJSON_CreateString(para->m_cam1xy);
    cJSON_AddItemToObject(root,"Cam1CenterXY",item1);

    cJSON *item2=cJSON_CreateString(para->m_cam2xy);
    cJSON_AddItemToObject(root,"Cam2CenterXY",item2);

    cJSON *item3=cJSON_CreateString(para->m_cam3xy);
    cJSON_AddItemToObject(root,"Cam2CenterXY",item3);

    char *pJson=cJSON_Print(root);
    write(fd,pJson,strlen(pJson));
    close(fd);
    cJSON_Delete(root);
    return 0;
}
/**************the end of file**********************/
