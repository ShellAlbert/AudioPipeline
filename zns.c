#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "libns/libns.h"
//data flow.
//arecord -> zsy.noise -> zns ->zsy.clean -> aplay
//                            ->zsy.opus  -> android
#define FILE_NOISE	"/tmp/zsy.noise"
#define FILE_CLEAN	"/tmp/zsy.clean"
#define FILE_OPUS	"/tmp/zsy.opus"

//json ctrl.
#include "cJSON.h"
#define FILE_JSON_RX "/tmp/zsy.json.rx"
#define FILE_JSON_TX "/tmp/zsy.json.tx"
#define FILE_JSON_CFG "zctrl.json"

//pid file.
#define FILE_PID    "/tmp/zns.pid"
int g_bExitFlag=0;
int g_nDenoise=0;
typedef struct
{
    char m_cam1xy[32];
    char m_cam2xy[32];
    char m_cam3xy[32];
    char m_denoise[10];
    char m_strongMode[10];
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
        return NULL;
    }
    fd_clean=open(FILE_CLEAN,O_RDWR);
    if(fd_clean<0)
    {
        printf("failed to open %s\n",FILE_CLEAN);
        return NULL;
    }
    fd_opus=open(FILE_OPUS,O_RDWR|O_NONBLOCK);
    if(fd_opus<0)
    {
        printf("failed to open %s\n",FILE_OPUS);
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
        return NULL;
    }

    printf("gThreadNs:enter loop\n");
    while(!g_bExitFlag)
    {
        int len;
        char buffer[960];
        //1.read pcm from fifo.
        len=read(fd_noise,buffer,sizeof(buffer));
        if(len<0)
        {
            printf("error at read %s\n",FILE_NOISE);
            break;
        }
        //2.process pcm.
        switch(g_nDenoise)
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

        //3.write pcm to zsy.clean for local playback.
        len=write(fd_clean,buffer,sizeof(buffer));
        if(len<0)
        {
            printf("error at write %s\n",FILE_CLEAN);
            break;
        }

        //4.try to write pcm to zsy.opus.
        len=write(fd_opus,buffer,sizeof(buffer));
        if(len<0)
        {
            printf("broken fifo %s\n",FILE_OPUS);
        }
    }
    close(fd_noise);
    close(fd_clean);
    close(fd_opus);
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
        if(iJsonLen==0 || iJsonLen>=512)
        {
            //something wrong.
            usleep(1000*100);
            continue;
        }
        printf("len:%d\n",iJsonLen);

        //2.read N bytes json data from rx fifo.
        len=read(fd_json_rx,cJsonRx,iJsonLen);
        if(len<0)
        {
            printf("error at read %s\n",FILE_JSON_RX);
            break;
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

    cJSON *jDeNoise=cJSON_GetObjectItem(rootRx,"DeNoise");
    if(jDeNoise)
    {
        char *cValue=cJSON_Print(jDeNoise);
        if(!strcmp(cValue,"query"))
        {
            //only query,no need to write.
            bWrCfgFile=0;
        }else{
            if(strlen(cValue)<sizeof(gCamPara.m_denoise))
            {
                strcpy(gCamPara.m_denoise,cValue);
            }else{
                printf("too long DeNoise!\n");
            }
        }
        //write feedback to tx fifo.
        cJSON *rootTx=cJSON_CreateObject();
        cJSON *item=cJSON_CreateString(gCamPara.m_denoise);
        cJSON_AddItemToObject(rootTx,"DeNoise",item);
        char *pJson=cJSON_Print(rootTx);
        int iJsonLen=strlen(pJson);
        write(fd,&iJsonLen,sizeof(iJsonLen));
        write(fd,pJson,iJsonLen);
        cJSON_Delete(rootTx);
    }

    cJSON *jStrongMode=cJSON_GetObjectItem(rootRx,"StrongMode");
    if(jStrongMode)
    {
        char *cValue=cJSON_Print(jStrongMode);
        if(!strcmp(cValue,"query"))
        {
            //only query,no need to write.
            bWrCfgFile=0;
        }else{
            if(strlen(cValue)<sizeof(gCamPara.m_strongMode))
            {
                strcpy(gCamPara.m_strongMode,cValue);
            }else{
                printf("too long StrongMode!\n");
            }
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
    if((fd=open(FILE_JSON_CFG,O_RDWR))<0)
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
    cJSON_Delete(root);
    return 0;
}
/**************the end of file**********************/
