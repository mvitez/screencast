#ifndef _ALSA_H_INCLUDED_
#define _ALSA_H_INCLUDED_

#ifdef __cplusplus
extern "C"
{
#endif

    void *au_open_play(const char *devname, unsigned sfreq, unsigned channels, unsigned nbytes, int lowdelay);
    void *au_open_record(const char *devname, unsigned sfreq, unsigned channels, unsigned nbytes, int lowdelay);
    int au_put(void *dev, void *buf, unsigned nbytes);
    int au_get(void *dev, void *buf);
    int au_close(void *dev);
    int au_getvumeters(void *dev, int *vumeters);
    int au_set_play_filter(void *dev, double cfreq, double q, double dB);
    int au_play_delay(void *dev);

    int alsa_open_play(unsigned sfreq, unsigned channels, unsigned nbytes, int lowdelay);
    int alsa_set_play_filter(double cfreq, double q, double dB);
    int alsa_put(void *buf, unsigned nbytes);
    int alsa_close_play();
    int alsa_play_delay();
    int alsa_open_record(unsigned sfreq, unsigned channels, unsigned nbytes, int lowdelay);
    int alsa_get(void *buf);
    int alsa_close_record();
    int alsa_getvumeters(int *vumeters);
    int alsa_init();
    int alsa_set_playback_volume(int mB);
    int alsa_get_playback_volume(int *mB);

#ifdef __cplusplus
}
#endif

#endif
