
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <arpa/inet.h>

#include <opus/opus.h>
#include <opus/opus_multistream.h>
#include <opus/opus_defines.h>
#include <opus/opus_types.h>

#include "libns/libns.h"
#include "zcvt.h"

//data flow.
//zsy.noise(i2s1 input) -> zns(sample rate convert & noise cut) -> zsy.clean(local play)
//                                                              -> zsy.clean2(opus enc & tx) -> TcpClient
#define FILE_NOISE				"/tmp/zsy/zsy.noise"
#define FILE_NOISE2				"/tmp/zsy/zsy.noise2"
#define FILE_CLEAN				"/tmp/zsy/zsy.clean"
#define FILE_CLEAN2				"/tmp/zsy/zsy.clean2"

//json ctrl.
#include "cJSON.h"
#define FILE_JSON_RX			"/tmp/zsy/zsy.json.rx"
#define FILE_JSON_TX			"/tmp/zsy/zsy.json.tx"
#define FILE_JSON_CFG			"zns.json"

//pid file.
#define FILE_PID				"/tmp/zsy/zns.pid"

//Android APP use 48khz,so here keep 48khz.
#define FRAME_SIZE				960
#define SAMPLE_RATE 			48000
#define CHANNELS				2
#define APPLICATION 			OPUS_APPLICATION_AUDIO
#define BITRATE 				64000
#define MAX_FRAME_SIZE			6*960
#define MAX_PACKET_SIZE 		(3*1276)

//i2s in.
//32khz,32bit
#define BYTES_32BITS    4 //32bit=4bytes.
#define BYTES_16BITS  2//16bit=2bytes.
typedef struct {
	char			m_cam1xy[32];
	char			m_cam2xy[32];
	char			m_cam3xy[32];

	//"DeNoise":"off/Strong/WebRTC/query"
	int 			m_iDeNoise;

	//"StrongMode":"mode1/mode2/mode3/mode4/mode5/mode6/mode7/mode8/mode9/mode10/query"
	int 			m_iStrongMode;
	int 			m_iStrongModeShadow;

	//"WebRtcGrade":"0/1/2"
	int			m_iWebRtcGrade;
	int			m_iWebRtcGradeShadow;
} ZCamPara;


ZCamPara		gCamPara;
int 			g_bExitFlag = 0;
int 			g_bOpusConnectedFlag = 0;
int 			g_iOpusFd;

//Demodulate Mode
//0:32khz i2s1 input.
//1:25khz analog capture input.
int 			g_DeMode = 0;

void gSIGHandler(int sigNo)
{
	switch (sigNo)
	{
		case SIGINT:
		case SIGKILL:
		case SIGTERM:
			printf("zns:got Ctrl-C...\n");
			g_bExitFlag = 1;
			g_bOpusConnectedFlag = 0;
			break;

		default:
			break;
	}
}


int gLoadCfgFile(ZCamPara * para);
int gSaveCfgFile(ZCamPara * para);
void * gThreadJson(void * para);
int gParseJson(char * jsonData, int jsonLen, int fd);
void * gThreadNs(void * para);
void * gThreadEncTx(void * para);
void * gThreadTCP(void * para);


