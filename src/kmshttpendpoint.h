/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */
#ifndef _KMS_HTTP_ENDPOINT_H_
#define _KMS_HTTP_ENDPOINT_H

#include "kmselement.h"

G_BEGIN_DECLS
#define KMS_TYPE_HTTP_ENDPOINT \
  (kms_http_endpoint_get_type())
#define KMS_HTTP_ENDPOINT(obj) (           \
  G_TYPE_CHECK_INSTANCE_CAST(              \
    (obj),                                 \
    KMS_TYPE_HTTP_ENDPOINT,                \
    KmsHttpEndpoint                        \
  )                                        \
)
#define KMS_HTTP_ENDPOINT_CLASS(klass) (   \
  G_TYPE_CHECK_CLASS_CAST (                \
    (klass),                               \
    KMS_TYPE_HTTP_ENDPOINT,                \
    KmsHttpEndpointClass                   \
  )                                        \
)
#define KMS_IS_HTTP_ENDPOINT(obj) (        \
  G_TYPE_CHECK_INSTANCE_TYPE (             \
    (obj),                                 \
    KMS_TYPE_HTTP_ENDPOINT                 \
  )                                        \
)
#define KMS_IS_HTTP_ENDPOINT_CLASS(klass) (  \
  G_TYPE_CHECK_CLASS_TYPE(                   \
    (klass),                                 \
    KMS_TYPE_HTTP_ENDPOINT                   \
  )                                          \
)
typedef struct _KmsHttpEndpoint KmsHttpEndpoint;
typedef struct _KmsHttpEndpointClass KmsHttpEndpointClass;
typedef struct _KmsHttpEndpointPrivate KmsHttpEndpointPrivate;

struct _KmsHttpEndpoint
{
  KmsElement parent;

  /*< private > */
  KmsHttpEndpointPrivate *priv;
};

struct _KmsHttpEndpointClass
{
  KmsElementClass parent_class;

  /* signals */
  void (*eos_signal) (KmsHttpEndpoint * self);
  GstFlowReturn (*new_sample) (KmsHttpEndpoint *appsink);

  /* actions */
  GstFlowReturn (*push_buffer) (KmsHttpEndpoint * self, GstBuffer * buffer);
  GstSample * (*pull_sample) (KmsHttpEndpoint * self);
  GstFlowReturn (*end_of_stream) (KmsHttpEndpoint * self);
};

GType kms_http_endpoint_get_type (void);

gboolean kms_http_endpoint_plugin_init (GstPlugin * plugin);

G_END_DECLS
#endif /* _KMS_HTTP_ENDPOINT_H */
