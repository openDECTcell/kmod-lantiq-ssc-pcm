# SPDX-License-Identifier: GPL-2.0-only
#
# kbuild for the Lantiq VR9 SSC PCM/TDM transport module.
#
obj-m += lantiq_ssc_pcm.o
lantiq_ssc_pcm-objs := lantiq_ssc_pcm_core.o ssc_pcm_hw.o

ccflags-y += -I$(src)/include/uapi
