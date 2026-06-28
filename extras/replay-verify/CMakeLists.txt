# Copyright (C) 2005 - 2026 Settlers Freaks <sf-team at siedler25.org>
#
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(replay-verify main.cpp)
target_link_libraries(replay-verify PRIVATE s25Main Boost::nowide)

if(WIN32)
    include(GatherDll)
    gather_dll_copy(replay-verify)
endif()
