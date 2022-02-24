#define _GNU_SOURCE

#include <ctype.h>
#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

struct virtualcam_data {
	obs_output_t *output;
	int device;
	uint32_t frame_size;
};

static const char *virtualcam_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Virtual Camera Output";
}

static void virtualcam_destroy(void *data)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)data;
	close(vcam->device);
	bfree(data);
}

static bool is_flatpak_sandbox(void)
{
	static bool flatpak_info_exists = false;
	static bool initialized = false;

	if (!initialized) {
		flatpak_info_exists = access("/.flatpak-info", F_OK) == 0;
		initialized = true;
	}

	return flatpak_info_exists;
}

static int run_command(const char *command)
{
	struct dstr str;
	int result;

	dstr_init_copy(&str, "PATH=\"$PATH:/sbin\" ");

	if (is_flatpak_sandbox())
		dstr_cat(&str, "flatpak-spawn --host ");

	dstr_cat(&str, command);
	result = system(str.array);
	dstr_free(&str);
	return result;
}

static bool loopback_module_loaded()
{
	bool loaded = false;

	char temp[512];

	FILE *fp = fopen("/proc/modules", "r");

	if (!fp)
		return false;

	while (fgets(temp, sizeof(temp), fp)) {
		if (strstr(temp, "v4l2loopback")) {
			loaded = true;
			break;
		}
	}

	if (fp)
		fclose(fp);

	return loaded;
}

bool loopback_module_available()
{
	if (loopback_module_loaded()) {
		return true;
	}

	if (run_command("modinfo v4l2loopback >/dev/null 2>&1") == 0) {
		return true;
	}

	return false;
}

static int loopback_module_load()
{
	return run_command(
		"pkexec modprobe v4l2loopback exclusive_caps=1 card_label='OBS Virtual Camera' && sleep 0.5");
}

static void *virtualcam_create(obs_data_t *settings, obs_output_t *output)
{
	struct virtualcam_data *vcam =
		(struct virtualcam_data *)bzalloc(sizeof(*vcam));
	vcam->output = output;

	UNUSED_PARAMETER(settings);
	return vcam;
}

static bool try_connect(void *data, const char *device)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)data;
	struct v4l2_format format;
	struct v4l2_capability capability;
	struct v4l2_streamparm parm;

	uint32_t width = obs_output_get_width(vcam->output);
	uint32_t height = obs_output_get_height(vcam->output);

	vcam->frame_size = width * height * 2;

	vcam->device = open(device, O_RDWR);

	if (vcam->device < 0)
		return false;

	if (ioctl(vcam->device, VIDIOC_QUERYCAP, &capability) < 0)
		return false;

	format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (ioctl(vcam->device, VIDIOC_G_FMT, &format) < 0)
		return false;

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	parm.parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	parm.parm.output.timeperframe.numerator = ovi.fps_den;
	parm.parm.output.timeperframe.denominator = ovi.fps_num;

	if (ioctl(vcam->device, VIDIOC_S_PARM, &parm) < 0)
		return false;

	format.fmt.pix.width = width;
	format.fmt.pix.height = height;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	format.fmt.pix.sizeimage = vcam->frame_size;

	if (ioctl(vcam->device, VIDIOC_S_FMT, &format) < 0)
		return false;

	struct video_scale_info vsi = {0};
	vsi.format = VIDEO_FORMAT_YUY2;
	vsi.width = width;
	vsi.height = height;
	obs_output_set_video_conversion(vcam->output, &vsi);

	blog(LOG_INFO, "Virtual camera started");
	obs_output_begin_data_capture(vcam->output, 0);

	return true;
}

static int scanfilter(const struct dirent *entry)
{
	return !astrcmp_n(entry->d_name, "video", 5);
}

#if defined(__FreeBSD__)
// versionsort is a glibc extension; implement it when on FreeBSD
// __strverscmp comes from glibc/string/strvercmp.c

/* states: S_N: normal, S_I: comparing integral part, S_F: comparing
           fractionnal parts, S_Z: idem but with leading Zeroes only */
