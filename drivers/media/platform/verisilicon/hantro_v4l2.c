// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU codec driver
 *
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * Copyright 2018 Google LLC.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2010-2011 Samsung Electronics Co., Ltd.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "hantro.h"
#include "hantro_hw.h"
#include "hantro_v4l2.h"

#define  HANTRO_DEFAULT_BIT_DEPTH 8

static int hantro_set_fmt_out(struct hantro_ctx *ctx,
			      struct v4l2_pix_format_mplane *pix_mp,
			      bool need_postproc);
static int hantro_set_fmt_cap(struct hantro_ctx *ctx,
			      struct v4l2_pix_format_mplane *pix_mp);

static const struct hantro_fmt *
hantro_get_formats(const struct hantro_ctx *ctx, unsigned int *num_fmts, bool need_postproc)
{
	const struct hantro_fmt *formats;

	if (need_postproc) {
		*num_fmts = 0;
		return NULL;
	}

	if (ctx->is_encoder) {
		formats = ctx->dev->variant->enc_fmts;
		*num_fmts = ctx->dev->variant->num_enc_fmts;
	} else {
		formats = ctx->dev->variant->dec_fmts;
		*num_fmts = ctx->dev->variant->num_dec_fmts;
	}

	return formats;
}

static const struct hantro_fmt *
hantro_get_postproc_formats(const struct hantro_ctx *ctx,
			    unsigned int *num_fmts)
{
	struct hantro_dev *vpu = ctx->dev;

	if (ctx->is_encoder || !vpu->variant->postproc_fmts) {
		*num_fmts = 0;
		return NULL;
	}

	*num_fmts = ctx->dev->variant->num_postproc_fmts;
	return ctx->dev->variant->postproc_fmts;
}

int hantro_get_format_depth(u32 fourcc)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_P010:
	case V4L2_PIX_FMT_P010_4L4:
	case V4L2_PIX_FMT_NV15:
	case V4L2_PIX_FMT_NV15_4L4:
		return 10;
	default:
		return 8;
	}
}

static bool
hantro_check_depth_match(const struct hantro_fmt *fmt, int bit_depth)
{
	int fmt_depth;

	if (!fmt->match_depth && !fmt->postprocessed)
		return true;

	/* 0 means default depth, which is 8 */
	if (!bit_depth)
		bit_depth = HANTRO_DEFAULT_BIT_DEPTH;

	fmt_depth = hantro_get_format_depth(fmt->fourcc);

	/*
	 * Allow only downconversion for postproc formats for now.
	 * It may be possible to relax that on some HW.
	 */
	if (!fmt->match_depth)
		return fmt_depth <= bit_depth;

	return fmt_depth == bit_depth;
}

static const struct hantro_fmt *
hantro_find_format(const struct hantro_ctx *ctx, u32 fourcc)
{
	const struct hantro_fmt *formats;
	unsigned int i, num_fmts;

	formats = hantro_get_formats(ctx, &num_fmts, HANTRO_AUTO_POSTPROC);
	for (i = 0; i < num_fmts; i++)
		if (formats[i].fourcc == fourcc)
			return &formats[i];

	formats = hantro_get_postproc_formats(ctx, &num_fmts);
	for (i = 0; i < num_fmts; i++)
		if (formats[i].fourcc == fourcc)
			return &formats[i];
	return NULL;
}

const struct hantro_fmt *
hantro_get_default_fmt(const struct hantro_ctx *ctx, bool bitstream,
		       int bit_depth, bool need_postproc)
{
	const struct hantro_fmt *formats;
	unsigned int i, num_fmts;

	formats = hantro_get_formats(ctx, &num_fmts, need_postproc);
	for (i = 0; i < num_fmts; i++) {
		if (bitstream == (formats[i].codec_mode !=
				  HANTRO_MODE_NONE) &&
		    hantro_check_depth_match(&formats[i], bit_depth))
			return &formats[i];
	}

	formats = hantro_get_postproc_formats(ctx, &num_fmts);
	for (i = 0; i < num_fmts; i++) {
		if (bitstream == (formats[i].codec_mode !=
				  HANTRO_MODE_NONE) &&
		    hantro_check_depth_match(&formats[i], bit_depth))
			return &formats[i];
	}

	return NULL;
}

