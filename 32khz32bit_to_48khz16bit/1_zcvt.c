#include "zcvt.h"

static signed short state_3232_4816 = 32767;
static signed short state2532_4816 = 32767;


static void UniversalResample(const signed short* src, const int src_len, signed short* dst, const int dst_len, signed short* resample_state)
{
#if 0
	if ((src_len == 0) || (dst_len == 0))
		return;

	if (src_len == dst_len)
	{
		memcpy(dst, src, src_len * sizeof(int16_t));
		return;
	}
#endif

	if ((*resample_state) == 32767)
		*resample_state = (src[0] >> 1);

	int j = 0, mod = 0, tmp = 0;
	dst[0] = (*resample_state);
	for (int i = 1; i < dst_len; i++)
	{
		tmp = i * src_len;
		j = tmp / dst_len;
		mod = tmp % dst_len;

		if (j == 0)
			dst[i] = (signed short)((((float)(*resample_state) * (dst_len - mod)) / dst_len) + (((float)src[j] * mod) / dst_len));
		else
			dst[i] = (signed short)((((float)src[j - 1] * (dst_len - mod)) / dst_len) + (((float)src[j] * mod) / dst_len));
	}

	*resample_state = src[src_len - 1];

	return;
}

int zcvt_init()
{
	state_3232_4816 = 32767; 
	state2532_4816 = 32767;

    return 0;
}

//convert 32khz 32bit pcm to 48khz 16bit pcm.
//param1: [const char *in],input pcm data in 32khz sample rate, 32bit depth.
//param2: [const int in_len],the length of input pcm data in byte unit. 320*4 bytes.
//param3: [char *out],output buffer to hold the pcm data after converted.
//param4: [int max_out_len],the maximum length of output buffer. 480*2 bytes.
//return: [int],return the converted pcm data length.480*2 bytes.
int zcvt_3232_to_4816(const char *in,const int in_len,char *out,int max_out_len)
{
	signed short* pTmpOut = (signed short*)out;
	signed short tmp32[320] = { 0 }; 

	//encoding from 32bit to 16bit.
	for (int j = 0; j < 320; j++)
	{
		signed short* pTmp = (signed short*)(&in[(j<<2)]);
		pTmp++;
		tmp32[j] = (signed short)pTmp[0];
	}

	//resampling from 32k to 48k.
	UniversalResample(tmp32, (in_len>>2), (signed short*)out, (max_out_len>>1), &state_3232_4816);

    return 0;
}

//convert 25khz 32bit pcm to 48khz 16bit pcm.
//param1: [const char *in],input pcm data in 25khz sample rate, 32bit depth.
//param2: [const int in_len],the length of input pcm data in byte unit.
//param3: [char *out],output buffer to hold the pcm data after converted.
//param4: [int max_out_len],the maximum length of output buffer.
//return: [int],return the converted pcm data length.
int zcvt_2532_to_4816(const char *in,const int in_len,char *out,int max_out_len)
{
    return 0;
}

int zcvt_uninit()
{
    return 0;
}
