/*
 * pommed - Apple laptops hotkeys handler daemon
 *
 * $Id$
 *
 * Copyright (C) 2006-2007 Julien BLACHE <jb@jblache.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include <syslog.h>

#include <errno.h>

#include <sys/epoll.h>
#include <sys/inotify.h>

#include <linux/input.h>

#include "pommed.h"
#include "conffile.h"
#include "evdev.h"
#include "kbd_backlight.h"
#include "lcd_backlight.h"
#include "cd_eject.h"
#include "audio.h"
#include "beep.h"


#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)    ((array[LONG(bit)] >> OFF(bit)) & 1)

/* Added to linux/input.h after Linux 2.6.18 */
#ifndef BUS_VIRTUAL
# define BUS_VIRTUAL 0x06
#endif


/* Open event devices */
static int ev_fds[EVDEV_MAX];

/* epoll fd */
static int epfd;

/* inotify fd */
static int ifd;


static int
evdev_try_add(int fd);


static int
evdev_add(int fd)
{
  int i;
  int ret;

  struct epoll_event epoll_ev;

  for (i = 0; i < EVDEV_MAX; i++)
    {
      if (ev_fds[i] == -1)
	{
	  ev_fds[i] = fd;

	  break;
	}
    }

  epoll_ev.events = EPOLLIN;
  epoll_ev.data.fd = fd;

  ret = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &epoll_ev);

  if (ret < 0)
    {
      logmsg(LOG_ERR, "Could not add device to epoll: %s", strerror(errno));

      return -1;
    }

  return 0;
}

static void
evdev_remove(int fd)
{
  int i;
  int ret;

  for (i = 0; i < EVDEV_MAX; i++)
    {
      if (ev_fds[i] == fd)
	{
	  ev_fds[i] = -1;

	  break;
	}
    }

  ret = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);

  if (ret < 0)
    logmsg(LOG_ERR, "Could not remove device from epoll: %s", strerror(errno));

  close(fd);
}


static void
evdev_process_events(int fd)
{
  int ret;

  struct input_event ev;

  ret = read(fd, &ev, sizeof(struct input_event));

  if (ret != sizeof(struct input_event))
    return;

  if (ev.type == EV_KEY)
    {
      /* key released - we don't care */
      if (ev.value == 0)
	return;

      /* Reset keyboard backlight idle timer */
      kbd_bck_info.idle = 0;
      kbd_backlight_inhibit_clear(KBD_INHIBIT_IDLE);

      switch (ev.code)
	{
	  case K_LCD_BCK_DOWN:
	    logdebug("\nKEY: LCD backlight down\n");

	    mops->lcd_backlight_step(STEP_DOWN);
	    break;

	  case K_LCD_BCK_UP:
	    logdebug("\nKEY: LCD backlight up\n");

	    mops->lcd_backlight_step(STEP_UP);
	    break;

	  case K_AUDIO_MUTE:
	    logdebug("\nKEY: audio mute\n");

	    audio_toggle_mute();
	    break;

	  case K_AUDIO_DOWN:
	    logdebug("\nKEY: audio down\n");

	    audio_step(STEP_DOWN);
	    break;

	  case K_AUDIO_UP:
	    logdebug("\nKEY: audio up\n");

	    audio_step(STEP_UP);
	    break;

	  case K_VIDEO_TOGGLE:
	    logdebug("\nKEY: video toggle\n");
	    break;

	  case K_KBD_BCK_OFF:
	    logdebug("\nKEY: keyboard backlight off\n");

	    if (!has_kbd_backlight())
	      break;

	    if (kbd_cfg.auto_on)
	      kbd_backlight_inhibit_toggle(KBD_INHIBIT_USER);
	    else
	      kbd_backlight_toggle();
	    break;

	  case K_KBD_BCK_DOWN:
	    logdebug("\nKEY: keyboard backlight down\n");

	    if (!has_kbd_backlight())
	      break;

	    kbd_backlight_step(STEP_DOWN);
	    break;

	  case K_KBD_BCK_UP:
	    logdebug("\nKEY: keyboard backlight up\n");

	    if (!has_kbd_backlight())
	      break;

	    kbd_backlight_step(STEP_UP);
	    break;

	  case K_CD_EJECT:
	    logdebug("\nKEY: CD eject\n");

	    cd_eject();
	    break;

	  case K_IR_FFWD:
	    logdebug("\nKEY: IR fast forward\n");
	    break;

	  case K_IR_REWD:
	    logdebug("\nKEY: IR rewind\n");
	    break;

	  case K_IR_PLAY:
	    logdebug("\nKEY: IR play/pause\n");
	    break;

	  case K_IR_MENU:
	    logdebug("\nKEY: IR menu\n");
	    break;

	  default:
#if 0
	    logdebug("\nKEY: %x\n", ev.code);
#endif /* 0 */
	    break;
	}
    }
  else if (ev.type == EV_SND)
    {
      /* Beeper device */
      if ((ev.code == SND_TONE) && (ev.value > 0))
	{
	  logdebug("\nBEEP: BEEP!\n");

	  beep_beep(); /* Catch that, Coyote */
	}
    }
  else if (ev.type == EV_SW)
    {
      /* Lid switch */
      if (ev.code == SW_LID)
	{
	  if (ev.value)
	    {
	      logdebug("\nLID: closed\n");

	      kbd_backlight_inhibit_set(KBD_INHIBIT_LID);
	    }
	  else
	    {
	      logdebug("\nLID: open\n");

	      kbd_backlight_inhibit_clear(KBD_INHIBIT_LID);
	    }
	}
    }
}