static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct hantro_dev *vpu = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, vpu->dev->driver->name, sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));
	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	struct hantro_ctx *ctx = fh_to_ctx(priv);
	const struct hantro_fmt *fmt;

	fmt = hantro_find_format(ctx, fsize->pixel_format);
	if (!fmt) {
		vpu_debug(0, "unsupported bitstream format (%08x)\n",
			  fsize->pixel_format);
		return -EINVAL;
	}

	/* For non-coded formats check if postprocessing scaling is possible */
	if (fmt->codec_mode == HANTRO_MODE_NONE) {
		if (hantro_needs_postproc(ctx, fmt))
			return hanto_postproc_enum_framesizes(ctx, fsize);
		else
			return -ENOTTY;
	} else if (fsize->index != 0) {
		vpu_debug(0, "invalid frame size index (expected 0, got %d)\n",
			  fsize->index);
		return -EINVAL;
	}

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = fmt->frmsize;

	return 0;
}

static int vidioc_enum_fmt(struct file *file, void *priv,
			   struct v4l2_fmtdesc *f, bool capture)

{
	struct hantro_ctx *ctx = fh_to_ctx(priv);
	const struct hantro_fmt *fmt, *formats;
	unsigned int num_fmts, i, j = 0;
	bool skip_mode_none;

	/*
	 * When dealing with an encoder:
	 *  - on the capture side we want to filter out all MODE_NONE formats.
	 *  - on the output side we want to filter out all formats that are
	 *    not MODE_NONE.
	 * When dealing with a decoder:
	 *  - on the capture side we want to filter out all formats that are
	 *    not MODE_NONE.
	 *  - on the output side we want to filter out all MODE_NONE formats.
	 */
	skip_mode_none = capture == ctx->is_encoder;

	formats = hantro_get_formats(ctx, &num_fmts, HANTRO_AUTO_POSTPROC);
	for (i = 0; i < num_fmts; i++) {
		bool mode_none = formats[i].codec_mode == HANTRO_MODE_NONE;
		fmt = &formats[i];

		if (skip_mode_none == mode_none)
			continue;
		if (!hantro_check_depth_match(fmt, ctx->bit_depth))
			continue;
		if (j == f->index) {
			f->pixelformat = fmt->fourcc;
			return 0;
		}
		++j;
	}

	/*
	 * Enumerate post-processed formats. As per the specification,
	 * we enumerated these formats after natively decoded formats
	 * as a hint for applications on what's the preferred fomat.
	 */
	if (!capture)
		return -EINVAL;
	formats = hantro_get_postproc_formats(ctx, &num_fmts);
	for (i = 0; i < num_fmts; i++) {
		fmt = &formats[i];

		if (!hantro_check_depth_match(fmt, ctx->bit_depth))
			continue;
		if (j == f->index) {
			f->pixelformat = fmt->fourcc;
			return 0;
		}
		++j;
	}

	return -EINVAL;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return vidioc_enum_fmt(file, priv, f, true);
}

static int vidioc_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return vidioc_enum_fmt(file, priv, f, false);
}

static int vidioc_g_fmt_out_mplane(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct hantro_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->src_fmt;

	return 0;
}

static int vidioc_g_fmt_cap_mplane(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct hantro_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->dst_fmt;

	return 0;
}

static int hantro_try_fmt(const struct hantro_ctx *ctx,
			  struct v4l2_pix_format_mplane *pix_mp,
			  enum v4l2_buf_type type)
{
	const struct hantro_fmt *fmt;
	const struct hantro_fmt *vpu_fmt;
	bool capture = V4L2_TYPE_IS_CAPTURE(type);
	bool coded;

	coded = capture == ctx->is_encoder;

	vpu_debug(4, "trying format %p4cc\n", &pix_mp->pixelformat);