int main(int argc, char * *argv)
{
	pthread_t		tidJson, tidNs, tidEncTx, tidTCP;

	//write my pid to file.
	int 			fd	= open(FILE_PID, O_RDWR | O_CREAT);

	if (fd < 0) {
		fprintf(stderr, "failed to open %s.\n", FILE_PID);
		return - 1;
	}

	char			pidBuffer[32];

	memset(pidBuffer, 0, sizeof(pidBuffer));
	sprintf(pidBuffer, "%d", getpid());
	write(fd, pidBuffer, strlen(pidBuffer));
	close(fd);

	//Set the signal callback for Ctrl-C.
	signal(SIGINT, gSIGHandler);
	signal(SIGPIPE, gSIGHandler);

	sigset_t		sigMask;

	sigemptyset(&sigMask);
	sigaddset(&sigMask, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &sigMask, NULL);


	//create threads.
	if (pthread_create(&tidJson, NULL, gThreadJson, NULL)) {
		fprintf(stderr, "failed to create json thread!\n");
		return - 1;
	}

	if (pthread_create(&tidNs, NULL, gThreadNs, NULL)) {
		fprintf(stderr, "failed to create ns thread!\n");
		return - 1;
	}

	if (pthread_create(&tidEncTx, NULL, gThreadEncTx, NULL)) {
		fprintf(stderr, "failed to create enctx thread!\n");
		return - 1;
	}

	if (pthread_create(&tidTCP, NULL, gThreadTCP, NULL)) {
		fprintf(stderr, "failed to create tcp thread!\n");
		return - 1;
	}


	pthread_join(tidJson, NULL);
	pthread_join(tidNs, NULL);
	pthread_join(tidEncTx, NULL);
	pthread_join(tidTCP, NULL);

	fprintf(stdout, "zns exit.\n");
	return 0;
}
void * gThreadNs(void * para)
{
	//for mkfifo in&out.
	//fd_noise(arecord i2s1)    ->  zns(sample rate convert & noise cut)->zsy.clean(i2s0 local play)
	//fd_noise2(analog capture input)->zns(sample rate convert & noise cut)->zsy.clean2(opus enc & tx)
	int 			fd_noise, fd_noise2,fd_clean,fd_clean2;

	fd_noise			= open(FILE_NOISE, O_RDONLY);

	if (fd_noise < 0) {
		fprintf(stderr, "failed to open %s\n", FILE_NOISE);
		g_bExitFlag 		= 1;
		return NULL;
	}
	fd_noise2			= open(FILE_NOISE2, O_RDONLY);

	if (fd_noise2 < 0) {
		fprintf(stderr, "failed to open %s\n", FILE_NOISE2);
		g_bExitFlag 		= 1;
		return NULL;
	}

	fd_clean			= open(FILE_CLEAN, O_RDWR);

	if (fd_clean < 0) {
		fprintf(stderr, "failed to open %s\n", FILE_CLEAN);
		g_bExitFlag 		= 1;
		return NULL;
	}

	fd_clean2			= open(FILE_CLEAN2, O_RDWR);

	if (fd_clean2 < 0) {
		fprintf(stderr, "failed to open %s\n", FILE_CLEAN2);
		g_bExitFlag 		= 1;
		return NULL;
	}
	//sample rate convret.
	zcvt_init();

	//for libns.
	//here call ns_init() or ns_custom_init() for call ns_uninit() later.
	gCamPara.m_iDeNoise=0;
	gCamPara.m_iStrongMode=0;
	gCamPara.m_iStrongModeShadow=0;
	gCamPara.m_iWebRtcGrade=0;
	gCamPara.m_iWebRtcGradeShadow=0;
	ns_init(0); 									//0~5.

	fprintf(stdout, "gThreadNs:enter loop\n");

	while (!g_bExitFlag) {
		int 		iErrFlag = 0;

		char		    pcm3232TwoCh[1280*2];
		char            pcm3232LftCh[1280];
		char            pcm3232RhtCh[1280];


		//opus_encoder() has special requirement,so we choose 10ms as the encode data block.
		//48khz,16bit,2ch:so 1s data size=48khz*16bit*2ch=192000 bytes= 192000/2=96000 short.
		//10ms = 1s(1000ms)/100.
		//so 10ms data size=96000 short/100=960 short(stereo), 960 short/2 ch=480 short(1ch).

		//we need (480 short * 16 bits * 2 ch) bytes to fill pcm4816TwoCh full.
		//thus sample rate converter need different data size.
		//from 32khz32bit to 48khz16bit: (320*4=1280) -> (480*2=960)
		//so, to get 960 bytes 48khz16bit data, we need 1280 bytes 32khz32bit data.
		char            pcm4816LftCh[480*BYTES_16BITS];
		char            pcm4816RhtCh[480*BYTES_16BITS];
		char            pcm4816TwoCh[480*CHANNELS*BYTES_16BITS];

		if(!g_DeMode)
		{
			//1.read 32khz,32bit,2 ch pcm from zsy.noise (i2s1 input)
			int 			iOffset = 0;
			int 			iNeedBytes = 1280*2; 
			while (iNeedBytes > 0) {
				int 			iRdBytes = read(fd_noise, pcm3232TwoCh + iOffset, iNeedBytes);

				if (iRdBytes < 0) {
					fprintf(stderr, "failed to read opus!\n");
					iErrFlag			= 1;
					break;
				}

				iNeedBytes			-= iRdBytes;
				iOffset 			+= iRdBytes;
			}

			if (iErrFlag) {
				break;
			}

			//2.split stereo to single channel.
			int iLftChIndex=0,iRhtChIndex=0;
			int iChFlag=0;
			for(int i=0;i<(1280*2);i+=4)
			{
				if(iChFlag)
				{
					memcpy(&pcm3232LftCh[iLftChIndex],&pcm3232TwoCh[i],4);
					iLftChIndex+=4;
				}else{
					memcpy(&pcm3232RhtCh[iRhtChIndex],&pcm3232TwoCh[i],4);
					iRhtChIndex+=4;
				}
				iChFlag=!iChFlag;
			}

			//3.do 32khz/32bit to 48khz/16bit convert.
			//input each size is 320*4=1280, output each size is 480*2=960.
			zcvt_3232_to_4816(pcm3232LftCh,320*4,pcm4816LftCh,480*2);
			zcvt_3232_to_4816(pcm3232RhtCh,320*4,pcm4816RhtCh,480*2);
		}else{
			//1.read 48khz,16bit,2 ch pcm from zsy.noise2 (analog input)
			int                     iOffset = 0;
			int                     iNeedBytes = 480*CHANNELS*BYTES_16BITS;
			while (iNeedBytes > 0) {
				int                     iRdBytes = read(fd_noise2, pcm4816TwoCh + iOffset, iNeedBytes);

				if (iRdBytes < 0) {
					fprintf(stderr, "failed to read opus!\n");
					iErrFlag                        = 1;
					break;
				}

				iNeedBytes                      -= iRdBytes;
				iOffset                         += iRdBytes;
			}

			if (iErrFlag) {
				break;
			}
			//2.split stereo to single channel.
			int iLftChIndex=0,iRhtChIndex=0;
			int iChFlag=0;
			for(int i=0;i<(480*CHANNELS*BYTES_16BITS);i+=4)
			{
				if(iChFlag)
				{
					memcpy(&pcm4816LftCh[iLftChIndex],&pcm4816TwoCh[i],4);
					iLftChIndex+=4;
				}else{
					memcpy(&pcm4816RhtCh[iRhtChIndex],&pcm4816TwoCh[i],4);
					iRhtChIndex+=4;
				}
				iChFlag=!iChFlag;
			}

		}
		//4.noise suppression apply on each channel.
		switch (gCamPara.m_iDeNoise)
		{
			case 0: //DeNoise Disabled.(off)
				break;

			case 1: //RNNoise.(Strong)
				//libns.so only process 960 bytes each time in 48khz,16bit.
				if(gCamPara.m_iStrongMode!=gCamPara.m_iStrongModeShadow)
				{
					ns_uninit();
					ns_init(gCamPara.m_iStrongMode);
					gCamPara.m_iStrongModeShadow=gCamPara.m_iStrongMode;
				}

				ns_processing(pcm4816LftCh,480*BYTES_16BITS);
				ns_processing(pcm4816RhtCh,480*BYTES_16BITS);
				break;

			case 2: //WebRTC.(WebRtc)
				//libns.so only process 960 bytes each time in 48khz,16bit.
				if(gCamPara.m_iWebRtcGrade!=gCamPara.m_iWebRtcGradeShadow)
				{
					ns_uninit();
					char customBandGains[8]={0};
					ns_custom_init(6,gCamPara.m_iWebRtcGrade,0,0,customBandGains,0);
					gCamPara.m_iWebRtcGradeShadow=gCamPara.m_iWebRtcGrade;
				}
				ns_processing(pcm4816LftCh,480*BYTES_16BITS);
				ns_processing(pcm4816RhtCh,480*BYTES_16BITS);
				break;

			default:
				break;
		}

		//5.combine two channels to stereo.
		int iTwoChIndex=0;
		for(int i=0;i<480*BYTES_16BITS;i+=2)
		{
			memcpy(&pcm4816TwoCh[iTwoChIndex],&pcm4816LftCh[i],2);
			iTwoChIndex+=2;
			memcpy(&pcm4816TwoCh[iTwoChIndex],&pcm4816RhtCh[i],2);
			iTwoChIndex+=2;
		}

		//6. write pcm to zsy.clean for local playback.
		int 			iNeedWrBytes = sizeof(pcm4816TwoCh);
		int 			iWrOffset = 0;

		while (iNeedWrBytes > 0) {
			int 			iWrBytes = write(fd_clean, pcm4816TwoCh + iWrOffset, iNeedWrBytes);

			if (iWrBytes < 0) {
				fprintf(stderr, "failed to write fd_clean!\n");
				break;
			}

			iNeedWrBytes		-= iWrBytes;
			iWrOffset		+= iWrBytes;
		}
		if(g_bOpusConnectedFlag)
		{
			//7. write pcm to zsy.clean for opus enc & tx.
			int 			iNeedWrBytes2 = sizeof(pcm4816TwoCh);
			int 			iWrOffset2 = 0;

			while (iNeedWrBytes2 > 0) {
				int 			iWrBytes2 = write(fd_clean2, pcm4816TwoCh + iWrOffset2, iNeedWrBytes2);

				if (iWrBytes2 < 0) {
					fprintf(stderr, "failed to write fd_clean2!\n");
					break;
				}

				iNeedWrBytes2		-= iWrBytes2;
				iWrOffset2		+= iWrBytes2;
			}
		}
	}

	close(fd_noise);
	close(fd_noise2);
	close(fd_clean);
	close(fd_clean2);

	fprintf(stdout, "gThreadNs:exit.\n");
	return NULL;
}

