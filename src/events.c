/* Copyright (C) 2000 Shawn Betts
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307 USA */

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysymdef.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#include "ratpoison.h"

extern Display *dpy;

void
spawn(char *prog)
{
  /*
   * ugly dance to avoid leaving zombies.  Could use SIGCHLD,
   * but it's not very portable.
   */
  if (fork() == 0) {
    if (fork() == 0) {
      putenv(DisplayString(dpy));
      execlp(prog, prog, 0);
      fprintf (stderr, "exec %s ", prog);
      perror(" failed");
      exit(EXIT_FAILURE);
    }
    exit(0);
  }
  wait((int *) 0);
  PRINT_DEBUG ("spawned %s\n", prog);
}

void
new_window (XCreateWindowEvent *e)
{
  rp_window *win;
  screen_info *s;

  if (e->override_redirect) return;

  s = find_screen (e->parent);
  win = find_window (e->window);

  if (s && !win && e->window != s->key_window && e->window != s->bar_window 
      && e->window != s->input_window)
    {
      win = add_to_window_list (s, e->window);
      win->state = STATE_UNMAPPED;
    }
}

void 
unmap_notify (XEvent *ev)
{
  screen_info *s;
  rp_window *win;

  s = find_screen (ev->xunmap.event);
  win = find_window (ev->xunmap.window);

  if (s && win)
    {
      /* Give back the window number. the window will get another one,
         if it in remapped. */
      return_window_number (win->number);
      win->number = -1;
      win->state = STATE_UNMAPPED;
      update_window_names (s);
    }
}

void 
map_request (XEvent *ev)
{
  screen_info *s;
  rp_window *win;

  s = find_screen (ev->xmap.event);
  win = find_window (ev->xmap.window);

  if (s && win) 
    {
      switch (win->state)
	{
	case STATE_UNMAPPED:
	  manage (win, s);
	case STATE_MAPPED:
	  XMapRaised (dpy, win->w);
	  rp_current_window = win;
	  set_active_window (rp_current_window);
	}
    }
  else
    {
      PRINT_DEBUG ("Not managed.\n");
      XMapWindow (dpy, ev->xmap.window);
    }
}

int
more_destroy_events ()
{
  XEvent ev;

  if (XCheckTypedEvent (dpy, DestroyNotify, &ev))
    {
      XPutBackEvent (dpy, &ev);
      return 1;
    }
  return 0;
}

void
destroy_window (XDestroyWindowEvent *ev)
{
  /* if there are multiple destroy events queued, and a mapped window
     is deleted then switch_window_pending is set to 1 and the window
     switch is done after all destroy events have been done. */
  static int switch_window_pending = 0; 
  int last_destroy_event;
  rp_window *win;

  win = find_window (ev->window);

  last_destroy_event = !more_destroy_events();
  if (win)
    {
      /* Goto the last accessed window. */
      if (win == rp_current_window) 
	{
	  PRINT_DEBUG ("Destroying current window.\n");
	  
	  /* tell ratpoison to switch to the last window when all the
             destroy events have been delt with. */
	  switch_window_pending = 1;
	  unmanage (win);
	}
      else
	{
	  PRINT_DEBUG ("Destroying some other window.\n");
	  unmanage (win);
	}
    }

  if (last_destroy_event && switch_window_pending)
    {
      last_window ();
      switch_window_pending = 0;
    }
}

void
configure_request (XConfigureRequestEvent *e)
{
  XConfigureEvent ce;
  rp_window *win;

  win = find_window (e->window);

  if (win)
    {
      ce.type = ConfigureNotify;
      ce.event = e->window;
      ce.window = e->window;
      ce.x = 0;
      ce.y = 0;
      ce.width = win->scr->root_attr.width;
      ce.height = win->scr->root_attr.height;
      ce.border_width = 0;      
      ce.above = None;
      ce.override_redirect = 0;

      if (e->value_mask & CWStackMode && win->state == STATE_MAPPED) 
	{
	  if (e->detail == Above)
	    {
	      rp_current_window = win;
	      set_active_window (rp_current_window);
	    }
	  else if (e->detail == Below && win == rp_current_window) 
	    {
	      last_window ();
	    }
	}

      XSendEvent(dpy, win->w, False, StructureNotifyMask, (XEvent*)&ce);
    }
}

void
delete_window ()
{
  XEvent ev;
  int status;

  if (rp_current_window == NULL) return;

  ev.xclient.type = ClientMessage;
  ev.xclient.window = rp_current_window->w;
  ev.xclient.message_type = wm_protocols;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = wm_delete;
  ev.xclient.data.l[1] = CurrentTime;

  status = XSendEvent(dpy, rp_current_window->w, False, 0, &ev);
  if (status == 0) fprintf(stderr, "ratpoison: delete window failed\n");
}

void 
kill_window ()
{
  if (rp_current_window == NULL) return;

  XKillClient(dpy, rp_current_window->w);
}

static void
client_msg (XClientMessageEvent *ev)
{
  PRINT_DEBUG ("Recieved client message.\n");

  if (ev->message_type == rp_restart)
    {
      PRINT_DEBUG ("Restarting\n");
      clean_up (); 
      execvp(myargv[0], myargv);
    }
  else if (ev->message_type == rp_kill)
    {
      PRINT_DEBUG ("Exiting\n");
      clean_up ();
      exit (EXIT_SUCCESS);
    }
}

static void
goto_win_by_name (screen_info *s)
{
  char winname[100];
  
  get_input (s, "Window: ", winname, 100);
  PRINT_DEBUG ("user entered: %s\n", winname);

  goto_window_name (winname);
}

