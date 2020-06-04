#ifndef _ZCVT_H__
#define _ZCVT_H__

/*extern */int zcvt_init();

//convert 32khz 32bit pcm to 48khz 16bit pcm.
//param1: [const char *in],input pcm data in 32khz sample rate, 32bit depth.
//param2: [const int in_len],the length of input pcm data in byte unit.
//param3: [char *out],output buffer to hold the pcm data after converted.
//param4: [int max_out_len],the maximum length of output buffer.
//return: [int],return the converted pcm data length.
/*extern */int zcvt_3232_to_4816(const char *in,const int in_len,char *out,int max_out_len);

//convert 25khz 32bit pcm to 48khz 16bit pcm.
//param1: [const char *in],input pcm data in 25khz sample rate, 32bit depth.
//param2: [const int in_len],the length of input pcm data in byte unit.
//param3: [char *out],output buffer to hold the pcm data after converted.
//param4: [int max_out_len],the maximum length of output buffer.
//return: [int],return the converted pcm data length.
/*extern */int zcvt_2532_to_4816(const char *in,const int in_len,char *out,int max_out_len);

/*extern */int zcvt_uninit();
#endif //_ZCVT_H__
