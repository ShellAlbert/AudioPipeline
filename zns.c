
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
#include "analog_agc.h"
#include "defines.h"
#include "gain_control.h"
#include "ns_core.h"
#include "real_fft.h"
#include "signal_processing_library.h"
#include "windows_private.h"
#include "complex_fft_tables.h"
#include "digital_agc.h"
#include "noise_suppression.h"
#include "nsx_core.h"
#include "resample_by_2_internal.h"
#include "spl_inl.h"
#include "cpu_features_wrapper.h"
#include "fft4g.h"
#include "noise_suppression_x.h"  
#include "nsx_defines.h"
#include "ring_buffer.h"
#include "typedefs.h"

//data flow.
//arecord -> zsy.noise -> zns ->zsy.clean -> aplay
//								->zsy.opus	-> android
#define FILE_NOISE				"/tmp/zsy/zsy.noise"
#define FILE_CLEAN				"/tmp/zsy/zsy.clean"
#define FILE_OPUS				"/tmp/zsy/zsy.opus"

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

#define RESAMPLE_25k_16k		1

typedef struct {
	char			m_cam1xy[32];
	char			m_cam2xy[32];
	char			m_cam3xy[32];

	//"DeNoise":"off/Strong/WebRTC/mmse/Bevis/NRAE/query"
	int 			m_iDeNoise;

	//"StrongMode":"mode1/mode2/mode3/mode4/mode5/mode6/mode7/mode8/mode9/mode10/query"
	int 			m_iStrongMode;
} ZCamPara;


ZCamPara		gCamPara;
int 			g_bExitFlag = 0;
int 			g_bOpusConnectedFlag = 0;
int 			g_iOpusFd;

//Demodulate Mode
//0:32khz i2s
//1:25khz i2s
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
			g_bOpusConnectedFlag = 1;
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
void * gThreadOpus(void * para);


int main(int argc, char * *argv)
{
	pthread_t		tidJson, tidNs, tidOpus;

	//write pid to file.
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

	//Set the signal callback for Ctrl-C
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

	if (pthread_create(&tidOpus, NULL, gThreadOpus, NULL)) {
		fprintf(stderr, "failed to create opus thread!\n");
		return - 1;
	}


	pthread_join(tidJson, NULL);
	pthread_join(tidNs, NULL);
	pthread_join(tidOpus, NULL);

	fprintf(stdout, "zns exit.\n");
	return 0;
}


#ifdef RESAMPLE_25k_16k

//declare the func of universal resample.
static void UniversalResample(const int16_t * src, const int src_len, int16_t * dst, const int dst_len, 
	int16_t * resample_state);

//defines the func of universal resample.
static void UniversalResample(const int16_t * src, const int src_len, int16_t * dst, const int dst_len, 
	int16_t * resample_state)
{
#if 0

	if ((src_len == 0) || (dst_len == 0))
		return;

	if (src_len == dst_len) {
		memcpy(dst, src, src_len * sizeof(int16_t));
		return;
	}

#endif

	if ((*resample_state) == 32767)
		*resample_state = (src[0] >> 1);

	int 			j	= 0, mod = 0, tmp = 0;

	dst[0]				= (*resample_state);

	for (int i = 1; i < dst_len; i++) {
		tmp 				= i * src_len;
		j					= tmp / dst_len;
		mod 				= tmp % dst_len;

		if (j == 0)
			dst[i] =
				 (int16_t) ((((float) (*resample_state) * (dst_len - mod)) / dst_len) + (((float) src[j] *mod) / dst_len));
		else 
			dst[i] =
				 (int16_t) ((((float) src[j - 1] * (dst_len - mod)) / dst_len) + (((float) src[j] *mod) / dst_len));
	}

	*resample_state 	= src[src_len - 1];

	return;
}


#endif


