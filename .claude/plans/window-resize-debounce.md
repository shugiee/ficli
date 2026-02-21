# Window Resize Debounce And Auto-Apply

## Goal

Handle terminal resize events without requiring extra user keypresses, and
reduce redraw churn during rapid resize bursts.

## Plan

1. Add shared resize-event propagation helper so modal/popout loops can
   requeue `KEY_RESIZE` back to the main loop before exiting.
2. Update all blocking `wgetch` modal loops (forms, import flow, help, confirms,
   error popup) to propagate resize events instead of consuming them.
3. Add top-level resize debounce in `ui.c` by briefly draining repeated
   `KEY_RESIZE` events before rebuilding layout.
4. Keep redraw centralized in the main layout handler so all screens reflow in
   one path.