#define  S_N    0x0
#define  S_I    0x3
#define  S_F    0x6
#define  S_Z    0x9
/* result_type: CMP: return diff; LEN: compare using len_diff/diff */
#define  CMP    2
#define  LEN    3
/* Compare S1 and S2 as strings holding indices/version numbers,
   returning less than, equal to or greater than zero if S1 is less than,
   equal to or greater than S2 (for more info, see the texinfo doc).
*/
int
__strverscmp (const char *s1, const char *s2)
{
  const unsigned char *p1 = (const unsigned char *) s1;
  const unsigned char *p2 = (const unsigned char *) s2;
  /* Symbol(s)    0       [1-9]   others
     Transition   (10) 0  (01) d  (00) x   */
  static const uint8_t next_state[] =
  {
      /* state    x    d    0  */
      /* S_N */  S_N, S_I, S_Z,
      /* S_I */  S_N, S_I, S_I,
      /* S_F */  S_N, S_F, S_F,
      /* S_Z */  S_N, S_F, S_Z
  };
  static const int8_t result_type[] =
  {
      /* state   x/x  x/d  x/0  d/x  d/d  d/0  0/x  0/d  0/0  */
      /* S_N */  CMP, CMP, CMP, CMP, LEN, CMP, CMP, CMP, CMP,
      /* S_I */  CMP, -1,  -1,  +1,  LEN, LEN, +1,  LEN, LEN,
      /* S_F */  CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP,
      /* S_Z */  CMP, +1,  +1,  -1,  CMP, CMP, -1,  CMP, CMP
  };
  if (p1 == p2)
    return 0;
  unsigned char c1 = *p1++;
  unsigned char c2 = *p2++;
  /* Hint: '0' is a digit too.  */
  int state = S_N + ((c1 == '0') + (isdigit (c1) != 0));
  int diff;
  while ((diff = c1 - c2) == 0)
    {
      if (c1 == '\0')
        return diff;
      state = next_state[state];
      c1 = *p1++;
      c2 = *p2++;
      state += (c1 == '0') + (isdigit (c1) != 0);
    }
  state = result_type[state * 3 + (((c2 == '0') + (isdigit (c2) != 0)))];
  switch (state)
  {
    case CMP:
      return diff;
    case LEN:
      while (isdigit (*p1++))
        if (!isdigit (*p2++))
          return 1;
      return isdigit (*p2) ? -1 : diff;
    default:
      return state;
  }
}

int
versionsort (const struct dirent **a, const struct dirent **b)
{
  return __strverscmp ((*a)->d_name, (*b)->d_name);
}
#endif

static bool virtualcam_start(void *data)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)data;
	struct dirent **list;
	bool success = false;
	int n;

	if (!loopback_module_loaded()) {
		if (loopback_module_load() != 0)
			return false;
	}

	n = scandir("/dev", &list, scanfilter, versionsort);
	if (n == -1)
		return false;

	for (int i = 0; i < n; i++) {
		char device[32];

		snprintf(device, 32, "/dev/%s", list[i]->d_name);

		if (try_connect(vcam, device)) {
			success = true;
			break;
		}
	}

	while (n--)
		free(list[n]);
	free(list);

	if (!success)
		blog(LOG_WARNING, "Failed to start virtual camera");

	return success;
}

static void virtualcam_stop(void *data, uint64_t ts)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)data;
	obs_output_end_data_capture(vcam->output);
	close(vcam->device);

	blog(LOG_INFO, "Virtual camera stopped");

	UNUSED_PARAMETER(ts);
}

static void virtual_video(void *param, struct video_data *frame)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)param;
	uint32_t frame_size = vcam->frame_size;
	while (frame_size > 0) {
		ssize_t written =
			write(vcam->device, frame->data[0], vcam->frame_size);
		if (written == -1)
			break;
		frame_size -= written;
	}
}

struct obs_output_info virtualcam_info = {
	.id = "virtualcam_output",
	.flags = OBS_OUTPUT_VIDEO,
	.get_name = virtualcam_name,
	.create = virtualcam_create,
	.destroy = virtualcam_destroy,
	.start = virtualcam_start,
	.stop = virtualcam_stop,
	.raw_video = virtual_video,
};
