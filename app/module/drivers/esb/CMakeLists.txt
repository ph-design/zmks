# Copyright (c) 2026 Team PHDesign
# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_ZMK_ESB esb.c)