static void
evdev_inotify_process(void)
{
  int ret;
  int fd;

  struct inotify_event ie[2];
  char evdev[32];

  ret = read(ifd, ie, sizeof(ie));
  if (ret < 0)
    {
      logdebug("inotify read failed: %s\n", strerror(errno));

      return;
    }

  /* ie[0] contains the inotify event information
   * the memory space for ie[1] contains the name of the file
   * see the inotify documentation
   */

  if ((ie[0].len == 0) || (ie[0].name == NULL))
    {
      logdebug("inotify event with no name\n");

      return;
    }

  logdebug("Found new event device %s/%s\n", EVDEV_DIR, ie[0].name);

  if (strncmp("event", ie[0].name, 5))
    {
      logdebug("Discarding %s/%s\n", EVDEV_DIR, ie[0].name);

      return;
    }

  ret = snprintf(evdev, 32, "%s/%s", EVDEV_DIR, ie[0].name);

  if ((ret <= 0) || (ret > 31))
    return;

  fd = open(evdev, O_RDWR);
  if (fd < 0)
    {
      if (errno != ENOENT)
	logmsg(LOG_WARNING, "Could not open %s: %s", evdev, strerror(errno));
    }

  evdev_try_add(fd);
}


int
evdev_event_loop(void)
{
  int i;
  int nfds;

  int inotify = 0;

  struct epoll_event events[MAX_EPOLL_EVENTS];

  nfds = epoll_wait(epfd, events, MAX_EPOLL_EVENTS, LOOP_TIMEOUT);

  if (nfds < 0)
    {
      if (errno == EINTR)
	return 1; /* pommed.c will go on with the events management */
      else
	{
	  logmsg(LOG_ERR, "epoll_wait() error: %s", strerror(errno));

	  return -1; /* pommed.c will exit */
	}
    }


  for (i = 0; i < nfds; i++)
    {
      /* some of the event devices cease to exist when suspending */
      if (events[i].events & (EPOLLERR | EPOLLHUP))
	{
	  logmsg(LOG_INFO, "Error condition signaled on event device");

	  if (events[i].data.fd == beep_info.fd)
	    logmsg(LOG_WARNING, "Beeper device lost; this should not happen");

	  if (events[i].data.fd == ifd)
	    {
	      logmsg(LOG_WARNING, "inotify fd lost; this should not happen");
	      ifd = -1;
	    }

	  evdev_remove(events[i].data.fd);

	  continue;
	}

      if (events[i].events & EPOLLIN)
	{
	  if (events[i].data.fd == ifd)
	    inotify = 1; /* defer inotify events processing */
	  else
	    evdev_process_events(events[i].data.fd);
	}
    }

  if (inotify)
    evdev_inotify_process();

  return nfds;
}


#ifdef __powerpc__
/* PowerBook G4 Titanium */
int
evdev_is_adb(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_ADB)
    return 0;

  if (id[ID_VENDOR] != 0x0001)
    return 0;

  if (product == ADB_PRODUCT_ID_KEYBOARD)
    {
      logdebug(" -> ADB keyboard\n");

      return 1;
    }

  if (product == ADB_PRODUCT_ID_PBBUTTONS)
    {
      logdebug(" -> ADB PowerBook buttons\n");

      return 1;
    }

  return 0;
}

