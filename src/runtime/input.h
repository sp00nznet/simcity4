/* Scripted input posted to the game's window (input.c). */
#pragma once

/* --click/--rclick X,Y@S, --key NAME@S: argv entries consumed, 0 if not ours. */
int input_arg(int argc, char** argv, int i);
void input_help(void);

/* The window the game draws in (GDriverWindow--*). */
void input_on_window(void* hwnd);

/* The main loop began (framework phase 7): the script's clock starts. */
void input_main_loop(void);

/* While a scripted pointer is set: its screen position, 1; else 0. */
int input_cursor(long* x, long* y);

/* GetAsyncKeyState/GetKeyState for vk: 0x8000 or 0 while the script holds
 * input, -1 to ask Windows. */
int input_key_state(int vk);

/* Start the script thread (no-op with no events). */
void input_start(void);
