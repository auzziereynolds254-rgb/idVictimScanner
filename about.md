# Level ID Scanner

Counts backwards from a starting level ID and prints every level whose name contains your keyword.

## Usage
1. Click the **ID Scan** button on the main menu.
2. Enter the **Starting ID** (e.g. `151067814`) and **Keywords** (e.g. `silent`). Capitalization doesn't matter. Separate several keywords with commas.
3. Press **Start**. A console window opens and prints:
   `LEVEL FOUND MY BRUDA!!!! : <id>, <name>`
   for every match. Matches are also saved to `found_levels.txt` in the mod's save folder.
4. Press **ID Scan** again, then **Stop**, to cancel.

## Settings
- **Requests per second**: how fast the scanner sends lookups. Each request checks one ID. Going much above 1.0 risks a rate-limit block from the game's servers, and can also make real levels come back empty without any error.
- **Only show unlisted levels**: when on, publicly listed levels are ignored and only unlisted ones that match your keyword are printed.

## Notes
- Each request checks one ID. Deleted and private levels return nothing, so they can't match. Unlisted levels only show up if **Only show unlisted levels** is on.
- Matches are appended to `found_levels.txt`, not overwritten, so results from earlier scans are kept.
