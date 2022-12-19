#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MAX_NUM_OF_FILTERS 16

typedef struct {
  float a1;
  float a2;
  float gain;
} filter_param_t;

typedef struct {
  filter_param_t param;
  float state[2];
} filter_t;

typedef struct {
  int num_filters;
  filter_t filter[MAX_NUM_OF_FILTERS];
} channel_t;

typedef struct {
  uint32_t version;
  uint32_t revision;
  uint32_t filter_sampling_rate;
  uint32_t num_filters;
  filter_param_t filter_param[MAX_NUM_OF_FILTERS];
} filterfile_t;

typedef struct {
  snd_pcm_extplug_t ext;
  filterfile_t *filterfile;
  int64_t filterfile_revision;
  uint32_t filter_sampling_rate;
  int num_channels;
  channel_t channel[];
} pcm_speakertweaker_t;

static int plugin_init(snd_pcm_extplug_t *ext);
static snd_pcm_sframes_t
plugin_transfer(snd_pcm_extplug_t *ext, const snd_pcm_channel_area_t *dst_areas,
                snd_pcm_uframes_t dst_offset,
                const snd_pcm_channel_area_t *src_areas,
                snd_pcm_uframes_t src_offset, snd_pcm_uframes_t size);
static int plugin_close(snd_pcm_extplug_t *ext);
static int sync_filter_parameters(pcm_speakertweaker_t *my);
static float filter(filter_t *filter, float in);

static snd_pcm_extplug_callback_t speakertweaker_callback = {
    .init = plugin_init,
    .transfer = plugin_transfer,
    .close = plugin_close,
};

SND_PCM_PLUGIN_SYMBOL(speakertweaker);

SND_PCM_PLUGIN_DEFINE_FUNC(speakertweaker) {
  snd_config_iterator_t i, next;
  snd_config_t *slave = NULL;
  const char *filterfile_name = "/var/lib/alsa/speakertweaker.bin";
  long num_channels = 2;

  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if (snd_config_get_id(n, &id) < 0) {
      continue;
    }
    if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 ||
        strcmp(id, "hint") == 0) {
      continue;
    }

    if (strcmp(id, "slave") == 0) {
      slave = n;
      continue;
    }

    if (strcmp(id, "filterfile") == 0) {
      snd_config_get_string(n, &filterfile_name);
      continue;
    }

    if (strcmp(id, "channels") == 0) {
      snd_config_get_integer(n, &num_channels);
      if (num_channels < 1) {
        SNDERR("channels < 1");
        return -EINVAL;
      }
      continue;
    }

    SNDERR("Unknown field %s", id);
    return -EINVAL;
  }

  if (!slave) {
    SNDERR("No slave defined for myplug");
    return -EINVAL;
  }

  pcm_speakertweaker_t *my = calloc(1, sizeof(pcm_speakertweaker_t) +
                                           num_channels * sizeof(channel_t));
  if (my == NULL) {
    return -ENOMEM;
  }

  my->ext.version = SND_PCM_EXTPLUG_VERSION;
  my->ext.name = "Speaker Tweaker";
  my->ext.callback = &speakertweaker_callback;
  my->ext.private_data = my;
  my->num_channels = num_channels;

  int fd = open(filterfile_name, O_RDONLY);
  if (fd < 0) {
    SNDERR("Filter file not found: %s", filterfile_name);
    free(my);
    return -EIO;
  }

  void *p = mmap(NULL, sizeof(filterfile_t), PROT_READ, MAP_SHARED, fd, 0);
  close(fd);

  if (p == (void *)-1) {
    free(my);
    return -EIO;
  }
  my->filterfile = (filterfile_t *)(p);
  my->filterfile_revision = -1;

  int err = snd_pcm_extplug_create(&my->ext, name, root, slave, stream, mode);

  if (err == 0) {
    err = snd_pcm_extplug_set_param(&my->ext, SND_PCM_EXTPLUG_HW_CHANNELS,
                                    my->num_channels);
  }
  if (err == 0) {
    err = snd_pcm_extplug_set_slave_param(&my->ext, SND_PCM_EXTPLUG_HW_CHANNELS,
                                          my->num_channels);
  }
  if (err == 0) {
    err = snd_pcm_extplug_set_param(&my->ext, SND_PCM_EXTPLUG_HW_FORMAT,
                                    SND_PCM_FORMAT_FLOAT);
  }
  if (err == 0) {
    err = snd_pcm_extplug_set_slave_param(&my->ext, SND_PCM_EXTPLUG_HW_FORMAT,
                                          SND_PCM_FORMAT_FLOAT);
  }
  if (err == 0) {
    err = snd_pcm_extplug_set_param_link(&my->ext, SND_PCM_EXTPLUG_HW_CHANNELS,
                                         1);
  }
  if (err != 0) {
    free(my);
    return err;
  }

  *pcmp = my->ext.pcm;
  return 0;
}

