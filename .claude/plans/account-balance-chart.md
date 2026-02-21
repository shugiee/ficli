# Show Chart of Account Balance Over Time

## Summary

Add a read-only 90-day balance trend chart to the top of the Transactions view for the currently selected account.

## Implementation

1. Add a DB query API that returns contiguous daily balance points for an account over a lookback window ending today.
2. Load the balance series in the transactions list reload path alongside existing summary stats.
3. Render a compact ASCII chart between the summary line and filter/header rows.
4. Shift transaction table rows down when the chart is visible.
5. Hide the chart on small terminals and show a one-line fallback message.

## Notes

- No schema migration is required.
- The chart is read-only in v1 (no chart-specific keybindings).
