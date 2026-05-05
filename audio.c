#include <stdint.h>
/*
 * audio.c - WAV player using Linux kernel ALSA PCM ioctls directly.
 *
 * Talks to /dev/snd/pcmC0D0p with raw SNDRV_PCM_IOCTL_* calls.
 * No libasound dependency.
 */

#include <sys/ioctl.h>
#include <sound/asound.h>

static int audio_play_wav(const char *path) {
    int wf = open(path, O_RDONLY);
    if (wf < 0) return -1;

    struct { char riff[4]; uint32_t size; char wave[4]; } hdr;
    if (read(wf, &hdr, 12) != 12 ||
        memcmp(hdr.riff,"RIFF",4) || memcmp(hdr.wave,"WAVE",4)) {
        close(wf); return -2;
    }

    uint16_t channels = 2, bits = 16;
    uint32_t rate = 44100;
    uint32_t data_size = 0;

    while (1) {
        char id[4]; uint32_t sz;
        if (read(wf, id, 4) != 4) break;
        if (read(wf, &sz, 4) != 4) break;
        if (memcmp(id,"fmt ",4) == 0) {
            char fmt[64] = {0};
            uint32_t to_read = sz < sizeof(fmt) ? sz : sizeof(fmt);
            if (read(wf, fmt, to_read) != (ssize_t)to_read) { close(wf); return -3; }
            if (sz > sizeof(fmt)) lseek(wf, sz - sizeof(fmt), SEEK_CUR);
            channels = *(uint16_t*)(fmt+2);
            rate     = *(uint32_t*)(fmt+4);
            bits     = *(uint16_t*)(fmt+14);
        } else if (memcmp(id,"data",4) == 0) {
            data_size = sz;
            break;
        } else {
            lseek(wf, sz, SEEK_CUR);
        }
    }

    /* Try card 0 device 0; fall back to others */
    int af = -1;
    const char *devs[] = {
        "/dev/snd/pcmC0D0p",
        "/dev/snd/pcmC1D0p",
        "/dev/snd/pcmC0D3p",
        "/dev/snd/pcmC0D7p",
        NULL
    };
    for (int i = 0; devs[i]; i++) {
        af = open(devs[i], O_WRONLY);
        if (af >= 0) break;
    }
    if (af < 0) { close(wf); return -4; }

    /* Set HW params via SNDRV_PCM_IOCTL_HW_PARAMS */
    struct snd_pcm_hw_params hw;
    memset(&hw, 0, sizeof(hw));
    /* Set all masks/intervals to "any" (all bits set) */
    for (int i = 0; i < SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1; i++) {
        memset(&hw.masks[i], 0xff, sizeof(hw.masks[i]));
    }
    for (int i = 0; i < SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1; i++) {
        hw.intervals[i].min = 0;
        hw.intervals[i].max = ~0u;
    }
    hw.rmask = ~0u;
    hw.cmask = 0;
    hw.info  = ~0u;

    /* Constrain: ACCESS = INTERLEAVED, FORMAT = S16_LE */
    int access_idx = SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK;
    memset(&hw.masks[access_idx], 0, sizeof(hw.masks[access_idx]));
    hw.masks[access_idx].bits[0] = 1u << SNDRV_PCM_ACCESS_RW_INTERLEAVED;

    int fmt_idx = SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK;
    memset(&hw.masks[fmt_idx], 0, sizeof(hw.masks[fmt_idx]));
    hw.masks[fmt_idx].bits[SNDRV_PCM_FORMAT_S16_LE / 32]
        |= 1u << (SNDRV_PCM_FORMAT_S16_LE % 32);

    /* Set channels, rate, sample bits */
    #define SET_INTERVAL(name, v) do { \
        int i = (name) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; \
        hw.intervals[i].min = (v); hw.intervals[i].max = (v); \
        hw.intervals[i].integer = 1; \
    } while(0)

    SET_INTERVAL(SNDRV_PCM_HW_PARAM_CHANNELS, channels);
    SET_INTERVAL(SNDRV_PCM_HW_PARAM_RATE, rate);
    SET_INTERVAL(SNDRV_PCM_HW_PARAM_SAMPLE_BITS, bits);

    /* Period and buffer sizes — request reasonable defaults */
    int period_idx = SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
    hw.intervals[period_idx].min = 512;
    hw.intervals[period_idx].max = 8192;

    int buf_idx = SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
    hw.intervals[buf_idx].min = 2048;
    hw.intervals[buf_idx].max = 65536;

    if (ioctl(af, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        close(af); close(wf); return -5;
    }

    /* Set SW params (use defaults — need to set start/stop thresholds) */
    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = 0;
    sw.period_step = 1;
    sw.avail_min   = 1;
    sw.start_threshold = 1;
    sw.stop_threshold  = ~0u;
    sw.silence_threshold = 0;
    sw.silence_size = 0;
    sw.boundary = 0x7fffffff;
    ioctl(af, SNDRV_PCM_IOCTL_SW_PARAMS, &sw);  /* may fail, that's OK */

    /* Prepare for playback */
    if (ioctl(af, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        close(af); close(wf); return -6;
    }

    /* Stream PCM data via WRITEI_FRAMES ioctl (or just write()) */
    int frame_bytes = channels * (bits / 8);
    char buf[8192];
    uint32_t remaining = data_size;
    while (remaining > 0) {
        int want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        /* round down to whole frames */
        want -= want % frame_bytes;
        if (want == 0) break;
        int got = read(wf, buf, want);
        if (got <= 0) break;

        struct snd_xferi xfer;
        xfer.buf = buf;
        xfer.frames = got / frame_bytes;
        xfer.result = 0;

        int rc = ioctl(af, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
        if (rc < 0) {
            /* Try recovery */
            if (errno == EPIPE) {
                ioctl(af, SNDRV_PCM_IOCTL_PREPARE);
                continue;
            }
            break;
        }
        remaining -= got;
    }

    /* Drain & close */
    ioctl(af, SNDRV_PCM_IOCTL_DRAIN);
    close(af);
    close(wf);
    return 0;
}

static void audio_play_wav_async(const char *path) {
    pid_t p = fork();
    if (p == 0) {
        audio_play_wav(path);
        _exit(0);
    }
}
