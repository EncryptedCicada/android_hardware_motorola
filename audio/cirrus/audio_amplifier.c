// FIXME:
/*
* On some firmwares the creativity level is high and the mixer
* names will be different.
*
* Instead of stereo configuration, make amends to incorporate RCV
*
*/

#define LOG_TAG "audio_amplifier_cs35l41_motorola"
/*#define LOG_NDEBUG 0*/

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <log/log.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <asm-generic/errno-base.h>
#include <audio_hw.h>
#include <hardware/audio_amplifier.h>
#include <hardware/hardware.h>
#include <log/log_main.h>
#include <malloc.h>
#include <platform.h>
#include <platform_api.h>

#define UNUSED __attribute__((unused))

#define GET_SPEAKER_CALIBRATIONS_FROM_PERSIST 1

/* Amplifier structures definition */
typedef struct amp_device {
    amplifier_device_t amp_dev;
    struct audio_device* adev;
    struct audio_usecase* usecase_tx;
    struct pcm* cs35l41_out;
} cs35l41_t;
static cs35l41_t* cs35l41_dev = NULL;

/* Cirrus playback status */
enum cirrus_playback_state {
    INIT = 0,
    CALIBRATING = 1,
    CALIBRATION_ERROR = 2,
    IDLE = 3,
    PLAYBACK = 4,
};

/* Payload struct for getting calibration result from DSP module */
struct __attribute__((__packed__)) cirrus_cal_result_t {
    uint8_t status[4];
    uint8_t checksum[4];
    uint8_t cal_r[4];
    bool cal_ok;
    bool cal_valid;
};

#ifdef CIRRUS_DIAG
struct cirrus_cal_diag_t {
    uint8_t diag_f0[4];
    uint8_t diag_f0_status[4];
    uint8_t diag_z_low_diff[4];
};
#endif

struct __attribute__((__packed__)) cirrus_cal_file_t {
    struct cirrus_cal_result_t spkl;
    struct cirrus_cal_result_t spkr;
    uint32_t magicsum;
    uint8_t is_stereo;
};

struct cirrus_playback_session {
    // void *adev_handle;
    pthread_mutex_t fb_prot_mutex;
    pthread_t calibration_thread;
    pthread_t failure_detect_thread;
    struct pcm* pcm_rx;
    struct cirrus_cal_result_t spkl;
    struct cirrus_cal_result_t spkr;
    // bool cirrus_drv_enabled;
    bool is_stereo;
    volatile int32_t state;
};

/* Persist paths for Cirrus codec params */
#define PERSIST_CIRRUS_CAL_GLOBAL_CAL_AMBIENT	"/mnt/vendor/persist/factory/audio/spk_ambient"
// #define PERSIST_CIRRUS_CAL_SPK_CAL_R	"/mnt/vendor/persist/factory/audio/spk_cal_r"
// #define PERSIST_CIRRUS_CAL_SPK_CAL_F0	"/mnt/vendor/persist/factory/audio/spk_f0"
/* 
* We've put in anticipated paths for now
* For SPKL and SPKR we've used the same path for cal_r just to make this work. We're going to 
* have to figure out which one works as a mono config later as we only have one cal_r 
*/
#define PERSIST_CIRRUS_CAL_SPKL_CAL_R		"/mnt/vendor/persist/factory/audio/spk_cal_r"
#define PERSIST_CIRRUS_CAL_SPKL_CAL_STATUS		"/mnt/vendor/persist/factory/audio/spkl_status"
#define PERSIST_CIRRUS_CAL_SPKL_CAL_CHECKSUM		"/mnt/vendor/persist/factory/audio/spkl_checksum"
// #define PERSIST_CIRRUS_CAL_SPKL_DIAG_F0		undefined
// #define PERSIST_CIRRUS_CAL_SPKL_DIAG_Z_LOW_DIFF	undefined
// #define PERSIST_CIRRUS_CAL_SPKL_DIAG_F0_STATUS	undefined
#define PERSIST_CIRRUS_CAL_SPKR_CAL_R		"/mnt/vendor/persist/factory/audio/spk_cal_r"
#define PERSIST_CIRRUS_CAL_SPKR_CAL_STATUS		"/mnt/vendor/persist/factory/audio/spkr_status"
#define PERSIST_CIRRUS_CAL_SPKR_CAL_CHECKSUM		"/mnt/vendor/persist/factory/audio/spkr_checksum"
// #define PERSIST_CIRRUS_CAL_SPKR_DIAG_F0		undefined
// #define PERSIST_CIRRUS_CAL_SPKR_DIAG_Z_LOW_DIFF	undefined
// #define PERSIST_CIRRUS_CAL_SPKR_DIAG_F0_STATUS	undefined

/* Mixer controls */
// We've put anticipated mixer controls in place 
#define CIRRUS_CTL_FORCE_WAKE		"SPK Hibernate Force Wake"
#define CIRRUS_CTL_CALI_DIAG_F0		"SPK DSP1X calibration cd DIAG_F0" // unidentified
#define CIRRUS_CTL_CALI_DIAG_F0_STATUS	"SPK DSP1X calibration cd DIAG_F0_STATUS" // unidentified
#define CIRRUS_CTL_CALI_DIAG_Z_LOW_DIFF	"SPK DSP1X calibration cd DIAG_Z_LOW_DIFF" // unidentified
#define CIRRUS_CTL_PROT_CAL_AMBIENT	"SPK DSP1X calibration cd CAL_AMBIENT" // unidentified
#define CIRRUS_CTL_PROT_DIAG_F0		"SPK DSP1X calibration cd DIAG_F0" // unidentified
#define CIRRUS_CTL_PROT_DIAG_F0_STATUS	"SPK DSP1X calibration cd DIAG_F0_STATUS" // unidentified
#define CIRRUS_CTL_PROT_DIAG_Z_LOW_DIFF	"SPK DSP1X calibration cd DIAG_Z_LOW_DIFF" // unidentified
#define CIRRUS_CTL_CALI_CAL_AMBIENT	"SPK DSP1X calibration cd CAL_AMBIENT" // unidentified
#define CIRRUS_CTL_CALI_CAL_R		"SPK DSP1X calibration cd CAL_R" // unidentified
#define CIRRUS_CTL_CALI_CAL_STATUS	"SPK DSP1X calibration cd CAL_STATUS" // unidentified
#define CIRRUS_CTL_CALI_CAL_CHECKSUM	"SPK DSP1X calibration cd CAL_CHECKSUM" // unidentified
#define CIRRUS_CTL_PROT_CAL_R		"SPK DSP1X protection cd CAL_R"
#define CIRRUS_CTL_PROT_CAL_STATUS	"SPK DSP1X protection CAL_STATUS" // unidentified
#define CIRRUS_CTL_PROT_CAL_CHECKSUM	"SPK DSP1X protection CAL_CHECKSUM" // unidentified
#define CIRRUS_CTL_PROT_CAL_STATUS_CD	"SPK DSP1X protection cd CAL_STATUS"
#define CIRRUS_CTL_PROT_CAL_CHECKSUM_CD	"SPK DSP1X protection cd CAL_CHECKSUM"

#define CIRRUS_CTL_PROT_CSPL_ERRORNO	"SPK DSP1X protection cd CSPL_ERRORNO"

#define CIRRUS_CTL_NAME_BUF 40
#define CIRRUS_ERROR_DETECT_SLEEP_US	250000

#define CIRRUS_FIRMWARE_LOAD_SLEEP_US	5000
#define CIRRUS_FIRMWARE_MAX_RETRY	30

/* Saved calibrations */
#ifndef CIRRUS_AUDIO_CAL_PATH
 #define CIRRUS_AUDIO_CAL_PATH "/data/vendor/audio/cirrus_motorola.cal"
#endif

