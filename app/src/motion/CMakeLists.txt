# SPDX-License-Identifier: MIT

target_sources(app PRIVATE motion.c ../events/motion_live_state_changed.c)
target_sources_ifdef(CONFIG_ZMK_MOTION_SAMPLER app PRIVATE motion_sample.c)
target_sources_ifdef(CONFIG_ZMK_MOTION_HARDWARE_TAP app PRIVATE motion_tap.c)