static void
handle_key (screen_info *s)
{
  int revert;
  Window fwin;
  XEvent ev;
  int keysym;

  PRINT_DEBUG ("handling key.\n");

  XGetInputFocus (dpy, &fwin, &revert);
  XSetInputFocus (dpy, s->key_window, RevertToPointerRoot, CurrentTime);
  XMaskEvent (dpy, KeyPressMask, &ev);
  XSetInputFocus (dpy, fwin, revert, CurrentTime);

  if (XLookupKeysym((XKeyEvent *) &ev, 0) == KEY_PREFIX && !ev.xkey.state)
    {
      /* Generate the prefix keystroke for the app */
      ev.xkey.window = fwin;
      ev.xkey.state = MODIFIER_PREFIX;
      XSendEvent (dpy, fwin, False, KeyPressMask, &ev);
      XSync (dpy, False);
      return;
    }

  keysym = XLookupKeysym((XKeyEvent *) &ev, 0);

  if (keysym == KEY_TOGGLEBAR)
    {
      toggle_bar (s);
      return;
    }

  /* All functions tested for after this point hide the program bar. */
  hide_bar (s);

  if (keysym >= '0' && keysym <= '9')
    {
      goto_window_number (XLookupKeysym((XKeyEvent *) &ev, 0) - '0');
      hide_bar (s);
      return;
    }

  switch (keysym)
    {
    case KEY_XTERM:
      spawn (TERM_PROG);
      break;
    case KEY_EMACS:
      spawn (EMACS_PROG);
      break;
    case KEY_PREVWINDOW:
      prev_window ();
      break;
    case KEY_NEXTWINDOW:
      next_window ();
      break;
    case KEY_LASTWINDOW:
      last_window ();
      break;
    case KEY_WINBYNAME:
      goto_win_by_name (s);
      break;
    case KEY_RENAME:
      rename_current_window ();
      break;
    case KEY_DELETE:
      if (ev.xkey.state & ShiftMask) kill_window ();
      else delete_window ();
      break;
    default:
      PRINT_DEBUG ("Unknown key command '%c'\n", (char)keysym);
      break;
    }
}

void
key_press (XEvent *ev)
{
  screen_info *s;
  unsigned int modifier = ev->xkey.state;
  int ks = XLookupKeysym((XKeyEvent *) ev, 0);

  s = find_screen (ev->xkey.root);

  if (s && ks == KEY_PREFIX && (modifier & MODIFIER_PREFIX))
    {
      handle_key (s);
    }
}

void
property_notify (XEvent *ev)
{
  rp_window *win;

  PRINT_DEBUG ("atom: %ld\n", ev->xproperty.atom);

  win = find_window (ev->xproperty.window);
  
  if (win)
    {
      if (ev->xproperty.atom == XA_WM_NAME) 
	{
	  PRINT_DEBUG ("updating window name\n");
	  if (update_window_name (win))
	    {
	      update_window_names (win->scr);
	    }
	}
    }
}

/* Given an event, call the correct function to handle it. */
void
delegate_event (XEvent *ev)
{
  switch (ev->type)
    {
    case ConfigureRequest:
      PRINT_DEBUG ("ConfigureRequest\n");
      configure_request (&ev->xconfigurerequest);
      break;
    case CirculateRequest:
      PRINT_DEBUG ("CirculateRequest\n");
      break;
    case CreateNotify:
      PRINT_DEBUG ("CreateNotify\n");
      new_window (&ev->xcreatewindow);
      break;
    case DestroyNotify:
      PRINT_DEBUG ("DestroyNotify\n");
      destroy_window (&ev->xdestroywindow);
      break;
    case ClientMessage:
      PRINT_DEBUG ("ClientMessage\n");
      client_msg (&ev->xclient);
      break;
    case ColormapNotify:
      PRINT_DEBUG ("ColormapNotify\n");
      break;
    case PropertyNotify:
      PRINT_DEBUG ("PropertyNotify\n");
      property_notify (ev);
      break;
    case SelectionClear:
      PRINT_DEBUG ("SelectionClear\n");
      break;
    case SelectionNotify:
      PRINT_DEBUG ("SelectionNotify\n");
      break;
    case SelectionRequest:
      PRINT_DEBUG ("SelectionRequest\n");
      break;
    case EnterNotify:
      PRINT_DEBUG ("EnterNotify\n");
      break;
    case ReparentNotify:
      PRINT_DEBUG ("ReparentNotify\n");
      break;
    case FocusIn:
      PRINT_DEBUG ("FocusIn\n");
      break;

    case MapRequest:
      PRINT_DEBUG ("MapRequest\n");
      map_request (ev);
      break;

    case KeyPress:
      PRINT_DEBUG ("KeyPress\n");
      key_press (ev);
      break;
      
    case UnmapNotify:
      PRINT_DEBUG ("UnmapNotify\n");
      unmap_notify (ev);
      break;

    case MotionNotify:
      PRINT_DEBUG ("MotionNotify\n");
      break;
    case Expose:
      PRINT_DEBUG ("Expose\n");
      break;
    case FocusOut:
      PRINT_DEBUG ("FocusOut\n");
      break;
    case ConfigureNotify:
      PRINT_DEBUG ("ConfigureNotify\n");
      break;
    case MapNotify:
      PRINT_DEBUG ("MapNotify\n");
      break;
    case MappingNotify:
      PRINT_DEBUG ("MappingNotify\n");
      break;
    default:
      PRINT_DEBUG ("Unhandled event %d\n", ev->type);
    }
}

void
handle_events ()
{
  XEvent ev;

  for (;;) 
    {
      XNextEvent (dpy, &ev);
      delegate_event (&ev);
    }
}

