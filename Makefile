WEBRTC= webrtc/analog_agc.c \
        webrtc/cross_correlation.c  \
        webrtc/downsample_fast.c  \
        webrtc/min_max_operations.c  \
        webrtc/nsx_core.c      \
        webrtc/resample_48khz.c   \
        webrtc/resample.c       \
        webrtc/splitting_filter.c \
        webrtc/complex_bit_reverse.c \
        webrtc/digital_agc.c      \
        webrtc/energy.c         \
        webrtc/noise_suppression.c  \
        webrtc/nsx_core_c.c       \
        webrtc/resample_by_2.c     \
        webrtc/resample_fractional.c \
        webrtc/spl_sqrt.c \
        webrtc/complex_fft.c   \
        webrtc/division_operations.c  \
        webrtc/fft4g.c        \
        webrtc/noise_suppression_x.c \
        webrtc/nsx_core_neon_offsets.c \
        webrtc/resample_by_2_internal.c \
        webrtc/ring_buffer.c     \
        webrtc/spl_sqrt_floor.c \
        webrtc/copy_set_operations.c \
        webrtc/dot_product_with_scale.c \
        webrtc/get_scaling_square.c \
        webrtc/ns_core.c        \
        webrtc/real_fft.c       \
        webrtc/resample_by_2_mips.c   \
        webrtc/spl_init.c        \
        webrtc/vector_scaling_operations.c
INCFLAGS = -I. -I/home/yntp16/Downloads/cJSON
LDFLAGS = -Llibns -lns -lpthread -lm -lcjson
all:
	gcc zsyns.c $(INCFLAGS) $(LDFLAGS) -o zsyns.bin