void * gThreadNs(void * para)
{
#ifdef RESAMPLE_25k_16k
	int16_t 		urState2550;
	int16_t 		urState4850;
	int16_t 		urState5096;
	int16_t 		urState5048;
	int16_t 		urState2516;

	WebRtcSpl_State48khzTo16khz state4816;
	WebRtcSpl_State16khzTo48khz state1648;

	int32_t 		DownsampleBy2_filtState1[8] = {
		0
	};
	int32_t 		UpsampleBy2_filtState1[8] = {
		0
	};
#endif

#ifdef RESAMPLE_25k_16k
	urState2550 		= 32767;
	urState4850 		= 32767;
	urState5096 		= 32767;
	urState5048 		= 32767;
	urState2516 		= 32767;

	WebRtcSpl_ResetResample48khzTo16khz(&state4816);
	WebRtcSpl_ResetResample16khzTo48khz(&state1648);

	memset(DownsampleBy2_filtState1, 0, sizeof(DownsampleBy2_filtState1));
	memset(UpsampleBy2_filtState1, 0, sizeof(UpsampleBy2_filtState1));

#endif

	//for mkfifo in&out.
	int 			fd_noise, fd_clean;

	fd_noise			= open(FILE_NOISE, O_RDONLY);

	if (fd_noise < 0) {
		fprintf(stderr, "failed to open %s\n", FILE_NOISE);
		g_bExitFlag 		= 1;
		return NULL;
	}

	fd_clean			= open(FILE_CLEAN, O_RDWR);

	if (fd_clean < 0) {
		fprintf(stderr, "failed to open %s\n", FILE_CLEAN);
		g_bExitFlag 		= 1;
		return NULL;
	}

	//for libns.
	//here call ns_init() or ns_custom_init() for call ns_uninit() later.
	ns_init(0); 									//0~5.
	int 			denoiseAlgorithm = 3;
	int 			denoiseLevel = 0;
	int 			enhancedType = 0;
	int 			enhancedLevel = 0;
	char			customBandGains[8];
	char			preEmphasisFlag = 0;

	memset(customBandGains, 0, 8);

	if (ns_custom_init(denoiseAlgorithm, denoiseLevel, enhancedType, enhancedLevel, customBandGains, preEmphasisFlag)) {
		fprintf(stderr, "ns:failed to init ns_custom_init().\n");
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
	fprintf(stdout, "gThreadNs:enter loop\n");

	FILE *			fp = fopen("zsy.32.pcm", "wr");
	FILE *			fpp = fopen("zsy.16.pcm", "wr");
	while (!g_bExitFlag) {

		char			pcmBuf[MAX_FRAME_SIZE * CHANNELS * sizeof(opus_int16)];

		opus_int16		in[FRAME_SIZE * CHANNELS * 2];
		int 			iIn32kLen = 0;
		int 			iIn48kLen = 0;
		opus_int16		out[MAX_FRAME_SIZE * CHANNELS];
		unsigned char	cbits[MAX_PACKET_SIZE];

		//buffer for converting from 32khz to 48khz.
		opus_int16		tmpLft48k[FRAME_SIZE * CHANNELS * 2];
		int 			tmpLft48kLen = 0;
		opus_int16		tmpRht48k[FRAME_SIZE * CHANNELS * 2];
		int 			tmpRht48kLen = 0;

		int 			iErrFlag = 0;

		//Step1:read pcm data from zsy.noise fifo.
		int 			iOffset = 0;
		int 			iNeedBytes = sizeof(opus_int16) *CHANNELS * FRAME_SIZE;

		printf("read bytes from fifo:%d\n", iNeedBytes);

		while (iNeedBytes > 0) {
			int 			iRdBytes = read(fd_noise, pcmBuf + iOffset, iNeedBytes);

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

		//00fc8900 00fc8900 00c08d00 00c0d800 .........
		//LeftChannelData is same as RightChannelData.
		//for(int i=0;i<16;i++)
		//{
		//	printf("%d:    %02x\n",i,pcmBuf[i]);
		//}
		fwrite((void*)pcmBuf,sizeof(opus_int16) *CHANNELS * FRAME_SIZE,1,fp);
		fflush(fp);
#if 0

		//read from zsy.noise and write to zsy.clean without any processing (S32_LE).
		//test okay,should be removed when publish.
		int 			iNeedWrBytes = sizeof(opus_int16) *CHANNELS * FRAME_SIZE;
		int 			iWrOffset = 0;

		while (iNeedWrBytes > 0) {
			int 			iWrBytes = write(fd_clean, pcmBuf + iWrOffset, iNeedWrBytes);

			if (iWrBytes < 0) {
				fprintf(stderr, "failed to write fd_clean!\n");
				break;
			}

			iNeedWrBytes		-= iWrBytes;
			iWrOffset			+= iWrBytes;
		}

#endif


		//32bit to 16bit.
		int 			iPcmBlk = sizeof(opus_int16) *CHANNELS * FRAME_SIZE;

		for (int i = 0; i < iPcmBlk; i += sizeof(int)) {
			int 			iPcm32Bit = 0;
			opus_int16		iPcm16Bit = 0;

			iPcm32Bit			|= pcmBuf[0] << 0;
			iPcm32Bit			|= pcmBuf[1] << 8;
			iPcm32Bit			|= pcmBuf[2] << 16;
			iPcm32Bit			|= pcmBuf[3] << 24;
			iPcm16Bit			= (opus_int16)(iPcm32Bit>>16-0x8000);

			printf("%02x,%02x,%02x,%02x %d(%08x)->%d(%04x)\n", ///<
			(unsigned char) pcmBuf[0], (unsigned char) pcmBuf[1], (unsigned char) pcmBuf[2],
				 (unsigned char) pcmBuf[3], 
				(unsigned int) iPcm32Bit, (unsigned int) iPcm32Bit, (unsigned short) iPcm16Bit,
				 (unsigned short) iPcm16Bit);

			in[iIn32kLen++] 	= iPcm16Bit;
		}
		fwrite((void*)in,iIn32kLen*sizeof(opus_int16),1,fpp);
		fflush(fpp);
		//printf("iPcmBlk:%d -> %d\n",iPcmBlk,iIn32kLen);
		continue;

		//read from zsy.noise and write to zsy.clean with convert 32bit to 16bit (S16_LE).
		//test okay,should be removed when publish.
		int 			iNeedWrBytes = iIn32kLen * sizeof(opus_int16);
		int 			iWrOffset = 0;
		char *			pPcm16bit = (char *) (in);

		while (iNeedWrBytes > 0) {
			int 			iWrBytes = write(fd_clean, pPcm16bit + iWrOffset, iNeedWrBytes);

			if (iWrBytes < 0) {
				fprintf(stderr, "failed to write fd_clean!\n");
				break;
			}

			iNeedWrBytes		-= iWrBytes;
			iWrOffset			+= iWrBytes;
		}

		continue;

		//Step2:convert int8_t to int16_t before frequency convert.
		for (int i = 0; i < CHANNELS * FRAME_SIZE; i++) {
			in[i]				= pcmBuf[2 * i] | pcmBuf[2 * i + 1] << 8;
		}

#if 1

		//Step3.do sample frequency convert.
		if (g_DeMode) {
			int16_t 		tmp25[250];
			int16_t 		tmp50[500];
			int16_t 		tmp96[960];
			int16_t 		tmp48[480];

			//25kHz ==> 50kHz ==> 96kHz ==> 48kHz 
			UniversalResample(tmp25, 250, tmp50, 500, &urState2550); //25kHz ==> 50kHz.
			UniversalResample(tmp50, 500, tmp96, 960, &urState5096); //50kHz ==> 96kHz.
			WebRtcSpl_DownsampleBy2(tmp96, 960, tmp48, DownsampleBy2_filtState1); //96kHz ==> 48kHz
		}
		else {
			//1.split stereo to 2*single channel.
			opus_int16		tmpLft32k[FRAME_SIZE];
			opus_int16		tmpRht32k[FRAME_SIZE];
			int 			iTmpLft32kLen = 0;
			int 			iTmpRht32kLen = 0;

			for (int i = 0; i < FRAME_SIZE * CHANNELS; i++) {
				if ((i + 1) % 2 == 0) {
					tmpLft32k[iTmpLft32kLen++] = in[i];
				}
				else {
					tmpRht32k[iTmpRht32kLen++] = in[i];
				}
			}

			//2.frequency converter only support 320 as a block at once.
			//loop to convert.
			int 			iLoops = FRAME_SIZE / 320;
			int 			iLftOft = 0;
			int 			iRhtOft = 0;

			for (int i = 0; i < iLoops; i++) {
				int16_t 		tmp32[320];
				int16_t 		tmp16[160];
				int16_t 		tmp48[480];
				int32_t 		tmpmem[480 * 2];

				//step1:left channel convert.
				memcpy(tmp32, &tmpLft32k[iLftOft], sizeof(int16_t) * 320);
				iLftOft 			+= sizeof(int16_t) * 320;

				//step2:32kHz-16khz-48Khz.
				WebRtcSpl_DownsampleBy2(tmp32, 320, tmp16, DownsampleBy2_filtState1); //32kHz ==> 16kHz
				WebRtcSpl_Resample16khzTo48khz(tmp16, tmp48, &state1648, tmpmem); //16kHz ==> 48kHz

				//step3:left channel temporary storage.
				memcpy(&tmpLft48k[tmpLft48kLen], tmp48, sizeof(int16_t) * 480);
				tmpLft48kLen		+= sizeof(int16_t) * 480;


				//step1:right channel convert.
				memcpy(tmp32, &tmpRht32k[iRhtOft], sizeof(int16_t) * 320);
				iRhtOft 			+= sizeof(int16_t) * 320;

				//step2:32kHz-16khz-48Khz.
				WebRtcSpl_DownsampleBy2(tmp32, 320, tmp16, DownsampleBy2_filtState1); //32kHz ==> 16kHz
				WebRtcSpl_Resample16khzTo48khz(tmp16, tmp48, &state1648, tmpmem); //16kHz ==> 48kHz

				//step3:right channel temporary storage.
				memcpy(&tmpRht48k[tmpRht48kLen], tmp48, sizeof(int16_t) * 480);
				tmpRht48kLen		+= sizeof(int16_t) * 480;
			}

			//3.combine 2*single channel to stereo interlaced.
			iIn48kLen			= 0;

			for (int i = 0; i < (tmpLft48kLen / sizeof(int16_t)); i++) {
				in[iIn48kLen++] 	= tmpLft48k[i];
				in[iIn48kLen++] 	= tmpRht48k[i];
			}

			printf("after 32khz-48khz,iInLen=%d(%d)\n", iIn48kLen, iIn48kLen * sizeof(int16_t));
		}

#endif

		//2.noise suppression.
		switch (gCamPara.m_iDeNoise)
		{
			case 0: //DeNoise Disabled.
				break;

			case 1: //RNNoise.
				//libns.so only process 960 bytes each time.
				//ns_processing(cBufOpusDec, len);
				break;

			case 2: //WebRTC.
				//libns.so only process 960 bytes each time.
				//ns_processing(cBufOpusDec, len);
				break;

			case 3: //NRAE.
				//libns.so only process 960 bytes each time.
				//ns_processing(cBufOpusDec, len);
				break;

			default:
				break;
		}


		//opus_enc only support Big-endian.
		//convert from little-endian ordering.
		for (int i = 0; i < iIn48kLen; i++) {
			in[i]				= htonl(in[i]);
		}


		//3.encode pcm to opus.
		//int			iEncBytes = opus_encode(encoder, in, FRAME_SIZE, cbits, MAX_PACKET_SIZE);
		int 			iEncBytes = opus_encode(encoder, in, iIn48kLen, cbits, MAX_PACKET_SIZE);

		if (iEncBytes < 0) {
			fprintf(stderr, "encode failed: %s\n", opus_strerror(iEncBytes));
			continue;
		}

		fprintf(stdout, "opus_encoder : %d(%d)->%d\n", iIn48kLen, iIn48kLen * CHANNELS * sizeof(int16_t), iEncBytes);


		//4.tx opus(len+data) to APP.
		if (g_bOpusConnectedFlag) {
			int 			iEncBytesBE = iEncBytes /*htonl(iEncBytes)*/;
			int 			len = write(g_iOpusFd, &iEncBytesBE, sizeof(iEncBytesBE));

			if (len != sizeof(iEncBytesBE)) {
				fprintf(stderr, "tx opus len failed:%d,%d\n", len, sizeof(iEncBytesBE));
				g_bOpusConnectedFlag = 0;
			}
			else {
				fprintf(stdout, "opus len:%d\n", iEncBytes);
				int 			iNeedTxBytes = iEncBytes;
				int 			iTxOffset = 0;

				while (iNeedTxBytes > 0) {
					int 			iTxBytes = write(g_iOpusFd, cbits + iTxOffset, iNeedTxBytes);

					if (iTxBytes < 0) {
						fprintf(stderr, "tx opus data failed!\n");
						g_bOpusConnectedFlag = 0;
						break;
					}

					fprintf(stdout, "tx %d bytes opus\n", iTxBytes);

					iNeedTxBytes		-= iTxBytes;
					iTxOffset			+= iTxBytes;
				}

			}
		}

		/* Decode the data. In this example, frame_size will be constant because
			the encoder is using a constant frame size. However, that may not
			be the case for all encoders, so the decoder must always check
			the frame size returned. */
		int 			iDecBytes = opus_decode(decoder, cbits, iEncBytes, out, MAX_FRAME_SIZE, 0);

		if (iDecBytes < 0) {
			fprintf(stderr, "decoder failed: %s\n", opus_strerror(iDecBytes));
			continue;
		}

		fprintf(stdout, "opus_decoder : %d->%d(%d)\n", iEncBytes, iDecBytes, iDecBytes * CHANNELS * sizeof(int16_t));


		/* Convert to little-endian ordering. */
		for (int i = 0; i < CHANNELS * iDecBytes; i++) {
			pcmBuf[2 * i]		= out[i] &0xFF;
			pcmBuf[2 * i + 1]	= (out[i] >> 8) & 0xFF;
		}

#if 0

		//write pcm to zsy.clean for local playback.
		int 			iNeedWrBytes = sizeof(opus_int16) *iDecBytes * CHANNELS;
		int 			iWrOffset = 0;

		while (iNeedWrBytes > 0) {
			int 			iWrBytes = write(fd_clean, pcmBuf + iWrOffset, iNeedWrBytes);

			if (iWrBytes < 0) {
				fprintf(stderr, "failed to write fd_clean!\n");
				break;
			}

			iNeedWrBytes		-= iWrBytes;
			iWrOffset			+= iWrBytes;
		}

#endif
	}

	close(fd_noise);
	close(fd_clean);
	opus_encoder_destroy(encoder);
	opus_decoder_destroy(decoder);

	fprintf(stdout, "gThreadNs:exit.\n");
	return NULL;
}


void * gThreadOpus(void * para)
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
	int 			bWrCfgFile = 1;

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

		if (!strcmp(cValue, "query")) {
			//only query,no need to write.
			bWrCfgFile			= 0;
		}
		else {
			if (strlen(cValue) < sizeof(gCamPara.m_cam1xy)) {
				strcpy(gCamPara.m_cam1xy, cValue);
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

		if (!strcmp(cValue, "query")) {
			//only query,no need to write.
			bWrCfgFile			= 0;
		}
		else {
			if (strlen(cValue) < sizeof(gCamPara.m_cam2xy)) {
				strcpy(gCamPara.m_cam2xy, cValue);
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

		if (!strcmp(cValue, "query")) {
			//only query,no need to write.
			bWrCfgFile			= 0;
		}
		else {
			if (strlen(cValue) < sizeof(gCamPara.m_cam3xy)) {
				strcpy(gCamPara.m_cam3xy, cValue);
			}
			else {
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

		if (!strcmp(cValue, "query")) {
			//only query,no need to write.
			bWrCfgFile			= 0;
		}
		else if (!strcmp(cValue, "off")) {
			gCamPara.m_iDeNoise = 0;
		}
		else if (!strcmp(cValue, "Strong")) {
			gCamPara.m_iDeNoise = 1;
		}
		else if (!strcmp(cValue, "WebRTC")) {
			gCamPara.m_iDeNoise = 2;
		}
		else if (!strcmp(cValue, "NRAE")) {
			gCamPara.m_iDeNoise = 3;
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

		if (!strcmp(cValue, "query")) {
			//only query,no need to write.
			bWrCfgFile			= 0;
		}
		else if (!strcmp(cValue, "mode1")) {
			gCamPara.m_iStrongMode = 1;
		}
		else if (!strcmp(cValue, "mode2")) {
			gCamPara.m_iStrongMode = 2;
		}
		else if (!strcmp(cValue, "mode3")) {
			gCamPara.m_iStrongMode = 3;
		}
		else if (!strcmp(cValue, "mode4")) {
			gCamPara.m_iStrongMode = 4;
		}
		else if (!strcmp(cValue, "mode5")) {
			gCamPara.m_iStrongMode = 5;
		}
		else if (!strcmp(cValue, "mode6")) {
			gCamPara.m_iStrongMode = 6;
		}
		else if (!strcmp(cValue, "mode7")) {
			gCamPara.m_iStrongMode = 7;
		}
		else if (!strcmp(cValue, "mode8")) {
			gCamPara.m_iStrongMode = 8;
		}
		else if (!strcmp(cValue, "mode9")) {
			gCamPara.m_iStrongMode = 9;
		}
		else if (!strcmp(cValue, "mode10")) {
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

	//SpkPlaybackVol
	cJSON * 		jSpkPlaybackVol = cJSON_GetObjectItem(rootRx, "SpkPlaybackVol");

	if (jSpkPlaybackVol) {
		char *			cValue = cJSON_Print(jSpkPlaybackVol);

		printf("SpkPlaybackVol:%s\n", cValue);

		//numid=682,iface=MIXER,name='MVC1 Vol'
		//	; type=INTEGER,access=rw------,values=1,min=0,max=16000,step=0
		//	: values=13000
		int 			iValue = atoi(cValue);

		if (iValue >= 0 && iValue <= 30) {
			char			command[512];

			//sprintf(command, "amixer -c tegrasndt186ref cset name=\"MVC1 Vol\" %d", iValue * 534);
			//system(command);
			//
			system("spidev_test -D /dev/spidev3.0 -H -p \"\x80\x00\x00\x00\x00\x00\x00\x64\"");
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

		if (!strcmp(cValue, "query")) {
			//only query,no need to write.
		}
		else if (!strcmp(cValue, "Normal")) {
			system("spidev_test -D /dev/spidev3.0 -H -p \"\x80\x00\x01\x00\x00\x00\x00\x00\"");
		}
		else if (!strcmp(cValue, "Wobble")) {
			system("spidev_test -D /dev/spidev3.0 -H -p \"\x80\x00\x01\x00\x00\x00\x00\x01\"");
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
	return 0;
}


/**************the end of file**********************/