/* PowerBook G4 */
int
evdev_is_fountain(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_USB)
    return 0;

  if (id[ID_VENDOR] != USB_VENDOR_ID_APPLE)
    return 0;

  if ((product == USB_PRODUCT_ID_FOUNTAIN_ANSI)
      || (product == USB_PRODUCT_ID_FOUNTAIN_ISO)
      || (product == USB_PRODUCT_ID_FOUNTAIN_JIS))
    {
      logdebug(" -> Fountain USB keyboard\n");

      return 1;
    }

  return 0;
}

int
evdev_is_geyser(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_USB)
    return 0;

  if (id[ID_VENDOR] != USB_VENDOR_ID_APPLE)
    return 0;

  if ((product == USB_PRODUCT_ID_GEYSER_ANSI)
      || (product == USB_PRODUCT_ID_GEYSER_ISO)
      || (product == USB_PRODUCT_ID_GEYSER_JIS))
    {
      logdebug(" -> Geyser USB keyboard\n");

      kbd_set_fnmode();

      return 1;
    }

  return 0;
}

/* PMU Lid switch */
static int
evdev_is_lidswitch(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_HOST)
    return 0;

  if (id[ID_VENDOR] != 0x0001)
    return 0;

  if (id[ID_VERSION] != 0x0100)
    return 0;

  if (product == 0x0001)
    {
      logdebug(" -> PMU LID switch\n");

      return 1;
    }

  return 0;
}

#else

/* Core Duo MacBook & MacBook Pro */
int
evdev_is_geyser3(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_USB)
    return 0;

  if (id[ID_VENDOR] != USB_VENDOR_ID_APPLE)
    return 0;

  if ((product == USB_PRODUCT_ID_GEYSER3_ANSI)
      || (product == USB_PRODUCT_ID_GEYSER3_ISO)
      || (product == USB_PRODUCT_ID_GEYSER3_JIS))
    {
      logdebug(" -> Geyser III USB keyboard\n");

      kbd_set_fnmode();

      return 1;
    }

  return 0;
}

/* Core2 Duo MacBook & MacBook Pro */
int
evdev_is_geyser4(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_USB)
    return 0;

  if (id[ID_VENDOR] != USB_VENDOR_ID_APPLE)
    return 0;

  if ((product == USB_PRODUCT_ID_GEYSER4_ANSI)
      || (product == USB_PRODUCT_ID_GEYSER4_ISO)
      || (product == USB_PRODUCT_ID_GEYSER4_JIS))
    {
      logdebug(" -> Geyser IV USB keyboard\n");

      kbd_set_fnmode();

      return 1;
    }

  return 0;
}

/* Core2 Duo Santa Rosa MacBook (MacBook3,1) */
int
evdev_is_geyser4hf(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_USB)
    return 0;

  if (id[ID_VENDOR] != USB_VENDOR_ID_APPLE)
    return 0;

  if ((product == USB_PRODUCT_ID_GEYSER4HF_ANSI)
      || (product == USB_PRODUCT_ID_GEYSER4HF_ISO)
      || (product == USB_PRODUCT_ID_GEYSER4HF_JIS))
    {
      logdebug(" -> Geyser IV-HF USB keyboard\n");

      kbd_set_fnmode();

      return 1;
    }

  return 0;
}

/* Apple Remote IR Receiver */
static int
evdev_is_appleir(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_USB)
    return 0;

  if (id[ID_VENDOR] != USB_VENDOR_ID_APPLE)
    return 0;

  if ((product == USB_PRODUCT_ID_APPLEIR)
      || (product == USB_PRODUCT_ID_APPLEIR_2))
    {
      logdebug(" -> Apple IR receiver\n");

      return 1;
    }

  return 0;
}

/* ACPI Lid switch */
static int
evdev_is_lidswitch(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_HOST)
    return 0;

  if (id[ID_VENDOR] != 0)
    return 0;

  if (product == 0x0005)
    {
      logdebug(" -> ACPI LID switch\n");

      return 1;
    }

  return 0;
}
#endif /* !__powerpc__ */

/* Mouseemu virtual keyboard */
static int
evdev_is_mouseemu(unsigned short *id)
{
  unsigned short product = id[ID_PRODUCT];

  if (id[ID_BUS] != BUS_VIRTUAL)
    return 0;

  if (id[ID_VENDOR] != 0x001f)
    return 0;

  if (product == 0x001f)
    {
      logdebug(" -> Mouseemu virtual keyboard\n");

      return 1;
    }

  return 0;
}


