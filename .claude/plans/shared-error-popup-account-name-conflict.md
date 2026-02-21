# Shared Error Popup For Duplicate Account Names

## Goal

Add a reusable error popup near the top of the UI with a red outline, and use it
when account creation fails due to an existing account name.

## Plan

1. Add a shared UI helper (`ui_show_error_popup`) that renders a centered
   top-aligned modal error popup and blocks for a keypress.
2. Add a dedicated error color pair for red-outline rendering.
3. In account creation flow, detect SQLite unique-constraint failures and show
   the popup with a duplicate-name message.
4. Keep all non-conflict account-creation errors on the existing inline
   message path.