	fmt = hantro_find_format(ctx, pix_mp->pixelformat);
	if (!fmt) {
		fmt = hantro_get_default_fmt(ctx, coded, HANTRO_DEFAULT_BIT_DEPTH, HANTRO_AUTO_POSTPROC);
		pix_mp->pixelformat = fmt->fourcc;
	}

	if (coded) {
		pix_mp->num_planes = 1;
		vpu_fmt = fmt;
	} else if (ctx->is_encoder) {
		vpu_fmt = hantro_find_format(ctx, ctx->dst_fmt.pixelformat);
	} else {
		/*
		 * Width/height on the CAPTURE end of a decoder are ignored and
		 * replaced by the OUTPUT ones.
		 */
		pix_mp->width = ctx->src_fmt.width;
		pix_mp->height = ctx->src_fmt.height;
		vpu_fmt = fmt;
	}

	pix_mp->field = V4L2_FIELD_NONE;

	v4l2_apply_frmsize_constraints(&pix_mp->width, &pix_mp->height,
				       &vpu_fmt->frmsize);

	if (!coded) {
		/* Fill remaining fields */
		v4l2_fill_pixfmt_mp(pix_mp, fmt->fourcc, pix_mp->width,
				    pix_mp->height);
		if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_H264_SLICE &&
		    !hantro_needs_postproc(ctx, fmt))
			pix_mp->plane_fmt[0].sizeimage +=
				hantro_h264_mv_size(pix_mp->width,
						    pix_mp->height);
		else if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_VP9_FRAME &&
			 !hantro_needs_postproc(ctx, fmt))
			pix_mp->plane_fmt[0].sizeimage +=
				hantro_vp9_mv_size(pix_mp->width,
						   pix_mp->height);
		else if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_HEVC_SLICE &&
			 !hantro_needs_postproc(ctx, fmt))
			pix_mp->plane_fmt[0].sizeimage +=
				hantro_hevc_mv_size(pix_mp->width,
						    pix_mp->height);
		else if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_AV1_FRAME &&
			 !hantro_needs_postproc(ctx, fmt))
			pix_mp->plane_fmt[0].sizeimage +=
				hantro_av1_mv_size(pix_mp->width,
						   pix_mp->height);
	} else if (!pix_mp->plane_fmt[0].sizeimage) {
		/*
		 * For coded formats the application can specify
		 * sizeimage. If the application passes a zero sizeimage,
		 * let's default to the maximum frame size.
		 */
		pix_mp->plane_fmt[0].sizeimage = fmt->header_size +
			pix_mp->width * pix_mp->height * fmt->max_depth;
	}

	return 0;
}

static int vidioc_try_fmt_cap_mplane(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	return hantro_try_fmt(fh_to_ctx(priv), &f->fmt.pix_mp, f->type);
}

static int vidioc_try_fmt_out_mplane(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	return hantro_try_fmt(fh_to_ctx(priv), &f->fmt.pix_mp, f->type);
}

