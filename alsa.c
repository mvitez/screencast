#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include "alsa.h"

typedef struct
{
	snd_pcm_t *handle;
	int sfreq, channels, bufsize, lowdelay, isinput;
	char devname[100];
	int vumeter[2];
	short hpfin[4];
	double hpfout[4];
	int iirf_enable;
	double iirf_a, iirf_b, iirf_g, iirf_level;
} AUDIO;

pthread_mutex_t g_audioout_mutex = PTHREAD_MUTEX_INITIALIZER;
AUDIO *g_audioout, *g_audioin;

enum
{
	LOG_ERR,
	LOG_NORM,
	LOG_VERBOSE
};

void lprintf(int type, const char *fmt, ...)
{
	if (type > LOG_NORM)
		return;
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static int snd_params(snd_pcm_t *handle, unsigned nbytes, unsigned sfreq, unsigned channels, int lowdelay)
{
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_sw_params_t *sw_params;
	int err;

	snd_pcm_hw_params_malloc(&hw_params);
	snd_pcm_hw_params_any(handle, hw_params);
	snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_rate_near(handle, hw_params, &sfreq, 0);
	snd_pcm_hw_params_set_channels(handle, hw_params, channels);
	snd_pcm_hw_params_set_period_size(handle, hw_params, nbytes / (2 * channels), 0);
	snd_pcm_hw_params_set_buffer_time(handle, hw_params, lowdelay ? 100000 : 500000, 0);
	if ((err = snd_pcm_hw_params(handle, hw_params)) < 0)
	{
		snd_pcm_hw_params_free(hw_params);
		lprintf(LOG_ERR, "Cannot set audio parameters: %s\n", snd_strerror(err));
		return -1;
	}
	snd_pcm_hw_params_free(hw_params);

	snd_pcm_sw_params_malloc(&sw_params);
	snd_pcm_sw_params_current(handle, sw_params);
	snd_pcm_sw_params_set_avail_min(handle, sw_params, nbytes / (2 * channels));
	snd_pcm_sw_params_set_start_threshold(handle, sw_params, 0);
	//	snd_pcm_sw_params_set_period_event(handle, sw_params, 1);
	if ((err = snd_pcm_sw_params(handle, sw_params)) < 0)
	{
		snd_pcm_sw_params_free(sw_params);
		lprintf(LOG_ERR, "Cannot set software parameters: %s\n", snd_strerror(err));
		return -1;
	}
	snd_pcm_sw_params_free(sw_params);
	return 0;
}

static void calcvumeter(const short *buf, int nsamples, int nchannels, int *vumeter)
{
	int n, x, max, max2;

	if (nchannels == 1)
	{
		max = 1;
		for (n = 0; n < nsamples; n++)
		{
			x = buf[n] * buf[n];
			if (x > max)
				max = x;
		}
		vumeter[0] = vumeter[1] = (int)(10.0 * log10(max / 1073741824.0));
	}
	else
	{
		max = max2 = 1;
		for (n = 0; n < nsamples; n++)
		{
			x = buf[n] * buf[n];
			if (x > max)
				max = x;
			n++;
			x = buf[n] * buf[n];
			if (x > max2)
				max2 = x;
		}
		vumeter[0] = (int)(10.0 * log10(max / 1073741824.0));
		vumeter[1] = (int)(10.0 * log10(max2 / 1073741824.0));
	}
}

int alsa_init()
{
	return 0;
}

void *au_open_play(const char *devname, unsigned sfreq, unsigned channels, unsigned nbytes, int lowdelay)
{
	int err;
	snd_pcm_t *handle;
	AUDIO *audioout;

	if ((err = snd_pcm_open(&handle, devname, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
	{
		lprintf(LOG_ERR, "[%s] Playback open error: %s\n", devname, snd_strerror(err));
		return 0;
	}
	if (snd_params(handle, nbytes, sfreq, channels, lowdelay))
	{
		snd_pcm_close(handle);
		return 0;
	}
	audioout = (AUDIO *)calloc(1, sizeof(AUDIO));
	strncpy(audioout->devname, devname, sizeof(audioout->devname) - 1);
	audioout->vumeter[0] = audioout->vumeter[1] = -100;
	audioout->sfreq = sfreq;
	audioout->bufsize = nbytes;
	audioout->channels = channels;
	audioout->handle = handle;
	audioout->lowdelay = lowdelay;
	lprintf(LOG_VERBOSE, "[%s] Playback opened: sfreq=%u, channels=%u, bufsize=%u, lowdelay=%d\n", devname, sfreq, channels, nbytes, lowdelay);
	return audioout;
}

int alsa_open_play(unsigned sfreq, unsigned channels, unsigned nbytes, int lowdelay)
{
	pthread_mutex_lock(&g_audioout_mutex);
	if (g_audioout)
	{
		pthread_mutex_unlock(&g_audioout_mutex);
		return -1;
	}
	g_audioout = au_open_play("default", sfreq, channels, nbytes, lowdelay);
	if (!g_audioout)
	{
		pthread_mutex_unlock(&g_audioout_mutex);
		return -1;
	}
	pthread_mutex_unlock(&g_audioout_mutex);
	return 0;
}

int au_set_play_filter(void *dev, double cfreq, double q, double dB)
{
	AUDIO *audioout = (AUDIO *)dev;
	if (cfreq != 0)
	{
		double o;

		o = cfreq * 2.0 * 3.1415926535897932384626433832795 / audioout->sfreq;
		audioout->iirf_b = 0.5 * (1 - tan(o / (2 * q))) / (1 + tan(o / (2 * q)));
		audioout->iirf_g = (0.5 + audioout->iirf_b) * cos(o);
		audioout->iirf_a = (0.5 - audioout->iirf_b) * 0.5;
		audioout->iirf_level = pow(10.0, dB / 20.0) - 1.0;
		audioout->iirf_enable = 1;
		memset(audioout->hpfout, 0, sizeof(audioout->hpfout));
		memset(audioout->hpfin, 0, sizeof(audioout->hpfin));
	}
	else
		audioout->iirf_enable = 0;
	return 0;
}

int alsa_set_play_filter(double cfreq, double q, double dB)
{
	int rc;

	pthread_mutex_lock(&g_audioout_mutex);
	rc = au_set_play_filter(g_audioout, cfreq, q, dB);
	pthread_mutex_unlock(&g_audioout_mutex);
	return rc;
}

static short IIRFilter(short data, short *inbuf, double *outbuf, double a, double b, double g, double level)
{
	double y;

	y = a * (data - inbuf[1]) + 2 * g * outbuf[0] - 2 * b * outbuf[1];
	outbuf[1] = outbuf[0];
	outbuf[0] = y;
	inbuf[1] = inbuf[0];
	inbuf[0] = data;
	y = data + y * level;
	if (y < -32768)
		return -32768;
	else if (y > 32767)
		return 32767;
	return (short)y;
}

int au_put(void *dev, void *buf, unsigned nbytes)
{
	int err;
	AUDIO *audioout = (AUDIO *)dev;

	if (!audioout)
		return -1;
	if (audioout->iirf_enable)
	{
		if (audioout->channels == 1)
		{
			int i, n = nbytes / 2;
			short *data = (short *)buf;

			for (i = 0; i < n; i++)
				data[i] = IIRFilter(data[i], audioout->hpfin, audioout->hpfout, audioout->iirf_a, audioout->iirf_b, audioout->iirf_g, audioout->iirf_level);
		}
		else
		{
			int i, n = nbytes / 4;
			short *data = (short *)buf;

			for (i = 0; i < n; i++)
			{
				data[2 * i] = IIRFilter(data[2 * i], audioout->hpfin, audioout->hpfout, audioout->iirf_a, audioout->iirf_b, audioout->iirf_g, audioout->iirf_level);
				data[2 * i + 1] = IIRFilter(data[2 * i + 1], audioout->hpfin + 2, audioout->hpfout + 2, audioout->iirf_a, audioout->iirf_b, audioout->iirf_g, audioout->iirf_level);
			}
		}
	}
	err = snd_pcm_writei(audioout->handle, buf, nbytes / (2 * audioout->channels));
	if (err < 0)
	{
		if (err == -EPIPE)
		{
			lprintf(LOG_ERR, "Audio playback underrun\n");
			snd_pcm_close(audioout->handle);
			if ((err = snd_pcm_open(&audioout->handle, audioout->devname, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
			{
				lprintf(LOG_ERR, "[%s] Playback open error: %s\n", audioout->devname, snd_strerror(err));
				audioout->handle = 0;
				return err;
			}
			if (snd_params(audioout->handle, audioout->bufsize, audioout->sfreq, audioout->channels, audioout->lowdelay))
			{
				snd_pcm_close(audioout->handle);
				return err;
			}
			snd_pcm_writei(audioout->handle, buf, nbytes / (2 * audioout->channels));
		}
		else
			lprintf(LOG_VERBOSE, "Playback error: %s\n", snd_strerror(err));
	}
	else
		calcvumeter((short *)buf, audioout->bufsize / (2 * audioout->channels), audioout->channels, audioout->vumeter);
	return err;
}

int alsa_put(void *buf, unsigned nbytes)
{
	int rc;

	pthread_mutex_lock(&g_audioout_mutex);
	rc = au_put(g_audioout, buf, nbytes);
	pthread_mutex_unlock(&g_audioout_mutex);
	return rc;
}

int au_play_delay(void *dev)
{
	snd_pcm_sframes_t delay;
	int err;
	AUDIO *audioout = (AUDIO *)dev;

	if (!audioout)
		return -1;
	if (snd_pcm_state(audioout->handle) != SND_PCM_STATE_RUNNING)
		return -1;
	if ((err = snd_pcm_delay(audioout->handle, &delay)) < 0)
	{
		lprintf(LOG_VERBOSE, "Error obtaining delay from audio device: %s\n", snd_strerror(err));
		return -1;
	}
	return delay * 2 * audioout->channels;
}

int alsa_play_delay()
{
	int rc;

	pthread_mutex_lock(&g_audioout_mutex);
	rc = au_play_delay(g_audioout);
	pthread_mutex_unlock(&g_audioout_mutex);
	return rc;
}

void *au_open_record(const char *devname, unsigned sfreq, unsigned channels, unsigned nbytes, int lowdelay)
{
	int err;
	snd_pcm_t *handle;
	AUDIO *audioin;

	if ((err = snd_pcm_open(&handle, devname, SND_PCM_STREAM_CAPTURE, 0)) < 0)
	{
		lprintf(LOG_ERR, "[%s] Record open error: %s\n", devname, snd_strerror(err));
		return 0;
	}
	if (snd_params(handle, nbytes, sfreq, channels, lowdelay))
	{
		snd_pcm_close(handle);
		return 0;
	}
	audioin = (AUDIO *)calloc(1, sizeof(AUDIO));
	audioin->vumeter[0] = audioin->vumeter[1] = -100;
	audioin->sfreq = sfreq;
	audioin->bufsize = nbytes;
	audioin->channels = channels;
	audioin->handle = handle;
	audioin->isinput = 1;
	strncpy(audioin->devname, devname, sizeof(audioin->devname) - 1);
	memset(audioin->hpfin, 0, sizeof(audioin->hpfin));
	memset(audioin->hpfout, 0, sizeof(audioin->hpfout));
	lprintf(LOG_VERBOSE, "[%s] Recording opened: sfreq=%u, channels=%u, bufsize=%u, lowdelay=%d\n", devname, sfreq, channels, nbytes, lowdelay);
	return audioin;
}

int alsa_open_record(unsigned sfreq, unsigned channels, unsigned nbytes, int lowdelay)
{
	if (g_audioin)
		return -1;
	g_audioin = au_open_record("default", sfreq, channels, nbytes, lowdelay);
	if (!g_audioin)
		return -1;
	return 0;
}

unsigned getusecs();

static void HPFilterStereo(short *in, short *out, int len, short *inmem, double *outmem)
{
	// butter(2,0.0002,'high');
	static const double coefB[3] = {0.99955581038761, -1.99911162077522, 0.99955581038761};
	static const double coefA[2] = {-1.99911142347080, 0.99911181807964};
	int i, ch, yi;
	double y;

	for (ch = 0; ch < 2; ch++)
	{
		y = coefB[0] * in[ch] + coefB[1] * inmem[ch] + coefB[2] * inmem[2 + ch] - coefA[0] * outmem[ch] - coefA[1] * outmem[2 + ch];
		yi = (int)y;
		out[ch] = yi > 32767 ? 32767 : yi < -32678 ? -32768
												   : yi;
		outmem[2 + ch] = outmem[ch];
		outmem[ch] = y;
		y = coefB[0] * in[2 + ch] + coefB[1] * in[ch] + coefB[2] * inmem[ch] - coefA[0] * outmem[ch] - coefA[1] * outmem[2 + ch];
		yi = (int)y;
		out[2 + ch] = yi > 32767 ? 32767 : yi < -32678 ? -32768
													   : yi;
		outmem[2 + ch] = outmem[ch];
		outmem[ch] = y;
		for (i = 2; i < len; i++)
		{
			y = coefB[0] * in[2 * i + ch] + coefB[1] * in[2 * (i - 1) + ch] + coefB[2] * in[2 * (i - 2) + ch] - coefA[0] * outmem[ch] - coefA[1] * outmem[2 + ch];
			yi = (int)y;
			out[2 * i + ch] = yi > 32767 ? 32767 : yi < -32678 ? -32768
															   : yi;
			outmem[2 + ch] = outmem[ch];
			outmem[ch] = y;
		}
		inmem[2 + ch] = in[2 * (len - 2) + ch];
		inmem[ch] = in[2 * (len - 1) + ch];
	}
}

static void HPFilter(short *in, short *out, int len, short *inbuf, double *outbuf)
{
	// butter(2,0.0002,'high');
	static const double coefB[3] = {0.99955581038761, -1.99911162077522, 0.99955581038761};
	static const double coefA[2] = {-1.99911142347080, 0.99911181807964};
	int i, yi;
	double y;

	y = coefB[0] * in[0] + coefB[1] * inbuf[0] + coefB[2] * inbuf[1] - coefA[0] * outbuf[0] - coefA[1] * outbuf[1];
	yi = (int)y;
	out[0] = yi > 32767 ? 32767 : yi < -32678 ? -32768
											  : yi;
	outbuf[1] = outbuf[0];
	outbuf[0] = y;
	y = coefB[0] * in[1] + coefB[1] * in[0] + coefB[2] * inbuf[0] - coefA[0] * outbuf[0] - coefA[1] * outbuf[1];
	yi = (int)y;
	out[1] = yi > 32767 ? 32767 : yi < -32678 ? -32768
											  : yi;
	outbuf[1] = outbuf[0];
	outbuf[0] = y;
	for (i = 2; i < len; i++)
	{
		y = coefB[0] * in[i] + coefB[1] * in[i - 1] + coefB[2] * in[i - 2] - coefA[0] * outbuf[0] - coefA[1] * outbuf[1];
		yi = (int)y;
		out[i] = yi > 32767 ? 32767 : yi < -32678 ? -32768
												  : yi;
		outbuf[1] = outbuf[0];
		outbuf[0] = y;
	}
	inbuf[1] = in[len - 2];
	inbuf[0] = in[len - 1];
}

int au_get(void *dev, void *buf)
{
	int err;
	short frame[1152 * 2];
	AUDIO *audioin = (AUDIO *)dev;
	// snd_pcm_sframes_t delay;

	if (!audioin)
		return -1;
	err = snd_pcm_readi(audioin->handle, frame, audioin->bufsize / (2 * audioin->channels));
	if (err < 0)
	{
		if (err == -EPIPE)
		{
			snd_pcm_prepare(audioin->handle);
			lprintf(LOG_ERR, "[%s] Audio recording overrun\n", audioin->devname);
		}
		else
		{
			lprintf(LOG_ERR, "[%s] Recording error: %s\n", audioin->devname, snd_strerror(err));
			usleep(100000);
		}
	}
	if (audioin->channels == 2)
		HPFilterStereo(frame, buf, audioin->bufsize / 4, audioin->hpfin, audioin->hpfout);
	else
		HPFilter(frame, buf, audioin->bufsize / 2, audioin->hpfin, audioin->hpfout);
	calcvumeter((short *)buf, audioin->bufsize / (2 * audioin->channels), audioin->channels, audioin->vumeter);

	/*	if((err = snd_pcm_delay(audioin->handle, &delay)) < 0)
		{
			lprintf(LOG_VERBOSE, "Error obtaining delay from audio device: %s\n", snd_strerror(err));
			return -1;
		}
		lprintf(LOG_VERBOSE, "Delay=%f\n", (double)delay/audioin->sfreq);*/
	return err;
}

int alsa_get(void *buf)
{
	return au_get(g_audioin, buf);
}

int au_close(void *dev)
{
	AUDIO *audio = (AUDIO *)dev;
	if (!audio)
		return -1;
	snd_pcm_close(audio->handle);
	if (audio->isinput)
		lprintf(LOG_VERBOSE, "[%s] Recording closed\n", audio->devname);
	else
		lprintf(LOG_VERBOSE, "[%s] Playback closed\n", audio->devname);
	free(audio);
	return 0;
}

int alsa_close_play()
{
	pthread_mutex_lock(&g_audioout_mutex);
	if (!g_audioout)
	{
		pthread_mutex_unlock(&g_audioout_mutex);
		return -1;
	}
	au_close(g_audioout);
	g_audioout = 0;
	pthread_mutex_unlock(&g_audioout_mutex);
	return 0;
}

int alsa_close_record()
{
	if (!g_audioin)
		return -1;
	au_close(g_audioin);
	g_audioin = 0;
	return 0;
}

int au_getvumeters(void *dev, int *vumeters)
{
	AUDIO *audio = (AUDIO *)dev;
	if (!audio)
	{
		vumeters[0] = -100;
		vumeters[1] = -100;
		return -1;
	}
	return 0;
}

int alsa_getvumeters(int *vumeters)
{
	if (g_audioin)
	{
		vumeters[0] = g_audioin->vumeter[0];
		vumeters[1] = g_audioin->vumeter[1];
	}
	else
	{
		vumeters[0] = -100;
		vumeters[1] = -100;
	}
	pthread_mutex_lock(&g_audioout_mutex);
	if (g_audioout)
	{
		vumeters[2] = g_audioout->vumeter[0];
		vumeters[3] = g_audioout->vumeter[1];
	}
	else
	{
		vumeters[2] = -100;
		vumeters[3] = -100;
	}
	pthread_mutex_unlock(&g_audioout_mutex);
	return 0;
}

int alsa_set_playback_volume(int mB)
{
	long min, max;
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	int rc = -1;

	// setenv("PULSE_INTERNAL", "0", 1); // Uncomment if you want to control alsa skipping pulse
	if (snd_mixer_open(&handle, 0))
		return -1;
	if (!snd_mixer_attach(handle, "default"))
	{
		if (!snd_mixer_selem_register(handle, NULL, NULL))
		{
			if (!snd_mixer_load(handle))
			{
				snd_mixer_selem_id_alloca(&sid);
				snd_mixer_selem_id_set_index(sid, 0);
				snd_mixer_selem_id_set_name(sid, "PCM");
				snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);
				if (!elem)
				{
					snd_mixer_selem_id_set_name(sid, "Digital");
					elem = snd_mixer_find_selem(handle, sid);
				}
				if (elem)
				{
					snd_mixer_selem_get_playback_dB_range(elem, &min, &max);
					if (mB < min)
						mB = min;
					else if (mB > max)
						mB = max;
					snd_mixer_selem_set_playback_dB_all(elem, mB, 0);
					rc = 0;
				}
			}
		}
		snd_mixer_detach(handle, "default");
	}
	snd_mixer_close(handle);
	return rc;
}

int alsa_get_playback_volume(int *mB)
{
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	int rc = -1;
	long ldB;

	if (snd_mixer_open(&handle, 0))
		return -1;
	if (!snd_mixer_attach(handle, "default"))
	{
		if (!snd_mixer_selem_register(handle, NULL, NULL))
		{
			if (!snd_mixer_load(handle))
			{
				snd_mixer_selem_id_alloca(&sid);
				snd_mixer_selem_id_set_index(sid, 0);
				snd_mixer_selem_id_set_name(sid, "PCM");
				snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);
				if (elem)
				{
					snd_mixer_selem_get_playback_dB(elem, 0, &ldB);
					*mB = ldB;
					rc = 0;
				}
			}
		}
		snd_mixer_detach(handle, "default");
	}
	snd_mixer_close(handle);
	return rc;
}
