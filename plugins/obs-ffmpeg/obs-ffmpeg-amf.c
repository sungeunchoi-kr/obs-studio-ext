/******************************************************************************
    Copyright (C) 2022 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "obs-ffmpeg-video-encoders.h"

#define do_log(level, format, ...)                \
	blog(level, "[ffmpeg-amf: '%s'] " format, \
	     obs_encoder_get_name(enc->ffve.encoder), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

struct ffmpeg_amf_encoder {
	struct ffmpeg_video_encoder ffve;
	DARRAY(uint8_t) header;
};

#define H264_ENCODER_NAME "AMD HW H.264"
#define H265_ENCODER_NAME "AMD HW H.265 (HEVC)"

static const char *ffmpeg_amf_avc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return H264_ENCODER_NAME;
}

static const char *ffmpeg_amf_hevc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return H265_ENCODER_NAME;
}

static inline bool valid_format(enum video_format format)
{
	return format == VIDEO_FORMAT_I420 || format == VIDEO_FORMAT_NV12;
}

static void ffmpeg_amf_video_info(void *data, struct video_scale_info *info)
{
	struct ffmpeg_amf_encoder *enc = data;
	enum video_format pref_format;

	pref_format = obs_encoder_get_preferred_video_format(enc->ffve.encoder);

	if (!valid_format(pref_format)) {
		pref_format = valid_format(info->format) ? info->format
							 : VIDEO_FORMAT_NV12;
	}

	info->format = pref_format;
}

static bool ffmpeg_amf_update(struct ffmpeg_amf_encoder *enc,
			      obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");

	video_t *video = obs_encoder_video(enc->ffve.encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	struct video_scale_info info;

	info.format = voi->format;
	info.colorspace = voi->colorspace;
	info.range = voi->range;

	ffmpeg_amf_video_info(enc, &info);
	av_opt_set(enc->ffve.context->priv_data, "profile", profile, 0);
	av_opt_set(enc->ffve.context->priv_data, "preset", preset, 0);

	if (astrcmpi(rc, "cqp") == 0) {
		av_opt_set(enc->ffve.context->priv_data, "rc", "cqp", 0);
		bitrate = 0;
		enc->ffve.context->global_quality = cqp;

	} else {
		const int64_t rate = bitrate * 1000LL;

		if (astrcmpi(rc, "vbr") == 0) {
			av_opt_set(enc->ffve.context->priv_data, "rc",
				   "vbr_peak", 0);
		} else { /* CBR by default */
			av_opt_set(enc->ffve.context->priv_data, "rc", "cbr",
				   0);
			enc->ffve.context->rc_min_rate = rate;
		}

		enc->ffve.context->rc_max_rate = rate;
		cqp = 0;
	}

	av_opt_set(enc->ffve.context->priv_data, "level", "auto", 0);

	const char *ffmpeg_opts = obs_data_get_string(settings, "ffmpeg_opts");
	ffmpeg_video_encoder_update(&enc->ffve, bitrate, keyint_sec, voi, &info,
				    ffmpeg_opts);

	info("settings:\n"
	     "\trate_control: %s\n"
	     "\tbitrate:      %d\n"
	     "\tcqp:          %d\n"
	     "\tkeyint:       %d\n"
	     "\tpreset:       %s\n"
	     "\tprofile:      %s\n"
	     "\twidth:        %d\n"
	     "\theight:       %d\n",
	     rc, bitrate, cqp, enc->ffve.context->gop_size, preset, profile,
	     enc->ffve.context->width, enc->ffve.context->height);

	return ffmpeg_video_encoder_init_codec(&enc->ffve);
}

static bool ffmpeg_amf_reconfigure(void *data, obs_data_t *settings)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 19, 101)
	struct ffmpeg_amf_encoder *enc = data;

	const int64_t bitrate = obs_data_get_int(settings, "bitrate");
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cbr = astrcmpi(rc, "CBR") == 0;
	bool vbr = astrcmpi(rc, "VBR") == 0;
	if (cbr || vbr) {
		const int64_t rate = bitrate * 1000;
		enc->ffve.context->bit_rate = rate;
		enc->ffve.context->rc_max_rate = rate;
	}
#endif
	return true;
}

static void ffmpeg_amf_destroy(void *data)
{
	struct ffmpeg_amf_encoder *enc = data;
	ffmpeg_video_encoder_free(&enc->ffve);
	da_free(enc->header);
	bfree(enc);
}

static void on_init_error(void *data, int ret)
{
	struct ffmpeg_amf_encoder *enc = data;
	struct dstr error_message = {0};

	dstr_copy(&error_message, obs_module_text("AMF.Error"));
	dstr_replace(&error_message, "%1", av_err2str(ret));
	dstr_cat(&error_message, "\r\n\r\n");

	if (ret == AVERROR_EXTERNAL) {
		dstr_cat(&error_message, obs_module_text("AMF.GenericError"));
	} else {
		dstr_cat(&error_message, obs_module_text("NVENC.CheckDrivers"));
	}

	obs_encoder_set_last_error(enc->ffve.encoder, error_message.array);
	dstr_free(&error_message);
}