struct pcm_config pcm_config_cirrus_rx = {
    .channels = 8,
    .rate = 48000,
    .period_size = 320,
    .period_count = 4,
    .format = PCM_FORMAT_S32_LE,
    .start_threshold = 0,
    .stop_threshold = INT_MAX,
    .avail_min = 0,
};

static struct cirrus_playback_session handle;
uint8_t cal_ambient[4];

static void* cirrus_do_calibration();
static void* cirrus_failure_detect_thread();

// DEBUG
void list_mixer_controls(int card_num) {
    struct mixer *mixer;
    unsigned int count, i;
    
    // Open the mixer for the specified sound card
    mixer = mixer_open(card_num);
    if (!mixer) {
        ALOGW("%s: Failed to open mixer for card %d\n", __func__, card_num);
        return;
    }
    
    // Get the number of controls
    count = mixer_get_num_ctls(mixer);
    ALOGI("%s: Card %d has %d controls\n", __func__, card_num, count);
    
    // Iterate through all controls
    for (i = 0; i < count; i++) {
        struct mixer_ctl *ctl = mixer_get_ctl(mixer, i);
        if (ctl) {
            const char *name = mixer_ctl_get_name(ctl);
            enum mixer_ctl_type type = mixer_ctl_get_type(ctl);
            
            ALOGI("%s: %3d: %s (", __func__, i, name);
            
            // Print the type of control
            switch (type) {
                case MIXER_CTL_TYPE_BOOL:     ALOGI("%s: %3d: %s (%s)", __func__, i, name, "BOOL"); break;
                case MIXER_CTL_TYPE_INT:      ALOGI("%s: %3d: %s (%s)", __func__, i, name, "INT"); break;
                case MIXER_CTL_TYPE_ENUM:     ALOGI("%s: %3d: %s (%s)", __func__, i, name, "ENUM"); break;
                case MIXER_CTL_TYPE_BYTE:     ALOGI("%s: %3d: %s (%s)", __func__, i, name, "BYTE"); break;
                case MIXER_CTL_TYPE_IEC958:   ALOGI("%s: %3d: %s (%s)", __func__, i, name, "IEC958"); break;
                case MIXER_CTL_TYPE_INT64:    ALOGI("%s: %3d: %s (%s)", __func__, i, name, "INT64"); break;
                case MIXER_CTL_TYPE_UNKNOWN:  ALOGI("%s: %3d: %s (%s)", __func__, i, name, "UNKNOWN"); break;
                default:                      ALOGI("%s: %3d: %s (%s)", __func__, i, name, "???"); break;
            }
            
            // For enum types, list the available values
            if (type == MIXER_CTL_TYPE_ENUM) {
                unsigned int num_enums = mixer_ctl_get_num_enums(ctl);
                for (unsigned int j = 0; j < num_enums; j++) {
                    ALOGI("%s: Enum valid value %3d is: \"%s\"", __func__, j, mixer_ctl_get_enum_string(ctl, j));
                }
            }
        }
    }
    
    mixer_close(mixer);
}
// END DEBUG

static int get_persist_value(const char* path, void* req_value) {
    FILE *file = NULL;
    int ret = 0;
    char *buffer = NULL;
    char *endptr = NULL;
    long value = 0;
    uint8_t *bytes_array = (uint8_t*)req_value; // Cast to uint8_t pointer

    file = fopen(path, "rb");
    if (file == NULL) {
        ALOGE("%s: Cannot read path: %s", __func__, path);
        ret = -EINVAL;
        goto end;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);

    // Check for errors and reasonable file size (more than 0 bytes, less than 32 bytes)
    if (ferror(file) || file_size <= 0 || file_size >= 32) {
        ALOGE("%s: '%s' Unreasonable file size error: %d, length: %ld", __func__,
            path, ferror(file), file_size);
        ret = -EINVAL;
        goto end;
    }

    // Reset file pointer to beginning
    rewind(file);

    // Allocate buffer for file contents
    buffer = (char *)calloc(file_size + 1, 1);
    if (buffer == NULL) {
        ALOGI("%s: memory allocation failure", __func__);
        ret = -ENOMEM;
        goto end;
    }

    // Read entire file
    if (fread(buffer, 1, file_size, file) != (size_t)file_size) {
        ALOGI("%s: fread() error during reading", __func__);
        ret = -EIO;
        goto cleanup;
    }

    // Ensure null termination
    buffer[file_size] = '\0';

    // Reset errno before conversion
    errno = 0;

    // Convert string to long
    value = strtol(buffer, &endptr, 0);
    if (errno != 0) {
        ALOGI("%s: strtol() error during conversion: %d", __func__, errno);
        ret = -EINVAL;
        goto cleanup;
    }

    // Check for complete conversion (should reach end of string)
    if (*endptr != '\0') {
        ALOGI("%s: strtol() data corruption detected", __func__);
        ALOGI("%s: strtol() parse failure", __func__);
        ret = -EINVAL;
        goto cleanup;
    }

    // Store the value as 4 bytes in the array pointed to by req_value
    bytes_array[0] = (value >> 0) & 0xFF;  // Least significant byte
    bytes_array[1] = (value >> 8) & 0xFF;
    bytes_array[2] = (value >> 16) & 0xFF;
    bytes_array[3] = (value >> 24) & 0xFF; // Most significant byte

    ret = 0;

cleanup:
    free(buffer);
end:
    if (file != NULL) {
        fclose(file);
    }
    return ret;
}

#define CSEED 0xC0FFEE
static unsigned int onecsum(struct cirrus_cal_result_t* cal) {
    unsigned int cs = CSEED;
    uint8_t* data = (uint8_t*)cal;
    int i;

    for (i = 0; i < sizeof(struct cirrus_cal_result_t); i++) {
        cs -= 1 + (unsigned int)*data++;
    }

    return cs;
}

static unsigned int calc_magicsum(struct cirrus_cal_result_t* cr, struct cirrus_cal_result_t* cl) {
    return (onecsum(cr) ^ onecsum(cl));
}

#ifndef GET_SPEAKER_CALIBRATIONS_FROM_PERSIST
static int cirrus_cal_from_file(struct cirrus_playback_session *hdl) {
    FILE* fp_calparams = NULL;
    struct cirrus_cal_file_t fdata;
    int ret = -EINVAL;

    /* Is calibration done already? */
    fp_calparams = fopen(CIRRUS_AUDIO_CAL_PATH, "rb");
    if (fp_calparams == NULL)
        return -EINVAL;

    if (fread(&fdata, sizeof(fdata), 1, fp_calparams) != 1) {
        ALOGD("%s: Failure: Unexpected calibration file content.", __func__);
        ret = -EINVAL;
        goto end;
    }

    if (onecsum(&fdata.spk) != fdata.checksum) {
        ALOGD("%s: Failure: File checksum mismatch", __func__);
        ret = -EINVAL;
        goto end;
    }

    ALOGD("%s: Using stored calibrations", __func__);
    memcpy((void*)&hdl->spkl, (const void*)&fdata.spkl, sizeof(fdata.spkl));
    memcpy((void*)&hdl->spkr, (const void*)&fdata.spkr, sizeof(fdata.spkr));
    hdl->is_stereo = (fdata.is_stereo == 1);
    ret = 0;

end:
    fclose(fp_calparams);
    return ret;
}
#endif

static int cirrus_save_calibration(struct cirrus_playback_session *hdl) {
    FILE* fp_calparams = NULL;
    struct cirrus_cal_file_t fdata;
    int ret = 0;

    fp_calparams = fopen(CIRRUS_AUDIO_CAL_PATH, "wb");
    if (fp_calparams == NULL) return -EINVAL;

    memcpy((void*)&fdata.spkl, (const void*)&hdl->spkl, sizeof(fdata.spkl));
    memcpy((void*)&fdata.spkr, (const void*)&hdl->spkr, sizeof(fdata.spkr));
    fdata.magicsum = calc_magicsum(&fdata.spkl, &fdata.spkr);
    fdata.is_stereo = (uint8_t)hdl->is_stereo;

    if (fwrite(&fdata, sizeof(fdata), 1, fp_calparams) != 1) ret = ferror(fp_calparams);

    fclose(fp_calparams);
    return ret;
}

