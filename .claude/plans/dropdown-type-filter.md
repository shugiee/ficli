# Dropdown Type Filtering

## Goal
Allow users to narrow dropdown selections by typing while a dropdown is open,
without rendering the typed filter text in the UI.

## Plan
1. Add hidden dropdown filter state to transaction form state.
2. Add case-insensitive matching over dropdown item labels for account and
   category dropdowns.
3. Handle printable characters and backspace in open-dropdown input loops to
   update selection based on best matching item.
4. Reset the hidden filter buffer when dropdowns are opened/closed.

## Notes
- Filtering is implemented for both the full transaction form dropdown and the
  category-only quick edit dropdown.
- In the category-only dropdown, existing `j/k` navigation remains available.
