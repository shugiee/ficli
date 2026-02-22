# Import Categories From QIF and CSV

## Goal
Carry category information from imported files into transactions, including
prompting for unknown QIF categories so users can decide how to map them.

## Plan
1. Extend import row data to keep source category text, whether a category was
   explicitly provided, and the resolved destination category id.
2. Parse category fields from CSV (`Category`/`Transaction Category`) and QIF
   (`L` lines), while skipping QIF transfer-account markers (`[Account]`).
3. Resolve known categories before import for both CSV and QIF. For unknown QIF
   categories, prompt user to create, assign to an existing category, or leave
   uncategorized.
4. During import, apply resolved category id when a source category was present;
   otherwise keep existing payee-based auto-categorization behavior.

## Notes
- Unknown CSV categories are currently treated as explicit uncategorized unless
  they already match an existing category.