static void
hantro_reset_fmt(struct v4l2_pix_format_mplane *fmt,
		 const struct hantro_fmt *vpu_fmt)
{
	memset(fmt, 0, sizeof(*fmt));

	fmt->pixelformat = vpu_fmt->fourcc;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_JPEG;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static void
hantro_reset_encoded_fmt(struct hantro_ctx *ctx)
{
	const struct hantro_fmt *vpu_fmt;
	struct v4l2_pix_format_mplane fmt;

	vpu_fmt = hantro_get_default_fmt(ctx, true, HANTRO_DEFAULT_BIT_DEPTH, HANTRO_AUTO_POSTPROC);
	if (!vpu_fmt)
		return;

	hantro_reset_fmt(&fmt, vpu_fmt);
	fmt.width = vpu_fmt->frmsize.min_width;
	fmt.height = vpu_fmt->frmsize.min_height;
	if (ctx->is_encoder)
		hantro_set_fmt_cap(ctx, &fmt);
	else
		hantro_set_fmt_out(ctx, &fmt, HANTRO_AUTO_POSTPROC);
}

int
hantro_reset_raw_fmt(struct hantro_ctx *ctx, int bit_depth, bool need_postproc)
{
	const struct hantro_fmt *raw_vpu_fmt;
	struct v4l2_pix_format_mplane raw_fmt, *encoded_fmt;
	int ret;

	raw_vpu_fmt = hantro_get_default_fmt(ctx, false, bit_depth, need_postproc);
	if (!raw_vpu_fmt)
		return -EINVAL;

	if (ctx->is_encoder) {
		encoded_fmt = &ctx->dst_fmt;
		ctx->vpu_src_fmt = raw_vpu_fmt;
	} else {
		encoded_fmt = &ctx->src_fmt;
	}

	hantro_reset_fmt(&raw_fmt, raw_vpu_fmt);
	raw_fmt.width = encoded_fmt->width;
	raw_fmt.height = encoded_fmt->height;
	if (ctx->is_encoder)
		ret = hantro_set_fmt_out(ctx, &raw_fmt, need_postproc);
	else
		ret = hantro_set_fmt_cap(ctx, &raw_fmt);

	if (!ret) {
		ctx->bit_depth = bit_depth;
		ctx->need_postproc = need_postproc;
	}

	return ret;
}

void hantro_reset_fmts(struct hantro_ctx *ctx)
{
	hantro_reset_encoded_fmt(ctx);
	hantro_reset_raw_fmt(ctx, HANTRO_DEFAULT_BIT_DEPTH, HANTRO_AUTO_POSTPROC);
}

static void
hantro_update_requires_request(struct hantro_ctx *ctx, u32 fourcc)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_JPEG:
		ctx->fh.m2m_ctx->out_q_ctx.q.requires_requests = false;
		break;
	case V4L2_PIX_FMT_MPEG2_SLICE:
	case V4L2_PIX_FMT_VP8_FRAME:
	case V4L2_PIX_FMT_H264_SLICE:
	case V4L2_PIX_FMT_HEVC_SLICE:
	case V4L2_PIX_FMT_VP9_FRAME:
		ctx->fh.m2m_ctx->out_q_ctx.q.requires_requests = true;
		break;
	default:
		break;
	}
}

static void
hantro_update_requires_hold_capture_buf(struct hantro_ctx *ctx, u32 fourcc)
{
	struct vb2_queue *vq;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

	switch (fourcc) {
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_MPEG2_SLICE:
	case V4L2_PIX_FMT_VP8_FRAME:
	case V4L2_PIX_FMT_HEVC_SLICE:
	case V4L2_PIX_FMT_VP9_FRAME:
		vq->subsystem_flags &= ~(VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF);
		break;
	case V4L2_PIX_FMT_H264_SLICE:
		vq->subsystem_flags |= VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF;
		break;
	default:
		break;
	}
}

static int hantro_set_fmt_out(struct hantro_ctx *ctx,
			      struct v4l2_pix_format_mplane *pix_mp,
			      bool need_postproc)
{
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	ret = hantro_try_fmt(ctx, pix_mp, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret)
		return ret;

	if (!ctx->is_encoder) {
		/*
		 * In order to support dynamic resolution change,
		 * the decoder admits a resolution change, as long
		 * as the pixelformat remains.
		 */
		if (vb2_is_streaming(vq) && pix_mp->pixelformat != ctx->src_fmt.pixelformat) {
			return -EBUSY;
		}
	} else {
		/*
		 * The encoder doesn't admit a format change if
		 * there are OUTPUT buffers allocated.
		 */
		if (vb2_is_busy(vq))
			return -EBUSY;
	}

	ctx->vpu_src_fmt = hantro_find_format(ctx, pix_mp->pixelformat);
	ctx->src_fmt = *pix_mp;

	/*
	 * Current raw format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the raw format again after we return, so we don't need
	 * anything smarter.
	 * Note that hantro_reset_raw_fmt() also propagates size
	 * changes to the raw format.
	 */
	if (!ctx->is_encoder)
		hantro_reset_raw_fmt(ctx,
				     hantro_get_format_depth(pix_mp->pixelformat),
				     need_postproc);

	/* Colorimetry information are always propagated. */
	ctx->dst_fmt.colorspace = pix_mp->colorspace;
	ctx->dst_fmt.ycbcr_enc = pix_mp->ycbcr_enc;
	ctx->dst_fmt.xfer_func = pix_mp->xfer_func;
	ctx->dst_fmt.quantization = pix_mp->quantization;

	hantro_update_requires_request(ctx, pix_mp->pixelformat);
	hantro_update_requires_hold_capture_buf(ctx, pix_mp->pixelformat);

	vpu_debug(0, "OUTPUT codec mode: %d\n", ctx->vpu_src_fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height);
	return 0;
}

