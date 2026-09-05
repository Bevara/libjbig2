/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / JBIG2 decoder filter, based on jbig2dec
 *  (https://github.com/ArtifexSoftware/jbig2dec).
 *
 *  Only standalone (non-embedded) JBIG2 files are handled: PDF-embedded JBIG2
 *  streams carry their symbol dictionary in a separate globals stream, which
 *  this filter has no way to receive.
 *
 *  JBIG2 pages are bi-level, 1 meaning black; they are expanded to 8-bit
 *  greyscale on output.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <jbig2.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_JBIG2DecCtx;

static GF_Err jbig2dec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_JBIG2DecCtx *ctx = (GF_JBIG2DecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));

	return GF_OK;
}

static Bool jbig2dec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_JBIG2DecCtx *ctx = (GF_JBIG2DecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

/* jbig2dec reports parse problems through this callback; route them to the
 * GPAC log instead of stderr. */
static void jbig2dec_error(void *data, const char *msg, Jbig2Severity severity, uint32_t seg_idx)
{
	/* Braces are needed on both branches: GF_LOG itself expands to an if
	 * statement, so an unbraced else would bind to the wrong one. */
	if (severity == JBIG2_SEVERITY_FATAL)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[JBIG2Dec] %s (segment %u)\n", msg, seg_idx));
	}
	else
	{
		GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[JBIG2Dec] %s (segment %u)\n", msg, seg_idx));
	}
}

static GF_Err jbig2dec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, out_size, x, y;
	Jbig2Ctx *jctx;
	Jbig2Image *image;
	GF_JBIG2DecCtx *ctx = (GF_JBIG2DecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	jctx = jbig2_ctx_new(NULL, 0, NULL, jbig2dec_error, NULL);
	if (!jctx)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	if (jbig2_data_in(jctx, (const unsigned char *)data, size) < 0)
	{
		jbig2_ctx_free(jctx);
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[JBIG2Dec] Failed to parse JBIG2 stream\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	gf_filter_pid_drop_packet(ctx->ipid);

	/* Files that end without an explicit end-of-page segment still have a
	 * usable page, hence the forced completion before reading it out. */
	jbig2_complete_page(jctx);

	image = jbig2_page_out(jctx);
	if (!image || !image->data || !image->width || !image->height)
	{
		if (image)
			jbig2_release_page(jctx, image);
		jbig2_ctx_free(jctx);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[JBIG2Dec] No page decoded\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	/* Expanded to RGB, not GF_PIXEL_GREYSCALE: see the same note in dec_jbig.c. */
	out_size = image->width * image->height * 3;

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(image->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(image->height));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(image->width * 3));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck)
	{
		jbig2_release_page(jctx, image);
		jbig2_ctx_free(jctx);
		return GF_OUT_OF_MEM;
	}

	for (y = 0; y < image->height; y++)
	{
		for (x = 0; x < image->width; x++)
		{
			u32 bit = image->data[y * image->stride + (x >> 3)] >> (7 - (x & 7)) & 1;
			u8 v = bit ? 0 : 255; /* 1 = black */
			u8 *px = output + ((size_t)y * image->width + x) * 3;
			px[0] = px[1] = px[2] = v;
		}
	}

	jbig2_release_page(jctx, image);
	jbig2_ctx_free(jctx);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void jbig2dec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability JBIG2DecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "jb2|jbig2"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "image/jbig2|image/x-jbig2"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister JBIG2DecoderRegister = {
	.name = "jbig2dec",
	GF_FS_SET_DESCRIPTION("JBIG2 image decoder")
		GF_FS_SET_HELP("This filter decodes standalone JBIG2 (ITU-T T.88) bi-level images using jbig2dec.")
			.private_size = sizeof(GF_JBIG2DecCtx),
	SETCAPS(JBIG2DecCaps),
	.configure_pid = jbig2dec_configure_pid,
	.process = jbig2dec_process,
	.process_event = jbig2dec_process_event,
	.finalize = jbig2dec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE jbig2dec_register(GF_FilterSession *session)
{
	return &JBIG2DecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_jbig2dec(void) {
    gf_filter_auto_register("jbig2dec", jbig2dec_register);
}
