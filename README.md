# mod-arcane-shredder

![Arcane Shredder — batch disenchant and lockbox unlocking for AzerothCore](banner.png)

Safe batch disenchanting and lockbox unlocking for AzerothCore WotLK 3.3.5a.

Arcane Shredder creates a server-authoritative preview of eligible items in the
player's regular inventory, issues a short-lived single-use confirmation token,
and validates every item again immediately before processing it. The companion
addon provides the player interface; the server remains the source of truth.

## Features

- Server-generated preview before any item is changed.
- Per-item exclusion and explicit confirmation.
- Configurable quality, item-level, combat, and high-risk item policies.
- Revalidation by item GUID, ownership, position, state, and fingerprint.
- Standard AzerothCore `LootTemplates_Disenchant` rewards and skill updates.
- Recovery mail fallback when generated materials do not fit in the inventory.
- Batch unlocking of lockboxes with the known Pick Lock spell and sufficient Lockpicking skill. Lockboxes and contents remain in the bags for normal opening later.
- No AzerothCore core patches, custom SQL, NPCs, game objects, spells, or items.

Equipped items, bag containers, bank, keyring, buyback, trade, mail, auction,
and guild-bank inventories are never scanned.

## Requirements and compatibility

- AzerothCore for World of Warcraft WotLK 3.3.5a.
- `AddonChannel = 1` in `worldserver.conf`.
- Disenchanting requires the Disenchant spell and sufficient Enchanting skill. Lockbox unlocking requires Pick Lock and sufficient Lockpicking skill for each lock.
- The matching [Arcane Shredder UI addon](https://github.com/eveletspb/arcane-sheredder-ui).

The server module and addon currently use ASHRED protocol version `2` and must be
updated together. The module was last compiled against
[`mod-playerbots/azerothcore-wotlk` commit `73957b8c9d6e`](https://github.com/mod-playerbots/azerothcore-wotlk/commit/73957b8c9d6efb48aef5145416e02bc0795adf75).
It has no compile-time dependency on Playerbots, but compatibility should be
rechecked when AzerothCore changes the inventory, loot, command, or ScriptMgr APIs.

## Installation

1. Clone the module into your AzerothCore checkout:

   ```bash
   git clone https://github.com/eveletspb/mod-arcane-shredder.git \
     modules/mod-arcane-shredder
   ```

2. Reconfigure and rebuild `worldserver` using your normal AzerothCore build flow.
3. Copy the installed `mod_arcane_shredder.conf.dist` to
   `mod_arcane_shredder.conf` in the server's module configuration directory.
4. Review the configuration before starting `worldserver`.
5. Install the companion addon from
   [eveletspb/arcane-sheredder-ui](https://github.com/eveletspb/arcane-sheredder-ui)
   into the WotLK client's `Interface/AddOns` directory.

No SQL migrations are required.

## Configuration

All options are documented in
[`conf/mod_arcane_shredder.conf.dist`](conf/mod_arcane_shredder.conf.dist).

| Option | Default | Purpose |
|---|---:|---|
| `ArcaneShredder.Enable` | `1` | Enables the module. |
| `ArcaneShredder.MaxBatchSize` | `25` | Limits one preview/confirmation batch. |
| `ArcaneShredder.PreviewLifetimeSeconds` | `30` | Sets confirmation-token lifetime. |
| `ArcaneShredder.CommandCooldownMilliseconds` | `500` | Limits state-changing protocol requests. |
| `ArcaneShredder.AllowInCombat` | `0` | Allows preview/confirmation in combat. |
| `ArcaneShredder.AllowRare` | `1` | Allows rare items within server policy. |
| `ArcaneShredder.AllowEpic` | `0` | Allows epic items within server policy. |
| `ArcaneShredder.AllowRefundable` | `0` | Allows refundable vendor items. |
| `ArcaneShredder.AllowBopTradeable` | `0` | Allows temporarily tradeable BoP items. |
| `ArcaneShredder.AllowEnchanted` | `0` | Allows enchanted items. |
| `ArcaneShredder.AllowSocketed` | `0` | Allows socketed items. |
| `ArcaneShredder.MaxItemLevel` | `0` | Sets the global item-level cap; `0` disables it. |
| `ArcaneShredder.ProtectedItemIds` | empty | Protects comma-separated item entry IDs. |
| `ArcaneShredder.AuditLog` | `1` | Writes one summary log line per confirmed batch. |

Configuration can be reloaded. A reload invalidates active previews so confirmation
always uses the current server policy.

## Usage

- `/ashred` opens the addon.
- `.ashred status` displays server and addon-channel diagnostics.
- `.ashred` displays command help.

The addon has separate **Disenchant** and **Unlock lockboxes** tabs. The lockbox tab uses bag and binding filters; the addon's quality and item-level controls apply only to disenchanting. The server's global item-level cap still applies to both modes. Both tabs provide a server-generated preview,
per-item exclusion, and a preview expiration timer. Preview, exclude, cancel, and
confirm requests are accepted only through AzerothCore's hidden addon-message
transport. Visible chat and `CHAT_MSG_SYSTEM` are not used as a machine API.

### Batch lockbox unlocking

The companion addon's **Unlock lockboxes** tab previews locked, lootable
containers in the regular inventory. A character must know **Pick Lock** and
have enough **Lockpicking** skill for each lock. The server applies the selected
bag and binding filters, its protected-item and item-level policies, and the
same preview size and expiry limits used for disenchanting.

After confirmation, the module resolves each item by GUID and repeats the
ownership, position, item-state, spell, and skill checks. Successful processing
sets the item's unlocked flag and updates Lockpicking skill as the Core's Pick
Lock effect does. It does **not** open the container, generate or collect loot,
or destroy the item. The player can open each unlocked lockbox normally
afterward. Already unlocked or changed items are skipped.

## Safety model

- The server determines item eligibility; addon filters can only narrow it.
- At most one preview is active per character.
- A newer preview, cancel, confirm, logout, timeout, configuration reload, module
  disable, or shutdown invalidates the token.
- Confirm consumes the token before processing and resolves every item again by GUID.
- Disenchant loot is validated before the destructive boundary.
- Disenchanting removes the source item only after every generated material has been delivered to inventory or passed to the standard recovery-mail helper.
- Unlocking only removes the lock flag. It does not open, loot, move, or consume the lockbox. The server repeats the Pick Lock spell and Lockpicking requirement checks before each item.
- Moving, equipping, trading, changing, or deleting an item after preview causes the
  current validation to reject or skip it.

Preview lists source items only. Disenchant loot is not pre-rolled, preventing
players from cancelling an unfavourable result and rerolling RNG.

## Troubleshooting

### The addon reports that the transport is unavailable

Run `.ashred status` and verify that `AddonChannel = 1` is set in
`worldserver.conf`. Restart `worldserver` after changing the core setting.

### The addon reports a protocol mismatch

Update both this server module and the companion addon. Their ASHRED protocol
versions must match.

### Preview contains no items

For disenchanting, verify that the character knows Disenchant, has sufficient Enchanting skill, and that the quality filters permit the items. For lockboxes, verify Pick Lock and the Lockpicking skill required by each lock; already unlocked boxes are excluded. Also check the selected bags, binding types, and server item-level policy.
Equipped, banked, traded, refundable, socketed, or enchanted items may be excluded
by the safety policy.

### Materials are not in the inventory

When inventory storage fails, Arcane Shredder uses AzerothCore's standard item
retrieval mail flow. Check the character's mailbox and the worldserver log.

## Verification

Run the repository contract tests:

```bash
python3 -m unittest discover -s tests -p 'test_*.py'
```

Release builds should also compile `worldserver` normally and with CMake option
`-DNOPCH=1`, as recommended by the AzerothCore skeleton module.

## Author and license

Author: [eveletspb](https://github.com/eveletspb)

Arcane Shredder is distributed under the
[GNU Affero General Public License v3.0](LICENSE) (`AGPL-3.0-only`).