static int hantro_set_fmt_cap(struct hantro_ctx *ctx,
			      struct v4l2_pix_format_mplane *pix_mp)
{
	int ret;

	if (ctx->is_encoder) {
		struct vb2_queue *peer_vq;

		/*
		 * Since format change on the CAPTURE queue will reset
		 * the OUTPUT queue, we can't allow doing so
		 * when the OUTPUT queue has buffers allocated.
		 */
		peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
					  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (vb2_is_busy(peer_vq) &&
		    (pix_mp->pixelformat != ctx->dst_fmt.pixelformat ||
		     pix_mp->height != ctx->dst_fmt.height ||
		     pix_mp->width != ctx->dst_fmt.width))
			return -EBUSY;
	}

	ret = hantro_try_fmt(ctx, pix_mp, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (ret)
		return ret;

	ctx->vpu_dst_fmt = hantro_find_format(ctx, pix_mp->pixelformat);
	ctx->dst_fmt = *pix_mp;

	/*
	 * Current raw format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the raw format again after we return, so we don't need
	 * anything smarter.
	 * Note that hantro_reset_raw_fmt() also propagates size
	 * changes to the raw format.
	 */
	if (ctx->is_encoder)
		hantro_reset_raw_fmt(ctx, HANTRO_DEFAULT_BIT_DEPTH, HANTRO_AUTO_POSTPROC);

	/* Colorimetry information are always propagated. */
	ctx->src_fmt.colorspace = pix_mp->colorspace;
	ctx->src_fmt.ycbcr_enc = pix_mp->ycbcr_enc;
	ctx->src_fmt.xfer_func = pix_mp->xfer_func;
	ctx->src_fmt.quantization = pix_mp->quantization;

	vpu_debug(0, "CAPTURE codec mode: %d\n", ctx->vpu_dst_fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height);

	hantro_update_requires_request(ctx, pix_mp->pixelformat);

	return 0;
}

static int
vidioc_s_fmt_out_mplane(struct file *file, void *priv, struct v4l2_format *f)
{
	return hantro_set_fmt_out(fh_to_ctx(priv), &f->fmt.pix_mp, HANTRO_AUTO_POSTPROC);
}

static int
vidioc_s_fmt_cap_mplane(struct file *file, void *priv, struct v4l2_format *f)
{
	return hantro_set_fmt_cap(fh_to_ctx(priv), &f->fmt.pix_mp);
}

static int vidioc_g_selection(struct file *file, void *priv,
			      struct v4l2_selection *sel)
{
	struct hantro_ctx *ctx = fh_to_ctx(priv);

