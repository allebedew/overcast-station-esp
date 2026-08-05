#pragma once

/* The graphical panel as the rest of the firmware sees it: one call that brings
 * up the transport and puts a frame on it. Owns the canvas — it is 8 KB, and
 * exactly one of them exists.
 *
 * Firmware-only, unlike the drawing layers below it: the host simulator renders
 * the same scenes to PNG and never links a transport. */

void gui_init(void);