static int cirrus_format_mixer_name(const char* name, const char* channel, char* buf_out,
                                    int buf_sz) {
    if (name == NULL) return -EINVAL;

    memset(buf_out, 0, buf_sz);

    /*
     * If we have two amps, then we have L and R controls, otherwise
     * in case of mono device (single amp), we have no L/R controls
     * and we don't append anything to the original control names
     *
     * Example:    MONO      STEREO L     STEREO R
     * *******   CCM Reset  L CCM Reset  R CCM Reset
     *
     * So, the "channel" variable may contain:
     * 0 for MONO, L or R for STEREO L/R.
     */
    if (channel == NULL || channel[0] < 'L') return snprintf(buf_out, buf_sz, "%s", name);

    return snprintf(buf_out, buf_sz, "%s %s", channel, name);
}

/* TODO: This function assumes that we are always using CARD 0 */
static int cirrus_set_mixer_value_by_name(char* ctl_name, int value) {
    struct mixer* card_mixer = NULL;
    struct mixer_ctl* ctl_config = NULL;
    int sndcard_id = 0, ret = 0;

    card_mixer = mixer_open(sndcard_id);
    if (!card_mixer) {
        ALOGE("%s: Cannot open mixer for card %d.", __func__, sndcard_id);
        return -1;
    }

    ctl_config = mixer_get_ctl_by_name(card_mixer, ctl_name);
    if (!ctl_config) {
        ALOGD("%s: Cannot get mixer control %s", __func__, ctl_name);
        ret = -1;
        goto exit;
    }

    ret = mixer_ctl_set_value(ctl_config, 0, value);
    if (ret < 0) ALOGE("%s: Cannot set mixer '%s' to '%d'", __func__, ctl_name, value);
exit:
    mixer_close(card_mixer);
    return ret;
}