	/* Crop only supported on source. */
	if (!ctx->is_encoder ||
	    sel->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = ctx->src_fmt.width;
		sel->r.height = ctx->src_fmt.height;
		break;
	case V4L2_SEL_TGT_CROP:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = ctx->dst_fmt.width;
		sel->r.height = ctx->dst_fmt.height;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int vidioc_s_selection(struct file *file, void *priv,
			      struct v4l2_selection *sel)
{
	struct hantro_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_rect *rect = &sel->r;
	struct vb2_queue *vq;

	/* Crop only supported on source. */
	if (!ctx->is_encoder ||
	    sel->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	/* Change not allowed if the queue is streaming. */
	vq = v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	/*
	 * We do not support offsets, and we can crop only inside
	 * right-most or bottom-most macroblocks.
	 */
	if (rect->left != 0 || rect->top != 0 ||
	    round_up(rect->width, MB_DIM) != ctx->src_fmt.width ||
	    round_up(rect->height, MB_DIM) != ctx->src_fmt.height) {
		/* Default to full frame for incorrect settings. */
		rect->left = 0;
		rect->top = 0;
		rect->width = ctx->src_fmt.width;
		rect->height = ctx->src_fmt.height;
	} else {
		/* We support widths aligned to 4 pixels and arbitrary heights. */
		rect->width = round_up(rect->width, 4);
	}

	ctx->dst_fmt.width = rect->width;
	ctx->dst_fmt.height = rect->height;

	return 0;
}

static const struct v4l2_event hantro_eos_event = {
	.type = V4L2_EVENT_EOS
};

static int vidioc_encoder_cmd(struct file *file, void *priv,
			      struct v4l2_encoder_cmd *ec)
{
	struct hantro_ctx *ctx = fh_to_ctx(priv);
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, priv, ec);
	if (ret < 0)
		return ret;

	if (!vb2_is_streaming(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)) ||
	    !vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return 0;

	ret = v4l2_m2m_ioctl_encoder_cmd(file, priv, ec);
	if (ret < 0)
		return ret;

	if (ec->cmd == V4L2_ENC_CMD_STOP &&
	    v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &hantro_eos_event);

	if (ec->cmd == V4L2_ENC_CMD_START)
		vb2_clear_last_buffer_dequeued(&ctx->fh.m2m_ctx->cap_q_ctx.q);

	return 0;
}

const struct v4l2_ioctl_ops hantro_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = vidioc_try_fmt_cap_mplane,
	.vidioc_try_fmt_vid_out_mplane = vidioc_try_fmt_out_mplane,
	.vidioc_s_fmt_vid_out_mplane = vidioc_s_fmt_out_mplane,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt_cap_mplane,
	.vidioc_g_fmt_vid_out_mplane = vidioc_g_fmt_out_mplane,
	.vidioc_g_fmt_vid_cap_mplane = vidioc_g_fmt_cap_mplane,
	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt_vid_out,
	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_remove_bufs = v4l2_m2m_ioctl_remove_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_g_selection = vidioc_g_selection,
	.vidioc_s_selection = vidioc_s_selection,

	.vidioc_decoder_cmd = v4l2_m2m_ioctl_stateless_decoder_cmd,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,

	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = vidioc_encoder_cmd,
};

static int
hantro_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
		   unsigned int *num_planes, unsigned int sizes[],
		   struct device *alloc_devs[])
{
	struct hantro_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pixfmt;
	int i;

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		pixfmt = &ctx->dst_fmt;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		pixfmt = &ctx->src_fmt;
		break;
	default:
		vpu_err("invalid queue type: %d\n", vq->type);
		return -EINVAL;
	}

	if (*num_planes) {
		if (*num_planes != pixfmt->num_planes)
			return -EINVAL;
		for (i = 0; i < pixfmt->num_planes; ++i)
			if (sizes[i] < pixfmt->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}

	*num_planes = pixfmt->num_planes;
	for (i = 0; i < pixfmt->num_planes; ++i)
		sizes[i] = pixfmt->plane_fmt[i].sizeimage;
	return 0;
}

static int
hantro_buf_plane_check(struct vb2_buffer *vb,
		       struct v4l2_pix_format_mplane *pixfmt)
{
	unsigned int sz;
	int i;

	for (i = 0; i < pixfmt->num_planes; ++i) {
		sz = pixfmt->plane_fmt[i].sizeimage;
		vpu_debug(4, "plane %d size: %ld, sizeimage: %u\n",
			  i, vb2_plane_size(vb, i), sz);
		if (vb2_plane_size(vb, i) < sz) {
			vpu_err("plane %d is too small for output\n", i);
			return -EINVAL;
		}
	}
	return 0;
}

static int hantro_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct hantro_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pix_fmt;
	int ret;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;
	ret = hantro_buf_plane_check(vb, pix_fmt);
	if (ret)
		return ret;
	/*
	 * Buffer's bytesused must be written by driver for CAPTURE buffers.
	 * (for OUTPUT buffers, if userspace passes 0 bytesused, v4l2-core sets
	 * it to buffer length).
	 */
	if (V4L2_TYPE_IS_CAPTURE(vq->type)) {
		if (ctx->is_encoder)
			vb2_set_plane_payload(vb, 0, 0);
		else
			vb2_set_plane_payload(vb, 0, pix_fmt->plane_fmt[0].sizeimage);
	}