static int
evdev_try_add(int fd)
{
  unsigned short id[4];
  unsigned long bit[EV_MAX][NBITS(KEY_MAX)];
  char devname[256];

  int ret;

  devname[0] = '\0';
  ioctl(fd, EVIOCGNAME(sizeof(devname)), devname);

  logdebug("\nInvestigating evdev [%s]\n", devname);

  ioctl(fd, EVIOCGID, id);

  if ((!mops->evdev_identify(id))
#ifndef __powerpc__
      && !(appleir_cfg.enabled && evdev_is_appleir(id))
#endif
      && !(has_kbd_backlight() && evdev_is_lidswitch(id))
      && !(evdev_is_mouseemu(id)))
    {
      logdebug("Discarding evdev: bus 0x%04x, vid 0x%04x, pid 0x%04x\n", id[ID_BUS], id[ID_VENDOR], id[ID_PRODUCT]);

      close(fd);

      return -1;
    }

  memset(bit, 0, sizeof(bit));

  ioctl(fd, EVIOCGBIT(0, EV_MAX), bit[0]);

  if (!test_bit(EV_KEY, bit[0]))
    {
      logdebug("evdev: no EV_KEY event type (not a keyboard)\n");

      if (!test_bit(EV_SW, bit[0]))
	{
	  logdebug("Discarding evdev: no EV_SW event type (not a switch)\n");

	  close(fd);

	  return -1;
	}
    }
  else if (test_bit(EV_ABS, bit[0]))
    {
      logdebug("Discarding evdev with EV_ABS event type (mouse/trackpad)\n");

      close(fd);

      return -1;
    }

  ret = evdev_add(fd);

  return ret;
}


static int
evdev_inotify_init(void)
{
  int ret;
  struct epoll_event epoll_ev;

  ifd = inotify_init();
  if (ifd < 0)
    {
      logmsg(LOG_ERR, "Failed to initialize inotify: %s", strerror(errno));

      return -1;
    }

  ret = inotify_add_watch(ifd, EVDEV_DIR, IN_CREATE | IN_ONLYDIR);
  if (ret < 0)
    {
      logmsg(LOG_ERR, "Failed to add inotify watch for %s: %s", EVDEV_DIR, strerror(errno));

      close(ifd);
      ifd = -1;

      return -1;
    }

  epoll_ev.events = EPOLLIN;
  epoll_ev.data.fd = ifd;

  ret = epoll_ctl(epfd, EPOLL_CTL_ADD, ifd, &epoll_ev);
  if (ret < 0)
    {
      logmsg(LOG_ERR, "Failed to add inotify fd to epoll: %s", strerror(errno));

      close(ifd);

      return -1;
    }

  return 0;
}


int
evdev_init(void)
{
  int ret;
  int i;

  char evdev[32];

  int ndevs;
  int fd;

  epfd = epoll_create(MAX_EPOLL_EVENTS);
  if (epfd < 0)
    {
      logmsg(LOG_ERR, "Could not create epoll fd: %s", strerror(errno));

      return -1;
    }

  for (i = 0; i < EVDEV_MAX; i++)
    {
      ev_fds[i] = -1;
    }

  ndevs = 0;
  for (i = 0; i < EVDEV_MAX; i++)
    {
      ret = snprintf(evdev, 32, "%s%d", EVDEV_BASE, i);

      if ((ret <= 0) || (ret > 31))
	return -1;

      fd = open(evdev, O_RDWR);
      if (fd < 0)
	{
	  if (errno != ENOENT)
	    logmsg(LOG_WARNING, "Could not open %s: %s", evdev, strerror(errno));

	  continue;
	}

      if (evdev_try_add(fd) == 0)
	ndevs++;
    }

  logdebug("\nFound %d devices\n", ndevs);

  /* Add the beeper device */
  ret = beep_open_device();
  if (ret == 0)
    {
      ret = evdev_add(beep_info.fd);

      if (ret == 0)
	  ndevs++;
    }

  /* Initialize inotify */
  evdev_inotify_init();

  return ndevs;
}

void
evdev_cleanup(void)
{
  int i;

  close(epfd);

  close(ifd);

  for (i = 0; i < EVDEV_MAX; i++)
    {
      if (ev_fds[i] == beep_info.fd)
	beep_close_device();
      else
	close(ev_fds[i]);
    }
}
