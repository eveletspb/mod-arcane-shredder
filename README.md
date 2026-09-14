# mod-arcane-shredder

![Arcane Shredder — mass disenchant for AzerothCore](banner.png)

Safe batch disenchanting for AzerothCore WotLK 3.3.5a.

The module creates a server-side preview of items in regular inventory bags, issues a short-lived single-use token, and validates every item again immediately before disenchanting it. Equipped items, bag containers, the bank, keyring, buyback, trade, mail, and auction inventories are never scanned.

## Installation

1. Place this repository in `modules/mod-arcane-shredder`.
2. Rebuild `worldserver`.
3. Copy `conf/mod_arcane_shredder.conf.dist` to the module configuration directory as a `.conf` file and adjust its settings.
4. Install the companion addon from [eveletspb/arcane-sheredder-ui](https://github.com/eveletspb/arcane-sheredder-ui).

No SQL migrations or AzerothCore core changes are required.

## Usage

- `/ashred` opens the addon.
- `.ashred status` displays the server diagnostic status.
- `.ashred` displays command help.

The addon window provides validated filters, safe defaults, a server-generated preview, per-item exclusion, and a preview expiration timer. The red confirmation button always shows the current number of selected items.

Preview, exclude, cancel, and confirm requests are accepted only through AzerothCore's hidden addon-message transport. Visible chat and `CHAT_MSG_SYSTEM` are not used as a machine API.

## Safety model

- The server determines item eligibility.
- The addon can only narrow the server-side filters.
- At most one preview can be active per character.
- A new preview, cancel, confirm, logout, timeout, configuration reload, or module shutdown invalidates the token.
- Confirm consumes the token first, then resolves every item again by GUID.
- Disenchant materials are generated through `LootTemplates_Disenchant`.
- The standard recovery mail flow is used when materials do not fit in the inventory.
- All loot is generated and validated before the destructive boundary.
- Immediately before delivery, the server validates each item's GUID, inventory position, and fingerprint again.
- Materials are stored in the inventory, while stacks that do not fit are passed to the standard recovery mail flow.
- The source item is removed only after all material records have been delivered successfully or handed to the recovery mail helper.

## Verification

```bash
python3 -m unittest discover -s tests -p 'test_*.py'
```

All configuration options are documented in `conf/mod_arcane_shredder.conf.dist`.
