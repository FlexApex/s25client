#!/bin/sh

# Copyright (C) 2005 - 2021 Settlers Freaks <sf-team at siedler25.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

# avoids black screen in Qubes
export SDL_VIDEO_X11_FORCE_EGL=1
export LIBGL_ALWAYS_SOFTWARE=1

cd build && exec ./start.sh "$@"
