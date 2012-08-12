/*
 * Display Core
 *
 * Copyright (C) 2012 Renesas Solutions Corp.
 *
 * Contacts: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>

/* -----------------------------------------------------------------------------
 * Display Entity
 */

struct display_entity;
struct videomode;

#define DISPLAY_ENTITY_NOTIFIER_CONNECT		1
#define DISPLAY_ENTITY_NOTIFIER_DISCONNECT	2

struct display_entity_notifier {
	int (*notify)(struct display_entity_notifier *, struct display_entity *,
		      int);
	struct device *dev;
	struct list_head list;
};

/**
 * enum display_entity_state - State of a display entity
 * @DISPLAY_ENTITY_STATE_OFF: The entity is turned off completely, possibly
 *	including its power supplies. Communication with a display entity in
 *	that state is not possible.
 * @DISPLAY_ENTITY_STATE_STANDBY: The entity is in a low-power state. Full
 *	communication with the display entity is supported, including pixel data
 *	transfer, but the output is kept blanked.
 * @DISPLAY_ENTITY_STATE_ON: The entity is fully operational.
 */
enum display_entity_state {
	DISPLAY_ENTITY_STATE_OFF,
	DISPLAY_ENTITY_STATE_STANDBY,
	DISPLAY_ENTITY_STATE_ON,
};

/**
 * enum display_entity_stream_state - State of a video stream
 * @DISPLAY_ENTITY_STREAM_STOPPED: The video stream is stopped, no frames are
 *	transferred.
 * @DISPLAY_ENTITY_STREAM_SINGLE_SHOT: The video stream has been started for
 *      single shot operation. The source entity will transfer a single frame
 *      and then stop.
 * @DISPLAY_ENTITY_STREAM_CONTINUOUS: The video stream is running, frames are
 *	transferred continuously by the source entity.
 */
enum display_entity_stream_state {
	DISPLAY_ENTITY_STREAM_STOPPED,
	DISPLAY_ENTITY_STREAM_SINGLE_SHOT,
	DISPLAY_ENTITY_STREAM_CONTINUOUS,
};

enum display_entity_interface_type {
	DISPLAY_ENTITY_INTERFACE_DPI,
};

struct display_entity_interface_params {
	enum display_entity_interface_type type;
};

struct display_entity_control_ops {
	int (*set_state)(struct display_entity *ent,
			 enum display_entity_state state);
	int (*update)(struct display_entity *ent);
	int (*get_modes)(struct display_entity *ent,
			 const struct videomode **modes);
	int (*get_params)(struct display_entity *ent,
			  struct display_entity_interface_params *params);
	int (*get_size)(struct display_entity *ent,
			unsigned int *width, unsigned int *height);
};

struct display_entity_video_ops {
	int (*set_stream)(struct display_entity *ent,
			  enum display_entity_stream_state state);
};

struct display_entity {
	struct list_head list;
	struct device *dev;
	struct module *owner;
	struct kref ref;

	struct display_entity *source;

	struct {
		const struct display_entity_control_ops *ctrl;
		const struct display_entity_video_ops *video;
	} ops;

	void(*release)(struct display_entity *ent);

	enum display_entity_state state;
};

int display_entity_set_state(struct display_entity *entity,
			     enum display_entity_state state);
int display_entity_update(struct display_entity *entity);
int display_entity_get_modes(struct display_entity *entity,
			     const struct videomode **modes);
int display_entity_get_params(struct display_entity *entity,
			      struct display_entity_interface_params *params);
int display_entity_get_size(struct display_entity *entity,
			    unsigned int *width, unsigned int *height);

int display_entity_set_stream(struct display_entity *entity,
			      enum display_entity_stream_state state);

static inline void display_entity_connect(struct display_entity *source,
					  struct display_entity *sink)
{
	sink->source = source;
}

static inline void display_entity_disconnect(struct display_entity *source,
					     struct display_entity *sink)
{
	sink->source = NULL;
}

struct display_entity *display_entity_get(struct display_entity *entity);
void display_entity_put(struct display_entity *entity);

int __must_check __display_entity_register(struct display_entity *entity,
					   struct module *owner);
void display_entity_unregister(struct display_entity *entity);

int display_entity_register_notifier(struct display_entity_notifier *notifier);
void display_entity_unregister_notifier(struct display_entity_notifier *notifier);

#define display_entity_register(display_entity) \
	__display_entity_register(display_entity, THIS_MODULE)

#endif /* __DISPLAY_H__ */