static void on_first_packet(void *data, AVPacket *pkt, struct darray *da)
{
	struct ffmpeg_amf_encoder *enc = data;

	if (enc->ffve.context->extradata_size)
		da_copy_array(enc->header, enc->ffve.context->extradata,
			      enc->ffve.context->extradata_size);
	darray_copy_array(1, da, pkt->data, pkt->size);
}

static void *ffmpeg_amf_create(obs_data_t *settings, obs_encoder_t *encoder,
			       bool hevc)
{
	struct ffmpeg_amf_encoder *enc = bzalloc(sizeof(*enc));

	if (!ffmpeg_video_encoder_init(&enc->ffve, enc, settings, encoder,
				       hevc ? "hevc_amf" : "h264_amf", NULL,
				       hevc ? H265_ENCODER_NAME
					    : H264_ENCODER_NAME,
				       on_init_error, on_first_packet))
		goto fail;
	if (!ffmpeg_amf_update(enc, settings))
		goto fail;

	return enc;

fail:
	ffmpeg_amf_destroy(enc);
	return NULL;
}

static void *ffmpeg_amf_avc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return ffmpeg_amf_create(settings, encoder, false);
}

static void *ffmpeg_amf_hevc_create(obs_data_t *settings,
				    obs_encoder_t *encoder)
{
	return ffmpeg_amf_create(settings, encoder, true);
}

static bool ffmpeg_amf_encode(void *data, struct encoder_frame *frame,
			      struct encoder_packet *packet,
			      bool *received_packet)
{
	struct ffmpeg_amf_encoder *enc = data;
	return ffmpeg_video_encode(&enc->ffve, frame, packet, received_packet);
}

void amf_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_int(settings, "cqp", 20);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_string(settings, "preset", "hq");
	obs_data_set_default_string(settings, "profile", "high");
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p,
				  obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cqp = astrcmpi(rc, "CQP") == 0;

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !cqp);
	p = obs_properties_get(ppts, "cqp");
	obs_property_set_visible(p, cqp);
	return true;
}

static obs_properties_t *amf_properties_internal(bool hevc)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "rate_control",
				    obs_module_text("RateControl"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "CBR", "CBR");
	obs_property_list_add_string(p, "CQP", "CQP");
	obs_property_list_add_string(p, "VBR", "VBR");

	obs_property_set_modified_callback(p, rate_control_modified);

	p = obs_properties_add_int(props, "bitrate", obs_module_text("Bitrate"),
				   50, 300000, 50);
	obs_property_int_set_suffix(p, " Kbps");

	obs_properties_add_int(props, "cqp", obs_module_text("NVENC.CQLevel"),
			       1, 30, 1);

	obs_properties_add_int(props, "keyint_sec",
			       obs_module_text("KeyframeIntervalSec"), 0, 10,
			       1);

	p = obs_properties_add_list(props, "preset", obs_module_text("Preset"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);

#define add_preset(val)                                                       \
	obs_property_list_add_string(p, obs_module_text("NVENC.Preset." val), \
				     val)
	add_preset("quality");
	add_preset("balanced");
	add_preset("speed");
#undef add_preset

	if (!hevc) {
		p = obs_properties_add_list(props, "profile",
					    obs_module_text("Profile"),
					    OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_STRING);

#define add_profile(val) obs_property_list_add_string(p, val, val)
		add_profile("high");
		add_profile("main");
		add_profile("baseline");
#undef add_profile
	}

	return props;
}

obs_properties_t *amf_avc_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	return amf_properties_internal(false);
}

obs_properties_t *amf_hevc_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	return amf_properties_internal(true);
}

static bool ffmpeg_amf_extra_data(void *data, uint8_t **extra_data,
				  size_t *size)
{
	struct ffmpeg_amf_encoder *enc = data;

	*extra_data = enc->header.array;
	*size = enc->header.num;
	return true;
}

struct obs_encoder_info ffmpeg_amf_avc_encoder_info = {
	.id = "h264_ffmpeg_amf",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = ffmpeg_amf_avc_getname,
	.create = ffmpeg_amf_avc_create,
	.destroy = ffmpeg_amf_destroy,
	.encode = ffmpeg_amf_encode,
	.update = ffmpeg_amf_reconfigure,
	.get_defaults = amf_defaults,
	.get_properties = amf_avc_properties,
	.get_extra_data = ffmpeg_amf_extra_data,
	.get_video_info = ffmpeg_amf_video_info,
#ifdef _WIN32
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL,
#else
	.caps = OBS_ENCODER_CAP_DYN_BITRATE,
#endif
};

struct obs_encoder_info ffmpeg_amf_hevc_encoder_info = {
	.id = "h265_ffmpeg_amf",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = ffmpeg_amf_hevc_getname,
	.create = ffmpeg_amf_hevc_create,
	.destroy = ffmpeg_amf_destroy,
	.encode = ffmpeg_amf_encode,
	.update = ffmpeg_amf_reconfigure,
	.get_defaults = amf_defaults,
	.get_properties = amf_hevc_properties,
	.get_extra_data = ffmpeg_amf_extra_data,
	.get_video_info = ffmpeg_amf_video_info,
#ifdef _WIN32
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL,
#else
	.caps = OBS_ENCODER_CAP_DYN_BITRATE,
#endif
};