	return 0;
}

static void hantro_buf_queue(struct vb2_buffer *vb)
{
	struct hantro_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (V4L2_TYPE_IS_CAPTURE(vb->vb2_queue->type) &&
	    vb2_is_streaming(vb->vb2_queue) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		unsigned int i;

		for (i = 0; i < vb->num_planes; i++)
			vb2_set_plane_payload(vb, i, 0);

		vbuf->field = V4L2_FIELD_NONE;
		vbuf->sequence = ctx->sequence_cap++;

		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, vbuf);
		v4l2_event_queue_fh(&ctx->fh, &hantro_eos_event);
		return;
	}

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static bool hantro_vq_is_coded(struct vb2_queue *q)
{
	struct hantro_ctx *ctx = vb2_get_drv_priv(q);

	return ctx->is_encoder != V4L2_TYPE_IS_OUTPUT(q->type);
}

static int hantro_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct hantro_ctx *ctx = vb2_get_drv_priv(q);
	int ret = 0;

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->sequence_out = 0;
	else
		ctx->sequence_cap = 0;

	if (hantro_vq_is_coded(q)) {
		enum hantro_codec_mode codec_mode;

		if (V4L2_TYPE_IS_OUTPUT(q->type))
			codec_mode = ctx->vpu_src_fmt->codec_mode;
		else
			codec_mode = ctx->vpu_dst_fmt->codec_mode;

		vpu_debug(4, "Codec mode = %d\n", codec_mode);
		ctx->codec_ops = &ctx->dev->variant->codec_ops[codec_mode];
		if (ctx->codec_ops->init) {
			ret = ctx->codec_ops->init(ctx);
			if (ret)
				return ret;
		}

		if (hantro_needs_postproc(ctx, ctx->vpu_dst_fmt)) {
			ret = hantro_postproc_init(ctx);
			if (ret)
				goto err_codec_exit;
		}
	}
	return ret;

err_codec_exit:
	if (ctx->codec_ops->exit)
		ctx->codec_ops->exit(ctx);
	return ret;
}

static void
hantro_return_bufs(struct vb2_queue *q,
		   struct vb2_v4l2_buffer *(*buf_remove)(struct v4l2_m2m_ctx *))
{
	struct hantro_ctx *ctx = vb2_get_drv_priv(q);

	for (;;) {
		struct vb2_v4l2_buffer *vbuf;

		vbuf = buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req,
					   &ctx->ctrl_handler);
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static void hantro_stop_streaming(struct vb2_queue *q)
{
	struct hantro_ctx *ctx = vb2_get_drv_priv(q);

	if (hantro_vq_is_coded(q)) {
		hantro_postproc_free(ctx);
		if (ctx->codec_ops && ctx->codec_ops->exit)
			ctx->codec_ops->exit(ctx);
	}

	/*
	 * The mem2mem framework calls v4l2_m2m_cancel_job before
	 * .stop_streaming, so there isn't any job running and
	 * it is safe to return all the buffers.
	 */
	if (V4L2_TYPE_IS_OUTPUT(q->type))
		hantro_return_bufs(q, v4l2_m2m_src_buf_remove);
	else
		hantro_return_bufs(q, v4l2_m2m_dst_buf_remove);

	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);

	if (V4L2_TYPE_IS_OUTPUT(q->type) &&
	    v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &hantro_eos_event);
}

static void hantro_buf_request_complete(struct vb2_buffer *vb)
{
	struct hantro_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_handler);
}

static int hantro_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

const struct vb2_ops hantro_queue_ops = {
	.queue_setup = hantro_queue_setup,
	.buf_prepare = hantro_buf_prepare,
	.buf_queue = hantro_buf_queue,
	.buf_out_validate = hantro_buf_out_validate,
	.buf_request_complete = hantro_buf_request_complete,
	.start_streaming = hantro_start_streaming,
	.stop_streaming = hantro_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};
