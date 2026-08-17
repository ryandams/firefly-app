/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Application-wide definitions. Mirrors the role of emd.h in
 * vestfrost-level3-imx-app.
 */
#ifndef _FIREFLY_H_
#define _FIREFLY_H_

#ifdef __cplusplus
extern "C"
{
#endif

/* The application version lives in ONE place: the VERSION file at the repo
 * root. Zephyr parses it (cmake/modules/version.cmake) and generates
 * app_version.h with APP_VERSION_STRING, so nothing here can drift out of
 * sync with what MCUboot stamps into the image header.
 *
 * The fallback covers builds with no VERSION file - the test applications
 * under tests/, for instance.
 */
#if defined(__has_include)
#if __has_include(<zephyr/app_version.h>)
#include <zephyr/app_version.h>
#define FIREFLY_SW_APP_VERSION APP_VERSION_STRING
#elif __has_include(<app_version.h>)
#include <app_version.h>
#define FIREFLY_SW_APP_VERSION APP_VERSION_STRING
#endif
#endif

#ifndef FIREFLY_SW_APP_VERSION
#define FIREFLY_SW_APP_VERSION "0.1.0-nover"
#endif

#define FIREFLY_CONFIG_PRODUCTION 0
#define FIREFLY_CONFIG_DEVELOPMENT 1
#define FIREFLY_CONFIG_VERSION FIREFLY_CONFIG_DEVELOPMENT

#define FIREFLY_MINUTE (60)
#define FIREFLY_HOUR (60 * FIREFLY_MINUTE)

#ifdef __cplusplus
}
#endif
#endif /* _FIREFLY_H_ */
