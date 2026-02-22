# QIF Import Support

## Goal
Allow users to import transactions from `.qif` files through the existing import
dialog, with duplicate prevention matching current CSV behavior.

## Plan
1. Extend the import parser to auto-detect QIF files and parse transaction
   records (`D`, `T`, `P`, `M`, terminated by `^`) into existing import rows.
2. Reuse existing date/amount normalization and dedup import paths so QIF
   imports use the same DB insertion and duplicate checks as CSV imports.
3. Update the import dialog copy from CSV-only language to generic file import
   language and preselect the destination account using QIF account metadata
   when available.

## Notes
- Current QIF support expects one account per file and surfaces an error when a
  file contains multiple transaction accounts.
- The account-selection flow is reused for QIF imports, including for credit
  card QIF data.