static int cirrus_set_mixer_value_by_name_lr(char* ctl_base_name, int value) {
    char ctl_name[CIRRUS_CTL_NAME_BUF];
    int ret = 0;

    ret = cirrus_format_mixer_name(ctl_base_name, "L", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_set_mixer_value_by_name(ctl_name, value);
    if (ret < 0) {
        ALOGE("%s: Cannot set mixer '%s' to '%d'", __func__, ctl_name, value);
        goto end;
    }

    ret = cirrus_format_mixer_name(ctl_base_name, "R", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_set_mixer_value_by_name(ctl_name, value);
    if (ret < 0) ALOGE("%s: Cannot set mixer '%s' to '%d'", __func__, ctl_name, value);
end:
    return ret;
}

static int cirrus_get_mixer_value_by_name(char* ctl_name) {
    struct mixer* card_mixer = NULL;
    struct mixer_ctl* ctl_config = NULL;
    int sndcard_id = 0, ret = -EINVAL;

    card_mixer = mixer_open(sndcard_id);
    if (!card_mixer) {
        ALOGE("%s: Cannot open mixer for card %d.", __func__, sndcard_id);
        return -1;
    }

    ctl_config = mixer_get_ctl_by_name(card_mixer, ctl_name);
    if (!ctl_config) {
        ALOGE("%s: Cannot get mixer control %s", __func__, ctl_name);
        ret = -1;
        goto exit;
    }

    ret = mixer_ctl_get_value(ctl_config, 0);
    if (ret < 0) ALOGE("%s: Cannot get mixer %s value: error %d", __func__, ctl_name, ret);
exit:
    mixer_close(card_mixer);
    return ret;
}

static int cirrus_set_mixer_array_by_name(char* ctl_name, void* array, size_t count) {
    struct mixer* card_mixer = NULL;
    struct mixer_ctl* ctl_config = NULL;
    int sndcard_id = 0, ret = 0;

    card_mixer = mixer_open(sndcard_id);
    if (!card_mixer) {
        ALOGE("%s: Cannot open mixer for card %d.", __func__, sndcard_id);
        return -1;
    }

    ctl_config = mixer_get_ctl_by_name(card_mixer, ctl_name);
    if (!ctl_config) {
        ALOGD("%s: Cannot get mixer control %s", __func__, ctl_name);
        ret = -1;
        goto exit;
    }

    ret = mixer_ctl_set_array(ctl_config, array, count);
    if (ret < 0) ALOGE("%s: Cannot set mixer %s", __func__, ctl_name);
exit:
    mixer_close(card_mixer);
    return ret;
}

static int cirrus_get_mixer_array_by_name(char* ctl_name, void* array, size_t count) {
    struct mixer* card_mixer = NULL;
    struct mixer_ctl* ctl_config = NULL;
    int sndcard_id = 0, ret = -EINVAL;

    card_mixer = mixer_open(sndcard_id);
    if (!card_mixer) {
        ALOGE("%s: Cannot open mixer for card %d.", __func__, sndcard_id);
        return -1;
    }

    ctl_config = mixer_get_ctl_by_name(card_mixer, ctl_name);
    if (!ctl_config) {
        ALOGE("%s: Cannot get mixer control %s", __func__, ctl_name);
        ret = -1;
        goto exit;
    }

    memset(array, 0, count);

    ret = mixer_ctl_get_array(ctl_config, array, count);
    if (ret < 0) ALOGE("%s: Cannot get mixer %s value: error %d", __func__, ctl_name, ret);
exit:
    mixer_close(card_mixer);
    return ret;
}

static int cirrus_set_mixer_enum_by_name(char* ctl_name, const char* value) {
    struct mixer* card_mixer = NULL;
    struct mixer_ctl* ctl_config = NULL;
    int sndcard_id = 0, ret = 0;

    card_mixer = mixer_open(sndcard_id);
    if (!card_mixer) {
        ALOGE("%s: Cannot open mixer for card %d.", __func__, sndcard_id);
        return -1;
    }

    ctl_config = mixer_get_ctl_by_name(card_mixer, ctl_name);
    if (!ctl_config) {
        ALOGE("%s: Cannot get mixer control '%s'", __func__, ctl_name);
        ret = -1;
        goto exit;
    }

    ret = mixer_ctl_set_enum_by_string(ctl_config, value);
    if (ret < 0) ALOGE("%s: Cannot set mixer '%s' to '%s'", __func__, ctl_name, value);
exit:
    mixer_close(card_mixer);
    return ret;
}

static int cirrus_play_silence(int seconds) {
    struct audio_device* adev = cs35l41_dev->adev;
    struct audio_usecase* uc_info_rx;

    uint8_t* silence = NULL;
    int i, ret = 0, silence_cnt = 1;
    unsigned int buffer_size = 0, frames_bytes = 0;
    int pcm_dev_rx_id, adev_retry = 5;

    if (!list_empty(&adev->usecase_list)) {
        ALOGD("%s: Usecase present retry speaker protection", __func__);
        return -EAGAIN;
    }

    uc_info_rx = (struct audio_usecase*)calloc(1, sizeof(struct audio_usecase));
    if (!uc_info_rx) {
        return -ENOMEM;
    }

    while ((!adev->primary_output || !adev->platform) && adev_retry) {
        ALOGI("%s: Waiting for audio device...", __func__);
        sleep(1);
        adev_retry--;
    }

    uc_info_rx->id = USECASE_AUDIO_PLAYBACK_DEEP_BUFFER;
    uc_info_rx->type = PCM_PLAYBACK;
    uc_info_rx->in_snd_device = SND_DEVICE_NONE;
    uc_info_rx->stream.out = adev->primary_output;
    list_init(&uc_info_rx->device_list);
    uc_info_rx->out_snd_device = SND_DEVICE_OUT_SPEAKER_PROTECTED;
    list_add_tail(&adev->usecase_list, &uc_info_rx->list);

    platform_check_and_set_codec_backend_cfg(adev, uc_info_rx, uc_info_rx->out_snd_device);

    enable_snd_device(adev, uc_info_rx->out_snd_device);
    enable_audio_route(adev, uc_info_rx);

    pcm_dev_rx_id = platform_get_pcm_device_id(uc_info_rx->id, PCM_PLAYBACK);
    ALOGV("%s: pcm device id %d", __func__, pcm_dev_rx_id);
    if (pcm_dev_rx_id < 0) {
        ALOGE("%s: Invalid pcm device for usecase (%d)", __func__, uc_info_rx->id);
        goto exit;
    }

    handle.pcm_rx = pcm_open(adev->snd_card, pcm_dev_rx_id, (PCM_OUT | PCM_MONOTONIC),
                             &pcm_config_cirrus_rx);
    if (!handle.pcm_rx) {
        ALOGE("%s: Cannot open output PCM", __func__);
        ret = -EIO;
        goto exit;
    }

    if (!pcm_is_ready(handle.pcm_rx)) {
        ALOGE("%s: The PCM device is not ready: %s", __func__, pcm_get_error(handle.pcm_rx));
        ret = -EIO;
        goto exit;
    }

    buffer_size = pcm_get_buffer_size(handle.pcm_rx);
    frames_bytes = pcm_frames_to_bytes(handle.pcm_rx, buffer_size);

    silence = (uint8_t*)calloc(1, frames_bytes);
    if (silence == NULL) {
        ALOGE("%s: Cannot allocate %d bytes: Memory exhausted.", __func__, frames_bytes);
        goto exit;
    }

    silence_cnt = pcm_frames_to_bytes(handle.pcm_rx, pcm_config_cirrus_rx.rate);
    silence_cnt = silence_cnt * seconds / frames_bytes + 1;

    ALOGD("%s: Start playing silence audio: sec=%d, count=%d", __func__, seconds, silence_cnt);
    for (i = 0; i <= silence_cnt; i++) {
        ret = pcm_write(handle.pcm_rx, silence, frames_bytes);
        if (ret) {
            ALOGE("%s: Cannot write PCM data: %d", __func__, ret);
            break;
        } else
            ALOGV("%s: Wrote PCM data", __func__);
    }
    ALOGD("%s: Stop playing silence audio", __func__);
    free(silence);

exit:
    if (handle.pcm_rx != NULL) {
        pcm_close(handle.pcm_rx);
        handle.pcm_rx = NULL;
    }

    disable_audio_route(adev, uc_info_rx);
    disable_snd_device(adev, uc_info_rx->out_snd_device);

    list_remove(&uc_info_rx->list);
    free(uc_info_rx);

    return ret;
}

static inline int cirrus_set_force_wake(bool enable) {
    int ret = 0;

    if (handle.is_stereo) {
        ret = cirrus_set_mixer_value_by_name_lr(CIRRUS_CTL_FORCE_WAKE, (int)enable);
    } else {
        ret = cirrus_set_mixer_value_by_name(CIRRUS_CTL_FORCE_WAKE, (int)enable);
    }

    if (ret < 0)
        ALOGE("%s: Cannot %s force wakeup", __func__, enable ? "enable" : "disable");
    else
        ALOGD("%s: Set %s %s", __func__, CIRRUS_CTL_FORCE_WAKE, enable ? "enable" : "disable");
    return ret;
}

static int cirrus_do_reset(const char* channel) {
    char ctl_name[CIRRUS_CTL_NAME_BUF];
    int ret = 0;

    ret = cirrus_format_mixer_name("CCM Reset", channel, ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_get_mixer_value_by_name(ctl_name);
    if (ret < 0) {
        ALOGE("%s: CCM Reset is missing!!!", __func__);
    } else {
        ret = cirrus_set_mixer_value_by_name(ctl_name, 1);
        ALOGI("%s: CCM Reset done.", __func__);
    }

    return ret;
}

static int cirrus_mixer_wait_for_setting(char* ctl, int val, int retry) {
    int i, ret;

    for (i = 0; i < retry; i++) {
        /* Start firmware download sequence: shut down DSP and reset states */
        ret = cirrus_get_mixer_value_by_name(ctl);
        if (ret < 0 || ret == val) break;

        usleep(10000);
    }
    if (ret < 0 && i == retry) return -ETIMEDOUT;

    return ret;
}

static int cirrus_exec_fw_download(const char* fw_type, const char* channel, int do_reset) {
    char ctl_name[CIRRUS_CTL_NAME_BUF];
    uint8_t cspl_ena[4] = { 0 };
    int retry = 0, ret;

    ALOGD("%s: Asking for %s %s firmware %s", __func__, fw_type,
          ((channel && channel[0] != 0) ? channel : "(mono/global)"),
          (do_reset ? "with reset" : "without reset"));
    if (do_reset) ret = cirrus_do_reset(channel);

    /* If this one is missing, we're not using our Cirrus codec... */
    ret = cirrus_format_mixer_name("SPK DSP Booted", channel, ctl_name, sizeof(ctl_name));
    if (ret < 0)
        return ret;
    ret = cirrus_get_mixer_value_by_name(ctl_name);
    if (ret < 0) {
        ALOGE("%s: %s control is missing. Bailing out.", __func__, ctl_name);
        ret = -ENODEV;
        goto exit;
    }

    /* Start firmware download sequence: shut down DSP and reset states */
    ret = cirrus_set_mixer_value_by_name(ctl_name, 0);
    if (ret < 0) {
        ALOGE("%s: Cannot reset %s status", __func__, ctl_name);
        goto exit;
    }

    ret = cirrus_mixer_wait_for_setting(ctl_name, 0, 10);
    if (ret < 0) {
        ALOGE("%s: %s wait setting error %d", __func__, ctl_name, ret);
        goto exit;
    }

    usleep(10000);

    ret = cirrus_format_mixer_name("SPK DSP1 Preload Switch", channel, ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_set_mixer_value_by_name(ctl_name, 0);
    if (ret < 0) {
        ALOGE("%s: Cannot reset %s", __func__, ctl_name);
        goto exit;
    }

    ret = cirrus_mixer_wait_for_setting(ctl_name, 0, 10);
    if (ret < 0) {
        ALOGE("%s: %s wait setting error %d", __func__, ctl_name, ret);
        goto exit;
    }

    usleep(10000);

    /* Determine what firmware to load and configure DSP */
    ret = cirrus_format_mixer_name("SPK DSP1 Firmware", channel, ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_set_mixer_enum_by_name(ctl_name, fw_type);
    if (ret < 0) {
        ALOGE("%s: Cannot set %s to %s", __func__, ctl_name, fw_type);
        goto exit;
    }

    ret = cirrus_format_mixer_name("SPK PCM Source", channel, ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_set_mixer_enum_by_name(ctl_name, "DSP");
    if (ret < 0) {
        ALOGE("%s: Cannot set %s to DSP", __func__, ctl_name);
        goto exit;
    }

    /* Send the firmware! */
    ret = cirrus_format_mixer_name("SPK DSP1 Preload Switch", channel, ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_set_mixer_value_by_name(ctl_name, 1);
    if (ret < 0) {
        ALOGE("%s: Cannot set %s to %s", __func__, ctl_name, fw_type);
        goto exit;
    }

    /* Motorola uses lowercase for fw_type */
    if (!strcmp(fw_type, "protection")) {
        ret = cirrus_format_mixer_name("SPK DSP1X protection cd CSPL_ENABLE", channel, ctl_name, sizeof(ctl_name));
    } else if (!strcmp(fw_type, "calibration")) {
        ret = cirrus_format_mixer_name("SPK DSP1X calibration cd CSPL_ENABLE", channel, ctl_name, sizeof(ctl_name));
    } else {
        ret = -EINVAL;
        ALOGE("%s: ERROR! Unsupported firmware type passed: %s",
              __func__, fw_type);
        goto exit;
    }

retry_fw:
    /*
     * Sleep for some time: checking right after sending the load command
     * is useless, the firmware at least won't be booted for sure.
     */
    usleep(CIRRUS_FIRMWARE_LOAD_SLEEP_US);

    ret = cirrus_get_mixer_array_by_name(ctl_name, &cspl_ena, 4);
    if (ret < 0) {
        if (retry < CIRRUS_FIRMWARE_MAX_RETRY) {
            retry++;
            ALOGI("%s: Retrying...\n", __func__);
            goto retry_fw;
        } else {
            ALOGE("%s: Cannot get %s stats", __func__, ctl_name);
            goto exit;
        }
    }

    if ((cspl_ena[0] + cspl_ena[1] + cspl_ena[2]) == 0 && cspl_ena[3] == 1) {
        ALOGI("%s: Cirrus %s Firmware Download SUCCESS.", __func__, fw_type);
        /* Wait for the hardware to stabilize */
        usleep(100000);
        ret = 0;
    } else {
        /*
         * Since we are using a poor hack to load the firmware, we cannot know
         * if the firmware was found nor if it finished loading remotely.
         * We also don't know how much time does the chip require to actually
         * boot it, so we will sleep and retry for X times, until it loads and
         * boots, or we assume that something went wrong: in that case the
         * only thing left to do is to return an error, hoping that developers
         * will catch it before going crazy...
         *
         * Perhaps, one day we will rewrite this messy part.
         */
        if (retry < CIRRUS_FIRMWARE_MAX_RETRY) {
            retry++;
            ALOGI("%s: Retrying...\n", __func__);
            goto retry_fw;
        }

        ALOGE("%s: Firmware download failure. CSPL Status: %u %u %u %u", __func__, cspl_ena[0], cspl_ena[1], cspl_ena[2], cspl_ena[3]);
        ret = -EINVAL;
    }

exit:
    return ret;
}

static int cirrus_mono_calibration(void) {
    struct audio_device *adev = handle.adev_handle;
#ifdef CIRRUS_DIAG
    struct cirrus_cal_diag_t cal_diag;
#endif
    bool stat_l_nok = true;
    int ret = 0;

    ret = cirrus_set_force_wake(true);
    if (ret < 0) {
        ALOGE("%s: Cannot force wakeup", __func__);
        goto exit;
    }

    ret = cirrus_set_mixer_array_by_name(CIRRUS_CTL_CALI_CAL_AMBIENT, cal_ambient, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot set ambient calibration", __func__);
        goto exit;
    }

    /* Play silence to run calibration internally */
    ret = cirrus_play_silence(2);
    if (ret < 0) ALOGW("%s: Playing silence went wrong, calibration may fail...", __func__);

#ifdef CIRRUS_DIAG
    ret = cirrus_get_mixer_array_by_name(CIRRUS_CTL_CALI_DIAG_F0, &cal_diag.diag_f0, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s stats", __func__, CIRRUS_CTL_CALI_DIAG_F0);
        goto exit;
    }

    ret = cirrus_get_mixer_array_by_name(CIRRUS_CTL_CALI_DIAG_F0_STATUS, &cal_diag.diag_f0_status, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, CIRRUS_CTL_CALI_DIAG_F0_STATUS);
        goto exit;
    }

    ret = cirrus_get_mixer_array_by_name(CIRRUS_CTL_CALI_DIAG_Z_LOW_DIFF, &cal_diag.diag_z_low_diff, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, CIRRUS_CTL_CALI_DIAG_Z_LOW_DIFF);
        goto exit;
    }

    ALOGD("%s: Diagnostics -- "
          "F0: 0x%x 0x%x 0x%x 0x%x   "
          "F0_STATUS: 0x%x 0x%x 0x%x 0x%x   "
          "Z_LOW_DIFF: 0x%x 0x%x 0x%x 0x%x", __func__,
          cal_diag.diag_f0[0], cal_diag.diag_f0[1],
          cal_diag.diag_f0[2], cal_diag.diag_f0[3],
          cal_diag.diag_f0_status[0], cal_diag.diag_f0_status[1],
          cal_diag.diag_f0_status[2], cal_diag.diag_f0_status[3],
          cal_diag.diag_z_low_diff[0], cal_diag.diag_z_low_diff[1],
          cal_diag.diag_z_low_diff[2], cal_diag.diag_z_low_diff[3]);
#endif

    ret = cirrus_get_mixer_array_by_name(CIRRUS_CTL_CALI_CAL_STATUS, &handle.spkr.status, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, CIRRUS_CTL_CALI_CAL_STATUS);
        goto exit;
    }

    stat_l_nok = !!(handle.spkr.status[0] | handle.spkr.status[1] |
                   handle.spkr.status[2]);
    if (stat_l_nok || handle.spkr.status[3] != 1) {
        if (!stat_l_nok && handle.spkr.status[3] == 3) ALOGE("%s: The calibration is out of range", __func__);
        ALOGE("%s: Calibration failure, status: 0x%x 0x%x 0x%x 0x%x", __func__, handle.spkr.status[0], handle.spkr.status[1], handle.spkr.status[2], handle.spkr.status[3]);
        ret = -EINVAL;
        goto exit;
    }

    ret = cirrus_get_mixer_array_by_name(CIRRUS_CTL_CALI_CAL_CHECKSUM, &handle.spkr.checksum, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, CIRRUS_CTL_CALI_CAL_CHECKSUM);
        goto exit;
    }

    ret = cirrus_get_mixer_array_by_name(CIRRUS_CTL_CALI_CAL_R, &handle.spkr.cal_r, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, CIRRUS_CTL_CALI_CAL_R);
        goto exit;
    }

#ifdef DEBUG_SHOW_VALUES
    ALOGE("%s: DEBUG! status: 0x%x 0x%x 0x%x 0x%x  "
          "csum: 0x%x 0x%x 0x%x 0x%x  "
          "Z: 0x%x 0x%x 0x%x 0x%x",
          __func__, handle.spkr.status[0], handle.spkr.status[1],
          handle.spkr.status[2], handle.spkr.status[3],
          handle.spkr.checksum[0], handle.spkr.checksum[1],
          handle.spkr.checksum[2], handle.spkr.checksum[3],
          handle.spkr.cal_r[0], handle.spkr.cal_r[1],
          handle.spkr.cal_r[2], handle.spkr.cal_r[3]);
#endif

    /* It HAS TO stay awake until Protection is loaded!!! */
    ret = cirrus_set_force_wake(true);
    if (ret < 0) {
        goto exit;
    }

    /* Calibration is done, smooth sailing! */
    handle.spkr.cal_ok = true;

exit:
    return ret;
}

static int cirrus_stereo_calibration(void) {
    char ctl_name[CIRRUS_CTL_NAME_BUF];
    bool stat_l_nok = true, stat_r_nok = true;
    int ret = 0;

    ret = cirrus_set_force_wake(true);
    if (ret < 0) goto exit;

    /* Same CAL_AMBIENT for both speakers */
    ret = cirrus_set_mixer_value_by_name_lr(CIRRUS_CTL_CALI_CAL_AMBIENT, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot set ambient calibration", __func__);
        goto exit;
    }

    /* Play silence to run calibration internally */
    ret = cirrus_play_silence(2);
    if (ret < 0) ALOGW("%s: Playing silence went wrong, calibration may fail...", __func__);

    ret = cirrus_format_mixer_name(CIRRUS_CTL_CALI_CAL_STATUS, "L", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_get_mixer_array_by_name(ctl_name, &handle.spkl.status, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, ctl_name);
        goto exit;
    }

    ret = cirrus_format_mixer_name(CIRRUS_CTL_CALI_CAL_STATUS, "R", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_get_mixer_array_by_name(ctl_name, &handle.spkr.status, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, ctl_name);
        goto exit;
    }

    stat_l_nok = !!(handle.spkl.status[0] | handle.spkl.status[1] | handle.spkl.status[2]);
    if (stat_l_nok || handle.spkl.status[3] != 1) {
        if (!stat_l_nok && handle.spkl.status[3] == 3)
            ALOGE("%s: The SPK L calibration is out of range", __func__);
        ALOGE("%s: SPK L Calibration failure, status: 0x%x 0x%x 0x%x 0x%x", __func__,
              handle.spkl.status[0], handle.spkl.status[1], handle.spkl.status[2],
              handle.spkl.status[3]);
        ret = -EINVAL;
        goto exit;
    }

    stat_r_nok = !!(handle.spkr.status[0] | handle.spkr.status[1] | handle.spkr.status[2]);
    if (stat_r_nok || handle.spkr.status[3] != 1) {
        if (!stat_l_nok && handle.spkr.status[3] == 3)
            ALOGE("%s: The SPK R calibration is out of range", __func__);
        ALOGE("%s: SPK R Calibration failure, status: 0x%x 0x%x 0x%x 0x%x", __func__,
              handle.spkr.status[0], handle.spkr.status[1], handle.spkr.status[2],
              handle.spkr.status[3]);
        ret = -EINVAL;
        goto exit;
    }

    ret = cirrus_format_mixer_name(CIRRUS_CTL_CALI_CAL_CHECKSUM, "L", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_get_mixer_array_by_name(ctl_name, &handle.spkl.checksum, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, ctl_name);
        goto exit;
    }

    ret = cirrus_format_mixer_name(CIRRUS_CTL_CALI_CAL_CHECKSUM, "R", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_get_mixer_array_by_name(ctl_name, &handle.spkr.checksum, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, ctl_name);
        goto exit;
    }

    ret = cirrus_format_mixer_name(CIRRUS_CTL_CALI_CAL_R, "L", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_get_mixer_array_by_name(ctl_name, &handle.spkl.cal_r, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, ctl_name);
        goto exit;
    }

    ret = cirrus_format_mixer_name(CIRRUS_CTL_CALI_CAL_R, "R", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_get_mixer_array_by_name(ctl_name, &handle.spkr.cal_r, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, ctl_name);
        goto exit;
    }

    ret = cirrus_set_force_wake(true);
    if (ret < 0) goto exit;

    /* Calibration is done, smooth sailing! */
    handle.spkl.cal_ok = true;
    handle.spkr.cal_ok = true;

exit:
    return ret;
}

static int cirrus_write_cal_checksum(struct cirrus_cal_result_t* cal, char* lr) {
    char ctl_name[CIRRUS_CTL_NAME_BUF];
    int ret;

    ret = cirrus_format_mixer_name(CIRRUS_CTL_PROT_CAL_CHECKSUM, lr, ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;

    ret = cirrus_set_mixer_array_by_name(ctl_name, cal->checksum, 4);
    if (ret >= 0) goto exit;

    /*
     * On some firmwares the creativity level is high and the mixer
     * names will be different.
     */
    ret = cirrus_format_mixer_name(CIRRUS_CTL_PROT_CAL_CHECKSUM_CD, lr, ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;

    ret = cirrus_set_mixer_array_by_name(ctl_name, cal->checksum, 4);
exit:
    return ret;
}

static int cirrus_write_cal_status(struct cirrus_cal_result_t* cal, char* lr) {
    char ctl_name[CIRRUS_CTL_NAME_BUF];
    int ret;

    ret = cirrus_format_mixer_name(CIRRUS_CTL_PROT_CAL_STATUS, lr, ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;

    ret = cirrus_set_mixer_array_by_name(ctl_name, cal->status, 4);
    if (ret >= 0) goto exit;

    ret = cirrus_format_mixer_name(CIRRUS_CTL_PROT_CAL_STATUS_CD, lr, ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;

    ret = cirrus_set_mixer_array_by_name(ctl_name, cal->status, 4);
exit:
    return ret;
}

static int cirrus_do_fw_mono_download(int do_reset) {
    bool cal_valid = false, status_ok = false, checksum_ok = false;
    int i, max_retries = 32, ret = 0;

    for (i = 0; i < max_retries; i++) {
        ret = cirrus_exec_fw_download("protection", 0, do_reset);
        if (ret == 0)
            break;
        usleep(500000);
    }
    if (ret != 0) {
        ALOGE("%s: Cannot send Protection firmware: bailing out.",
              __func__);
        return -EINVAL;
    }

    /* If the calibration is not valid, keep the fw loaded but get out. */
    if (!handle.spkr.cal_ok) return -EINVAL;

    ret = cirrus_set_force_wake(true);
    if (ret < 0) goto exit;

    ret = cirrus_set_mixer_array_by_name(CIRRUS_CTL_PROT_CAL_R, &handle.spkr.cal_r, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot set Z calibration", __func__);
        goto exit;
    }

    ret = cirrus_write_cal_status(&handle.spkr, 0);
    if (ret < 0) {
        ALOGE("%s: Cannot set calibration status", __func__);
        goto exit;
    }

    ret = cirrus_write_cal_checksum(&handle.spkr, 0);
    if (ret < 0) {
        ALOGE("%s: Cannot set calibration checksum", __func__);
        goto exit;
    }

    /* Time to get some rest: work is done! */
    ret = cirrus_set_force_wake(false);
    if (ret < 0)
        goto exit;

exit:
    ret += cirrus_play_silence(0);
    return ret;
}

static int cirrus_do_fw_stereo_download(int do_reset) {
    char ctl_name[CIRRUS_CTL_NAME_BUF];
    int i, max_retries = 32, ret = 0;

    ALOGI("%s: Sending speaker protection stereo firmware", __func__);

    for (i = 0; i < max_retries; i++) {
        ret = cirrus_exec_fw_download("protection", "R", do_reset);
        if (ret == 0) break;
        usleep(500000);
    }
    if (ret != 0) {
        ALOGE("%s: Cannot send Protection R firmware: bailing out.", __func__);
        return -EINVAL;
    }

    /*
     * Guarantee that we retry for at least 3 seconds... but the
     * other firmware should just load instantly, since we've been
     * waiting for the DSP at R-SPK loading time.
     *
     * This is only to paranoidly account any possible future issue.
     */
    if (max_retries < 6) max_retries = 6;

    for (i = 0; i < max_retries; i++) {
        ret = cirrus_exec_fw_download("protection", "L", do_reset);
        if (ret == 0) break;
        usleep(500000);
    }
    if (ret != 0) {
        ALOGE("%s: Cannot send Protection L firmware: bailing out.", __func__);
        return -EINVAL;
    }

    /* If the calibration is not valid, keep the fw loaded but get out. */
    if (!handle.spkl.cal_valid || !handle.spkr.cal_valid) {
        ALOGI("%s: The calibration data is not valid, just return with fw loaded", __func__);
        return 0;
    }

    ret = cirrus_set_force_wake(true);
    if (ret < 0) goto exit;

    ret = cirrus_format_mixer_name(CIRRUS_CTL_PROT_CAL_R, "R", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_set_mixer_array_by_name(ctl_name, &handle.spkr.cal_r, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot set Z-R calibration", __func__);
        goto exit;
    }

    ret = cirrus_write_cal_status(&handle.spkr, "R");
    if (ret < 0) {
        ALOGE("%s: Cannot set calibration R status", __func__);
        goto exit;
    }

    ret = cirrus_write_cal_checksum(&handle.spkr, "R");
    if (ret < 0) {
        ALOGE("%s: Cannot set checksum R", __func__);
        goto exit;
    }

    ret = cirrus_format_mixer_name(CIRRUS_CTL_PROT_CAL_R, "L", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_set_mixer_array_by_name(ctl_name, &handle.spkl.cal_r, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot set Z-L calibration", __func__);
        goto exit;
    }

    ret = cirrus_write_cal_status(&handle.spkl, "L");
    if (ret < 0) {
        ALOGE("%s: Cannot set calibration L status", __func__);
        goto exit;
    }

    ret = cirrus_write_cal_checksum(&handle.spkl, "L");
    if (ret < 0) {
        ALOGE("%s: Cannot set checksum L", __func__);
        goto exit;
    }

    /* Time to get some rest: work is done! */
    ret = cirrus_set_force_wake(false);
    if (ret < 0) goto exit;

exit:
    ret += cirrus_play_silence(0);
    return ret;
}

static int cirrus_do_fw_calibration_download(struct cirrus_playback_session* hdl) {
    int ret = 0;

    ret = cirrus_exec_fw_download("calibration", 0, 0);
    if (ret < 0) {
        ret = cirrus_exec_fw_download("calibration", "L", 0);
        ret += cirrus_exec_fw_download("calibration", "R", 0);
        if (ret != 0) return ret;

        /* Dual amp case */
        hdl->is_stereo = true;
    }

    return ret;
}

static void* cirrus_do_calibration() {
    struct audio_device* adev = cs35l41_dev->adev;
    int ret = 0;

    handle.state = CALIBRATING;

    if (handle.spkl.cal_ok && handle.spkr.cal_ok) goto skip_calibration;

    ALOGI("%s: Calibrating with ambient values 0x%x 0x%x 0x%x 0x%x", __func__, cal_ambient[0],
          cal_ambient[1], cal_ambient[2], cal_ambient[3]);

    ret = cirrus_do_fw_calibration_download(&handle);
    if (ret != 0) {
        ALOGE("%s: Cannot send Calibration firmware: bailing out.", __func__);
        ret = -EINVAL;
        goto end;
    }

    if (handle.is_stereo)
        ret = cirrus_stereo_calibration();
    else
        ret = cirrus_mono_calibration();

    if (ret < 0) {
        ALOGE("%s: CRITICAL: Calibration failure", __func__);
        goto end;
    }
    ALOGI("%s: Calibration success! Saving state and waiting for DSP...", __func__);

    ret = cirrus_save_calibration(&handle);
    if (ret) {
        /* We don't trigger a failure here: audio will still work... */
        ALOGW("%s: Cannot save calibration to file (%d)!!!", __func__, ret);
        ret = 0;
    }

skip_calibration:
    if (handle.is_stereo)
        ret = cirrus_do_fw_stereo_download(0);
    else
        ret = cirrus_do_fw_mono_download(0);
    if (ret < 0) ALOGE("%s: Cannot send speaker protection FW", __func__);

end:
    pthread_mutex_lock(&adev->lock);
    if (ret < 0)
        handle.state = CALIBRATION_ERROR;
    else
        handle.state = IDLE;
    pthread_mutex_unlock(&adev->lock);

    pthread_exit(0);
    return NULL;
}

static int cirrus_check_error_state_mono(void) {
    uint8_t cspl_error[4] = {0};
    int ret = 0;

    ret = cirrus_set_force_wake(true);
    if (ret < 0) return ret;

    ret = cirrus_get_mixer_array_by_name(CIRRUS_CTL_PROT_CSPL_ERRORNO, &cspl_error, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get CSPL status", __func__);
        goto exit;
    }

    if (cspl_error[3] != 0) {
        ALOGE("%s: Error state detected!", __func__);
        ret = -EREMOTEIO;
    }

exit:
    ret = cirrus_set_force_wake(false);
    return ret;
}

static int cirrus_check_error_state_stereo(void) {
    char ctl_name[CIRRUS_CTL_NAME_BUF];
    uint8_t cspl_error[4] = {0};
    int ret = 0;

    ret = cirrus_set_force_wake(true);
    if (ret < 0) return ret;

    ret = cirrus_format_mixer_name(CIRRUS_CTL_PROT_CSPL_ERRORNO, "L", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_get_mixer_array_by_name(ctl_name, &cspl_error, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, ctl_name);
        goto exit;
    }

    if (cspl_error[3] != 0) {
        ALOGE("%s: Error state detected on SPK-L!", __func__);
        ret = -EREMOTEIO;
        goto exit;
    }

    ret = cirrus_format_mixer_name(CIRRUS_CTL_PROT_CSPL_ERRORNO, "R", ctl_name, sizeof(ctl_name));
    if (ret < 0) return ret;
    ret = cirrus_get_mixer_array_by_name(ctl_name, &cspl_error, 4);
    if (ret < 0) {
        ALOGE("%s: Cannot get %s", __func__, ctl_name);
        goto exit;
    }

    if (cspl_error[3] != 0) {
        ALOGE("%s: Error state detected on SPK-R!", __func__);
        ret = -EREMOTEIO;
        goto exit;
    }

exit:
    ret = cirrus_set_force_wake(false);
    return ret;
}

static int cirrus_check_error_state(void) {
    if (handle.is_stereo) return cirrus_check_error_state_stereo();

    return cirrus_check_error_state_mono();
}

static int cirrus_check_error_fatal(void) {
    int ret = 0;

    pthread_mutex_lock(&handle.fb_prot_mutex);

    ret = cirrus_check_error_state();
    if (ret == 0) goto success;

    /* Ouch! Error! Let's wait just a lil and retry to be extra sure... */
    usleep(CIRRUS_ERROR_DETECT_SLEEP_US);
    ret = cirrus_check_error_state();
    if (ret == 0) goto success;

    /* Error recovery: this should actually never happen, anyway... */
    ALOGE("%s: Cirrus DSP is in error state: resetting device", __func__);
    cirrus_do_reset(0);
    cirrus_do_reset("L");
    cirrus_do_reset("R");

    ALOGE("%s: Reset done, crashing HAL to reload firmware...", __func__);
    ALOGE("%s: Goodbye, infamous world...", __func__);
    abort();

success:
    pthread_mutex_unlock(&handle.fb_prot_mutex);
    return ret;
}

static void* cirrus_failure_detect_thread() {
    ALOGD("%s: Entry", __func__);

    (void)cirrus_check_error_fatal();

    ALOGD("%s: Exit ", __func__);

    pthread_exit(0);
    return NULL;
}

/* Amplifier funtions */
static void cs35l41_enable_output(UNUSED struct audio_device* adev,
                                  UNUSED snd_device_t snd_device) {
    ALOGV("%s: Entry", __func__);

    if (!adev) {
        return;
    }

    if (pthread_self() == handle.calibration_thread) {
        // Succeed without doing anything; the calibration already
        // selects the right paths, and we do not want the failure
        // detect thread to run just yet.
        ALOGV("%s: We are the calibration thread", __func__);
        goto end;
    }

    pthread_mutex_lock(&handle.fb_prot_mutex);

    ALOGV("%s: current state %d", __func__, handle.state);

    /*
     * If we are still in calibration phase, we cannot play audio...
     * and it's the same if we got an error during the process.
     *
     * Reason is that if we try playing audio during calibration, then
     * the result will be bad and we will end up a poorly calibrated
     * speaker. Also, the DSP may get left in a bad state and not accept
     * the protection firmware when we're ready for it.
     */
    if (handle.state == CALIBRATING || handle.state == CALIBRATION_ERROR) {
        ALOGI("%s: Forbidden. Calibration %s", __func__,
              handle.state == CALIBRATING ? "is in progress..." : "failed.");
        goto end;
    }

    cirrus_set_force_wake(true);

    if (handle.state == IDLE)
        (void)pthread_create(&handle.failure_detect_thread, (const pthread_attr_t*)NULL,
                             cirrus_failure_detect_thread, &handle);
    handle.state = PLAYBACK;
end:
    pthread_mutex_unlock(&handle.fb_prot_mutex);

    ALOGV("%s: Exit", __func__);
}

static void cs35l41_disable_output(UNUSED struct audio_device* adev,
                                   UNUSED snd_device_t snd_device) {
    ALOGV("%s: Entry", __func__);

    pthread_mutex_lock(&handle.fb_prot_mutex);

    if (pthread_self() == handle.calibration_thread) {
        // This happens when stopping the device from calibration. We bailed
        // and never set PLAYBACK, so we should also never update the audio
        // route nor unconditionally set the state back to IDLE
        ALOGV("%s: We are the calibration thread", __func__);
        goto end;
    }

    if (handle.state != PLAYBACK) {
        ALOGV("%s: Cannot stop processing, state is not PLAYBACK (but %d)", __func__, handle.state);
        goto end;
    }

    handle.state = IDLE;

end:
    pthread_mutex_unlock(&handle.fb_prot_mutex);

    ALOGV("%s: Exit", __func__);
}

static int amp_enable_devices(UNUSED struct amplifier_device* device, uint32_t devices,
                              bool enable) {
    struct audio_device* adev = cs35l41_dev->adev;
    ALOGV("%s: Entry", __func__);

    if (!adev) {
        ALOGE("%s: Invalid params", __func__);
        return -EINVAL;
    }

    if (enable) {
        cs35l41_enable_output(cs35l41_dev->adev, devices);
    } else {
        cs35l41_disable_output(cs35l41_dev->adev, devices);
    }

    return 0;
}

static int amp_dev_close(hw_device_t* device) {
    pthread_join(handle.failure_detect_thread, NULL);
    pthread_join(handle.calibration_thread, NULL);
    pthread_mutex_destroy(&handle.fb_prot_mutex);

    cs35l41_t* dev = (cs35l41_t*)device;

    if (dev) free(dev);
    return 0;
}

static int amp_calib(UNUSED struct amplifier_device* device, void* adev) {
    int ret = 0;

    if (!adev) {
        ALOGE("%s: adev is NULL!", __func__);
        return -EINVAL;
    }

    cs35l41_dev->adev = adev;

    memset(&handle, 0, sizeof(handle));
    memset(&cal_ambient, 0, sizeof(cal_ambient));

    ALOGI("%s: Initialize Cirrus Logic Playback module", __func__);

    handle.state = INIT;

    /* Ambient */
    ret = get_persist_value(PERSIST_CIRRUS_CAL_GLOBAL_CAL_AMBIENT, &cal_ambient);
    if (!ret)
        return;

#ifdef GET_SPEAKER_CALIBRATIONS_FROM_PERSIST
    /* Speaker LEFT */
    ret = get_persist_value(PERSIST_CIRRUS_CAL_SPKL_CAL_R, &handle.spkl.cal_r, true);
    ret = get_persist_value(PERSIST_CIRRUS_CAL_SPKL_CAL_STATUS, &handle.spkl.status, true);
    ret = get_persist_value(PERSIST_CIRRUS_CAL_SPKL_CAL_CHECKSUM, &handle.spkl.checksum, true);

    /* Speaker RIGHT */
    ret = get_persist_value(PERSIST_CIRRUS_CAL_SPKR_CAL_R, &handle.spkr.cal_r, true);
    ret = get_persist_value(PERSIST_CIRRUS_CAL_SPKR_CAL_STATUS, &handle.spkr.status, true);
    ret = get_persist_value(PERSIST_CIRRUS_CAL_SPKR_CAL_CHECKSUM, &handle.spkr.checksum, true);

    handle.spkl.cal_ok = true;
    handle.spkr.cal_ok = true;

    int spkl_cal_valid = 0;
    for (int i = 0; i < 4; i++) {
        spkl_cal_valid |= handle.spkl.cal_r[i];
    }

    int spkr_cal_valid = 0;
    for (int i = 0; i < 4; i++) {
        spkr_cal_valid |= handle.spkr.cal_r[i];
    }

    // Workaround a empty TA partition: both spkr and spkl's TA data is 0x0
    handle.spkl.cal_valid = spkl_cal_valid;
    handle.spkr.cal_valid = spkr_cal_valid;
    ALOGI("%s: spkl valid: %d, spkr valid: %d", __func__, handle.spkl.cal_valid,
          handle.spkr.cal_valid);

    ret = cirrus_get_mixer_value_by_name("SPK DSP Booted");
    ALOGI("%s: %s speaker found", __func__, ret < 0 ? "Stereo" : "Mono");
    handle.is_stereo = ret < 0;
#else

    /* Do we want to load or calibrate? */
    ret = cirrus_cal_from_file(&handle);
    if (ret == 0) {
        handle.spkl.cal_ok = true;
        handle.spkr.cal_ok = true;
    } else {
        handle.spkl.cal_ok = false;
        handle.spkr.cal_ok = false;
    }

#endif

    pthread_mutex_init(&handle.fb_prot_mutex, NULL);

    (void)pthread_create(&handle.calibration_thread, (const pthread_attr_t*)NULL,
                         cirrus_do_calibration, &handle);

    return 0;
}

static int amp_module_open(const hw_module_t* module, const char* name, hw_device_t** device) {
    /* Init common amplifier tools */
    if (strcmp(name, AMPLIFIER_HARDWARE_INTERFACE)) {
        ALOGE("%s:%d: %s does not match amplifier hardware interface name\n", __func__, __LINE__,
              name);
        return -ENODEV;
    }

    cs35l41_dev = calloc(1, sizeof(cs35l41_t));
    if (!cs35l41_dev) {
        ALOGE("%s:%d: Unable to allocate memory for amplifier device\n", __func__, __LINE__);
        return -ENOMEM;
    }

    cs35l41_dev->amp_dev.common.tag = HARDWARE_DEVICE_TAG;
    cs35l41_dev->amp_dev.common.module = (hw_module_t*)module;
    cs35l41_dev->amp_dev.common.version = HARDWARE_DEVICE_API_VERSION(1, 0);
    cs35l41_dev->amp_dev.common.close = amp_dev_close;

    cs35l41_dev->amp_dev.set_input_devices = NULL;
    cs35l41_dev->amp_dev.set_output_devices = NULL;
    cs35l41_dev->amp_dev.enable_input_devices = NULL;
    cs35l41_dev->amp_dev.enable_output_devices = amp_enable_devices;
    cs35l41_dev->amp_dev.input_stream_standby = NULL;
    cs35l41_dev->amp_dev.output_stream_standby = NULL;
    cs35l41_dev->amp_dev.set_mode = NULL;
    cs35l41_dev->amp_dev.input_stream_start = NULL;
    cs35l41_dev->amp_dev.output_stream_start = NULL;
    cs35l41_dev->amp_dev.set_parameters = NULL;
    cs35l41_dev->amp_dev.in_set_parameters = NULL;
    cs35l41_dev->amp_dev.out_set_parameters = NULL;
    cs35l41_dev->amp_dev.set_feedback = NULL;
    cs35l41_dev->amp_dev.calibrate = amp_calib;

    *device = (hw_device_t*)cs35l41_dev;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = amp_module_open,
};

amplifier_module_t HAL_MODULE_INFO_SYM = {
    .common =
            {
                    .tag = HARDWARE_MODULE_TAG,
                    .module_api_version = AMPLIFIER_MODULE_API_VERSION_0_1,
                    .hal_api_version = HARDWARE_HAL_API_VERSION,
                    .id = AMPLIFIER_HARDWARE_MODULE_ID,
                    .name = "Cirrus Logic CS35L41 amplifier HAL",
                    .author = "The LineageOS Project",
                    .methods = &hal_module_methods,
            },
};
