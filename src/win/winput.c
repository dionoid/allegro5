/*         ______   ___    ___ 
 *        /\  _  \ /\_ \  /\_ \ 
 *        \ \ \L\ \\//\ \ \//\ \      __     __   _ __   ___ 
 *         \ \  __ \ \ \ \  \ \ \   /'__`\ /'_ `\/\`'__\/ __`\
 *          \ \ \/\ \ \_\ \_ \_\ \_/\  __//\ \L\ \ \ \//\ \L\ \
 *           \ \_\ \_\/\____\/\____\ \____\ \____ \ \_\\ \____/
 *            \/_/\/_/\/____/\/____/\/____/\/___L\ \/_/ \/___/
 *                                           /\____/
 *                                           \_/__/
 *
 *      Input management functions.
 *
 *      By Eric Botcazou.
 *
 *      Rewritten to work with multiple A5 windows by Milan Mimica.
 *
 *      See readme.txt for copyright information.
 */


/* For APC */
#define _WIN32_WINNT 0x400


#include "allegro5/allegro5.h"
#include "allegro5/internal/aintern.h"
#include "allegro5/platform/aintwin.h"

#ifndef SCAN_DEPEND
   #include <process.h>
#endif


#ifndef ALLEGRO_WINDOWS
   #error something is wrong with the makefile
#endif

#define PREFIX_I                "al-winput INFO: "
#define PREFIX_W                "al-winput WARNING: "
#define PREFIX_E                "al-winput ERROR: "


#define MAX_EVENTS 20

/* events in event queue */
static int _win_input_events;
/* input thread event queue */
static HANDLE _win_input_event_id[MAX_EVENTS];
/* event handler */
static void *_win_input_event_handler_param[MAX_EVENTS];
static void (*_win_input_event_handler[MAX_EVENTS])(void *);

/* handle of the input thread */
static HANDLE input_thread = NULL;

/* internal input thread management */
static HANDLE ack_event = NULL;

static volatile bool input_thread_is_over = false;

typedef struct KEY_EVENT_INFO {
   HANDLE id;
   void *param;
   void (*handler)(void *);
} KEY_EVENT_INFO;


/* input_proc: [input thread]
 * This the input thread.
 */
static DWORD __stdcall input_proc(void *unused)
{
   DWORD result;
   TRACE(PREFIX_I "Input thread started.\n");

   while (!input_thread_is_over) {
      if (_win_input_events) {
         result = WaitForMultipleObjectsEx(_win_input_events,
                                           _win_input_event_id,
                                           FALSE, INFINITE, TRUE);
      }
      else {
         result = SleepEx(INFINITE, TRUE);
      }

      if (result < (DWORD) WAIT_OBJECT_0 + _win_input_events) {
         /* one of the registered events is in signaled state,
          * call its handler. */
         int index = result - WAIT_OBJECT_0;
         (*_win_input_event_handler[index])(_win_input_event_handler_param[index]);
      }
      else if (result == (DWORD) WAIT_IO_COMPLETION) {
         /* An Asynchronous Procedure Call has been executed.
          * We use APCs to:
          * - add events to the even queue
          * - remove events from even queue
          * - stop the thread
          */
         /* Acknowledge the thread invoking the APC that is finished. */
         SetEvent(ack_event);
      }
      else if (result == WAIT_FAILED) {
         TRACE(PREFIX_E "Input thread waiting failed!\n");
         input_thread_is_over = true;
      }
   }

   TRACE(PREFIX_I "Input thread exiting.\n");

   ExitThread(0);
   return 0;
}


/* quit_input_thread: Asynchronous Procedure Call
 * APC invoked by primary thread, interrupts the input thread, executes and
 * makes the input thread finish.
 */
static VOID CALLBACK quit_input_thread(ULONG_PTR *useless)
{
   input_thread_is_over = true;
   TRACE("input thread quit request\n");
}


/* register_event: Asynchronous Procedure Call
 * APC invoked by primary thread, interrupts the input thread, executes and
 * registers an event and its handler.
 */
static VOID CALLBACK register_event(ULONG_PTR *param)
{
   KEY_EVENT_INFO  *event_info = (KEY_EVENT_INFO*)param;

   /* add the event to the queue */
   _win_input_event_id[_win_input_events] = event_info->id;

   /* set the event handler */
   _win_input_event_handler[_win_input_events] = event_info->handler;
   _win_input_event_handler_param[_win_input_events] = event_info->param;

   /* adjust the size of the queue */
   _win_input_events++;
}



/* unregister_event: Asynchronous Procedure Call
 * APC invoked by primary thread, interrupts the input thread, executes and
 * unregisters an event and its handler.
 */
static VOID CALLBACK unregister_event(ULONG_PTR *param)
{
   int i, found = -1;
   HANDLE event_id = (HANDLE*)param;

   /* look for the pending event in the event queue */
   for (i = 0; i < _win_input_events; i++) {
      if (_win_input_event_id[i] == event_id) {
         found = i;
         break;
      }
   }

   ASSERT(found >= 0);

   if (found >= 0) {
      /* shift the queue to the left */
      for (i = found; i < _win_input_events - 1; i++) {
         _win_input_event_id[i] = _win_input_event_id[i+1];
         _win_input_event_handler[i] = _win_input_event_handler[i+1];
         _win_input_event_handler_param[i] = _win_input_event_handler_param[i+1];
      }

      /* adjust the size of the queue */
      _win_input_events--;
   }
}



/* _win_input_register_event: [primary thread]
 *  Adds an event to the input thread event queue.
 */
bool _win_input_register_event(HANDLE event_id,
                               void (*event_handler)(void*),
                               void *event_handler_param)
{
   KEY_EVENT_INFO event_info;
   event_info.id = event_id;
   event_info.handler = event_handler;
   event_info.param = event_handler_param;

   if (_win_input_events == MAX_EVENTS)
      return false;

   QueueUserAPC((PAPCFUNC)register_event, input_thread, (ULONG_PTR)&event_info);
   /* wait for the input thread to acknowledge */
   WaitForSingleObject(ack_event, INFINITE);

   return true;
}



/* _win_input_unregister_event: [primary thread]
 *  Removes an event from the input thread event queue.
 */
bool _win_input_unregister_event(HANDLE event_id)
{
   QueueUserAPC((PAPCFUNC)unregister_event, input_thread, (ULONG_PTR)event_id);
   /* wait for the input thread to acknowledge */
   WaitForSingleObject(ack_event, INFINITE);

   return true;
}



/* _win_input_init: [primary thread]
 *  Initializes the module.
 */
void _win_input_init(void)
{
   input_thread = CreateThread(NULL, 0, input_proc, NULL, 0, NULL);
   if (!input_thread) {
      TRACE(PREFIX_E "Failed to spawn the input thread.\n");
   }

   ack_event = CreateEvent(NULL, FALSE, FALSE, NULL);
}



/* _win_input_exit: [primary thread]
 *  Shuts down the module.
 */
void _win_input_exit(void)
{
   if (input_thread) {
      QueueUserAPC((PAPCFUNC)quit_input_thread, input_thread, (ULONG_PTR)NULL);
      /* wait for the input thread to acknowledge */
      WaitForSingleObject(ack_event, INFINITE);
   }

   _win_input_events = 0;

   if (ack_event)
      CloseHandle(ack_event);
}

