/*
Spancam OBS Studio plugin — "Spancam Camera" video source
Copyright (C) 2026 Aditya Dewaskar <support@adewaskar.com>
SPDX-License-Identifier: GPL-2.0-or-later

An OBS async video source fed by a Spancam phone. Nothing is connected yet —
this is the registration skeleton, so the source shows up in the "Add source"
list and its properties panel renders.
*/

#include <obs-module.h>
#include <plugin-support.h>

#define SPANCAM_DEFAULT_PORT 8892

struct spancam_source {
	obs_source_t *source;
	char *host;
	int port;
};

static const char *spancam_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Spancam.Source.Name");
}

static void spancam_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "port", SPANCAM_DEFAULT_PORT);
}

static obs_properties_t *spancam_source_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "host", obs_module_text("Spancam.Prop.Host"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "port", obs_module_text("Spancam.Prop.Port"), 1, 65535, 1);
	return props;
}

static void spancam_source_update(void *data, obs_data_t *settings)
{
	struct spancam_source *ctx = data;
	bfree(ctx->host);
	ctx->host = bstrdup(obs_data_get_string(settings, "host"));
	ctx->port = (int)obs_data_get_int(settings, "port");
}

static void *spancam_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct spancam_source *ctx = bzalloc(sizeof(struct spancam_source));
	ctx->source = source;
	spancam_source_update(ctx, settings);
	return ctx;
}

static void spancam_source_destroy(void *data)
{
	struct spancam_source *ctx = data;
	bfree(ctx->host);
	bfree(ctx);
}

struct obs_source_info spancam_source_info = {
	.id = "spancam_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = spancam_source_get_name,
	.create = spancam_source_create,
	.destroy = spancam_source_destroy,
	.update = spancam_source_update,
	.get_defaults = spancam_source_get_defaults,
	.get_properties = spancam_source_get_properties,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};
