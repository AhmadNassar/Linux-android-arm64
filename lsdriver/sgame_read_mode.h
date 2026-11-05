/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SGAME_READ_MODE_H
#define SGAME_READ_MODE_H
#ifndef SGAME_READ_NC
#define SGAME_READ_NC 0
#endif
#if SGAME_READ_NC
#define SGAME_READ_MODE_NAME "nc"
#else
#define SGAME_READ_MODE_NAME "cached"
#endif
#endif
