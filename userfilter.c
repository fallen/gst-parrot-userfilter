/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2016 Yann Sionneau <<yann.sionneau@parrot.com>>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-myfilter
 *
 * FIXME:Describe myfilter here.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v -m fakesrc ! myfilter ! fakesink silent=TRUE
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif
#include <gst/video/gstvideofilter.h>

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
#include <sys/socket.h>
#include <stdint.h>
#include <string.h>

#include "libpomp.h"
#include "libmuta.h"
#include "userfilter.h"

GST_DEBUG_CATEGORY_STATIC (gst_my_filter_debug);
#define GST_CAT_DEFAULT gst_my_filter_debug

enum
{
  SEND_FD,
  BUFFER_PROCESSING_DONE,
};

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SILENT
};

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=YUY2")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=YUY2")
    );

#define gst_my_filter_parent_class parent_class
G_DEFINE_TYPE (GstMyFilter, gst_my_filter, GST_TYPE_VIDEO_FILTER);

static void gst_my_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_my_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_my_filter_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);

static GstTask *task;
static struct pomp_ctx *pomp_ctx;
static struct pomp_loop *pomp_loop;
void loop_task(gpointer user_data);


/**
 */
static void server_event_cb(struct pomp_ctx *ctx, enum pomp_event event,
                struct pomp_conn *conn, const struct pomp_msg *msg,
                void *userdata)
{
  uint32_t msgid;
  GstMyFilter *filter = userdata;

  switch(event) {
    case POMP_EVENT_CONNECTED:
      g_info("new client!");
      break;

    case POMP_EVENT_DISCONNECTED:
      g_info("client disconnected!");
      break;

    case POMP_EVENT_MSG:
      msgid = pomp_msg_get_id(msg);
      if (msgid == BUFFER_PROCESSING_DONE) {
        g_mutex_lock(&filter->usermutex);
        filter->buffer_processed = TRUE;
        g_mutex_unlock(&filter->usermutex);
        g_cond_signal(&filter->usercond);
      }
      g_info("got message id %d", msgid);
      break;

    default:
      g_error("Unknown event: %d", event);
      break;
  }
}

static int server_start(const struct sockaddr *addr, uint32_t addrlen)
{
  int res = 0;

  /* Start listening for incoming connections */
  res = pomp_ctx_listen(pomp_ctx, addr, addrlen);
  if (res < 0)
    g_error("pomp_ctx_listen : err=%d(%s)", res, strerror(-res));

  return res;
}

void loop_task(gpointer user_data)
{
  while (1);
    pomp_loop_wait_and_process(pomp_loop, -1);

  if (!gst_task_stop(task))
    g_critical("could not stop loop_task");
}

static GstFlowReturn
gst_my_filter_transform_frame_ip (GstVideoFilter *trans, GstVideoFrame *frame)
{
  struct pomp_conn *client = NULL;
  unsigned int bufsize;
  int fd;
  GstBuffer *buf = frame->buffer;
  GstMyFilter *filter = GST_MYFILTER (trans);

  g_assert (1 == gst_buffer_n_memory (buf));
  GstMemory *mem = gst_buffer_peek_memory (buf, 0);
  g_assert (gst_is_dmabuf_memory (mem));
  fd = gst_dmabuf_memory_get_fd (mem);
  bufsize = gst_buffer_get_size (buf);

  while ((client = pomp_ctx_get_next_conn(pomp_ctx, client)) != NULL) {
    gint64 end_time;
    g_mutex_lock(&filter->usermutex);
    filter->buffer_processed = FALSE;
    pomp_conn_send(client, SEND_FD, "%x %u %u %u %u", fd, bufsize, frame->info.finfo->format, frame->info.width, frame->info.height);
    end_time = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
    while (!filter->buffer_processed) {
      if (!g_cond_wait_until (&filter->usercond, &filter->usermutex, end_time)) {
        g_warning("client took too long to process video buffer");
        break;
      }
    }
    g_mutex_unlock (&filter->usermutex);
    g_info("looping on clients");
  }
  g_info("end of loop");

  return GST_FLOW_OK;
}


/* GObject vmethod implementations */

/* initialize the myfilter's class */
static void
gst_my_filter_class_init (GstMyFilterClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstVideoFilterClass *gstvideofilter_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstvideofilter_class = (GstVideoFilterClass *) klass;

  gobject_class->set_property = gst_my_filter_set_property;
  gobject_class->get_property = gst_my_filter_get_property;
  gstvideofilter_class->transform_frame_ip = gst_my_filter_transform_frame_ip;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));

  gst_element_class_set_details_simple(gstelement_class,
    "MyFilter",
    "FIXME:Generic",
    "FIXME:Generic Template Element",
    "Yann Sionneau <<yann.sionneau@parrot.com>>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_my_filter_init (GstMyFilter * filter)
{
  struct sockaddr_storage addr_storage;
  struct sockaddr *addr = NULL;
  uint32_t addrlen = 0;

  pomp_ctx = pomp_ctx_new(server_event_cb, filter);
  pomp_loop = pomp_ctx_get_loop(pomp_ctx);
  memset(&addr_storage, 0, sizeof(addr_storage));
  addr = (struct sockaddr *)&addr_storage;
  addrlen = sizeof(addr_storage);
  pomp_addr_parse(LIBMUTA_IMAGE_SOCKET, addr, &addrlen);

  server_start(addr, addrlen);
  task = gst_task_new(loop_task, &task, NULL);
  g_mutex_init(&filter->usermutex);
  g_cond_init(&filter->usercond);
  g_rec_mutex_init(&filter->mutex);
  gst_task_set_lock(task, &filter->mutex);
  gst_task_start(task);

  filter->silent = FALSE;
}

static void
gst_my_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMyFilter *filter = GST_MYFILTER (object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_my_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstMyFilter *filter = GST_MYFILTER (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstElement vmethod implementations */

/* this function handles sink events */
static gboolean
gst_my_filter_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstMyFilter *filter;
  gboolean ret;

  filter = GST_MYFILTER (parent);

  GST_LOG_OBJECT (filter, "Received %s event: %" GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps * caps;

      gst_event_parse_caps (event, &caps);
      /* do something with the caps */

      /* and forward */
      ret = gst_pad_event_default (pad, parent, event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }
  return ret;
}



/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
myfilter_init (GstPlugin * myfilter)
{
  /* debug category for filtering log messages
   *
   * exchange the string 'Template myfilter' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_my_filter_debug, "myfilter",
      0, "Template myfilter");


  return gst_element_register (myfilter, "myfilter", GST_RANK_NONE,
      GST_TYPE_MYFILTER);
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "myfirstmyfilter"
#endif

/* gstreamer looks for this structure to register myfilters
 *
 * exchange the string 'Template myfilter' with your myfilter description
 */
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    myfilter,
    "Template myfilter",
    myfilter_init,
    "1.0.0",
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)