static int plugin_init(snd_pcm_extplug_t *ext) {
  pcm_speakertweaker_t *my = ext->private_data;

  if (sync_filter_parameters(my) < 0) {
    SNDERR("Filter cannot be loaded due to unknown file version.");
  } else {
    if (ext->rate != my->filter_sampling_rate) {
      SNDERR("The filter parameters of the Speaker Tweaker ALSA plugin only "
             "supports a sample rate of %i. The current rate is %i.",
             my->filter_sampling_rate, ext->rate);
    }
  }

  return 0;
}

static snd_pcm_sframes_t
plugin_transfer(snd_pcm_extplug_t *ext, const snd_pcm_channel_area_t *dst_areas,
                snd_pcm_uframes_t dst_offset,
                const snd_pcm_channel_area_t *src_areas,
                snd_pcm_uframes_t src_offset, snd_pcm_uframes_t size) {
  pcm_speakertweaker_t *my = ext->private_data;

  /* calculate sample buffer locations */
  float *src =
      (float *)src_areas->addr +
      (src_areas->first + src_areas->step * src_offset) / (8 * sizeof(float));
  float *dst =
      (float *)dst_areas->addr +
      (dst_areas->first + dst_areas->step * dst_offset) / (8 * sizeof(float));
  memcpy(dst, src, sizeof(float) * size * my->num_channels);

  sync_filter_parameters(my);

  if (ext->rate == my->filter_sampling_rate) {
    for (int i = 0; i < (size * my->num_channels); i += my->num_channels) {
      for (int ch = 0; ch < my->num_channels; ch++) {
        for (int f = 0; f < my->channel[ch].num_filters; f++) {
          dst[i + ch] = filter(&my->channel[ch].filter[f], dst[i + ch]);
        }
      }
    }
  }

  return size;
}

static int plugin_close(snd_pcm_extplug_t *ext) {
  free(ext->private_data);
  return 0;
}

static int sync_filter_parameters(pcm_speakertweaker_t *my) {
  if (my->filterfile->version != 1) {
    return -1;
  }

  /* Load new filter parameters if the revision in filterfile has changed.
   * Repeat, if the revision changed during the read. */
  while (my->filterfile->revision != my->filterfile_revision) {
    my->filterfile_revision = my->filterfile->revision;

    my->filter_sampling_rate = my->filterfile->filter_sampling_rate;
    for (int ch = 0; ch < my->num_channels; ch++) {
      my->channel[ch].num_filters = my->filterfile->num_filters;
      for (int f = 0; f < my->filterfile->num_filters; f++) {
        my->channel[ch].filter[f].param = my->filterfile->filter_param[f];
      }
    }
  }

  return 0;
}

/*******************************************************************************
 * This IIR filter implementation is optimized for minimal computing power.
 * Since the filter coefficients b0, b1, and b2 are fixed at 1, 0, and -1, three
 * out of the six floating point multiplications can be saved.
 * In addition to the actual filter, the input is passed through to the output.
 * By choosing the filter coefficients a1, a2 and gain, it is possible to
 * amplified or attenuated the desired frequency range with a certain width.
 * In order to correct more than one frequency range or to get a steeper filter,
 * several filter stages can be chained.
 *
 *  ────►┬───────────────────────────────────►(+)────►
 *       │                                     ↑
 *       ├──►(b0)───►(+)─────►─────┬──►(gain)──┘
 *       │            ↑            │
 *       │         ┏━━┷━━┓         │
 *       │         ┃ z⁻¹ ┃         │
 *       │         ┗━━┯━━┛         │
 *       │            ↑            │
 *       ├──►(b1)───►(+)◄───(a1)◄──┤
 *       │            ↑            │
 *       │         ┏━━┷━━┓         │
 *       │         ┃ z⁻¹ ┃         │
 *       │         ┗━━┯━━┛         │
 *       │            ↑            │
 *       └──►(b2)───►(+)◄───(a2)◄──┘
 *
 ******************************************************************************/
static float filter(filter_t *filter, float in) {
  float out = in + filter->state[0];
  filter->state[0] = filter->state[1] - out * filter->param.a1;
  filter->state[1] = -in - out * filter->param.a2;
  return in + out * filter->param.gain;
}
