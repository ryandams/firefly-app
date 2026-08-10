/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Application-wide definitions. Mirrors the role of emd.h in
 * vestfrost-level3-imx-app.
 */
#ifndef _FIREFLY_H_
#define _FIREFLY_H_

#ifdef __cplusplus
extern "C" {
#endif

#define FIREFLY_SW_APP_VERSION "v0.1.0"

#define FIREFLY_CONFIG_PRODUCTION  0
#define FIREFLY_CONFIG_DEVELOPMENT 1
#define FIREFLY_CONFIG_VERSION     FIREFLY_CONFIG_DEVELOPMENT

#define FIREFLY_MINUTE (60)
#define FIREFLY_HOUR   (60 * FIREFLY_MINUTE)

#ifdef __cplusplus
}
#endif
#endif /* _FIREFLY_H_ */
