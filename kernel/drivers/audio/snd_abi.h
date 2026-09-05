#ifndef NEOOS_SND_ABI_H
#define NEOOS_SND_ABI_H

#include <stdint.h>

// Every struct and ioctl number below is copied verbatim from upstream
// Linux's include/uapi/sound/asound.h (fetched and checked against
// that source while writing the plan this implements -- not
// reconstructed from memory), per this project's Linux-ABI-shape rule.

typedef unsigned long snd_pcm_uframes_t;
typedef signed long   snd_pcm_sframes_t;

#define SNDRV_MASK_MAX 256
struct snd_mask {
    uint32_t bits[(SNDRV_MASK_MAX + 31) / 32];   // 8 x uint32_t = 32 bytes
};
_Static_assert(sizeof(struct snd_mask) == 32, "snd_mask ABI");

struct snd_interval {
    unsigned int min, max;
    unsigned int openmin:1, openmax:1, integer:1, empty:1;
};
_Static_assert(sizeof(struct snd_interval) == 12, "snd_interval ABI");

#define SNDRV_PCM_HW_PARAM_ACCESS        0
#define SNDRV_PCM_HW_PARAM_FORMAT        1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT     2
#define SNDRV_PCM_HW_PARAM_FIRST_MASK    SNDRV_PCM_HW_PARAM_ACCESS
#define SNDRV_PCM_HW_PARAM_LAST_MASK     SNDRV_PCM_HW_PARAM_SUBFORMAT

#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS   8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS    9
#define SNDRV_PCM_HW_PARAM_CHANNELS      10
#define SNDRV_PCM_HW_PARAM_RATE          11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME   12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE   13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES  14
#define SNDRV_PCM_HW_PARAM_PERIODS       15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME   16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE   17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES  18
#define SNDRV_PCM_HW_PARAM_TICK_TIME     19
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL SNDRV_PCM_HW_PARAM_SAMPLE_BITS
#define SNDRV_PCM_HW_PARAM_LAST_INTERVAL  SNDRV_PCM_HW_PARAM_TICK_TIME

#define SNDRV_PCM_FORMAT_S16_LE  2

struct snd_pcm_hw_params {
    unsigned int flags;
    struct snd_mask masks[SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1];
    struct snd_mask mres[5];
    struct snd_interval intervals[SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1];
    struct snd_interval ires[9];
    unsigned int rmask;
    unsigned int cmask;
    unsigned int info;
    unsigned int msbits;
    unsigned int rate_num;
    unsigned int rate_den;
    snd_pcm_uframes_t fifo_size;
    unsigned char sync[16];
    unsigned char reserved[48];
};
_Static_assert(sizeof(struct snd_pcm_hw_params) == 608, "snd_pcm_hw_params ABI");

struct snd_pcm_sw_params {
    int tstamp_mode;
    unsigned int period_step;
    unsigned int sleep_min;
    snd_pcm_uframes_t avail_min;
    snd_pcm_uframes_t xfer_align;
    snd_pcm_uframes_t start_threshold;
    snd_pcm_uframes_t stop_threshold;
    snd_pcm_uframes_t silence_threshold;
    snd_pcm_uframes_t silence_size;
    snd_pcm_uframes_t boundary;
    unsigned int proto;
    unsigned int tstamp_type;
    unsigned char reserved[56];
};
_Static_assert(sizeof(struct snd_pcm_sw_params) == 136, "snd_pcm_sw_params ABI");

struct snd_ctl_card_info {
    int card;
    int pad;
    unsigned char id[16];
    unsigned char driver[16];
    unsigned char name[32];
    unsigned char longname[80];
    unsigned char reserved_[16];
    unsigned char mixername[80];
    unsigned char components[128];
};
_Static_assert(sizeof(struct snd_ctl_card_info) == 376, "snd_ctl_card_info ABI");

// _IOC-encoded ioctl numbers, precomputed (dir<<30 | size<<16 | type<<8
// | nr) from upstream's _IOR/_IOW/_IOWR/_IO macro invocations -- same
// "precomputed hex, comment names the source macro" style fb.h already
// uses for FBIOGET_VSCREENINFO etc.
#define SNDRV_CTL_IOCTL_PVERSION   0x80045500u   // _IOR('U',0x00,int)
#define SNDRV_CTL_IOCTL_CARD_INFO  0x81785501u   // _IOR('U',0x01,struct snd_ctl_card_info)

#define SNDRV_PCM_IOCTL_PVERSION   0x80044100u   // _IOR('A',0x00,int)
#define SNDRV_PCM_IOCTL_HW_PARAMS  0xC2604111u   // _IOWR('A',0x11,struct snd_pcm_hw_params)
#define SNDRV_PCM_IOCTL_SW_PARAMS  0xC0884113u   // _IOWR('A',0x13,struct snd_pcm_sw_params)
#define SNDRV_PCM_IOCTL_PREPARE    0x4140u        // _IO('A',0x40)
#define SNDRV_PCM_IOCTL_START      0x4142u        // _IO('A',0x42)
#define SNDRV_PCM_IOCTL_DROP       0x4143u        // _IO('A',0x43)

#endif