void * gThreadEncTx(void * para)
{
	//for mkfifo in&out.
	int 			 fd_clean2;

	fd_clean2			= open(FILE_CLEAN2, O_RDONLY);

	if (fd_clean2 < 0) {
		fprintf(stderr, "failed to open %s\n", FILE_CLEAN2);
		g_bExitFlag 		= 1;
		return NULL;
	}

	/** Allocates and initializes a multistream encoder state.
	 * Call opus_multistream_encoder_destroy() to release
	 * this object when finished.

	 * @param coupled_streams <tt>int</tt>: Number of coupled (2 channel) streams
	 *									 to encode.
	 *									 This must be no larger than the total
	 *									 number of streams.
	 *									 Additionally, The total number of
	 *									 encoded channels (<code>streams +
	 *									 coupled_streams</code>) must be no
	 *									 more than the number of input channels.
	 * @param[in] mapping <code>const unsigned char[channels]</code>: Mapping from
	 *					 encoded channels to input channels, as described in
	 *					 @ref opus_multistream. As an extra constraint, the
	 *					 multistream encoder does not allow encoding coupled
	 *					 streams for which one channel is unused since this
	 *					 is never a good idea.
	 * @param application <tt>int</tt>: The target encoder application.
	 *								 This must be one of the following:
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
	 *									code (see @ref opus_errorcodes) on
	 *									failure.
	 */
	int 			err;

	//////////////////////encoder///////////////////////////
	OpusEncoder *	encoder;

	encoder 			= opus_encoder_create(48000, CHANNELS, OPUS_APPLICATION_AUDIO, &err);

	if (err != OPUS_OK || encoder == NULL) {
		fprintf(stderr, "error at create opus encode:%s.\n", opus_strerror(err));
		g_bExitFlag 		= 1;
		return NULL;
	}

	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));

	//////////////////////////decoder///////////////////////
	OpusDecoder *	decoder;

	decoder 			= opus_decoder_create(48000, CHANNELS, &err);

	if (err < 0 || decoder == NULL) {
		fprintf(stderr, "failed to create decoder: %s\n", opus_strerror(err));
		g_bExitFlag 		= 1;
		return NULL;
	}
	//opus_decoder_ctl(decoder, OPUS_SET_BITRATE(64000));

	while(!g_bExitFlag)
	{
		int               iErrFlag=0;
		char            pcm4816TwoCh[480*CHANNELS*BYTES_16BITS];
		//opus enc/dec.
		opus_int16		in4816_2Ch[480*CHANNELS];
		opus_int16		out[MAX_FRAME_SIZE * CHANNELS];
		unsigned char	cbits[MAX_PACKET_SIZE];

		//1.read pcm from zsy.clean2.
		int 			iOffset = 0;
		int 			iNeedBytes = 480*CHANNELS*BYTES_16BITS; 
		while (iNeedBytes > 0) {
			int 			iRdBytes = read(fd_clean2, pcm4816TwoCh + iOffset, iNeedBytes);

			if (iRdBytes < 0) {
				fprintf(stderr, "failed to read opus!\n");
				iErrFlag			= 1;
				break;
			}

			iNeedBytes			-= iRdBytes;
			iOffset 			+= iRdBytes;
		}

		if (iErrFlag) {
			break;
		}
		//2.convert from little-endian ordering to big-endian for encoding.
		for (int i = 0; i < (480*CHANNELS); i++) {
			in4816_2Ch[i]				= pcm4816TwoCh[2 * i + 1] << 8 | pcm4816TwoCh[2 * i];
		}

		//8.encode pcm to opus.
		//To encode a frame, opus_encode() or opus_encode_float() must be called with exactly one frame (2.5, 5, 10, 20, 40 or 60 ms) of audio data.
		//48khz,16bit,2ch
		//so 1s data size=48khz*16bit*2ch=192000 bytes= 192000/2=96000 short.
		//10ms = 1s(1000ms)/100.
		//so 10ms data size=96000 short/100=960 short(stereo), 960 short/2 ch=480 short(1ch).
		int 			iEncBytes = opus_encode(encoder, in4816_2Ch, 480, cbits, MAX_PACKET_SIZE);

		if (iEncBytes < 0) {
			fprintf(stderr, "encode failed: %s\n", opus_strerror(iEncBytes));
			continue;
		}
		//fprintf(stdout,"encode okay:%d bytes\n",iEncBytes);

		//9.tx opus(len+data) to APP.
		if (g_bOpusConnectedFlag) {
			int 			iEncBytesBE = iEncBytes /*htonl(iEncBytes)*/;
			int 			len = write(g_iOpusFd, &iEncBytesBE, sizeof(iEncBytesBE));

			if (len != sizeof(iEncBytesBE)) {
				fprintf(stderr, "tx opus len failed:%d,%ld\n", len, sizeof(iEncBytesBE));
				g_bOpusConnectedFlag = 0;
			}
			else {
				//fprintf(stdout, "opus len:%d\n", iEncBytes);
				int 			iNeedTxBytes = iEncBytes;
				int 			iTxOffset = 0;

				while (iNeedTxBytes > 0) {
					int 			iTxBytes = write(g_iOpusFd, cbits + iTxOffset, iNeedTxBytes);

					if (iTxBytes < 0) {
						fprintf(stderr, "tx opus data failed!\n");
						g_bOpusConnectedFlag = 0;
						break;
					}

					//fprintf(stdout, "tx %d bytes opus\n", iTxBytes);

					iNeedTxBytes		-= iTxBytes;
					iTxOffset			+= iTxBytes;
				}

			}
		}
#if 0
		//10.decode.
		/* Decode the data. In this example, frame_size will be constant because
		   the encoder is using a constant frame size. However, that may not
		   be the case for all encoders, so the decoder must always check
		   the frame size returned. */
		int 			iDecBlks = opus_decode(decoder, cbits, iEncBytes, out, MAX_FRAME_SIZE, 0);

		if (iDecBlks < 0) {
			fprintf(stderr, "decoder failed: %s\n", opus_strerror(iDecBlks));
			continue;
		}
		//fprintf(stdout,"decoder okay:%d blocks\n",iDecBlks);
#endif
#if 0
		/* Convert from big-endian to little-endian ordering for local play*/
		for (int i = 0; i < CHANNELS * iDecBlks; i++) {
			pcm4816TwoCh[2 * i]		= out[i] &0xFF;
			pcm4816TwoCh[2 * i + 1]	= (out[i] >> 8) & 0xFF;
		}
		//11. write pcm to zsy.clean for local playback.
		int                     iNeedWrBytes = sizeof(pcm4816TwoCh);
		int                     iWrOffset = 0;

		while (iNeedWrBytes > 0) {
			int                     iWrBytes = write(fd_clean, pcm4816TwoCh + iWrOffset, iNeedWrBytes);

			if (iWrBytes < 0) {
				fprintf(stderr, "failed to write fd_clean!\n");
				break;
			}

			iNeedWrBytes            -= iWrBytes;
			iWrOffset               += iWrBytes;
		}
#endif
	}
	opus_encoder_destroy(encoder);
	opus_decoder_destroy(decoder);
	close(fd_clean2);
	return NULL;
}
void * gThreadTCP(void * para)
{
	int 			fd	= socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		fprintf(stderr, "ERROR opening socket");
		g_bExitFlag 		= 1;
		return NULL;
	}

	/* setsockopt: Handy debugging trick that lets 
	 * us rerun the server immediately after we kill it; 
	 * otherwise we have to wait about 20 secs. 
	 * Eliminates "ERROR on binding: Address already in use" error. 
	 */
	int 			optval = 1;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval, sizeof(int));

	struct sockaddr_in serveraddr;
	bzero((char *) &serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons((unsigned short) 6801);

	if (bind(fd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) {
		fprintf(stderr, "ERROR on binding");
		g_bExitFlag 		= 1;
		return NULL;
	}

	if (listen(fd, 1) < 0) {
		fprintf(stderr, "ERROR on listen");
		g_bExitFlag 		= 1;
		return NULL;
	}

	while (!g_bExitFlag) {
		struct sockaddr_in clientaddr;
		int 			clientlen = sizeof(clientaddr);
		int 			fd2 = accept(fd, (struct sockaddr *) &clientaddr, &clientlen);

		if (fd2 < 0) {
			fprintf(stderr, "ERROR on accept");
			continue;
		}

		g_bOpusConnectedFlag = 1;
		g_iOpusFd			= fd2;

		while (g_bOpusConnectedFlag) {
			sleep(1);
		}

		close(fd2);
		fprintf(stderr, "gThreadOpus disconnect.");
	}

	fprintf(stdout, "gThreadOpus exit.");
}


void * gThreadJson(void * para)
{
	//for mkfifo in&out.
	int 			fd_json_rx, fd_json_tx;

	fd_json_rx			= open(FILE_JSON_RX, O_RDWR);

	if (fd_json_rx < 0) {
		fprintf(stderr, "failed to open %s\n", FILE_JSON_RX);
		return NULL;
	}

	fd_json_tx			= open(FILE_JSON_TX, O_RDWR);

	if (fd_json_tx < 0) {
		fprintf(stderr, "failed to open %s\n", FILE_JSON_TX);
		return NULL;
	}

	//load	zctrl.json.
	gLoadCfgFile(&gCamPara);

	fprintf(stdout, "gThreadJson:enter loop\n");

	while (!g_bExitFlag) {
		int 			len = 0;
		int 			iJsonLen = 0;
		char			cJsonRx[512];

		//1.read 4 bytes json length from rx fifo.
		len 				= read(fd_json_rx, &iJsonLen, sizeof(iJsonLen));

		if (len < 0) {
			fprintf(stderr, "error at read %s\n", FILE_JSON_RX);
			break;
		}

		if (iJsonLen <= 0 || iJsonLen >= 512) {
			//something wrong.
			usleep(1000 * 100);
			continue;
		}

		fprintf(stdout, "len:%d\n", iJsonLen);

		//2.read N bytes json data from rx fifo.
		len 				= read(fd_json_rx, cJsonRx, iJsonLen);

		if (len <= 0) {
			fprintf(stderr, "error at read %s\n", FILE_JSON_RX);
			continue;
		}

		cJsonRx[len]		= '\0';

		//3.parse out json.
		fprintf(stdout, "%s\n\n", cJsonRx);
		(void)
			gParseJson(cJsonRx, len, fd_json_tx);

		//reduce heavy cpu load.
		usleep(1000 * 100);
	}

	close(fd_json_rx);
	close(fd_json_tx);
	fprintf(stdout, "gThreadJson:exit.\n");
	return NULL;
}


int gParseJson(char * jsonData, int jsonLen, int fd)
{
	int 			bWrCfgFile = 0;

	//Ns:Noise Suppression Progress.
	(void)
		jsonLen;
	cJSON * 		rootRx = cJSON_Parse(jsonData);

	if (rootRx == NULL) {
		printf("parse error\n");
		return - 1;
	}

	cJSON * 		jCam1CenterXY = cJSON_GetObjectItem(rootRx, "Cam1CenterXY");

	if (jCam1CenterXY) {
		char *			cValue = cJSON_Print(jCam1CenterXY);
		printf("cam1centerxy:%ld,%s\n",strlen(cValue),cValue);
		if (!strcmp(cValue, "\"query\"")) {
			//only query,no need to write.
		}
		else {
			//x,y
			if ( strlen(cValue)>=3 && (strlen(cValue) < sizeof(gCamPara.m_cam1xy)) ) {
				strncpy(gCamPara.m_cam1xy, &cValue[1],strlen(cValue)-2);
				bWrCfgFile			= 1;
			}
			else {
				printf("too long Cam1CenterXY!\n");
			}
		}

		//write feedback to tx fifo.
		cJSON * 		rootTx = cJSON_CreateObject();
		cJSON * 		item = cJSON_CreateString(gCamPara.m_cam1xy);

		cJSON_AddItemToObject(rootTx, "Cam1CenterXY", item);
		char *			pJson = cJSON_Print(rootTx);
		int 			iJsonLen = strlen(pJson);

		write(fd, &iJsonLen, sizeof(iJsonLen));
		write(fd, pJson, iJsonLen);
		cJSON_Delete(rootTx);
	}

	cJSON * 		jCam2CenterXY = cJSON_GetObjectItem(rootRx, "Cam2CenterXY");

	if (jCam2CenterXY) {
		char *			cValue = cJSON_Print(jCam2CenterXY);
		printf("cam2centerxy:%ld,%s\n",strlen(cValue),cValue);

		if (!strcmp(cValue, "\"query\"")) {
			//only query,no need to write.
		}
		else {
			//x,y
			if ( strlen(cValue)>=3 && (strlen(cValue) < sizeof(gCamPara.m_cam2xy)) ) {
				strncpy(gCamPara.m_cam2xy,&cValue[1],strlen(cValue)-2);
				bWrCfgFile			= 1;
			}
			else {
				printf("too long Cam2CenterXY!\n");
			}
		}

		//write feedback to tx fifo.
		cJSON * 		rootTx = cJSON_CreateObject();
		cJSON * 		item = cJSON_CreateString(gCamPara.m_cam2xy);

		cJSON_AddItemToObject(rootTx, "Cam2CenterXY", item);
		char *			pJson = cJSON_Print(rootTx);
		int 			iJsonLen = strlen(pJson);

		write(fd, &iJsonLen, sizeof(iJsonLen));
		write(fd, pJson, iJsonLen);
		cJSON_Delete(rootTx);
	}

	cJSON * 		jCam3CenterXY = cJSON_GetObjectItem(rootRx, "Cam3CenterXY");

	if (jCam3CenterXY) {
		char *			cValue = cJSON_Print(jCam3CenterXY);
		printf("cam3centerxy:%ld,%s\n",strlen(cValue),cValue);

		if (!strcmp(cValue, "\"query\"")) {
			//only query,no need to write.
		}
		else {
			//x,y
			if (strlen(cValue)>=3 && (strlen(cValue) < sizeof(gCamPara.m_cam3xy)) ) {
				strncpy(gCamPara.m_cam3xy, &cValue[1],strlen(cValue)-2);
				bWrCfgFile			= 1;
			}else {
				printf("too long Cam3CenterXY!\n");
			}
		}

		//write feedback to tx fifo.
		cJSON * 		rootTx = cJSON_CreateObject();
		cJSON * 		item = cJSON_CreateString(gCamPara.m_cam2xy);

		cJSON_AddItemToObject(rootTx, "Cam3CenterXY", item);
		char *			pJson = cJSON_Print(rootTx);
		int 			iJsonLen = strlen(pJson);

		write(fd, &iJsonLen, sizeof(iJsonLen));
		write(fd, pJson, iJsonLen);
		cJSON_Delete(rootTx);
	}

	//"DeNoise":"off/Strong/WebRTC/mmse/Bevis/NRAE/query"
	cJSON * 		jDeNoise = cJSON_GetObjectItem(rootRx, "DeNoise");

	if (jDeNoise) {
		char *			cValue = cJSON_Print(jDeNoise);

		if (!strcmp(cValue, "\"query\"")) {
			//only query,no need to write.
		}
		else if (!strcmp(cValue, "\"off\"")) {
			gCamPara.m_iDeNoise = 0;
		}
		else if (!strcmp(cValue, "\"Strong\"")) {
			gCamPara.m_iDeNoise = 1;
		}
		else if (!strcmp(cValue, "\"WebRTC\"")) {
			gCamPara.m_iDeNoise = 2;
		}

		//write feedback to tx fifo.
		cJSON * 		rootTx = cJSON_CreateObject();
		cJSON * 		item = cJSON_CreateString(cValue);

		cJSON_AddItemToObject(rootTx, "DeNoise", item);
		char *			pJson = cJSON_Print(rootTx);
		int 			iJsonLen = strlen(pJson);

		write(fd, &iJsonLen, sizeof(iJsonLen));
		write(fd, pJson, iJsonLen);
		cJSON_Delete(rootTx);
	}

	//"StrongMode":"mode1/mode2/mode3/mode4/mode5/mode6/mode7/mode8/mode9/mode10/query"
	cJSON * 		jStrongMode = cJSON_GetObjectItem(rootRx, "StrongMode");

	if (jStrongMode) {
		char *			cValue = cJSON_Print(jStrongMode);

		if (!strcmp(cValue, "\"query\"")) {
			//only query,no need to write.
		}
		else if (!strcmp(cValue, "\"mode1\"")) {
			gCamPara.m_iStrongMode = 1;
		}
		else if (!strcmp(cValue, "\"mode2\"")) {
			gCamPara.m_iStrongMode = 2;
		}
		else if (!strcmp(cValue, "\"mode3\"")) {
			gCamPara.m_iStrongMode = 3;
		}
		else if (!strcmp(cValue, "\"mode4\"")) {
			gCamPara.m_iStrongMode = 4;
		}
		else if (!strcmp(cValue, "\"mode5\"")) {
			gCamPara.m_iStrongMode = 5;
		}
		else if (!strcmp(cValue, "\"mode6\"")) {
			gCamPara.m_iStrongMode = 6;
		}
		else if (!strcmp(cValue, "\"mode7\"")) {
			gCamPara.m_iStrongMode = 7;
		}
		else if (!strcmp(cValue, "\"mode8\"")) {
			gCamPara.m_iStrongMode = 8;
		}
		else if (!strcmp(cValue, "\"mode9\"")) {
			gCamPara.m_iStrongMode = 9;
		}
		else if (!strcmp(cValue, "\"mode10\"")) {
			gCamPara.m_iStrongMode = 10;
		}

		//4.write feedback to tx fifo.
		cJSON * 		root = cJSON_CreateObject();
		cJSON * 		item = cJSON_CreateString(cValue);

		cJSON_AddItemToObject(root, "StrongMode", item);
		char *			pJson = cJSON_Print(root);
		int 			iJsonLen = strlen(pJson);

		write(fd, &iJsonLen, sizeof(iJsonLen));
		write(fd, pJson, iJsonLen);
		cJSON_Delete(root);
	}

	//"WebRtcGrade":"0/1/2/query"
	cJSON * 		jWebRtcGrade = cJSON_GetObjectItem(rootRx, "WebRtcGrade");

	if (jWebRtcGrade) {
		char *			cValue = cJSON_Print(jWebRtcGrade);

		if (!strcmp(cValue, "\"query\"")) {
			//only query,no need to write.
		}
		else if (!strcmp(cValue, "\"0\"")) {
			gCamPara.m_iWebRtcGrade = 0;
		}
		else if (!strcmp(cValue, "\"1\"")) {
			gCamPara.m_iWebRtcGrade = 1;
		}
		else if (!strcmp(cValue, "\"2\"")) {
			gCamPara.m_iWebRtcGrade = 2;
		}

		//4.write feedback to tx fifo.
		cJSON * 		root = cJSON_CreateObject();
		cJSON * 		item = cJSON_CreateString(cValue);

		cJSON_AddItemToObject(root, "WebRtcGrade", item);
		char *			pJson = cJSON_Print(root);
		int 			iJsonLen = strlen(pJson);

		write(fd, &iJsonLen, sizeof(iJsonLen));
		write(fd, pJson, iJsonLen);
		cJSON_Delete(root);
	}


	//SpkPlaybackVol
	cJSON * 		jSpkPlaybackVol = cJSON_GetObjectItem(rootRx, "SpkPlaybackVol");

	if (jSpkPlaybackVol) {
		char *			cValue = cJSON_Print(jSpkPlaybackVol);

		printf("SpkPlaybackVol:%s\n", cValue);

		//numid=682,iface=MIXER,name='MVC1 Vol'
		//	; type=INTEGER,access=rw------,values=1,min=0,max=16000,step=0
		//	: values=13000
		char  vol[32];
		memset(vol,0,sizeof(vol));
		memcpy(vol,&cValue[1],strlen(cValue)-2);
		int iValue = atoi(vol);
		//printf("%s,%d\n",vol,iValue);

		if (iValue >= 32 && iValue <= 1000) {
			char cVolHigh=(char)((iValue>>8)&0xFF);
			char cVolLow=(char)(iValue&0xFF);
			char			command[512];

			sprintf(command,"spidev_test -D /dev/spidev3.0 -H -p \"\\x00\\x00\\x00\\x00\\x00\\x00\\x%02x\\x%02x\"",cVolHigh,cVolLow);
			//printf(command);
			system(command);
		}

		//write feedback to tx fifo.
		cJSON * 		root = cJSON_CreateObject();
		cJSON * 		item = cJSON_CreateString(cValue);

		cJSON_AddItemToObject(root, "SpkPlaybackVol", item);
		char *			pJson = cJSON_Print(root);
		int 			iJsonLen = strlen(pJson);

		write(fd, &iJsonLen, sizeof(iJsonLen));
		write(fd, pJson, iJsonLen);
		cJSON_Delete(root);
	}

	//DeMode=Normal/Wobble/query
	cJSON * 		jDeMode = cJSON_GetObjectItem(rootRx, "DeMode");

	if (jDeMode) {
		char *			cValue = cJSON_Print(jDeMode);

		printf("DeMode:%s\n", cValue);

		if (!strcmp(cValue, "\"query\"")) {
			//only query,no need to write.
		}
		else if (!strcmp(cValue, "\"Normal\"")) {
			system("spidev_test -D /dev/spidev3.0 -H -p \"\\x00\\x00\\x01\\x00\\x00\\x00\\x00\\x00\"");
			g_DeMode=0;
		}
		else if (!strcmp(cValue, "\"Wobble\"")) {
			system("spidev_test -D /dev/spidev3.0 -H -p \"\\x00\\x00\\x01\\x00\\x00\\x00\\x00\\x01\"");
			g_DeMode=1;
		}
	}

	cJSON_Delete(rootRx);

	if (bWrCfgFile) {
		gSaveCfgFile(&gCamPara);
	}

	return 0;
}


int gLoadCfgFile(ZCamPara * para)
{
	int 			fd;
	char			buffer[256];
	int 			len;

	if (access(FILE_JSON_CFG, F_OK) < 0) {
		strcpy(para->m_cam1xy, "0,0");
		strcpy(para->m_cam2xy, "0,0");
		strcpy(para->m_cam3xy, "0,0");
		para->m_iDeNoise	= 0;
		para->m_iStrongMode = 1;
		fprintf(stdout,"load default cfg file!\n");
		return - 1;
	}

	if ((fd = open(FILE_JSON_CFG, O_RDWR)) < 0) {
		printf("failed to open %s\n", FILE_JSON_CFG);
		return - 1;
	}

	len 				= read(fd, buffer, sizeof(buffer));

	if (len < 0) {
		printf("failed to read %s\n", FILE_JSON_CFG);
		return - 1;
	}

	buffer[len] 		= '\0';
	close(fd);

	//parse out.
	cJSON * 		root = cJSON_Parse(buffer);

	if (root == NULL) {
		printf("parse error\n");
		return - 1;
	}

	cJSON * 		jCam1CenterXY = cJSON_GetObjectItem(root, "Cam1CenterXY");

	if (jCam1CenterXY) {
		char *			cValue = cJSON_Print(jCam1CenterXY);

		if (strlen(cValue) < sizeof(para->m_cam1xy)) {
			strcpy(para->m_cam1xy, cValue);
		}
		else {
			printf("Cam1CenterXY is too long!\n");
			strcpy(para->m_cam1xy, "0,0");
		}
	}

	cJSON * 		jCam2CenterXY = cJSON_GetObjectItem(root, "Cam2CenterXY");

	if (jCam2CenterXY) {
		char *			cValue = cJSON_Print(jCam2CenterXY);

		if (strlen(cValue) < sizeof(para->m_cam2xy)) {
			strcpy(para->m_cam2xy, cValue);
		}
		else {
			printf("Cam2CenterXY is too long!\n");
			strcpy(para->m_cam2xy, "0,0");
		}
	}

	cJSON * 		jCam3CenterXY = cJSON_GetObjectItem(root, "Cam3CenterXY");

	if (jCam3CenterXY) {
		char *			cValue = cJSON_Print(jCam3CenterXY);

		if (strlen(cValue) < sizeof(para->m_cam3xy)) {
			strcpy(para->m_cam3xy, cValue);
		}
		else {
			printf("Cam3CenterXY is too long!\n");
			strcpy(para->m_cam3xy, "0,0");
		}
	}

	cJSON_Delete(root);
	return 0;
}


int gSaveCfgFile(ZCamPara * para)
{
	int 			fd;

	if ((fd = open(FILE_JSON_CFG, O_RDWR | O_CREAT)) < 0) {
		printf("failed to open %s\n", FILE_JSON_CFG);
		return - 1;
	}

	cJSON * 		root = cJSON_CreateObject();

	cJSON * 		item1 = cJSON_CreateString(para->m_cam1xy);

	cJSON_AddItemToObject(root, "Cam1CenterXY", item1);

	cJSON * 		item2 = cJSON_CreateString(para->m_cam2xy);

	cJSON_AddItemToObject(root, "Cam2CenterXY", item2);

	cJSON * 		item3 = cJSON_CreateString(para->m_cam3xy);

	cJSON_AddItemToObject(root, "Cam2CenterXY", item3);

	char *			pJson = cJSON_Print(root);

	write(fd, pJson, strlen(pJson));
	close(fd);
	cJSON_Delete(root);

	printf("write cfg file:%s\n",para->m_cam1xy);
	return 0;
}


/**************the end of file**********************/
