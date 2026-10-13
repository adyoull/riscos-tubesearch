/* TubeSearch: browse YouTube on RISC OS and play videos in Reel. main.c only
   polls; ui.c does the rest. Part of TubeSearch. MIT licence. */
#include <kernel.h>
#include <swis.h>
#include <unixlib/local.h>
#include "ui.h"

/* Every file name here is a RISC OS one (<Wimp$ScrapDir>.x, Choices...):
   UnixLib is not to translate them. */
int __riscosify_control = __RISCOSIFY_NO_PROCESS;

/* The heap in a dynamic area of its own. It needs about 40 MB at most
   (150 pictures of 230K, the lists), and the area reserves its whole
   maximum of logical address space while TubeSearch runs, so it asks for
   64 MB, not more. If it ever ran out, UnixLib (5.0.3.x) would add a
   "TubeSearch Heap 2" rather than fail. */
const char *const __dynamic_da_name = "TubeSearch Heap";
int __dynamic_da_max_size = 64 << 20;

int main (void)
{
  int block[64], reason = 0;
  if (ui_init ())
    {
      ui_close ();
      return 1;
    }
  for (;;)
    {
      /* null events only while something is being fetched; then the
         Wimp is asked to come back within a centisecond */
      if (ui_busy ())
        {
          int t = 0;
          _swix (OS_ReadMonotonicTime, _OUT (0), &t);
          if (_swix (Wimp_PollIdle, _INR (0, 2) | _OUT (0), 0x3830, block, t + 1, &reason))
            reason = 0;
        }
      else if (_swix (Wimp_Poll, _INR (0, 1) | _OUT (0), 0x3831, block, &reason))
        continue;
      if (ui_event (reason, block))
        break;
    }
  ui_close ();
  return 0;
}
